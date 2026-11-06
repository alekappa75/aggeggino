/* ============================================================================
 * audio.c — UI sound feedback.
 *
 * HARDWARE BRING-UP:
 *   - reset the AW88298 through AW9523 P0_2 (it comes up held in reset)
 *   - configure the AW88298 over I2C (16-bit big-endian registers)
 *   - I2S TX, 16 bit, WITHOUT MCLK — GPIO0 is a strapping pin and driving it
 *     disturbed the W5500
 *
 * PLAYBACK:
 *   - samples PRE-RENDERED in flash (audio_samples.c): no RAM, no synthesis
 *   - a task on CORE 1 that SLEEPS on the queue, so idle costs nothing
 *   - audio_play() does not block: it enqueues, overwriting, and returns
 *   - a new event REPLACES the current one at the first zero crossing
 * ==========================================================================*/
#include "audio.h"
#include "audio_samples.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp_cores3.h"

static const char *TAG = "audio";

/* ---- CoreS3-SE I2S pins ----
 *
 * MCLK (GPIO0) is deliberately unused: it is an ESP32-S3 strapping pin, and the
 * AW88298 does not need a master clock — it locks to BCLK. [M] Driving it
 * disturbed the W5500. */
#define AUDIO_I2S_BCLK   GPIO_NUM_34
#define AUDIO_I2S_LRCLK  GPIO_NUM_33
#define AUDIO_I2S_DOUT   GPIO_NUM_13

#define AUDIO_SAMPLE_RATE   16000
#define AW88298_ADDR        0x36
#define AW9523_ADDR         0x58
#define AW88298_REG06_16K   0x14C3

/* Block playback: how many samples between queue checks, for a possible new
 * event and its zero-crossing replacement. 80 samples at 16 kHz = 5 ms. */
#define AUDIO_BLOCK         80

static bool                    s_ready  = false;
static bool                    s_enabled = true;   /* global sound on/off (UI setting) */
static uint8_t                 s_volume = 30;   /* ~15 dB below maximum: the 1 W
                                                  * speaker distorts if pushed */
static i2s_chan_handle_t       s_tx     = NULL;
static i2c_master_dev_handle_t s_aw88298 = NULL;
static QueueHandle_t           s_queue  = NULL;   /* one-slot queue, overwriting */
static TaskHandle_t            s_task   = NULL;

/* Event to sample mapping, in flash. */
/* Event to sample mapping, in flash.
 *
 * 'atomic' marks a short sound that must ALWAYS play in full, with no
 * zero-crossing replacement: clicks are transients a few milliseconds long,
 * with all their energy in the initial attack, and truncating one makes it
 * sound weak or skipped. Long sounds (beeps) stay interruptible, and there
 * replacement makes sense. */
typedef struct { const int16_t *pcm; uint32_t len; bool atomic; } audio_clip_t;
static audio_clip_t s_clip[AUDIO_EVENT_COUNT];
static volatile bool s_busy = false;   /* is an atomic sound in progress? */

/* ---- AW88298 register write: 16 bit, BIG-ENDIAN ---- */
static esp_err_t aw_write(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_aw88298, buf, sizeof(buf), 100);
}

/* Volume 0..100 mapped onto register 0x0C (HAGCCFG4).
 *
 * [E] Structure from the AW88298 datasheet:
 *   bits 15:8 = VOL: [7:4] steps of -6 dB, [3:0] steps of -0.5 dB. 0x00 is
 *               0 dB, and larger means MORE attenuation, down to about -96 dB.
 *   bits 7:0  = HOLDTH: the AGC hold time. It is NOT volume — default 0x64.
 *
 * That second field is the trap: putting attenuation in the wrong byte, or
 * treating the whole register as a volume, yields a control that appears not
 * to respond at all.
 *
 * We map 0..100 onto 0..-40 dB linearly: 100 is 0 dB, 0 is nearly mute. */
static uint16_t vol_to_reg(uint8_t v)
{
    if (v > 100) v = 100;
    int att_db = (100 - v) * 40 / 100;        /* 0..40 dB of attenuation */
    int hi = att_db / 6;                      /* 6 dB steps   (high nibble) */
    int lo = (int)((att_db - hi * 6) / 0.5);  /* 0.5 dB steps (low nibble) */
    if (hi > 15) hi = 15;
    if (lo > 15) lo = 15;
    uint8_t volbyte = (uint8_t)((hi << 4) | lo);
    return ((uint16_t)volbyte << 8) | 0x64;   /* low byte = HOLDTH default */
}

static esp_err_t aw88298_configure(void)
{
    ESP_RETURN_ON_ERROR(aw_write(0x61, 0x0673), TAG, "aw");   /* boost mode             */
    ESP_RETURN_ON_ERROR(aw_write(0x04, 0x4040), TAG, "aw");   /* I2SEN=1 AMPPD=0 PWDN=0 */
    ESP_RETURN_ON_ERROR(aw_write(0x05, 0x0008), TAG, "aw");   /* HMUTE=0 and so on      */
    ESP_RETURN_ON_ERROR(aw_write(0x06, AW88298_REG06_16K), TAG, "aw");
    ESP_RETURN_ON_ERROR(aw_write(0x0C, vol_to_reg(s_volume)), TAG, "aw");
    return ESP_OK;
}

static esp_err_t i2s_setup(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* DMA buffers smaller than the default (frame=240 -> 15 ms, desc=6 -> 90 ms).
     *
     * The default's large buffer left a 15 ms "misalignment window": when a
     * click was written, its attack could land in a buffer the DMA had already
     * partly transmitted, and was occasionally eaten. With frame=64, 4 ms, the
     * window shrinks to 4 ms and the attack almost always starts clean.
     *
     * We stay conservative and do NOT go down to 2-3 ms, to keep away from
     * underrun risk: desc=8 x 4 ms leaves 32 ms of reserve. And auto-clear
     * emits zeros if a buffer runs dry, so silences never glitch. */
    cc.dma_frame_num = 64;    /* 4 ms per buffer, default 240 = 15 ms  */
    cc.dma_desc_num  = 8;     /* 32 ms of total reserve, default 6   */
    /* --- AUTO-CLEAR: 'before', not 'after' ---------------------------------
     *
     * Auto-clear exists so the DMA emits ZEROS when we are not writing, rather
     * than recycling the last buffer. The channel can then stay enabled all the
     * time: silences are clean, and there is no enable/disable per sound, which
     * would click like a relay.
     *
     * BUT WHEN IT CLEARS MATTERS. The legacy `auto_clear` field is an alias of
     * `auto_clear_after_cb`, which clears a descriptor AFTER transmitting it.
     * If i2s_channel_write has meanwhile copied the next sound into it, the
     * clear ERASES that data — heard as a 4 ms hole, one descriptor, INSIDE the
     * sound.
     *
     * That explains why the defect appeared only on RAPID REPETITION: with
     * isolated clicks the descriptors are idle and already zeroed, so a write
     * lands cleanly; repeating fast they cycle continuously and the race
     * happens.
     *
     * `auto_clear_before_cb` clears BEFORE, at a point where it cannot
     * overwrite freshly written data.
     *
     * IF THAT WERE NOT ENOUGH: the safe route is to disable auto-clear entirely
     * and append to every sound a TAIL OF SILENCE longer than the DMA reserve
     * (32 ms). Then we zero the buffers ourselves, with our own data, and no
     * ISR touches them — no race possible, at the cost of 2 KB written after
     * each sound. */
    cc.auto_clear           = false;
    cc.auto_clear_before_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx, NULL), TAG, "i2s");

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_BCLK,
            .ws   = AUDIO_I2S_LRCLK,
            .dout = AUDIO_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &sc), TAG, "i2s");
    /* Enabled ONCE and left running: with auto-clear the silences are zeros,
     * so there is no click from enabling and disabling around each sound. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s");
    return ESP_OK;
}

/* ---- play one clip, in blocks, with zero-crossing replacement --------------
 *
 * Returns true if the clip was interrupted by a new event, already read into
 * *next, and false if it reached the end. It writes in blocks of AUDIO_BLOCK
 * samples and peeks at the queue between blocks: if a new event is waiting, it
 * finds the next zero crossing — a sign change — and cuts there, which makes
 * the transition clean instead of clicking. */
/* Leading silence for atomic sounds: one DMA descriptor at 16 kHz stereo.
 * const, so it lives in flash and costs no RAM. */
#define PREROLL_FRAMES  64                       /* 4 ms at 16 kHz */
static const int16_t s_preroll[PREROLL_FRAMES * 2] = { 0 };

static bool play_clip(const audio_clip_t *c, audio_event_t *next)
{
    /* c->len counts int16 values of interleaved stereo (L,R,L,R...). Everything
     * here works in pairs: indices advance by two, and block sizes are even. */
    /* --- ATOMIC SOUNDS: A SINGLE WRITE -------------------------------------
     *
     * Clicks are not interruptible, so splitting them into AUDIO_BLOCK chunks
     * buys nothing — that split exists only to check the queue between blocks
     * of long sounds.
     *
     * AND IT COSTS: every return from i2s_channel_write is a point where the
     * task can be taken off the CPU. A 30 ms click is twelve writes, hence
     * twelve opportunities; if the task stalls longer than the DMA reserve
     * (32 ms) the DMA plays zeros and the hole falls INSIDE the sound — heard
     * as a click that is chewed up rather than one that is missing.
     *
     * With a single write those preemption points disappear: pacing towards the
     * DMA is left to the driver, which knows when there is room, instead of
     * returning to the caller after every piece. */
    if (c->atomic) {
        size_t w = 0;
        /* --- LEADING SILENCE ------------------------------------------------
         *
         * THE PROBLEM. The I2S channel is always active and the DMA runs
         * continuously around eight 4 ms descriptors. When a sound arrives, the
         * DMA sits at an ARBITRARY point of that ring — it depends on how many
         * microseconds have passed since the last sound, which nobody controls.
         * Part of the sample's head can land in a descriptor already passed,
         * and is never played.
         *
         * On a percussive sound it is audible: the attack carries the peak
         * dynamics, and losing even a millisecond makes it distinctly quieter.
         * On a sustained beep the same loss is imperceptible, because the
         * energy is spread out — which is exactly the difference heard.
         *
         * THE REMEDY does not remove the misalignment, it makes it HARMLESS:
         * whatever is lost falls in the silence, and the attack always arrives
         * whole.
         *
         * HOW MUCH: 4 ms, EXACTLY ONE DESCRIPTOR — the largest loss the
         * mechanism can produce.
         *
         * [M] Verified by ear: 4 ms is enough and the attack is consistent.
         * That the smallest working value is one descriptor also confirms the
         * model. Do NOT widen it "to be safe": that would only add latency and
         * hide a future regression.
         *
         * TWO WRITES, but the split point is HARMLESS: if the task were
         * preempted between them the DMA would play more zeros — more silence,
         * not a hole in the sound. That is why the pre-roll is a separate array
         * rather than a copy of the sample in RAM: the defect the single write
         * avoids does not arise here.
         *
         * COST: 4 ms of latency, below the threshold of perception for tactile
         * feedback, and 256 bytes of flash, since the array is const. */
        i2s_channel_write(s_tx, s_preroll, sizeof(s_preroll), &w, portMAX_DELAY);
        i2s_channel_write(s_tx, c->pcm, c->len * sizeof(int16_t), &w, portMAX_DELAY);
        return false;
    }

    size_t i = 0;
    while (i < c->len) {
        /* Zero-crossing replacement ONLY for interruptible (long) sounds;
         * atomic ones play in full. The sign change is evaluated on the left
         * channel, the even indices, and the cut falls on a pair boundary. */
        if (!c->atomic && xQueuePeek(s_queue, next, 0) == pdTRUE) {
            size_t z = i;
            while (z + 2 < c->len &&
                   !((c->pcm[z] <= 0 && c->pcm[z+2] > 0) ||
                     (c->pcm[z] >= 0 && c->pcm[z+2] < 0))) {
                z += 2;
            }
            if (z > i) {
                size_t w = 0;
                i2s_channel_write(s_tx, &c->pcm[i], (z - i) * sizeof(int16_t), &w, portMAX_DELAY);
            }
            xQueueReceive(s_queue, next, 0);   /* take the event and cut here */
            return true;
        }
        size_t n = c->len - i;
        if (n > AUDIO_BLOCK) n = AUDIO_BLOCK;   /* AUDIO_BLOCK is even: whole pairs */
        size_t written = 0;
        i2s_channel_write(s_tx, &c->pcm[i], n * sizeof(int16_t), &written, portMAX_DELAY);
        i += n;
    }
    return false;
}

/* ---- audio task: sleep on the queue, play, go back to sleep ---- */
static void audio_task(void *arg)
{
    (void)arg;
    audio_event_t ev;
    for (;;) {
        /* Sleep until an event arrives: at rest this costs ZERO CPU, because
         * auto-clear has the DMA emitting zeros on its own. */
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) continue;

        /* The I2S channel is ALWAYS enabled (see i2s_setup) and runs in
         * STEREO: every DMA frame is an explicit pair, so the start of a sound
         * is aligned deterministically. */
        for (;;) {
            if (ev >= AUDIO_EVENT_COUNT || !s_clip[ev].pcm) break;
            s_busy = s_clip[ev].atomic;    /* during a click, ignore new events */
            audio_event_t next;
            bool interrupted = play_clip(&s_clip[ev], &next);
            s_busy = false;
            if (interrupted) { ev = next; continue; }   /* long sounds only: chain on */
            break;                                       /* done: back to sleep */
        }
    }
}

esp_err_t audio_init(void)
{
    s_ready = false;

    /* Release the AW88298 from reset through AW9523 P0_2. */
    {
        i2c_master_dev_handle_t aw9523 = NULL;
        esp_err_t e = bsp_i2c_add_device(AW9523_ADDR, &aw9523);
        if (e != ESP_OK) { ESP_LOGW(TAG, "AW9523 unreachable (%s): audio off", esp_err_to_name(e)); return e; }
        uint8_t rreg = 0x02, p0 = 0;
        e = i2c_master_transmit_receive(aw9523, &rreg, 1, &p0, 1, 100);
        if (e != ESP_OK) { ESP_LOGW(TAG, "AW9523 read P0 (%s): audio off", esp_err_to_name(e)); return e; }
        uint8_t p0n[2] = { 0x02, (uint8_t)(p0 | 0x04) };
        e = i2c_master_transmit(aw9523, p0n, 2, 100);
        if (e != ESP_OK) { ESP_LOGW(TAG, "AW9523 set P0_2 (%s): audio off", esp_err_to_name(e)); return e; }
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "AW88298 out of reset (AW9523 P0_2 high)");
    }

    esp_err_t err = bsp_i2c_add_device(AW88298_ADDR, &s_aw88298);
    if (err != ESP_OK) { ESP_LOGW(TAG, "AW88298 handshake failed (%s): audio off", esp_err_to_name(err)); return err; }

    uint8_t reg = 0x00, rx[2] = {0};
    err = i2c_master_transmit_receive(s_aw88298, &reg, 1, rx, 2, 100);
    if (err != ESP_OK) { ESP_LOGW(TAG, "AW88298 not answering (%s): audio off", esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "AW88298 reg0x00 = 0x%02x%02x (chip present)", rx[0], rx[1]);

    err = aw88298_configure();
    if (err != ESP_OK) { ESP_LOGW(TAG, "AW88298 config failed (%s): audio off", esp_err_to_name(err)); return err; }

    err = i2s_setup();
    if (err != ESP_OK) { ESP_LOGW(TAG, "I2S setup failed (%s): audio off", esp_err_to_name(err)); return err; }

    /* atomic=true marks short sounds (clicks) played in full; atomic=false
     * marks long, interruptible ones. */
    s_clip[AUDIO_BRIDGE_ON]  = (audio_clip_t){ audio_smp_beep_up,   audio_smp_beep_up_len,   false };
    s_clip[AUDIO_BRIDGE_OFF] = (audio_clip_t){ audio_smp_beep_down, audio_smp_beep_down_len, false };
    s_clip[AUDIO_PHASE_STEP] = (audio_clip_t){ audio_smp_click,     audio_smp_click_len,     true  };
    s_clip[AUDIO_LOCK_ON]    = (audio_clip_t){ audio_smp_lock_on,   audio_smp_lock_on_len,   true  };
    s_clip[AUDIO_LOCK_OFF]   = (audio_clip_t){ audio_smp_lock_off,  audio_smp_lock_off_len,  true  };
    s_clip[AUDIO_REJECT]     = (audio_clip_t){ audio_smp_reject,    audio_smp_reject_len,    false };
    s_clip[AUDIO_BOOT]       = (audio_clip_t){ audio_smp_boot,      audio_smp_boot_len,      false };
    s_clip[AUDIO_NAV_BEEP]   = (audio_clip_t){ audio_smp_nav_beep,  audio_smp_nav_beep_len,  true  };

    /* One-slot overwriting queue: the latest event wins, which is what a
     * real-time feedback path wants. */
    s_queue = xQueueCreate(1, sizeof(audio_event_t));
    if (!s_queue) { ESP_LOGW(TAG, "audio queue not created: audio off"); return ESP_ERR_NO_MEM; }

    /* PRIORITY 4, one step above LVGL and below TinyUSB and link.
     *
     * The choice is deliberately cautious. The main fix for chewed-up clicks is
     * the SINGLE WRITE for atomic sounds, which removes the twelve preemption
     * points inside one click; raising priority is the secondary measure. If
     * the write alone suffices, going higher buys nothing — and changing two
     * parameters at once and seeing it work does not say WHICH one worked,
     * which is the information needed next time.
     *
     * The cost is nil either way: this task is almost always ASLEEP on a queue,
     * and its priority only weighs in the rare instants when there really is a
     * sound to play. */
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 4, &s_task, 1);
    if (ok != pdPASS) { ESP_LOGW(TAG, "audio task not created: audio off"); return ESP_ERR_NO_MEM; }

    s_ready = true;
    ESP_LOGI(TAG, "audio ready (AW88298 @0x36, I2S %d Hz, task on core 1, volume %d)",
             AUDIO_SAMPLE_RATE, s_volume);
    return ESP_OK;
}

bool audio_ready(void) { return s_ready; }

void audio_play(audio_event_t ev)
{
    if (!s_ready || !s_enabled || ev >= AUDIO_EVENT_COUNT) return;
    /* Short sounds: while an atomic click is playing, a new event is IGNORED —
     * neither queued nor substituted. That way every click plays in full, and
     * repeating one gives a clean ticking at the rate of the taps. For long
     * sounds s_busy stays false, so the normal overwrite with zero-crossing
     * replacement still applies. */
    if (s_busy && s_clip[ev].atomic) return;
    xQueueOverwrite(s_queue, &ev);
}

void audio_set_volume(uint8_t v)
{
    if (v > 100) v = 100;
    s_volume = v;
    if (s_ready) aw_write(0x0C, vol_to_reg(s_volume));
}

void audio_set_enabled(bool on)
{
    s_enabled = on;
}

bool audio_get_enabled(void)
{
    return s_enabled;
}
