/* sdcard — see sdcard.h for the bus problem, the timing rules and what goes in
 * the playlist. */
#include "sdcard.h"
#include "settings.h"
#include "display.h"
#include "bsp_cores3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdcard";

/* The slot's chip select. The other three lines — clock, MOSI, MISO — are the
 * bus everything else on this board shares, already brought up by bsp. */
#define SD_CS_GPIO       4
#define SD_MOUNT         "/sd"

/* [D] 20 MHz. The card and the wiring would take more, but the bus is shared
 * with a display that has its own timing and a W5500 that carries the beat:
 * going faster buys milliseconds on a write that happens every few minutes,
 * and risks the two things that must not wobble. */
#define SD_FREQ_KHZ      20000

static sdcard_state_t s_state;
static sdmmc_card_t  *s_card;

/* The threshold is a setting now — see settings_get_sd_threshold_s. It does two
 * jobs: how long a track must play to count, and how much silence closes one
 * playlist file and opens the next. */
static int64_t threshold_us(void)
{
    return (int64_t)settings_get_sd_threshold_s() * 1000000;
}
#define PLAYERS_MAX         8      /* device numbers 1..6, generously */
#define TITLE_MAX          64
#define META_WAIT_MS     8000      /* how long a line waits for its title */
/* The last track we were told the title of. [M] Metadata arrive some ten
 * seconds after the status that announced the track, and the threshold is a
 * minute: they are known long BEFORE the line is prepared, not after, so they
 * have to be kept until the line needs them.
 *
 * ONE SLOT IS ENOUGH because they are fetched for the master alone. A second
 * player's track goes in with its id and no title, which still identifies it
 * in rekordbox afterwards. */
static struct {
    uint32_t track_id;
    char     title[TITLE_MAX];
    char     artist[TITLE_MAX];
} s_meta;

static FILE *s_csv;
static int64_t s_last_play_us;   /* when a player was last seen playing */
static bool  s_csv_failed;   /* a failure stops the retrying, see csv_open */
static sdcard_format_t s_fmt = SDCARD_FMT_NONE;
static volatile bool s_fmt_wanted;

/* The line waiting to be written, if any. Held here rather than written on the
 * spot because the moment may not allow it — see the note at the top. */
static struct {
    bool     pending;
    uint8_t  device;
    uint32_t track_id;
    int64_t  when_us;                  /* time since boot, for the stamp */

    /* Where the track lives, so the title can be asked for. The player that
     * PLAYS it and the one that HOLDS the medium are not always the same. */
    uint8_t  media_dev, media_slot, media_type;
    int64_t  asked_us;                 /* when we asked, 0 = not yet   */
    bool     no_title;                 /* asked, and given up waiting  */
    char     title[TITLE_MAX];
    char     artist[TITLE_MAX];
    double   bpm;
} s_line;

sdcard_state_t sdcard_state(void) { return s_state; }

/* --- HOLDING THE BUS THE WAY EVERYONE ELSE DOES ------------------------------
 *
 * The card goes through ESP-IDF's SPI driver, which serialises the bus for us
 * but knows nothing of the quiet window: left alone it would start a
 * transaction in the middle of one. So every card operation is bracketed the
 * same way the display brackets a flush — wait for the window to close, take
 * the lock, do the work, release.
 *
 * Taking the lock for a whole FATFS call also covers the gaps between the card
 * transactions inside it, which is more than strictly needed. It is the only
 * granularity available: FATFS does not let us in between its own operations,
 * and the alternative is not bracketing at all.
 *
 * [M] A MOUNT WITH A CARD IN HOLDS THE BUS FOR 81 ms — measured with two
 * players running and the bridge active — against 23 ms on an empty slot. The
 * beat was untouched through it: the quiet window did its job, and the figure
 * for that window was +0..+2825 us with 15 us spent waiting for the bus.
 *
 * [U] IT IS COUNTED AS "network" IN THE SPI FIGURES. bsp_spi_lock is described
 * as "the W5500, and anyone but the display", so a card operation appears under
 * the network's name — which is how 81 ms of card showed up as 81 ms of rete.
 * Harmless to the running, misleading to whoever reads the numbers. */
static void sd_bus_take(void)
{
    while (bsp_spi_in_quiet_window()) vTaskDelay(1);
    bsp_spi_lock();
    bsp_dc_as_miso();          /* the card answers on MISO, as the W5500 does */
}

static void sd_bus_give(void)
{
    bsp_spi_unlock();
}


/* --- IS THERE A CARD? A PURELY ELECTRICAL LOOK ------------------------------
 *
 * An SD card holds a pull-up on DAT3, which in SPI mode is the chip select —
 * this pin. Pulling it down from our side and reading it back therefore says
 * whether a card is in the slot, for a hundred microseconds and without a
 * single byte on the bus.
 *
 * [M] IT WORKS, measured on hardware:
 *
 *     empty slot   pulled down -> 0, pulled up -> 1     (floating)
 *     card in      pulled down -> 1, pulled up -> 1     (held high)
 *
 * The card's pull-up beats the ESP32's internal pull-down of some 45k, which
 * was the doubt worth checking: had the two been comparable the divider would
 * have given a middling voltage and the reading a coin toss.
 *
 * READ TWICE, and it is the PULLED-DOWN one that carries the answer. Pulled up,
 * both cases read 1 and say nothing; the second reading is there to tell a
 * floating pin from one held low, which would mean something unexpected on this
 * line.
 *
 * WHY GPIO 4 AND NOT MISO. GPIO 35 is also the display's DC line: reconfiguring
 * it mid-frame leaves the panel unable to tell commands from data, and if the
 * W5500 is mid-transaction what comes back is the W5500's answer, not the
 * card's. The chip select is ours alone, and it is where the pull-up actually
 * is — on DAT0/MISO many cards have one but the specification does not require
 * it.
 *
 * ONLY WITH THE CARD UNMOUNTED. Once mounted, the SPI driver owns this pin and
 * reconfiguring it underneath would break the next transaction. */
static int probe_cs_electrically(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SD_CS_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    esp_rom_delay_us(50);                  /* let the line settle */
    const int down = gpio_get_level(SD_CS_GPIO);

    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    esp_rom_delay_us(50);
    const int up = gpio_get_level(SD_CS_GPIO);

    /* Left as it was found: the mount that follows configures the pin itself,
     * and leaving a pull enabled under the SPI driver is asking for trouble. */
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);

    /* REPORTED WHEN IT CHANGES, not for the first few attempts and then never:
     * the reading that matters is the one taken with a card in, and a card
     * arrives when it arrives — [M] fifty seconds after a fixed count of six
     * had already run out, in the first field test. */
    static int last = -1;
    const int reading = (down << 1) | up;
    if (reading != last) {
        last = reading;
        const char *what = (down && up)   ? "held HIGH: a card"
                         : (!down && up)  ? "floating: empty slot"
                         : (!down && !up) ? "held LOW: unexpected"
                                          : "inconsistent";
        ESP_LOGI(TAG, "cs probe: pulled down -> %d, pulled up -> %d  (%s)",
                 down, up, what);
    }
    return down;
}


/* The mount itself, shared by the first attempt at start-up and every later one
 * when a card appears. Sets s_state and returns what happened. */
static esp_err_t mount_now(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = BSP_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_GPIO;
    slot.host_id = BSP_SPI_HOST;

    /* format_if_mount_failed STAYS FALSE. A card that will not mount is a card
     * with something on it we do not understand — somebody else's data, a
     * filesystem we cannot read, a failing card. Formatting it would destroy
     * whatever that is to gain a feature nobody asked for at that moment. The
     * badge turns red instead, and the user decides. */
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files              = 2,
        .allocation_unit_size   = 16 * 1024,
    };



    sd_bus_take();
    const esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &cfg, &s_card);
    sd_bus_give();
    if (err == ESP_OK) {
        s_state = SDCARD_IDLE;
        return ESP_OK;
    }

    /* NO CARD AND A BROKEN CARD ARE DIFFERENT THINGS, and the badge says which:
     * absent means the slot is empty and nothing is wrong, an error means there
     * is a card we cannot use — which is worth seeing. */
    s_card  = NULL;
    s_state = (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND)
            ? SDCARD_ABSENT : SDCARD_ERROR;
    return err;
}


/* How often the electrical probe looks. [M] It costs 100 us and no bus time,
 * so it can be asked often: half a second means a card is noticed almost as
 * soon as it goes in. */
#define PROBE_MS            500
#define CHECK_PRESENT_MS   2000

static int64_t s_next_check_us;
static int64_t s_next_probe_us;

/* Undoes the mount. The card may already be gone, in which case unmounting
 * fails and it does not matter: what we are after is our own state, not the
 * card's. */
static void unmount(void)
{
    /* fclose flushes, and the unmount writes too: both talk to the card and
     * both need the bus held, like everything else here. */
    sd_bus_take();
    if (s_csv) { fclose(s_csv); s_csv = NULL; }

    /* THE CARD COMING OUT CLEARS THE GIVING-UP. Taking it out and putting it
     * back is the gesture that means "try again": a different card, or the same
     * one seated properly, deserves a fresh attempt. */
    s_csv_failed   = false;
    s_fmt          = SDCARD_FMT_NONE;
    s_line.pending = false;

    if (s_card) esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
    sd_bus_give();
    s_card  = NULL;
    s_state = SDCARD_ABSENT;
}

/* ============================================================================
 * THE PLAYLIST
 *
 * A track counts as played once it has been in CONTINUOUS PLAYBACK past
 * the threshold. Pausing does not reset the stopwatch — it stops it — so a
 * track played, paused briefly and resumed is one entry, not two.
 *
 * THE STATE IS PER PLAYER. Each has its own stopwatch and remembers the last
 * track it wrote, which is what makes the rules come out right:
 *
 *   A, pause, A on one player   ONE line: nothing displaced the memory
 *   A, B, A on one player       TWO lines for A: B displaced it in between
 *   A on two players            TWO lines, and the device column says which
 *
 * ONLY THE LAST TRACK IS REMEMBERED, not every track of the evening: a playlist
 * is a sequence, and a track played again later is a thing that happened again.
 * ==========================================================================*/

typedef struct {
    uint32_t track_id;         /* what is loaded now, 0 = nothing        */
    uint32_t written_id;       /* the last one we wrote for this player  */
    int64_t  played_us;        /* how long it has been playing, total    */
    int64_t  since_us;         /* when the current playing spell started */
    bool     playing;

    /* WHERE THE TRACK LIVES, KEPT FROM WHEN IT WAS TOLD. [M] The Dr/Sr/Tr
     * fields are not filled in every status: the same player, for the same
     * track, gives device 1 slot 3 in one packet and zeros in the next. Read
     * at the moment the line is prepared they are as likely to be empty as
     * not, so they are taken whenever they look valid and kept.
     *
     * The player that PLAYS and the one that HOLDS are often different: [M] a
     * track played on player 2 from a medium in player 1 — the query goes to
     * the one that holds it. */
    uint8_t  media_dev, media_slot, media_type;
} player_t;

static player_t s_player[PLAYERS_MAX];



/* THE NAME MUST FIT 8.3, and that is not a stylistic choice: [M] ESP-IDF ships
 * with CONFIG_FATFS_LFN_NONE, so long names are off and a name of more than
 * eight characters cannot be created — fopen simply fails, and says nothing
 * about why.
 *
 * PLST0001.CSV uses the eight exactly: four for the prefix, four for the
 * number. That leaves room to make the prefix configurable later, as long as
 * whatever the user types is cut to four.
 *
 * CALLED WITH THE BUS ALREADY HELD, from csv_open: the directory scan talks to
 * the card like everything else, and taking the lock again here would be a
 * second, needless acquisition in the middle of one that is already ours. */
#define PLAYLIST_PREFIX  "PLST"

/* Finds the first free one by looking at what is already there.
 *
 * ONE PASS OVER THE DIRECTORY, not a stat per candidate: with n files present,
 * trying 1, 2, 3 in turn searches the directory n times over. Reading it once
 * and taking the highest is linear, and [D] with a hundred files on the card
 * costs a few milliseconds — paid once, when the file is opened.
 *
 * THE NUMBER COMES FROM THE CARD, not from a counter in NVS. A counter is a
 * piece of truth living apart from the files it describes, and the two drift:
 * format the card and the names would carry on from a number nobody can see.
 * Here, formatting the card starts again from 1 because there is nothing to
 * remember. */

static int next_playlist_number(void)
{
    DIR *d = opendir(SD_MOUNT);
    if (!d) return 1;

    int highest = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, PLAYLIST_PREFIX "%04d.CSV", &n) == 1 && n > highest)
            highest = n;
    }
    closedir(d);
    return highest + 1;
}

/* EVERY FILESYSTEM CALL HOLDS THE BUS THE SAME WAY the mount does. opendir,
 * fopen, fprintf and fsync all talk to the card, and leaving them unbracketed
 * lets the display take the bus in the middle of a card transaction: [M] the
 * card then answers 0x107 to everything, the write fails, and the retry fails
 * the same way — 1.04 SECONDS of bus held and 1.3 s of drains missed, measured
 * in the field.
 *
 * ONE FAILURE IS ENOUGH TO STOP. A card that will not take a file will not take
 * it a second later either, and retrying every turn is how a single failure
 * became a permanent storm. The badge turns red and the playlist gives up until
 * the card is taken out and put back — which is the gesture that means "try
 * again" here. */
static bool csv_open(void)
{
    if (s_csv)       return true;
    if (s_csv_failed) return false;

    sd_bus_take();

    char path[64];
    snprintf(path, sizeof(path), "%s/" PLAYLIST_PREFIX "%04d.CSV",
             SD_MOUNT, next_playlist_number());
    s_csv = fopen(path, "w");
    if (s_csv) {
        fprintf(s_csv, "time,player,track_id,title,artist,bpm\n");
        fflush(s_csv);
        fsync(fileno(s_csv));
    }
    sd_bus_give();

    if (!s_csv) {
        ESP_LOGE(TAG, "cannot create %s: giving up until the card is replaced", path);
        s_csv_failed = true;
        return false;
    }
    ESP_LOGI(TAG, "writing %s", path);
    return true;
}

/* Writes the pending line and makes sure it is ON THE CARD before returning.
 *
 * fsync AFTER EVERY LINE, and the file stays open in between. This device is
 * switched off by pulling its power — deliberately or otherwise — so a line
 * still sitting in a buffer is a line lost. fsync does what fclose does short
 * of releasing the handle: the data reaches the medium and the directory entry
 * is updated. Keeping the file open then costs nothing and saves searching the
 * directory for it on every track. */
static void csv_write_pending(void)
{
    if (!s_line.pending || !csv_open()) return;

    const uint32_t s = (uint32_t)(s_line.when_us / 1000000);

    sd_bus_take();
    const int written = fprintf(s_csv, "%02u:%02u:%02u,%u,%lu,\"%s\",\"%s\",%.2f\n",
            (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
            (unsigned)s_line.device, (unsigned long)s_line.track_id,
            s_line.title, s_line.artist, s_line.bpm);
    fflush(s_csv);
    fsync(fileno(s_csv));
    sd_bus_give();

    if (written <= 0) {
        ESP_LOGE(TAG, "write failed: giving up until the card is replaced");
        s_csv_failed = true;
        s_line.pending = false;
        return;
    }

    ESP_LOGI(TAG, "playlist: player %u, track %lu%s%s",
             (unsigned)s_line.device, (unsigned long)s_line.track_id,
             s_line.title[0] ? " - " : "", s_line.title);
    s_line.pending = false;
}

/* Is there a title to ask for, and for which track?
 *
 * ASKED ONLY FOR TRACKS THAT ARE GOING INTO THE FILE, once each. Metadata are a
 * TCP conversation with a player — [M] some 200 ms of handshake, setup and
 * query — and asking for every track the moment it is loaded would mean asking
 * for everything a DJ auditions and discards. A track that has passed the
 * threshold is one that was actually played, and there is at most one of those
 * a minute per player.
 *
 * Returns false when there is nothing to ask, when we have already asked, or
 * when the title is already known. */
bool sdcard_wants_metadata(uint8_t *player, uint8_t *dev, uint8_t *slot,
                           uint8_t *type, uint32_t *id)
{
    if (!s_line.pending || s_line.title[0] || s_line.asked_us) return false;
    if (s_line.no_title)                                       return false;
    if (!s_line.media_dev || s_line.media_type != 0x01)        return false;

    s_line.asked_us = esp_timer_get_time();
    *player = s_line.device;          /* who plays it: the caller keeps the
                                       * answer under that player's name */
    *dev  = s_line.media_dev;
    *slot = s_line.media_slot;
    *type = s_line.media_type;
    *id   = s_line.track_id;
    return true;
}


/* The title for a track, which arrives by its own road and on its own schedule.
 *
 * KEPT, NOT USED ON THE SPOT. [M] Metadata come some ten seconds after the
 * status that announced the track, while the line is prepared a minute in:
 * using them only to fill a waiting line would find nothing waiting, every
 * time, and every entry would go in titleless.
 *
 * A line ALREADY waiting is filled in too, for the case where a track passes
 * the threshold before its title has come back. */
void sdcard_note_metadata(uint32_t track_id, const char *title, const char *artist)
{
    if (!track_id) return;

    s_meta.track_id = track_id;
    snprintf(s_meta.title,  TITLE_MAX, "%s", title  ? title  : "");
    snprintf(s_meta.artist, TITLE_MAX, "%s", artist ? artist : "");

    if (s_line.pending && s_line.track_id == track_id) {
        snprintf(s_line.title,  TITLE_MAX, "%s", s_meta.title);
        snprintf(s_line.artist, TITLE_MAX, "%s", s_meta.artist);
    }
}


void sdcard_note_player(uint8_t device, bool playing, uint32_t track_id,
                        uint8_t media_dev, uint8_t media_slot,
                        uint8_t media_type, double bpm)
{
    if (!settings_get_sd_playlist()) return;
    if (device == 0 || device >= PLAYERS_MAX) return;

    player_t *p = &s_player[device];
    const int64_t now = esp_timer_get_time();

    /* A DIFFERENT TRACK RESETS THE STOPWATCH for this player, which counts one
     * track's playing time, and leaves the memory of what was written — that is
     * what makes A, B, A come out as two entries for A.
     *
     * THE MEDIUM IS NOT RESET. A new track is almost always on the same stick
     * in the same player, so keeping the coordinates means a line prepared
     * before the new status has carried them still knows where to ask. The risk
     * is asking the wrong player after a medium is swapped, and the answer to
     * that is an id and no title — which is what we would have had anyway. */
    if (track_id != p->track_id) {
        p->track_id  = track_id;
        p->played_us = 0;
        p->playing   = false;
    }

    /* Taken only when they say something: a zero device or a type that is not
     * rekordbox is the status not carrying them this time round, not the
     * track having moved. */
    if (media_dev && media_type == 0x01) {
        p->media_dev  = media_dev;
        p->media_slot = media_slot;
        p->media_type = media_type;
    }

    if (playing && !p->playing) {
        p->since_us = now;                 /* started, or resumed */
    } else if (!playing && p->playing) {
        p->played_us += now - p->since_us; /* paused: the total keeps its value */
    }
    p->playing = playing;
    if (playing) s_last_play_us = now;

    if (!playing || !track_id) return;

    const int64_t total = p->played_us + (now - p->since_us);
    if (total < threshold_us()) return;
    if (track_id == p->written_id) return;          /* already in the list */

    /* Past the threshold and not yet written. The line is prepared here and
     * written when the moment allows: a playlist entry is the least urgent
     * thing this device does, and the track has already been playing a minute. */
    if (s_line.pending) return;                     /* one at a time */
    s_line.pending  = true;
    s_line.device   = device;
    s_line.track_id = track_id;
    s_line.when_us  = now;
    s_line.bpm        = bpm;
    s_line.media_dev  = p->media_dev;
    s_line.media_slot = p->media_slot;
    s_line.media_type = p->media_type;
    s_line.asked_us   = 0;
    s_line.no_title   = false;
    /* The title if we happen to know it: kept from when the metadata came
     * back, which is long before this. */
    const bool known = (s_meta.track_id == track_id);
    snprintf(s_line.title,  TITLE_MAX, "%s", known ? s_meta.title  : "");
    snprintf(s_line.artist, TITLE_MAX, "%s", known ? s_meta.artist : "");

    p->written_id = track_id;

    /* WHY A TITLE WILL OR WILL NOT BE ASKED FOR. The two conditions that can
     * silently skip the request are the medium's device number and the track
     * type, and neither is visible from outside: a line that comes out bare
     * looks the same whether nobody was asked or the answer never came. */
    ESP_LOGI(TAG, "line for player %u track %lu: medium dev=%u slot=%u type=0x%02x%s",
             (unsigned)device, (unsigned long)track_id,
             (unsigned)media_dev, (unsigned)media_slot, (unsigned)media_type,
             (media_dev && media_type == 0x01) ? "" : "  -> no title will be asked");
}


/* --- THE CARD RUNS IN ITS OWN TASK -------------------------------------------
 *
 * [M] A mount attempt on an empty slot takes 23 ms. Run from the main loop that
 * was 23 ms of main not polling its sockets — absorbed by the W5500's buffers,
 * but lost all the same, and for work nobody is waiting on. In a task it costs
 * nothing but the task.
 *
 * PRIORITY AND CORE ARE NOT WHAT PROTECT THE BEAT: the bus is shared, and a
 * low-priority task holding it blocks exactly as a high-priority one would.
 * Low priority here only keeps the card from displacing anything on the CPU.
 * What keeps it off the wire at the wrong moment is sd_bus_take(), which waits
 * out the quiet window exactly as the display does. */
#define SD_TASK_STACK   4096
#define SD_TASK_PRIO    2
#define SD_TASK_CORE    1

static volatile bool s_relaxed;

/* --- FORMATTING, WHICH IS ASKED FOR HERE AND DONE IN THE TASK --------------
 *
 * ONLY ON AN EXPLICIT REQUEST. There is no path that reaches this by itself,
 * and the mount deliberately does not offer to format what it cannot read: a
 * card that will not mount holds something we do not understand, and erasing it
 * to gain a feature nobody asked for would be the worst thing this component
 * could do.
 *
 * THE REQUEST AND THE WORK ARE SEPARATE because the work takes SECONDS. Run
 * from the touch handler it would block the interface for its whole duration —
 * and the "formatting" line, which is the one thing the user needs to see,
 * would never be drawn. The handler leaves a request; the task picks it up and
 * publishes what is happening.
 *
 * IT HOLDS THE BUS THROUGHOUT, far beyond anything else here. The caller is
 * expected to have made sure no bridge is running. */


sdcard_format_t sdcard_format_state(void) { return s_fmt; }

/* The outcome has been seen: the row goes back to normal.
 *
 * NOT ON A TIMER. A format takes minutes, and whoever starts one goes and does
 * something else — a message that clears itself after a few seconds is one the
 * user very likely never sees. Leaving the menu is the signal that the outcome
 * has been read, and it is a deliberate gesture rather than a guess about
 * attention. */
void sdcard_format_ack(void)
{
    if (s_fmt == SDCARD_FMT_DONE || s_fmt == SDCARD_FMT_FAILED)
        s_fmt = SDCARD_FMT_NONE;
}


#if DJB_DEVELOPER
/* ============================================================================
 * SCREENSHOTS
 *
 * BMP, NOT PNG. The framebuffer is already RGB565 and a 16-bit BMP takes it
 * VERBATIM: a 70-byte header and the pixels, no compression, no library, no
 * CPU. PNG would mean zlib and a hundred milliseconds of squeezing to save
 * space on a card that has plenty. Converting on a PC costs nothing.
 *
 * TWO THINGS HAVE TO BE FIXED on the way out, and both are one line:
 *
 *   BYTE ORDER   the panel wants big-endian, a file wants little — the same
 *                swap display.c documents, applied backwards
 *   ROW ORDER    BMP stores the bottom row first, unless the height is written
 *                negative. Negative height is used here: it is understood
 *                everywhere that matters and saves reversing 240 rows
 *
 * A LINE AT A TIME, 640 bytes, because the framebuffer is in PSRAM and the card
 * wants a buffer it can send: copying the whole 150 KB into internal memory is
 * not possible, and one row is small enough to sit on the stack.
 * ==========================================================================*/
#define SHOT_PREFIX  "SCRN"
#define SHOT_W        320
#define SHOT_H        240

/* Called with the bus already held, like next_playlist_number. */
static int next_shot_number(void)
{
    DIR *d = opendir(SD_MOUNT);
    if (!d) return 1;

    int highest = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, SHOT_PREFIX "%04d.BMP", &n) == 1 && n > highest)
            highest = n;
    }
    closedir(d);
    return highest + 1;
}

/* Writes the 70-byte header of a 16-bit BMP with the 565 bit masks.
 *
 * BITMAPV4HEADER, not the older BITMAPINFOHEADER: 16-bit BMPs are ambiguous
 * without explicit masks — readers assume 555 and every colour comes out wrong,
 * shifted a bit to the green. The V4 header carries the masks and settles it. */
/* Every multi-byte field of a BMP is little-endian. Writing them through these
 * keeps the masking explicit: assigning a wider value to a uint8_t truncates
 * silently, which is right when intended and hides the case where it is not. */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

/* The 54-byte header of a plain 24-bit BMP.
 *
 * 24 BIT AND NOT 16, though the framebuffer is RGB565 and 16 would copy it
 * verbatim. A 16-bit BMP needs bit masks to say which bits are which colour,
 * those masks live in a header that comes in three incompatible lengths, and
 * readers disagree about all of it — [M] the first attempt here wrote the masks
 * of one variant with the length of another, and macOS Preview refused the
 * file. A 24-bit BMP has none of that: BITMAPINFOHEADER, no masks, and
 * everything opens it.
 *
 * The cost is three shifts per pixel and 230 KB instead of 150, on a card with
 * room to spare. */
static void bmp_header(uint8_t *h, uint32_t pixel_bytes)
{
    memset(h, 0, 54);
    h[0] = 'B'; h[1] = 'M';
    put32(&h[2],  54 + pixel_bytes);          /* file size */
    put32(&h[10], 54);                        /* pixels start here */
    put32(&h[14], 40);                        /* BITMAPINFOHEADER */
    put32(&h[18], SHOT_W);

    /* NEGATIVE HEIGHT means rows are stored top to bottom, which is how they
     * sit in the framebuffer: the alternative is reversing 240 rows on the way
     * out. Written as an unsigned pattern on purpose — two's complement is what
     * the file wants, and going through uint32_t says so. */
    put32(&h[22], (uint32_t)(-(int32_t)SHOT_H));

    put16(&h[26], 1);                         /* planes */
    put16(&h[28], 24);                        /* bits per pixel */
    put32(&h[30], 0);                         /* BI_RGB: no compression */
    put32(&h[34], pixel_bytes);
}

bool sdcard_save_screenshot(void)
{
    if (!s_card || s_csv_failed) return false;

    const uint16_t *fb = display_framebuffer();
    if (!fb) return false;

    char path[64];
    uint8_t hdr[54];
    uint8_t row[SHOT_W * 3];      /* 960 bytes, a multiple of 4: no padding */
    bool ok = true;

    sd_bus_take();
    snprintf(path, sizeof(path), "%s/" SHOT_PREFIX "%04d.BMP",
             SD_MOUNT, next_shot_number());
    FILE *f = fopen(path, "wb");
    if (f) {
        bmp_header(hdr, (uint32_t)SHOT_W * SHOT_H * 3);
        ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

        for (int y = 0; y < SHOT_H && ok; y++) {
            for (int x = 0; x < SHOT_W; x++) {
                /* TWO CONVERSIONS AT ONCE.
                 *
                 * The framebuffer is BIG-ENDIAN RGB565 — the panel's order,
                 * established by drawing three bands of red and looking, not by
                 * assumption (see display.c). So the halves are swapped first.
                 *
                 * Then 565 to 888, by moving each field to the top of its byte
                 * and REPEATING ITS HIGH BITS in the gap: 0x1F must become 0xFF
                 * and not 0xF8, or white comes out slightly grey and the whole
                 * picture reads dark.
                 *
                 * BMP stores BLUE FIRST. */
                const uint16_t p = __builtin_bswap16(fb[(size_t)y * SHOT_W + x]);
                const uint8_t r5 = (p >> 11) & 0x1f;
                const uint8_t g6 = (p >>  5) & 0x3f;
                const uint8_t b5 =  p        & 0x1f;
                row[x * 3 + 0] = (uint8_t)((b5 << 3) | (b5 >> 2));
                row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
                row[x * 3 + 2] = (uint8_t)((r5 << 3) | (r5 >> 2));
            }
            ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
        }
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    } else {
        ok = false;
    }
    sd_bus_give();

    if (ok) ESP_LOGI(TAG, "screenshot: %s", path);
    else    ESP_LOGE(TAG, "screenshot failed: %s", path);
    return ok;
}
#endif


void sdcard_request_format(void)
{
    if (!s_card || s_fmt == SDCARD_FMT_BUSY) return;
    s_fmt_wanted = true;
    s_fmt        = SDCARD_FMT_BUSY;   /* set HERE so the first redraw shows it */
}

/* Runs in the task. */
static void format_now(void)
{
    s_fmt_wanted = false;

    /* The playlist file is closed first: formatting the medium under an open
     * handle would leave FATFS holding a reference to something that is no
     * longer there. */
    if (s_csv) {
        sd_bus_take();
        fclose(s_csv);
        sd_bus_give();
        s_csv = NULL;
    }
    s_line.pending = false;

    /* [M] 98 SECONDS on a card of ordinary size, measured — not the "few
     * seconds" one would guess. The bus is held throughout, so the display is
     * frozen for all of it and nothing can report progress: the honest thing
     * is to say beforehand how long it takes. */
    ESP_LOGW(TAG, "formatting the card: this takes a minute or two, "
                  "and holds the bus throughout");
    sd_bus_take();
    const esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT, s_card);
    sd_bus_give();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        /* THE CARD IS OUT OF USE, exactly as after a failed write: whatever
         * state the medium is in after a format that did not finish, it is not
         * one to start writing a playlist on. The badge turns red and stays
         * red until the card is taken out. */
        s_csv_failed = true;
        s_fmt        = SDCARD_FMT_FAILED;
        return;
    }

    ESP_LOGI(TAG, "card formatted");
    s_csv_failed  = false;
    s_fmt         = SDCARD_FMT_DONE;
}


void sdcard_set_relaxed(bool relaxed) { s_relaxed = relaxed; }

static void sd_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int64_t now = esp_timer_get_time();

        /* MOUNTED OR NOT is what splits the two branches, and not the state:
         * a mounted card that refuses to take a file is SDCARD_ERROR too, and
         * sending that one down the mounting path would have it try to mount
         * what is already mounted. */
        /* THE FORMAT COMES FIRST, before anything else this turn: it is the
         * only thing here the user is actually waiting for. */
        if (s_fmt_wanted && s_card) {
            /* A MOMENT FOR THE MESSAGE TO REACH THE SCREEN. Once the format
             * starts the display cannot draw at all — GPIO 35 is its DC line
             * and the card's MISO at once, so the bus is held for the whole
             * operation — and whatever is on screen at that instant is what
             * stays there. [M] For 98 seconds, measured. Starting before the
             * "formatting" line has been painted would leave the user looking
             * at the previous frame and wondering. */
            vTaskDelay(pdMS_TO_TICKS(400));
            format_now();
        }

        if (s_card) {
            if (now >= s_next_check_us) {
                s_next_check_us = now + (int64_t)CHECK_PRESENT_MS * 1000;

                /* STILL THERE? The question goes to the CARD, not to the
                 * filesystem: one pulled mid-write leaves FATFS believing all
                 * is well. */
                sd_bus_take();
                const esp_err_t st = sdmmc_get_status(s_card);
                sd_bus_give();
                if (st != ESP_OK) {
                    ESP_LOGW(TAG, "card gone");
                    unmount();
                }
            }

            /* THE PENDING LINE, if any, and only if the moment allows. Same
             * rule as the mount: a write holds the bus, and holding it through
             * a tempo change is that change arriving late in every player
             * following us. A playlist entry can wait.
             *
             * THE SETTING IS CHECKED HERE TOO, not only where the line is
             * prepared: switching the option off with a line already queued
             * should mean no more writing, not one more line. */
            const bool want = settings_get_sd_playlist();
            if (!want) s_line.pending = false;

            /* --- SILENCE ENDS A PLAYLIST -----------------------------------
             *
             * When no player has been playing for as long as a track needs to
             * count, the evening is over as far as this file is concerned: it
             * is closed, and whatever comes next opens a new one.
             *
             * THE SAME FIGURE FOR BOTH, by choice: one number to set instead of
             * two, and the two ideas are related — the silence that separates
             * two sets is longer than the gap between two tracks, and so is the
             * time that makes a track count as played.
             *
             * NOTHING IS LOST BY CLOSING: every line was written and synced as
             * it came. Closing only means the next line starts a new file. */
            if (s_csv && s_last_play_us &&
                now - s_last_play_us > threshold_us()) {
                ESP_LOGI(TAG, "no player for %us: closing this playlist",
                         (unsigned)settings_get_sd_threshold_s());
                sd_bus_take();
                fclose(s_csv);
                sd_bus_give();
                s_csv = NULL;
                s_last_play_us = 0;      /* not again until someone plays */
            }

            if (s_line.pending) {
                /* A TITLE IS WORTH WAITING FOR, BUT NOT FOR EVER. If no answer
                 * has come in META_WAIT_MS the line goes in with its id alone:
                 * the player may be busy, the medium may have been ejected, and
                 * an entry with an id still identifies the track.
                 *
                 * SEQUENTIAL, NOT ALTERNATIVE: giving up and writing happen on
                 * the SAME turn. Chained as else-if, the turn that gave up
                 * would not write, and the line would wait another 200 ms for
                 * no reason. */
                if (s_line.asked_us && !s_line.title[0] && !s_line.no_title &&
                    now - s_line.asked_us > (int64_t)META_WAIT_MS * 1000) {
                    ESP_LOGW(TAG, "no title for track %lu: writing it without",
                             (unsigned long)s_line.track_id);
                    s_line.no_title = true;
                }

                /* Written once the title is in, or once we have stopped waiting
                 * for it — and only when the moment allows. */
                const bool ready = s_line.title[0] || s_line.no_title ||
                                   !s_line.media_dev;
                if (ready && s_relaxed) csv_write_pending();
            }

        } else if (now >= s_next_probe_us) {
            s_next_probe_us = now + (int64_t)PROBE_MS * 1000;

            /* THE ELECTRICAL LOOK FIRST, and a mount only if it says there is
             * something to mount. [M] The probe costs 100 us and no bus time at
             * all; a mount on an empty slot costs 23 ms of waiting for an
             * answer that will not come. Asking the cheap question first means
             * the expensive one is only asked when it can succeed.
             *
             * The mount still waits for a relaxed moment — [M] it holds the bus
             * for 81 ms, which is what needs room before the next beat. The
             * probe does not: it touches no bus. */
            if (probe_cs_electrically() && s_relaxed) {

                const int64_t t0 = esp_timer_get_time();
                const esp_err_t err = mount_now();
                const int ms = (int)((esp_timer_get_time() - t0) / 1000);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "card mounted, %d ms", ms);
                } else {
                    /* The pin says a card is there and it will not mount: a
                     * filesystem we cannot read, or a card on its way out. */
                    ESP_LOGW(TAG, "a card is in the slot but will not mount: %s",
                             esp_err_to_name(err));
                }
            }
        }


        /* THE BADGE, derived from everything that is true right now and from
         * nothing that was true earlier.
         *
         * IT SITS OUTSIDE BOTH BRANCHES on purpose. Computed inside the mounted
         * branch it would run after unmount() had already set ABSENT, and put
         * the badge back to green for a card that had just been pulled — which
         * is what happens when a value is assigned in one place and corrected
         * in another.
         *
         * s_card answers "is there a card", s_csv_failed "has the playlist
         * given up", the setting "does the user want one": three questions, one
         * expression, no order to get wrong. */
        s_state = !s_card                        ? SDCARD_ABSENT
                : s_csv_failed                   ? SDCARD_ERROR
                : settings_get_sd_playlist()     ? SDCARD_WRITING
                                                 : SDCARD_IDLE;


        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t sdcard_init(void)
{
    /* QUIET THE TWO LAYERS UNDERNEATH, both of which announce things that are
     * not faults.
     *
     * vfs_fat_sdmmc prints an error at E level for every failed mount — one per
     * probe, for as long as a card is missing.
     *
     * sdspi_transaction reports "cmd=5, R1 response: command not supported" on
     * every successful mount. CMD5 asks whether the thing in the slot is an
     * SDIO device; a memory card does not implement it, and that refusal is
     * exactly how the driver learns it is talking to a memory card. A step of
     * the identification working as designed, announced as if it were a
     * problem.
     *
     * Our own badge and log say what there is to say about the card. */
    esp_log_level_set("vfs_fat_sdmmc",     ESP_LOG_NONE);
    esp_log_level_set("sdspi_transaction", ESP_LOG_NONE);
    const esp_err_t err = mount_now();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "card mounted: %lluMB, %s",
                 ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024),
                 s_card->cid.name);
    } else if (s_state == SDCARD_ABSENT) {
        ESP_LOGI(TAG, "no card in the slot");
    } else {
        ESP_LOGW(TAG, "card present but unusable: %s", esp_err_to_name(err));
    }

    /* The task runs whatever the outcome: a slot empty now may not be later. */
    xTaskCreatePinnedToCore(sd_task, "sdcard", SD_TASK_STACK, NULL,
                            SD_TASK_PRIO, NULL, SD_TASK_CORE);
    return err;
}

