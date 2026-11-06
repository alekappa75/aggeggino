/*
 * main.c — the bridge follows the tempo of the MASTER player, identified from
 * the CDJ status packets on :50002 (flag F, bit 5), rather than that of the
 * last packet received. Beats are filtered by the master's device number, so
 * several players at different tempos do not contend. With no master it
 * propagates nothing — Ableton keeps its last tempo — and says so.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"          /* IRAM_ATTR for the W5500 ISR */
#include "driver/gpio.h"       /* gpio_config / gpio_isr_handler_add */

#include "bsp_cores3.h"
#include "audio.h"
#include "display.h"
#include "w5500_net.h"
#include "dhcp_lite.h"
#include "sdcard.h"
#include "djlink.h"
#include "wifi_core.h"
#include "wifi_ap.h"
#include "wifi_client.h"
#include "settings_ui.h"
#include "linkbridge.h"
#include "osc_out.h"
#include "midi_clock.h"
/* Always included: the DJB_MIDI_NET flag that governs the code is defined
 * further down with the other DJB_*, so it would not be visible here yet. */
#include "midi2_net.h"
#include "midi2_ump.h"
#include "esp_mac.h"
#include "driver/uart.h"
/* Always included: the DJB_* flags are defined further down and would not be
 * visible here. Same reason midi2_net.h is unconditional. */
#include "midi_usb.h"
#include "leds.h"
#include "ui.h"
#include "dbserver.h"
#include "settings.h"

static const char *TAG = "M4c";

/* ---- Per-core CPU metrics -------------------------------------------------
 *
 * CPU, SPI, beat jitter and stack watermarks. They exist to CROSS-CHECK our own
 * timings against a Wireshark capture.
 *
 * THEY FOLLOW DJB_DEVELOPER, set in the top-level CMakeLists, and have no
 * switch of their own — deliberately. A separate one could be turned off while
 * the FreeRTOS instrumentation that feeds these stayed on, paying the cost at
 * every context switch to collect numbers nobody reads. One switch moves the
 * code and its instrumentation together.
 */

/* UI sound feedback (AW88298 amplifier over I2S).
 *
 * Defined here like the other DJB_* flags: idf.py's -D goes to CMake and NOT to
 * the C preprocessor, so the flag has to live in the source. At 0 the audio
 * code is not even compiled into app_main. */
#define DJB_AUDIO_ON 1

/* MIDI clock engine. It builds the 24 PPQN grid and hands it to a sink, which
 * forwards it to whichever transports are enabled below — DIN, USB, network.
 * At 0 it is not compiled into app_main at all. */
#define DJB_MIDI_CLOCK 1

/* MIDI output over the network (Network MIDI 2.0, UMP over UDP). The clock
 * engine is unchanged: this is only a TRANSPORT hooking into its sink. */
#define DJB_MIDI_NET 1

/* MIDI output over DIN, through the M5Stack Unit MIDI (U187) on Port C. */
#define DJB_MIDI_DIN 1

/* MIDI output over USB-C: a class-compliant MIDI device through TinyUSB.
 *
 * MIND THIS ONE — IT COSTS MORE THAN THE OTHERS. At 1 the USB-C port is handed
 * to TinyUSB and the USB-Serial-JTAG console DISAPPEARS: from then on the log
 * is only readable from Port B. Flashing still works, because the ROM
 * bootloader owns USB before the application configures it, but a manual reset
 * is needed. */
#define DJB_MIDI_USB 1


/* Heartbeat: 30 s. NOTE that UART logging BLOCKS — about 9 ms per line at
 * 115200 — so seven lines mean roughly 60 ms of stalled loop. Worth remembering
 * when reading the timings. */
#define HB_PERIOD_US  30000000

/* HOW BEAT PACKETS ARE RECEIVED.
 *
 * A high-priority task, woken by the W5500's INTn, marks the packet's ARRIVAL
 * instant and queues it; the main loop only consumes it. The phase is therefore
 * anchored to when the packet actually arrived.
 *
 * There is no polling alternative any more. Reading the socket from the main
 * loop put the timestamp at the moment of the POLL — later than the arrival,
 * and by a varying amount — which is exactly the error this path removes.
 *
 * The INTn pin is consequently REQUIRED, not optional: on the LAN module it is
 * the jumper between pin 2 and pin 26 (see bsp_cores3.h). Without it the task
 * would never be woken and no beat would ever be read, so the build is stopped
 * here rather than leaving that to be discovered on the network. */
#if BSP_W5500_INT_GPIO < 0
#error "BSP_W5500_INT_GPIO must be a real pin: beat reception needs the interrupt."
#endif
/* The eight W5500 sockets. The chip has exactly eight and the buffers fill its
 * 16 KB; everything addresses them by NAME, so moving a service from one to
 * another is a matter of changing the number here. */
#define UDP_SN_BEAT      0        /* :50001 beat packets, timing critical */
#define UDP_SN_STATUS    1        /* :50002 CDJ status                    */
#define UDP_SN_KEEPALIVE 2        /* :50000 keep-alive and media queries  */
#define TCP_SN_DBSERVER  3        /* the "switchboard" on :12523          */
#define TCP_SN_DBDATA    4        /* the real data service on :1051       */
#define TCP_SN_PROBE     5        /* CLIENT: we query a real player       */
#define UDP_SN_PORTMAP   6        /* :111 RPC portmapper: we answer NO    */
#define UDP_SN_DHCP      7        /* DHCP client, see dhcp_lite.h          */

/* ALL EIGHT ARE NOW SPOKEN FOR. The chip has no ninth, so a new service means
 * taking one from something else. The likeliest candidate is TCP_SN_PROBE,
 * which is a client used once at start-up.
 *
 * The DHCP socket is open all evening but drained only during an exchange:
 * an open socket the loop does not read costs no SPI at all. */

/* [M] The portmapper coexists with the TCP client: the same session shows
 * rpc_no rising and metadata retrieved successfully.
 *
 * Set to 0 only to observe the effect on error E-8309, which [M] reappears in
 * the field without the portmapper. */
#define DJB_PORTMAP_ON   1

#define PORTMAP_PORT     111
#define DBSERVER_PORT    12523    /* switchboard: tells us which port to use */
#define DBDATA_PORT      1051     /* the real data service, as the CDJs use  */
/* Keep-alive every 2 s: [M] the interval real players use, measured on a
 * Wireshark capture at 1999.8 ms. */
#define KEEPALIVE_MS    2000

/* SUBNET broadcast, not limited broadcast.
 *
 * [M] Measured on a capture: EVERY broadcast packet from real players —
 * keep-alive, beat, claim — goes to 169.254.255.255, the broadcast address of
 * their link-local /16. The limited broadcast 255.255.255.255 is an address
 * network stacks treat differently, and that no Pioneer player ever uses. */
static const uint8_t DJB_BCAST[4] = { 169, 254, 255, 255 };
#define RX_POLL_MS      5

/* --- Polling the sockets at two different rates ----------------------------
 *
 * The loop runs at 1000/RX_POLL_MS Hz, and every socket polled costs one SPI
 * read of Sn_RX_RSR even when there is nothing there. Sockets without latency
 * constraints do not need that rate, and are polled once every SLOW_POLL_DIV
 * turns:
 *   :111 (RPC) -> refusal replies, reopened by a 2 s timer
 *
 * At full speed remain the beat (:50001, timing critical), the status
 * (:50002), and the keep-alive (:50000, which also carries the media queries
 * that must be answered at once).
 *
 * Setting SLOW_POLL_DIV to 1 restores polling every socket on every turn. */
#define SLOW_POLL_DIV   10        /* 200 Hz / 10 = 20 Hz on the slow sockets */
#define CLOCK_MS        50
#define CHECK_MS        200

/* Room the SD card needs before the next beat. [M] A mount holds the bus for
 * 81 ms; at 120 BPM the beat period is 500, so 250 leaves the operation room
 * to finish and the beat room to be late without meeting it. */
#define SD_BEAT_ROOM_US 250000

#define LINK_QUANTUM    4.0
#define INITIAL_BPM     120.0
#define FRESH_US        1500000    /* master beat still fresh  */
#define PHASE_ALPHA     0.2         /* smooths the downbeat accumulator  */
#define PHASE_STALE_US  1000000     /* reset the accumulator if gap > 1s */
#define PHASE_HARD_MS   120.0       /* force only on a large desync          */
/* Phase dead zone: below this deviation we do not force again. The comparison
 * is strict (dev_ms > PHASE_DEAD_MS) and on the absolute value, so the zone is
 * symmetric — from -PHASE_DEAD_MS to +PHASE_DEAD_MS around the target, which
 * the manual offset has already shifted.
 *
 * MIND NARROWING IT. [M] With the noise measured in the field (RMS 0.89 ms), at
 * 4 ms the noise alone never trips anything; at 3 ms roughly one false trigger
 * every 50 minutes; at 2 ms one every 90 s even with the phase perfectly
 * aligned. And every FORCE resets the drift corrector's 60 s window, so too
 * tight a threshold can start a vicious circle: more re-locks -> less
 * compensation -> more drift -> more re-locks. 3 ms keeps the noise out of the
 * game. */
#define PHASE_DEAD_MS   3.0
/* Master change: below this deviation the two players are describing the same
 * audio timeline, and the anchor is kept.
 *
 * [M] Measured in the field over six handovers: +74 to +618 us, spread 306 us,
 * and NO systematic component — 1->2 and 2->1 give the same mean, so what is
 * seen is per-packet jitter and not an offset between the players. The first
 * value tried, 20 ms, had thirty times the margin: it was not a threshold, it
 * was a formality.
 *
 * 5 ms is chosen so: it sits at five sigma even taking the cautious estimate of
 * the jitter (1 ms rather than the 306 us measured), so it never fires on
 * noise; and it is the order of magnitude at which a misalignment between two
 * players begins to be heard as flam. Below it, intervening would not be worth
 * it; above, it is.
 *
 * THE TWO ERRORS DO NOT WEIGH THE SAME. Too low a threshold means re-anchoring
 * and a FORCE at every master change, which was the original symptom. Too high
 * means keeping a wrong anchor — but the filter drags it towards the new master
 * and the dead zone absorbs the remainder within a bar or two: one loses in
 * tidiness, not in correctness. Hence erring on the high side is the safer
 * mistake. */
#define PHASE_MASTER_KEEP_US 5000.0
/* Slack on the tempo: two synced players report track_bpm * pitch each computed
 * on its own grid, so a few hundredths of difference are normal. Beyond half a
 * BPM they are no longer playing the same tempo. */
#define PHASE_MASTER_KEEP_BPM 0.5

/* Tempo divergence above which we rewrite Link. */
#define TEMPO_EPS       0.005
/* Minimum interval between two tempo writes to Link: 40 ms, so at most 25
 * commits per second. The granularity that matters downstream is that of the
 * audio buffers (128-512 samples, that is 3-11 ms), so 25/s is comfortably
 * within what a DAW can act on — tightening further would buy nothing.
 *
 * With the master's status arriving every ~64 ms this cap does NOT come into
 * play: it is a guard for the worst case — a player transmitting more densely,
 * or more sources in future — not a constraint on normal operation. A value the
 * cap drops stays pending and is written by tempo_tick(), so the ceiling cannot
 * lose the final value. */
#define TEMPO_MIN_GAP_US 40000LL
static int64_t s_tempo_last_us = 0;   /* last write                           */
static double  s_tempo_pend    = 0.0; /* value the cap dropped, 0 = none      */
#define MASTER_FRESH_US 2000000    /* master status still fresh, 2 s */
#define NET_FRESH_US    2000000    /* fresh DJ Link packets = network alive  */

typedef enum { FALLBACK_HOLD, FALLBACK_RELEASE } fallback_mode_t;
static fallback_mode_t g_fallback = FALLBACK_HOLD;

typedef struct {
    uint32_t beat_pkts;
    float    dj_bpm;         /* from the BEATS: the authoritative source, feeds the clock */
    /* From the STATUS packets: the last valid tempo the master declared. It
     * serves the DISPLAY ONLY, so that a tempo can be shown even for stopped
     * players, which send no beats. It does NOT feed the clock: that must
     * follow the beats and nothing else.
     *
     * [M] Verified on a capture: a stopped player declares 0xFFFF ("no tempo")
     * in about 90% of its packets, but every so often it does state the tempo —
     * and hearing it once is enough. */
    float    dj_bpm_seen;
    uint8_t  dj_beat, dj_dev;
    int64_t  last_rx_us;
    uint8_t  ver;
    bool     link_up;
    ui_sync_t sync;
    double   link_bpm;
    /* master */
    uint8_t  master_dev;      /* device declaring itself master, 0 = none */
    int64_t  master_last_us;  /* last status carrying a master */
} state_t;

static state_t st;
/* --- phase accumulator: the master's smoothed downbeat --- */
static int64_t s_db_us = 0;           /* estimated instant of a downbeat */




/* ===========================================================================
 * REACTING TO A PITCH CHANGE
 *
 * A separate matter from drift compensation, with a logic of its own: that one
 * chases a slow, uncertain quantity, this one reacts to a FACT.
 *
 * The PHASE_ALPHA filter exists to smooth the jitter of an uncertain
 * measurement. When the DJ moves the pitch, though, the discontinuity is not
 * uncertain: the status packets have just STATED it. Continuing to average
 * means chasing for seven seconds a value already known.
 *
 * [M] The field showed what that costs: after a ten-BPM move, dev read +28 ms
 * when the true error was +5, and it took five beats for the filter to notice.
 * Everything deciding about phase in those seconds decides on a false number.
 *
 * The remedy is the one the code already uses for bar-phase jumps: do NOT
 * smooth, RE-ANCHOR to the fresh downbeat. From there dev tells the truth at
 * once and the threshold forcing — left exactly as it is — decides on a real
 * number, at the next bar instead of seven seconds later.
 *
 * Nothing is forced as a matter of course: if the pitch moved by a trifle, the
 * anchor updates and the threshold does not fire. If it moved a great deal, it
 * fires as it always has, but on a correct measurement.
 * =========================================================================== */
/* Declared here because phase_pitch_tick, just below, uses it. */
static bool    s_force_next = false;   /* master resumed -> force guaranteed */

#define PITCH_STEP_BPM  0.02       /* beyond this it is a real movement      */
#define PITCH_SETTLE_US 300000LL   /* hysteresis: still this long = move over */
#define PITCH_REALIGN_MIN_MS 1.0   /* below this, re-anchoring is pointless   */
static bool    s_phase_reanchor = false;
static double  s_phase_bpm_seen = 0.0;
static uint8_t s_phase_bpm_dev  = 0;   /* which player that tempo came from */
static int64_t s_pitch_last_us  = 0;   /* last pitch movement                 */
static bool    s_pitch_pending  = false;/* a move to close with a re-anchor   */
static bool    s_pitch_realign  = false;/* re-anchor proposed, to weigh on dev */

static void phase_note_bpm(double bpm, uint8_t dev, int64_t now)
{
    /* The comparison only holds BETWEEN VALUES FROM THE SAME PLAYER. Two synced
     * players each compute track_bpm * pitch on their own grid: audibly
     * identical, but differing by a few hundredths — enough to exceed
     * PITCH_STEP_BPM. Without this check a change of master would disguise
     * itself as a fader movement and trigger a re-anchor that is not needed. */
    if (dev == s_phase_bpm_dev && s_phase_bpm_seen > 0.0 &&
        fabs(bpm - s_phase_bpm_seen) > PITCH_STEP_BPM) {
        s_phase_reanchor = true;      /* consumed at the master's next beat */
        s_pitch_last_us  = now;
        s_pitch_pending  = true;
    }
    s_phase_bpm_seen = bpm;
    s_phase_bpm_dev  = dev;
}

/* Closing the manoeuvre. Re-anchoring alone keeps the phase clean during the
 * movement, but if a FORCE fired in the middle the filter restarts slowly and
 * the remainder settles below the threshold, where nobody collects it: [M] in
 * the field it was seen parking at -3 to -4 ms for the rest of the session.
 *
 * Once the pitch has been still for PITCH_SETTLE_US we lock once, and RE-ANCHOR
 * BEFORE doing so: that way the force acts on the player's fresh downbeat
 * rather than on a filtered reading. It is the same treatment as the dead zone,
 * but triggered by a known fact instead of by a threshold being crossed. */
static void phase_pitch_tick(int64_t now)
{
    if (!s_pitch_pending || now - s_pitch_last_us < PITCH_SETTLE_US) return;
    s_pitch_pending  = false;
    s_phase_reanchor = true;      /* restart from the fresh measurement... */
    s_pitch_realign  = true;      /* ...and weigh whether to re-lock       */
}
static bool    s_db_valid = false;
/* The MASTER's Mm, taken from its status. The field is named "Master
 * meaningful", and its values are:
 *     00  the device is NOT master
 *     01  master, playing a rekordbox track -> tempo and beats are valid
 *     02  master, but the track is NOT rekordbox -> it cannot send tempo
 *
 * MIND THE CONFUSING PART: Mm does NOT report the track type in general. A
 * non-master player with an unanalysed track reports 00, not 02. The track type
 * is a different field, `Tr` at 0x2a.
 *
 * The `== 2` test here is nonetheless CORRECT, because this variable is written
 * only inside `if (s.is_master)`: it is the master's Mm, and for the master a 2
 * really does mean "not a rekordbox track". */
static uint8_t s_master_mm = 0;
static bool    s_master_playing = false;
static int64_t s_last_djlink_us = 0;   /* last DJ Link packet, of any kind */
static uint8_t s_master_mh = 0xff;     /* the current master's Mh: a handover in progress */
typedef enum {
    NET_OFFLINE,    /* link down or W5500 absent                     */
    NET_CENSUS,     /* online: listening to the network to census the slots */
    NET_OBSERVER,   /* censused: device 7, observer (default)        */
    NET_CLAIMING,   /* claiming a slot 1-4, needed to be master       */
    NET_HOLDING,    /* slot acquired, held with keep-alives; not master yet   */
    NET_MASTER      /* Ableton source: master, with a device number 1-4       */
} net_role_t;

static net_role_t s_net_role   = NET_OFFLINE;
static uint8_t    s_claim_num  = 0;    /* the player number claimed, or being
                                        * claimed. It lives here because
                                        * drain_status needs it, to notice that
                                        * somebody has beaten us to it. */
static bool       s_want_master = false;   /* WANTED: Ableton is the source */
static int64_t    s_census_start = 0;

static void net_set_role(net_role_t r, int64_t now);   /* fwd: used by the drains */
static void claim_conflict(uint8_t who, int64_t now);  /* fwd: defined below     */
#define NP_TITLE_MAX  64


/* --- WHAT EACH PLAYER IS PLAYING, kept per player -------------------------
 *
 * ONE PLACE FOR TWO USES. The display wants the master's title; the playlist
 * wants the title of anything played past its threshold. Keeping a single
 * "last track fetched" for both means each overwrites the other's, and they
 * end up asking in turn for ever.
 *
 * Held per player, invalidated when that player's track changes, it serves
 * both: a mix that goes back and forth between two decks asks once per track,
 * not once per handover.
 *
 * INDEXED BY THE PLAYER THAT PLAYS, while the query goes to the one that HOLDS
 * the medium — often a different device, and that is what dev/slot/type record:
 * where the track lives, not who is playing it. */
#define META_SLOTS  8

typedef struct {
    uint32_t id;                      /* rekordbox id, 0 = nothing known */
    uint8_t  dev, slot, type;         /* where the medium is */
    char     title[NP_TITLE_MAX];
    char     artist[NP_TITLE_MAX];
} track_meta_t;

static track_meta_t s_meta[META_SLOTS];

/* Forgets what this player was playing. Called when its track changes: the
 * title we hold belongs to the previous one. */
static void meta_clear(uint8_t player)
{
    if (player >= META_SLOTS) return;
    memset(&s_meta[player], 0, sizeof(s_meta[player]));
}

/* True when we already know the title of this exact track on this player. */
static bool meta_known(uint8_t player, const djlink_status_t *s)
{
    if (player >= META_SLOTS) return false;
    const track_meta_t *m = &s_meta[player];
    return m->id == s->track_id && m->id != 0 && m->title[0] &&
           m->dev == s->track_device && m->slot == s->track_slot &&
           m->type == s->track_type;
}

static void nowplaying_note(const djlink_status_t *s); /* fwd: used by the drains */

/* Beat engine state, declared here because status_tick uses it too. */
static TaskHandle_t     s_beat_task = NULL;
static esp_timer_handle_t s_beat_timer = NULL;
static volatile uint32_t s_beat_bb    = 1;      /* beat-in-bar to send */
static volatile double   s_beat_bpm   = 120.0;


static uint32_t s_beat_tx_count = 0;
/* A beat already scheduled is NOT touched again until it has gone: rescheduling
 * it at 20 Hz overwrote the target while the timer was about to fire — a race
 * that displaced beats and produced absurd measurements, down to -469 ms. */
static volatile int64_t s_beat_armed_us = 0;   /* T_send of the beat in flight, 0 = none */
/* Beat counter kept CONSISTENT with Bb. Players check the invariant
 *      ((beat_counter - 1) % 4) + 1  ==  Bb
 * because in their world beat 1 of the track is the first beat of the bar.
 * Counting "how many beats we have sent" is NOT aligned to Ableton's bar: [M]
 * that gave 97% of status packets with beat and Bb contradicting each other,
 * measured on a Wireshark capture. */
static volatile uint32_t s_beat_ctr = 0;
#if DJB_DEVELOPER
static volatile int64_t s_beat_target = 0;      /* for measuring the jitter */
static int32_t s_beat_jit_max = 0, s_beat_jit_min = 0;   /* task wake-up     */
static int32_t s_beat_out_max = 0, s_beat_out_min = 0;   /* ACTUAL DEPARTURE */
/* Duration of the SEND, measured PAIRED on the same beat.
 *
 * PAIRED IS THE POINT. "Wake-up" and "actual departure" are two MAXIMA over the
 * whole window and may come from different beats, so their difference is the
 * duration of nothing. Measuring both on the SAME beat separates how late the
 * task wakes from what the sending then costs it, SPI bus wait included. */
static int32_t s_beat_tx_max = 0;
/* Breakdown of the beat's send: how much of those microseconds is BUS WAIT, and
 * behind whom. The general bsp_spi statistics do not serve the purpose: they
 * are dominated by the main loop, which takes the bus thousands of times a
 * second while the beat takes it sixty times a minute. */
static uint32_t s_beat_busw_max = 0, s_beat_busw_sum_max = 0, s_beat_busn_max = 0;
static bool     s_beat_busw_disp = false;
#endif


static uint8_t  s_yield_to  = 0;       /* HANDOFF: who we are yielding master to, 0 = nobody */
/* Virtual identity FORCED by the user (menu: DJ Link ID = OBSERVER). Declared
 * here rather than next to its function because the status drain reads it too,
 * and that comes much earlier. */
static bool     s_force_virtual = false;

/* We hold a PLAYER number (1-4), not a virtual one. Declared here with the other
 * network state: the status drain reads it as well. */
static bool     s_identity_real = false;
static uint8_t  s_prev_master = 0;     /* who was master before us: we hand it back */
static int64_t  s_yield_since = 0;     /* since when we have been yielding */
#define YIELD_TIMEOUT_US  5000000      /* 5 s: beyond that we simply let go */
/* Master negotiation: [M] measured on hardware at 3.8-5.8 s (claiming 3.7 s
 * plus handoff 0.1-2.1 s). 10 s gives ample margin — beyond that, something is
 * wrong. */
#define MASTER_NEGOTIATION_TIMEOUT_US  10000000
#define MASTER_FAIL_SHOW_US            2000000   /* how long to show the red */
static int64_t s_negotiation_start = 0;
static int64_t s_master_failed_us  = 0;
static uint32_t s_sync_n    = 1;       /* our Syncn, rises when we yield */
static uint32_t s_sync_n_max = 0;      /* highest Syncn seen on the network */
static bool    s_ap_ok = false;        /* SoftAP started successfully */
static volatile bool s_wifi_client_mode = false;  /* current WiFi mode: true = client */

/* --- Census of the device numbers in use -----------------------------------
 * The last sighting of EVERY device number is recorded, from any status or beat
 * packet, before any filtering. [E] Lesson from beat-link: do not rely on the
 * defences alone, since some players do not defend their number — so we take
 * the census from the status packets we receive. */
#define DEVNUM_MAX       16

/* ===========================================================================
 * DIAGNOSTICS WITH ABLETON AS THE SOURCE
 *
 * In the reverse direction WE are the master, so st.dj_bpm — which only updates
 * from the master's beats — stays frozen at its value from before the takeover.
 * Reading the heartbeat it looked as though the players were not following, but
 * that number had simply stopped measuring them.
 *
 * Here the tempo of EVERY player is kept, taken from any beat packet, and
 * compared with the tempo we are imposing. It is the only way to know whether,
 * and how quickly, the players actually reach the target.
 * =========================================================================== */
static float   s_chase_dev_bpm[DEVNUM_MAX];   /* tempo from the BEAT packet, single pitch at 0x54 */
static float   s_chase_dev_bpms[DEVNUM_MAX];  /* tempo from the STATUS, effective pitch at 0x98 */

/* CHASE STOPWATCH.
 *
 * A heartbeat every 30 s is far too coarse to see the transient. Here a
 * stopwatch is armed at every change of the imposed tempo and stopped when each
 * player gets there: it answers "how long do they take", which is the only
 * question left once it is established that they do follow. */
#define CHASE_TOL_BPM   0.30
#define CHASE_GIVEUP_US 20000000LL
static double  s_chase_target   = 0.0;
static int64_t s_chase_t0 = 0;
static bool    s_chase_active[DEVNUM_MAX];

/* PLAYERS' OFFSET against OUR beats.
 *
 * This is the quantity that really matters: the symptom observed is not that
 * the players get the tempo wrong, it is that their beats drift out of
 * alignment and recover slowly. Tempo alone does not show it.
 *
 * The measurement: when a player's beat arrives at instant T — marked by the
 * ISR, hence precise — its distance from OUR nearest beat is taken. The result
 * is folded to +-half a beat, so it reads as a signed offset. The last value
 * and the maximum are kept, both reset at every heartbeat. */
static int64_t s_chase_our_beat_us = 0;      /* instant of our own last beat  */
static int32_t s_chase_dev_off[DEVNUM_MAX];  /* offset in us, per player      */
static int32_t s_chase_dev_offmax[DEVNUM_MAX];

static void note_player_beat(uint8_t dev, int64_t T, double bpm)
{
    if (dev >= DEVNUM_MAX || s_chase_our_beat_us == 0 || bpm < 1.0) return;
    double beat_us = 60000000.0 / bpm;
    double d = (double)(T - s_chase_our_beat_us);
    d -= floor(d / beat_us + 0.5) * beat_us;      /* fold to +-half a beat */
    s_chase_dev_off[dev] = (int32_t)d;
    if (labs((long)d) > labs((long)s_chase_dev_offmax[dev])) s_chase_dev_offmax[dev] = (int32_t)d;
}

/* Does Link ramp or jump? If the session tempo changes in steps rather than at
 * once, the "gradual recovery" seen on the players would originate upstream of
 * us. An episode opens at the first movement, closes after 600 ms of quiet, and
 * prints ONE line with the extremes, the duration and the number of
 * intermediate steps. */
static double  s_linkstep_seen = 0.0;
static double  s_linkstep_lo = 0.0, s_linkstep_hi = 0.0, s_linkstep_from = 0.0;
static int64_t s_linkstep_t0 = 0, s_linkstep_last = 0;
static int     s_linkstep_n = 0;
static bool    s_linkstep_active = false;

static void note_link_tempo(double bpm, int64_t now)
{
    if (bpm < 1.0) return;
    if (s_linkstep_seen <= 0.0) { s_linkstep_seen = bpm; return; }

    if (fabs(bpm - s_linkstep_seen) > 0.005) {
        if (!s_linkstep_active) {
            s_linkstep_active = true; s_linkstep_t0 = now; s_linkstep_n = 0;
            s_linkstep_from = s_linkstep_seen;
            s_linkstep_lo = s_linkstep_hi = s_linkstep_seen;
        }
        if (bpm < s_linkstep_lo) s_linkstep_lo = bpm;
        if (bpm > s_linkstep_hi) s_linkstep_hi = bpm;
        s_linkstep_n++;
        s_linkstep_last = now;
        s_linkstep_seen = bpm;
        /* new target: the stopwatch restarts for every player */
        if (s_net_role == NET_MASTER) {
            s_chase_target = bpm; s_chase_t0 = now;
            for (int d = 0; d < DEVNUM_MAX; d++) s_chase_active[d] = true;
        }
    } else if (s_linkstep_active && now - s_linkstep_last > 600000LL) {
        s_linkstep_active = false;
        ESP_LOGI(TAG, "[rev] Link %d.%02d -> %d.%02d in %ldms, %d steps "
                      "(range %d.%02d..%d.%02d)",
                 (int)s_linkstep_from, (int)(s_linkstep_from * 100) % 100,
                 (int)s_linkstep_seen, (int)(s_linkstep_seen * 100) % 100,
                 (long)((s_linkstep_last - s_linkstep_t0) / 1000), s_linkstep_n,
                 (int)s_linkstep_lo, (int)(s_linkstep_lo * 100) % 100,
                 (int)s_linkstep_hi, (int)(s_linkstep_hi * 100) % 100);
    }
}
#define DEVNUM_FRESH_US  3000000     /* seen within 3 s = in use */
static int64_t s_dev_seen_us[DEVNUM_MAX];
static uint8_t s_dev_ip[DEVNUM_MAX][4];   /* source IP per device, for unicast status */


/* The device number we present ourselves with, virtual or claimed. Declared
 * here because the transport aggregate has to exclude us. */
static uint8_t s_ka_device = DJLINK_VCDJ_DEVICE;  /* the number used in keep-alives */

/* ---------------------------------------------------------------------------
 * TRANSPORT (PLAY/STOP)
 *
 * The aggregate "at least one player is playing" is the OR of is_playing across
 * all players. START propagates only on the transition none->at least one, STOP
 * only when they ALL stop. That way the master role does not enter the
 * equation: during a transition between decks the aggregate stays true.
 *
 * FRESHNESS IS TIED TO THE STATUS PACKETS (:50002, about 5 Hz), not to the
 * keep-alives: the playing state travels only in the status packets, so they
 * are the only ones that can attest to it. A player that disappears stops
 * refreshing its entry and after TRANSPORT_TTL_US leaves the aggregate; a
 * player that is stopped but present keeps sending status with
 * is_playing=false, so it stays known as "present, not playing".
 *
 * Note that this TTL is separate from DEVNUM_FRESH_US, which feeds the number
 * census from any packet: different purposes, different expiries.
 * ------------------------------------------------------------------------- */
#define TRANSPORT_TTL_US   2000000   /* about 10 lost status packets at 5 Hz */
#define TRANSPORT_DEV_MIN  1         /* players only: 1..6 */
#define TRANSPORT_DEV_MAX  6

static bool    s_dev_playing[DEVNUM_MAX]   = {0};
static int64_t s_dev_status_us[DEVNUM_MAX] = {0};

/* --- Diagnostics on RECEIVING the status packets ---------------------------
 *
 * [M] Born of a precise and still unexplained episode: for about 7 seconds no
 * status arrived from device 1 WHILE its beat packets kept arriving regularly —
 * 60 in 30 s, exactly 2/s at 120 BPM, without a single gap. The player
 * therefore stopped seeing our master, took the role itself, and the reverse
 * bridge switched itself off.
 *
 * Beats and status travel on different sockets and by different mechanisms —
 * :50001 by interrupt, :50002 by polling from the main loop — so the
 * combination is possible, but the only trace left were two transport log
 * lines. These counters exist to tell apart three causes we currently cannot
 * separate:
 *
 *   read=0             the receive path has stalled (main loop blocked, socket
 *                      buffer overflowed)
 *   read>0 but n[1]=0  device 1 really did stop transmitting
 *   read>0 and n[1]>0  they arrived and the parser discarded them
 *
 * The largest gap is reported AT ONCE when it exceeds the TTL, not only in the
 * heartbeat aggregate: a 7-second hole inside a 30-second window disappears
 * into the average, and what is needed is the instant at which it happened. */
static uint32_t s_st_rx_n[DEVNUM_MAX]    = {0};   /* status packets received in the window */
static int64_t  s_st_rx_last[DEVNUM_MAX] = {0};   /* last arrival, for the gap            */
static uint32_t s_st_gap_ms[DEVNUM_MAX]  = {0};   /* largest gap, in ms                   */
static uint32_t s_st_drained    = 0;   /* packets read from :50002, of any kind     */
static uint32_t s_st_quiet_skip = 0;   /* drains deferred by the quiet window        */
static bool    s_any_playing   = false;   /* last value of the aggregate  */
static bool    s_transport_init = false;  /* initial state already adopted? */

/* --- REVERSE direction: Ableton's transport -> the players (fader start) ---
 *
 * Link's callback runs on one of the library's threads: the W5500 cannot be
 * touched from there. We only raise a flag, and the main loop sends the command.
 *
 * No further echo suppression is needed: the two directions are mutually
 * exclusive on the dashboard, so when it is us imposing the state on Link — the
 * forward direction — the reverse one is off and the command never goes out. */
static volatile bool s_link_tr_pending = false;
static volatile bool s_link_tr_playing = false;

static void on_link_transport(bool is_playing)
{
    s_link_tr_playing = is_playing;
    s_link_tr_pending = true;
}


static inline void devnum_seen(uint8_t dev)
{
    if (dev < DEVNUM_MAX) s_dev_seen_us[dev] = esp_timer_get_time();
}

/* Record a player's playing state, called from the status parsing. Our own
 * number is ignored: counting ourselves would make the aggregate self-
 * sustaining, and it would never return to false. */
/* ---------------------------------------------------------------------------
 * TEST ACTIONS (Developer menu)
 *
 * Fader Start: the semantics are NOT play/pause but those of a DJM channel
 * fader.
 *   PLAY starts only if the player is stopped ON ITS CUE POINT
 *   STOP stops and RETURNS TO THE CUE — a jump backwards, not a pause
 *
 * It exists to find out whether our hardware responds at all: [E] the XDJ-XZ
 * and CDJ-3000 ignore it, and on the XDJ-700 it is undocumented. We send it as
 * a broadcast — each player reads its own slot — putting "no action" in our own
 * slot, so as not to command ourselves.
 * ------------------------------------------------------------------------- */
static void dev_send_fader(uint8_t what)
{
    uint8_t cmd[4];
    for (int i = 0; i < 4; i++) {
        uint8_t dev = (uint8_t)(i + 1);
        cmd[i] = (dev == s_ka_device) ? DJLINK_FADER_NONE : what;
    }
    uint8_t buf[DJLINK_FADER_LEN];
    size_t len = djlink_build_fader_start(buf, "aggeggino", s_ka_device, cmd);
    w5500_net_sendto(UDP_SN_BEAT, buf, len, DJB_BCAST, DJLINK_PORT_BEAT);
    ESP_LOGI(TAG, "[dev] fader %s -> broadcast (C1..C4 = %02x %02x %02x %02x)",
             (what == DJLINK_FADER_PLAY) ? "PLAY" : "STOP",
             cmd[0], cmd[1], cmd[2], cmd[3]);
}


/* REVERSE direction: a transport change arriving from Ableton becomes a fader
 * start towards the players. Carried out by the main loop, not by the
 * callback. */
static void link_transport_tick(void)
{
    if (!s_link_tr_pending) return;
    s_link_tr_pending = false;
    bool playing = s_link_tr_playing;

    /* the same two conditions as the forward direction, mirrored */
    bool reverse = ui_clock_src() == CLOCK_SRC_ABLETON;
    bool allowed = playing ? settings_get_start_sync() : settings_get_stop_sync();
    if (!reverse || !allowed) {
        ESP_LOGI(TAG, "[transport] Link %s ignored (%s)",
                 playing ? "PLAY" : "STOP",
                 !reverse ? "bridge not reverse" : "filter off");
        return;
    }
    /* MIND THE SEMANTICS: PLAY only starts players stopped ON THE CUE, and STOP
     * returns them to the cue — it is not a pause. */
    dev_send_fader(playing ? DJLINK_FADER_PLAY : DJLINK_FADER_STOP);
    ESP_LOGI(TAG, "[transport] Link %s -> fader %s to the players",
             playing ? "PLAY" : "STOP", playing ? "PLAY" : "STOP");
}

static void transport_note_status(uint8_t dev, bool playing, int64_t now)
{
    if (dev < TRANSPORT_DEV_MIN || dev > TRANSPORT_DEV_MAX) return;  /* not a player */
    if (dev == s_ka_device) return;                                  /* that is us   */

    /* Receive accounting, see the note on the counters. A gap is reported AT
     * THE MOMENT it closes, with its timestamp: it is the only way to know when
     * it happened, because in the heartbeat aggregate a 7 s hole out of 30 is
     * indistinguishable from noise. */
    if (s_st_rx_last[dev]) {
        uint32_t gap = (uint32_t)((now - s_st_rx_last[dev]) / 1000);
        if (gap > s_st_gap_ms[dev]) s_st_gap_ms[dev] = gap;
        if ((int64_t)gap * 1000 >= TRANSPORT_TTL_US) {
            ESP_LOGI(TAG, "[rx] GAP of %u ms in device %u status "
                          "(TTL %d ms): it was stale for all of it",
                     (unsigned)gap, (unsigned)dev, (int)(TRANSPORT_TTL_US / 1000));
            /* A snapshot of the chip's registers, socket by socket. From
             * outside, a healthy socket receiving nothing and a closed socket
             * look identical; here they are told apart. Sn_SR must read 0x22
             * (SOCK_UDP): any other value means the socket is no longer what we
             * believe it to be, and then the remedy is to reopen it, not to keep
             * reading it.
             *
             * Printed AFTER the gap, so it shows the state at RECOVERY — if it
             * is healthy the socket recovered on its own and the fault was
             * transient; if it is sick, we have caught it. */
            static const uint8_t socks[] = { UDP_SN_BEAT, UDP_SN_STATUS,
                                             UDP_SN_KEEPALIVE, UDP_SN_PORTMAP };
            char sb[128]; size_t o = 0; sb[0] = '\0';
            for (unsigned i = 0; i < sizeof(socks) / sizeof(socks[0]); i++) {
                uint8_t sr; uint16_t rx, tx;
                w5500_net_sock_state(socks[i], &sr, &rx, &tx);
                if (o >= sizeof(sb) - 1) break;
                int w = snprintf(sb + o, sizeof(sb) - o, " s%u:SR=0x%02x rx=%u tx=%u",
                                 socks[i], sr, (unsigned)rx, (unsigned)tx);
                if (w > 0) o += (size_t)w;
                if (o >= sizeof(sb)) { o = sizeof(sb) - 1; break; }
            }
            ESP_LOGI(TAG, "[rx] socket state (0x22=SOCK_UDP healthy):%s", sb);
        }
    }
    s_st_rx_last[dev] = now;
    s_st_rx_n[dev]++;

    s_dev_playing[dev]   = playing;
    s_dev_status_us[dev] = now;
}

/* OR across all fresh players. One that has not sent a status for
 * TRANSPORT_TTL_US no longer counts: otherwise a player switched off while
 * playing would hold the aggregate true forever. */
static bool transport_any_playing(int64_t now)
{
    for (uint8_t d = TRANSPORT_DEV_MIN; d <= TRANSPORT_DEV_MAX && d < DEVNUM_MAX; d++) {
        if (d == s_ka_device) continue;
        if (!s_dev_playing[d]) continue;
        if (s_dev_status_us[d] == 0) continue;
        if ((now - s_dev_status_us[d]) >= TRANSPORT_TTL_US) continue;   /* stale */
        return true;
    }
    return false;
}

/* Evaluate the aggregate, log the transitions, and propagate the transport to
 * Ableton Link when the direction and the filters allow it — see the note
 * further down. */
static void transport_tick(int64_t now)
{
    bool any = transport_any_playing(now);

    if (!s_transport_init) {
        /* Silent adoption of the initial state: if somebody is already playing
         * at power-on we do not emit a START, we simply align to it. */
        s_any_playing   = any;
        s_transport_init = true;
        ESP_LOGI(TAG, "[transport] initial state adopted: %s",
                 any ? "PLAYING" : "STOPPED");
        return;
    }

    if (any == s_any_playing) return;
    s_any_playing = any;

    /* Detail of the players at the moment of the transition: it shows in the
     * field who caused it. */
    char who[48]; int off = 0; who[0] = 0;
    for (uint8_t d = TRANSPORT_DEV_MIN; d <= TRANSPORT_DEV_MAX && d < DEVNUM_MAX; d++) {
        if (d == s_ka_device || s_dev_status_us[d] == 0) continue;
        bool fresh = (now - s_dev_status_us[d]) < TRANSPORT_TTL_US;
        off += snprintf(who + off, sizeof(who) - off, "%s%d:%c",
                        off ? " " : "", d,
                        !fresh ? '?' : (s_dev_playing[d] ? 'P' : '.'));
        if (off >= (int)sizeof(who) - 6) break;
    }

    /* PROPAGATION towards Ableton.
     *
     * The internal state above updates ALWAYS; here it is only decided whether
     * the command goes out. Two conditions:
     *   - the active direction must be the FORWARD one (the dashboard's sync
     *     toggle): with Ableton as the source it is Ableton that commands, not the
     *     other way round, and propagating both ways would form a loop;
     *   - the filter matching the transition must be enabled. */
    bool direct  = ui_clock_src() == CLOCK_SRC_DJLINK;
    bool allowed = any ? settings_get_start_sync() : settings_get_stop_sync();
    bool sent    = direct && allowed;
    if (sent) {
        /* On START we ask for alignment to the downbeat: without it, playback
         * begins with whatever offset the session happens to have. On STOP
         * there is no phase to align. */
        linkbridge_set_playing(any, LINK_QUANTUM, any);
    }

    ESP_LOGI(TAG, "[transport] %s  (players: %s)%s",
             any ? "START  (none -> at least one)"
                 : "STOP   (all stopped)", who,
             sent    ? " -> Link"
             : !direct ? " (bridge not direct)"
                       : " (filter off)");
}
static bool devnum_used(uint8_t dev, int64_t now)
{
    return dev < DEVNUM_MAX && s_dev_seen_us[dev] != 0
           && (now - s_dev_seen_us[dev]) < DEVNUM_FRESH_US;
}
/* First free slot counting down from 4 to 1, that is the highest free one;
 * 0 when they are all taken. */
static uint8_t devnum_pick_free(int64_t now)
{
    for (uint8_t d = 4; d >= 1; d--)
        if (!devnum_used(d, now)) return d;
    return 0;
}

/* A free VIRTUAL number (7-15, the range of the "fake players": beat-link,
 * rekordbox, another aggeggino...). This too must be chosen by looking: taking
 * 7 sight unseen means colliding with anybody else doing the same — and that is
 * a concrete scenario, one only has to run beat-link beside the bridge. */
/* Device number for the OBSERVER role, that is when there is no need to be a
 * real player — which would require 1..4, see the metadata constraint.
 *
 * THE RANGE IS 7..15, NOT 5..7. The earlier fallbacks were 6 and 5, and they
 * are WRONG: [E] CDJ-3000s use channels 5 and 6 as legitimate player numbers.
 * On a network with a CDJ-3000 set to 5 or 6 we would be contending for the
 * number with a real device — and the contention would not even be detected,
 * since we do not handle the conflict packet (type 0x08).
 *
 * 7..15 is the range beat-link uses, for the same reason. 7 stays the preferred
 * one because it is what the project was field-tested with, and changing it
 * without cause would introduce a variable. */
static uint8_t devnum_pick_free_virtual(int64_t now)
{
    for (uint8_t n = DJLINK_VCDJ_DEVICE; n <= 15; n++)
        if (!devnum_used(n, now)) return n;
    return 0;
}
static double  s_last_set_bpm   = 0.0;
static uint8_t s_phase_last_dev = 0;
    static double  s_phase_last_bpm = 0.0;  /* the last master's tempo, for comparison */
static int64_t s_phase_last_beat_us = 0;

static void phase_on_master_beat(uint8_t b, int64_t T);  /* fwd */
/* ===========================================================================
 * DRIFT COMPENSATION
 *
 * The master player and the CoreS3 have different crystals, and the ratio
 * between them is not exactly one: [M] measured in the field at about -24 ppm.
 * Writing the tempo the player declares, Link advances at a slightly different
 * rate from the audio, the phase drifts, and every ~170 s a FORCE fires — an
 * audible jump in position.
 *
 * The remedy does not act on POSITION but on RATE: we write to Link the
 * player's TRUE rate instead of its declared one. The phase then does not
 * drift, and there is nothing to resynchronise.
 *
 * IT IS NOT A CONTROL LOOP. The probe measures the player's rate directly in
 * our own clock, so the correction IS the measured value, not the integral of
 * an error. No gains to tune, nothing to stabilise.
 *
 * The correction is held in PARTS PER MILLION, not in BPM, because it is a
 * ratio between clocks: that way a pitch manoeuvre rescales it by itself
 * instead of invalidating it. What must be suspended during the transient is
 * the MEASUREMENT, not the estimate.
 *
 * With the setting off, tempo_corrected() returns the tempo unchanged and every
 * comparison behaves exactly as before.
 * =========================================================================== */
/* MEASUREMENT WINDOW: one only, 60 seconds.
 *
 * ONE WINDOW, NOT TWO. A short window switching to a long one on the slope
 * cannot work while the crystal is warming: [M] at the first update a 20 s
 * window measured +3 ppm where the true residual was about +20. A 20 s
 * measurement is worth +-14 ppm, so the decision would rest on a number too
 * imprecise to decide with — and the wrong choice sticks for two minutes.
 *
 * Sixty seconds give +-2.7 ppm: a residual of that size takes 25 minutes to
 * reach 4 ms, so the extra precision of 120 s buys nothing useful and cost half
 * the responsiveness. */
#define RATE_WIN_US        60000000LL     /* 60 s -> +-2.7 ppm, a correction each minute */

#define CORR_MAX_PPM   200.0    /* beyond this the estimate is not credible   */
#define CORR_K         0.7      /* feedback gain, chosen in simulation        */
#define CORR_OFF_TAU_S 120.0    /* how slowly the offset is recovered         */
/* Ceiling on the contribution of the offset term. Without it, a large offset
 * produces an enormous tempo bias: [M] in the field, after a pitch manoeuvre,
 * 2.4 ms of debris left by the FORCEs produced 20 ppm — twice the slope — and
 * the correction shot from -18 to -48, then took three minutes to come back.
 * The loop was using RATE to remedy an error of POSITION, which is the wrong
 * instrument. With the ceiling, large offsets still come back, only without
 * overwhelming the rate estimate. */
#define CORR_OFF_MAX   8.0      /* ppm: ceiling on the recovery term          */


#define CORR_WRITE_EPS 0.00025  /* about one Link quantum at 132 BPM: below it, silence */

static bool   s_corr_on    = false;  /* from the setting                       */
static bool   s_corr_valid = false;  /* we have at least one measurement       */
static bool   s_corr_dirty = false;  /* the estimate moved: needs rewriting    */
static double s_corr       = 0.0;    /* a fraction, e.g. -24e-6                */
static bool   s_corr_probe = false;  /* round-trip check still to be done      */
static bool   s_corr_readopt = false;/* the next measurement counts on its own   */
static double s_dev_ms       = 0.0;  /* phase deviation, + = Link ahead          */

/* DISCONTINUITY.
 * A change of master, a pitch movement, a gap in the beats, the bridge switched
 * back on: in all these cases the measurement IN PROGRESS is void and must be
 * started again.
 *
 * The ESTIMATE, on the other hand, is kept, and that is not an oversight. The
 * correction is a ratio between clocks: pitch does not change it, and a change
 * of master changes only the part that depends on the player — the part that
 * depends on us remains. Zeroing it would mean returning to the full error for
 * half a minute, whereas keeping it leaves a smaller error from the first
 * instant.
 *
 * The next measurement is however marked to be adopted in full rather than
 * damped: if the residual really has changed we are there within one window;
 * if it has not, nothing is lost. */
static void corr_discontinuity(const char *why)
{
    if (!s_corr_on) return;
    s_corr_readopt = true;
    ESP_LOGI(TAG, "[drift] %s: measuring again from scratch (keeping %+d ppm)",
             why, (int)lround(s_corr * 1e6));
}

/* The tempo we WANT on Link. The one place the correction enters. */
static double tempo_corrected(double bpm)
{
    if (!s_corr_on || !s_corr_valid) return bpm;
    return bpm * (1.0 + s_corr);
}

/* Link carries microseconds per beat: 1 us is worth about 0.00029 BPM at 132,
 * and our correction is worth some ten of those quanta. In theory it gets
 * through. But that is reasoning about the format, not a measurement — so the
 * first time we write a corrected value we read it back and report what came
 * back. If the discrepancy were of the order of the correction itself, the
 * mechanism does not reach its destination and must be rethought by applying
 * the correction to the phase instead. */
/* Called by settings_ui when the menu entry is toggled. Switching it off resets
 * ALL the state, so an A/B comparison by ear always starts clean and the bridge
 * goes back to exactly what it was. */
static void on_drift_comp(bool on)
{
    s_corr_on = on;
    if (!on) {
        s_corr = 0.0; s_corr_valid = false; s_corr_probe = false;
    }
    s_corr_dirty = true;      /* rewrite the right tempo at once, either way */
    ESP_LOGI(TAG, "[drift] compensation %s", on ? "ON" : "off");
}

/* Compact label for the heartbeat: it shows at a glance whether the corrector
 * is off, on but still without an estimate, or how much it is correcting. */
static const char *corr_label(void)
{
    static char buf[12];
    if (!s_corr_on)    return "OFF";
    if (!s_corr_valid) return "wait";
    snprintf(buf, sizeof(buf), "%+dppm", (int)lround(s_corr * 1e6));
    return buf;
}

static void corr_roundtrip_check(double wrote)
{
    if (!s_corr_probe) return;
    s_corr_probe = false;
    double back = linkbridge_tempo();
    double err  = (wrote > 0.0) ? (back - wrote) / wrote * 1e6 : 0.0;
    ESP_LOGI(TAG, "[drift] round trip: wrote %d.%05d -> read back %d.%05d "
                  "(keeping %+d ppm)",
             (int)wrote, (int)(wrote * 100000) % 100000,
             (int)back,  (int)(back  * 100000) % 100000,
             (int)lround(err));
}

static void tempo_push(double bpm, uint8_t dev);  /* fwd: used by drain_status */
/* Shared with the ISR. Declared HERE and not further down with the rest of the
 * block: drain_w5500 reads s_int_last_us, and comes first. */
static volatile int64_t  s_int_last_us = 0;    /* instant of the last trigger */
static TaskHandle_t      s_int_task    = NULL;

/* --- RING OF TIME-STAMPED PACKETS ------------------------------------------
 *
 * The high-priority task marks the instant BEFORE reading, then reads: that way
 * the instant and the contents travel together, bound by construction. There is
 * no correspondence to assume between interrupts and packets — an assumption
 * that some unknown player, with new packet types, could one day break in
 * silence.
 *
 * A static ring (in BSS, not on the heap: internal RAM is the scarce resource)
 * with a single producer and a single consumer, hence without locks. 128 bytes
 * per slot cover a beat (96) and a takeover (40); a longer packet keeps its
 * true length but with truncated data, which affects only the diagnostics. */
#define RX_SLOTS   6
#define RX_SLOTLEN 128

typedef struct {
    int64_t  t_arr;                 /* instant marked by the task, before reading */
    uint16_t len;                   /* the TRUE length, even when truncated */
    uint8_t  ip[4];
    uint8_t  data[RX_SLOTLEN];
} rx_slot_t;

static rx_slot_t         s_rx[RX_SLOTS];
static volatile uint32_t s_rx_head = 0;   /* advanced by the task      */
static volatile uint32_t s_rx_tail = 0;   /* advanced by the main loop */


/* Current master device, 0 if absent or expired. */
static uint8_t master_current(int64_t now)
{
    if (st.master_dev && (now - st.master_last_us) < MASTER_FRESH_US)
        return st.master_dev;
    return 0;
}

/* Virtual CDJ keep-alive, built once with the W5500's MAC and IP. */
static uint8_t s_keepalive[DJLINK_KEEPALIVE_LEN];
static bool    s_ka_ready = false;

static void build_keepalive_buf(void)
{
    uint8_t mac[6], ip[4];
    w5500_net_get_mac(mac);
    w5500_net_get_ip(ip);
    djlink_build_keepalive(s_keepalive, s_ka_device, mac, ip, "aggeggino", 1);
    s_ka_ready = true;
    ESP_LOGI(TAG, "Virtual CDJ keep-alive ready (dev %d, IP %d.%d.%d.%d)",
             s_ka_device, ip[0], ip[1], ip[2], ip[3]);
}

static void set_keepalive_device(uint8_t dev)
{
    if (dev != s_ka_device) {
        s_ka_device = dev;
        build_keepalive_buf();
        /* The settings' Info page shows the number in use and the MAC we
         * announce ourselves with: they are pushed from here, where they
         * change, rather than giving settings_ui a dependency on the network. */
        uint8_t mac[6] = { 0 }, ip[4] = { 0 };
        w5500_net_get_mac(mac);
        w5500_net_get_ip(ip);
        settings_ui_set_ident(mac, ip, dev);

    }
}

#if DJB_DEVELOPER
static uint32_t s_tx_max_us = 0, s_tx_sum_us = 0, s_tx_n = 0;
#endif

/* Keep-alive accounting. The largest gap between two announcements ACTUALLY
 * sent is the number that matters: it must stay around 2000 ms at all times. If
 * it rises, the players drop us from their device list and stop sending us
 * unicast traffic — status packets, NFS queries — while still broadcasting
 * their beats. */
static uint32_t s_ka_sent = 0, s_ka_defer = 0, s_ka_gap_max = 0;
static int64_t  s_ka_last_us = 0;

/* Returns TRUE only if the packet actually went out.
 *
 * THE RETURN VALUE IS NOT A DETAIL. A caller that advanced last_ka regardless
 * would leave an announcement skipped by the quiet window unretried, and the
 * next one is 2 seconds away: a single skip means 4 seconds of silence, twice
 * the cadence of real players and enough to have us expire from their device
 * list. */
/* Number of devices seen on the network, OURSELVES INCLUDED: it is field `p` of
 * the keep-alive, at byte 0x30. It counts the same table used to choose our own
 * number, with the same freshness window — so a device switched off drops out
 * of the count after DEVNUM_FRESH_US, which is exactly the behaviour the
 * protocol describes. */
static uint8_t devnum_peer_count(int64_t now)
{
    uint8_t n = 1;                               /* ourselves */
    for (uint8_t d = 1; d < DEVNUM_MAX; d++)
        if (d != s_ka_device && devnum_used(d, now)) n++;
    /* No upper bound to impose: the loop scans DEVNUM_MAX slots (16), so n
     * cannot exceed that and fits a uint8_t by construction. */
    return n;
}

static bool send_keepalive(void)
{
    if (bsp_spi_in_quiet_window()) { s_ka_defer++; return false; }  /* the caller retries */
    if (!s_ka_ready) return false;
    /* The peer counter is refreshed ON EVERY SEND: the packet is built once,
     * but this field is the only one that changes over time. */
    s_keepalive[0x30] = devnum_peer_count(esp_timer_get_time());
    w5500_net_sendto(UDP_SN_KEEPALIVE, s_keepalive, DJLINK_KEEPALIVE_LEN,
                     DJB_BCAST, DJLINK_PORT_KEEPALIVE);

    int64_t now = esp_timer_get_time();
    if (s_ka_last_us) {
        uint32_t gap = (uint32_t)((now - s_ka_last_us) / 1000);
        if (gap > s_ka_gap_max) s_ka_gap_max = gap;
    }
    s_ka_last_us = now;
    s_ka_sent++;
    return true;
}

/* DJ Link packets we receive and do NOT handle: they are only LOGGED, never
 * answered. As device 4 we are a real player, so the others may send us
 * requests they would never send an observer on device 7 — and this is how to
 * see them.
 *
 * The hypothesis that an unanswered request leads to E-8309 applies to the
 * services the player EXPECTS — the switchboard, the data service, the RPC
 * portmapper — and those have answers, elsewhere. What lands here is the
 * remainder: the hellos and the three claim stages that players send on
 * power-up, for which nothing suggests an answer is expected. */
static void log_unhandled(const char *port, const uint8_t *b, int32_t n, const uint8_t *fip)
{
    if (n < 11) return;
    if (!djlink_has_magic(b, (size_t)n)) {
        ESP_LOGW(TAG, "[?%s] from %d.%d.%d.%d: %ld bytes with NO DJ Link magic",
                 port, fip[0], fip[1], fip[2], fip[3], (long)n);
        return;
    }
    uint8_t type = b[0x0a];
    char hex[3 * 24 + 1]; int k = 0;
    for (int i = 0; i < n && i < 24; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02x ", b[i]);
    ESP_LOGW(TAG, "[?%s] kind 0x%02x from %d.%d.%d.%d (%ld bytes) NOT HANDLED: %s",
             port, type, fip[0], fip[1], fip[2], fip[3], (long)n, hex);
}

/* MEDIA QUERY (0x05): the player asks "what medium do you have in slot S?".
 * We declare a rekordbox track in our status packets, so if we do NOT answer
 * this question it times out and shows "E-8309 Link Access Error". We answer by
 * describing our virtual medium. */
static void handle_media_query(const uint8_t *b, int32_t n, const uint8_t *fip)
{
    uint8_t from = 0, slot = 0;
    if (!djlink_parse_media_query(b, (size_t)n, &from, &slot)) return;

    /* Answers DIFFERENTIATED per slot, consistent with what we are:
     *   CD  (01) -> empty: we have no CD drive
     *   SD  (02) -> empty: no card inserted
     *   USB (03) -> OUR medium: the one fiction, and the smallest possible
     * Answering the same for all three — three identical media! — would
     * contradict our own status packets, where we declare Sr=03. */
    const char *media = NULL;
    uint16_t    tracks = 0;
    const char *what   = "slot EMPTY";
#if DJB_DECLARE_MEDIA
    if (slot == DJLINK_SLOT_USB) {
        media = "Ableton Link"; tracks = 1;
        what  = "OUR media (Ableton Link, 1 track)";
    }
#endif

    uint8_t rsp[DJLINK_MEDIARSP_LEN];
    size_t len = djlink_build_media_resp(rsp, "aggeggino", s_ka_device, slot,
                                         media, tracks);
    w5500_net_sendto(UDP_SN_STATUS, rsp, len, fip, DJLINK_PORT_STATUS);
    ESP_LOGI(TAG, "[media] query from device %d (slot %d) -> %s", from, slot, what);
}


static void drain_status(void)
{
    /* THE BEAT COMES FIRST. With Ableton as the source it is we who give
     * the players their phase reference, and the beat must leave at the right
     * instant: [M] in the field it was going out up to 8.6 ms late — not
     * because of the timer, which is punctual to within 118 us, but because of
     * the wait for the SPI bus. The quiet window already existed, but ONLY the
     * display respected it.
     *
     * The drain is DEFERRED here, not lost: the next turn of the loop comes
     * within 5 ms and the W5500's buffers cover hundreds of milliseconds.
     *
     * In the FORWARD direction the window is never open — only
     * beat_schedule_next opens it, and that runs only as master — so this check
     * is inert there and draining keeps its priority, as it should. */
    if (bsp_spi_in_quiet_window()) { s_st_quiet_skip++; return; }

    static uint8_t rxbuf[512];
    static int dbg = 0;
    uint8_t fip[4]; uint16_t fport;
    int32_t n;
    while ((n = w5500_net_recv(UDP_SN_STATUS, rxbuf, sizeof(rxbuf), fip, &fport)) > 0) {
        s_st_drained++;      /* read from the socket, BEFORE any filtering */
        if (n >= 11 && rxbuf[0x0a] == 0x05) { handle_media_query(rxbuf, n, fip); continue; }
        if (n >= 11 && rxbuf[0x0a] != 0x0a) log_unhandled("50002", rxbuf, n, fip);
        if (dbg < 10) {
            int type = (n > 0x0a) ? rxbuf[0x0a] : -1;
            int dev  = (n > 0x21) ? rxbuf[0x21] : -1;
            int f89  = (n > 0x89) ? rxbuf[0x89] : -1;
            ESP_LOGI(TAG, "STATUS len=%ld type=0x%02x dev=%d b0x89=0x%02x master=%d",
                     (long)n, type, dev, f89, (f89 >= 0) ? ((f89 & 0x20) != 0) : -1);
            dbg++;
        }
        djlink_status_t s;
        if (djlink_parse_status(rxbuf, (size_t)n, &s)) {
            s_last_djlink_us = esp_timer_get_time();   /* the Pioneer network is alive */
            devnum_seen(s.device);                     /* census of the numbers in use */
            /* transport: record this player's playing state */
            transport_note_status(s.device, s.is_playing, s_last_djlink_us);
            /* THE TABLE IS INVALIDATED WHEN THE TRACK CHANGES, here where every
             * status passes: the title we hold belongs to the piece before, and
             * keeping it would put the wrong name on the display the moment
             * this player became master.
             *
             * ONLY ON A STATUS THAT ACTUALLY CARRIES A TRACK. [M] Dr/Sr/Tr and
             * the id are not filled in every packet, and a zero id means "this
             * status is not telling", not "no track". Treating it as a change
             * empties the table between the moment a title is fetched and the
             * moment it is wanted — which is a lookup thrown away and asked
             * again. */
            if (s.device < META_SLOTS && s.track_id &&
                s_meta[s.device].id && s_meta[s.device].id != s.track_id) {
                meta_clear(s.device);
            }

            /* PLAYLIST: what this player is doing, once per status packet.
             * The title arrives by another road — the table above — because it
             * is fetched when a track passes the threshold, long after the
             * status that announced it. */
            sdcard_note_player(s.device, s.is_playing, s.track_id,
                               s.track_device, s.track_slot, s.track_type, s.bpm);
            if (s.device < DEVNUM_MAX && s.bpm > 20.0f && s.bpm < 300.0f) {
                s_chase_dev_bpms[s.device] = (float)s.bpm;   /* from the EFFECTIVE pitch */
                if (s_chase_active[s.device] && s_chase_target > 1.0 &&
                    s.device != s_ka_device) {
                    int64_t el = esp_timer_get_time() - s_chase_t0;
                    if (fabs((double)s.bpm - s_chase_target) < CHASE_TOL_BPM) {
                        s_chase_active[s.device] = false;
                        ESP_LOGI(TAG, "[rev] player %d REACHED %d.%02d in %ldms",
                                 s.device, (int)s_chase_target,
                                 (int)(s_chase_target * 100) % 100, (long)(el / 1000));
                    } else if (el > CHASE_GIVEUP_US) {
                        s_chase_active[s.device] = false;
                        ESP_LOGE(TAG, "[rev] player %d did NOT reach it: target %d.%02d, "
                                      "still at %d.%02d after %lds",
                                 s.device, (int)s_chase_target,
                                 (int)(s_chase_target * 100) % 100,
                                 (int)s.bpm, (int)(s.bpm * 100) % 100,
                                 (long)(el / 1000000));
                    }
                }
            }
            /* are we claiming that very number? somebody beat us to it */
            if (s_net_role == NET_CLAIMING && s.device == s_claim_num)
                claim_conflict(s.device, esp_timer_get_time());
            if (s.device < DEVNUM_MAX) memcpy(s_dev_ip[s.device], fip, 4);
            nowplaying_note(&s);                       /* has the master's track changed? */
            /* yielding, and the recipient has taken master? then we withdraw */
            if (s_yield_to && s.device == s_yield_to && s.is_master) {
                ESP_LOGI(TAG, "[master] device %d took the master: withdrawing", s_yield_to);
                s_sync_n   = s_sync_n_max + 1;        /* as the protocol requires */
                s_yield_to = 0;
                ui_set_clock_src(CLOCK_SRC_NONE);        /* behaviour A: source cleared */
                int64_t tnow = esp_timer_get_time();
                /* If the user has meanwhile chosen OBSERVER, the master role
                 * has been yielded but the 1-4 NUMBER is still ours: it must be
                 * released, or we would remain an observer occupying a player
                 * slot. The census drops it and takes a virtual one. */
                if (s_force_virtual) {
                    ESP_LOGI(TAG, "[ident] master handed over: now releasing the "
                                  "number and switching to observer");
                    s_identity_real = false;
                    net_set_role(NET_CENSUS, tnow);
                } else {
                    net_set_role(NET_OBSERVER, tnow);
                }
            }
            if (s.is_master) {
                st.master_dev     = s.device;
                st.master_last_us = esp_timer_get_time();
                s_master_mm       = s.mm;
                s_master_mh       = s.master_handoff;   /* cessione master? */
                /* Declared tempo: 0xFFFF means "I have none", a stopped
                 * player. When it is there, we keep it: it is the only way to
                 * show a tempo without waiting for somebody to press play. */
                if (s.track_bpm != 0xFFFF && s.track_bpm != 0 && s.bpm > 20.0f && s.bpm < 300.0f) {
                    st.dj_bpm_seen = (float)s.bpm;
                    /* TEMPO channel: here we run at about 5 Hz instead of the
                     * beat packet's 2 Hz. The phase stays locked to the beat. */
                    tempo_push((double)s.bpm, s.device);
                }
                if (s.is_playing && !s_master_playing) {
                    s_db_valid   = false;        /* resume: re-anchor the phase at once */
                    s_force_next = true;   /* ...and guarantee a force on the first beat */
                }
                s_master_playing  = s.is_playing;
            }
        }
    }
}


/* A takeover request (0x26) arriving on OUR :50001 -> we yield, politely. The
 * DJ always has the last word: we never fight to keep the master role. */
static void handle_takeover_req(const uint8_t *buf, int32_t n)
{
    uint8_t who = djlink_parse_takeover_req(buf, (size_t)n);
    if (!who || who == s_ka_device) return;
    if (s_net_role != NET_MASTER) return;        /* not master: nothing to yield */

    if (s_yield_to != who) {
        s_yield_to = who;                        /* our statuses now carry Mh=who */
        ESP_LOGI(TAG, "[master] device %d asks for the master: YIELDING (courtesy to the DJ)", who);
    }
    if (who < DEVNUM_MAX) {                      /* explicit consent (0x27) */
        uint8_t rsp[DJLINK_TKRESP_LEN];
        djlink_build_takeover_resp(rsp, "aggeggino", s_ka_device);
        w5500_net_sendto(UDP_SN_BEAT, rsp, DJLINK_TKRESP_LEN, s_dev_ip[who], DJLINK_PORT_BEAT);
    }
}


/* :50000 — discovery, presence, master election.
 *
 * WHAT WE DO WITH IT: nothing but log. As device 4 we are a real player, and
 * the players may send us requests here that they would never send an observer
 * on device 7 — this drain exists to SEE them, not to answer.
 *
 * On power-up one indeed sees their hellos (0x0a) and the three stages of
 * number claiming (0x00, 0x02, 0x04), the same ones we send.
 *
 * The socket must be drained regardless: it is also used to SEND the
 * keep-alives, and its receive buffer would fill with nobody emptying it. */
static void drain_discovery(void)
{
    if (bsp_spi_in_quiet_window()) return;   /* the beat comes first, see drain_status */

    static uint8_t rxbuf[512];
    uint8_t fip[4]; uint16_t fport;
    int32_t n;

    while ((n = w5500_net_recv(UDP_SN_KEEPALIVE, rxbuf, sizeof(rxbuf), fip, &fport)) > 0) {
        if (!djlink_has_magic(rxbuf, (size_t)n)) continue;
        uint8_t type = rxbuf[0x0a];
        s_last_djlink_us = esp_timer_get_time();
        /* 0x06 = somebody else's keep-alive: normal, we use them for the census */
        if (type == 0x06) {
            if (n > 0x24) devnum_seen(rxbuf[0x24]);
            continue;
        }
        /* 0x05 = MEDIA QUERY: must be answered, or Link Access Error follows */
        if (type == 0x05) { handle_media_query(rxbuf, n, fip); continue; }

        /* --- 0x08: somebody DEFENDS the number we are taking ---------------
         *
         * This is the protocol's explicit signal: a device that hears its own
         * number being claimed answers with this, and whoever receives it must
         * give up.
         *
         * Listening for it matters because the conflict WOULD be discovered
         * anyway, but by an indirect and SLOWER route — hearing a status
         * carrying that number, see the call to claim_conflict in drain_status —
         * which requires the other device to be already running and
         * transmitting. The defence packet instead arrives AT ONCE, during our
         * own sequence, and from a device that may not have sent a status yet.
         *
         * The remedy already existed: claim_conflict() marks the number as
         * taken and looks for another. Here only the trigger is added. */
        if (type == DJLINK_TYPE_CONFLICT) {
            if (n <= 0x24) continue;                  /* too short: unknown */
            uint8_t defended = rxbuf[0x24];
            /* Marked as taken regardless: it is good information even when we
             * are not claiming that particular number. */
            devnum_seen(defended);
            if (s_net_role == NET_CLAIMING && defended == s_claim_num) {
                ESP_LOGI(TAG, "[claim] device %d DEFENDS its number "
                              "(from %d.%d.%d.%d)", defended,
                         fip[0], fip[1], fip[2], fip[3]);
                claim_conflict(defended, esp_timer_get_time());
            } else {
                ESP_LOGI(TAG, "[claim] number %d defended by %d.%d.%d.%d "
                              "(not ours)", defended,
                         fip[0], fip[1], fip[2], fip[3]);
            }
            continue;
        }
        log_unhandled("50000", rxbuf, n, fip);     /* everything else: to be understood */
    }
}

/* beat on :50001 -> accept ONLY the current master's beats */
static void drain_w5500(void)
{
    if (bsp_spi_in_quiet_window()) return;   /* the beat comes first, see drain_status */

    uint8_t m = master_current(esp_timer_get_time());

    /* In interrupt mode the packets arrive already read and time-stamped by the
     * task: here they are only consumed. The socket is NOT polled. */
    while (s_rx_tail != s_rx_head) {
        rx_slot_t *sl = &s_rx[s_rx_tail % RX_SLOTS];
        const uint8_t *rxbuf = sl->data;
        int32_t  n     = (int32_t)sl->len;
        int64_t  t_arr = sl->t_arr;
        const uint8_t *fip = sl->ip;
        djlink_beat_t b;
        handle_takeover_req(rxbuf, n);      /* is somebody asking us for master? */
        /* ONE interpretation per packet. */
        bool is_beat = djlink_parse_beat(rxbuf, (size_t)n, &b);
        bool noto = is_beat || (n >= 11 && (rxbuf[0x0a] == 0x26 || rxbuf[0x0a] == 0x27));
        if (!noto) log_unhandled("50001", rxbuf, n, fip);

        if (is_beat) {
            s_last_djlink_us = esp_timer_get_time();   /* the Pioneer network is alive */
            devnum_seen(b.device);                     /* census of the slots in use */
            if (b.device < DEVNUM_MAX) memcpy(s_dev_ip[b.device], fip, 4);

                /* EVERY player, not only the master. In the reverse direction
                 * the master is US, so a condition on `b.device == master`
                 * would never be true and the value would stay frozen at its
                 * pre-takeover reading. Comparing beat against status exists
                 * precisely to see whether the two sources tell the same story,
                 * and that must be done for ALL players. */
                if (b.device < DEVNUM_MAX) s_chase_dev_bpm[b.device] = b.bpm;
                if (s_net_role == NET_MASTER)
                    note_player_beat(b.device, t_arr, b.bpm);

            if (m && b.device == m) {
                int64_t T = t_arr;          /* the marked instant, not "now" */
                st.beat_pkts++;
                st.dj_bpm = b.bpm; st.dj_beat = b.beat_in_bar; st.dj_dev = b.device;
                st.last_rx_us = T;
                phase_on_master_beat(b.beat_in_bar, T);
            }
        }
        s_rx_tail++;                        /* slot released */
    }
}

/* Phase accumulator and lock, called on every beat OF THE MASTER. It smooths
 * the downbeat to filter jitter and aligns Link by request, with the offset in
 * milliseconds. The lock is skipped when sync is off, or when the track is not
 * a rekordbox one (Mm=2). */
/* ===========================================================================
 * TEMPO SEPARATED FROM PHASE
 *
 * The beat packet arrives once per beat: at 120 BPM that is 500 ms, and taking
 * the TEMPO from there too means noticing a tug on the pitch up to half a
 * second after it happened. The status packet carries the same tempo at about
 * 5 Hz and we are decoding it already: using it for the tempo alone more than
 * halves the detection latency WITHOUT adding any network traffic.
 *
 * The PHASE stays on the beat packet, which is the only one that says where we
 * are in the bar. Two distinct channels, each fed by the right source.
 * =========================================================================== */
/* The actual tempo write. Both the immediate path and the flushing of a pending
 * value go through here, so the rate-limit accounting lives in one place. */
static void tempo_commit(double bpm_w, int64_t now)
{
    double link_now = linkbridge_tempo();
    linkbridge_set_tempo(bpm_w);
    s_corr_dirty    = false;
    corr_roundtrip_check(bpm_w);
    s_last_set_bpm  = bpm_w;   /* remembered to recognise a peer's intervention */
    s_tempo_last_us = now;
    s_tempo_pend    = 0.0;
    ESP_LOGI(TAG, "[tempo] dj=%d.%02d link was %d.%02d -> updated",
             (int)bpm_w, (int)(bpm_w * 100) % 100,
             (int)link_now, (int)(link_now * 100) % 100);
}

/* Flushed from the main loop: it guarantees that the FINAL value of a ramp
 * reaches Link even if the rate limit discarded the last update. Without it, a
 * manoeuvre could end leaving Link at an intermediate value. */
static void tempo_tick(int64_t now)
{
    /* With the bridge off we NEVER write: if a pending value was left there
     * from the moment it was switched off, it is dropped rather than delivered
     * late. */
    if (ui_clock_src() != CLOCK_SRC_DJLINK || s_master_mm == 2) { s_tempo_pend = 0.0; return; }
    if (s_tempo_pend <= 0.0) return;
    if (now - s_tempo_last_us < TEMPO_MIN_GAP_US) return;
    tempo_commit(s_tempo_pend, now);
}

static void tempo_push(double bpm, uint8_t dev)
{
    if (bpm < 20.0 || bpm > 300.0) return;              /* not a credible value */
    if (ui_clock_src() != CLOCK_SRC_DJLINK || s_master_mm == 2) return; /* source is not DJ Link, or not rekordbox */

    int64_t now = esp_timer_get_time();
    phase_note_bpm(bpm, dev, now);           /* pitch responsiveness: see above */

    double bpm_w    = tempo_corrected(bpm);   /* what we want Link to BE */
    double link_now = linkbridge_tempo();
    /* s_corr_dirty: the estimate has moved, so the tempo must be rewritten even
     * if the DJ has touched nothing. With the corrector off it is always false
     * and the condition behaves as it would without it. */
    if (fabs(bpm_w - link_now) <= TEMPO_EPS && !s_corr_dirty)
        return;                                         /* already aligned: silence */

    /* Rate limit: during a pitch ramp the status packets arrive every 64 ms or
     * so, and [M] in the field we wrote Link's tempo 19 times in 1.16 s — 16
     * commits per second, each propagated to every peer. A discarded value is
     * not lost: it stays pending and tempo_tick() writes it. */
    if (now - s_tempo_last_us < TEMPO_MIN_GAP_US) { s_tempo_pend = bpm_w; return; }
    tempo_commit(bpm_w, now);
}

/* ===========================================================================
 * RECEIVING BEAT PACKETS BY INTERRUPT
 *
 * The W5500 raises INTn when a packet lands on the beat socket. A
 * high-priority task is woken, MARKS the arrival instant, and only then reads
 * over SPI — the reading costs milliseconds, the marking does not.
 *
 * What this buys, and it is the whole point: the phase is anchored to WHEN THE
 * PACKET ARRIVED rather than to when we got round to looking. Polling put the
 * timestamp somewhere between 0 and RX_POLL_MS after the arrival, wandering
 * slowly within that interval — the sawtooth that made the sync feel loose.
 * =========================================================================== */


/* The ISR does ONE thing: it stamps the instant and wakes the task.
 * esp_timer_get_time() is safe in interrupt context; SPI is not, so the
 * acknowledgement and the read are left to the task. */
static void IRAM_ATTR w5500_int_isr(void *arg)
{
    (void)arg;
    s_int_last_us = esp_timer_get_time();
    BaseType_t hpw = pdFALSE;
    if (s_int_task) vTaskNotifyGiveFromISR(s_int_task, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

/* High-priority task: woken by the ISR, it MARKS the instant and then reads.
 * The order matters — the SPI read costs milliseconds, the marking does not —
 * but above all it matters that marking and reading lie on the same path: the
 * packet carries its own instant, and no external correlation can get it wrong.
 *
 * Nothing is interpreted here and the bridge state is not touched: all of that
 * stays with the main loop. */
static void int_rx_task(void *arg)
{
    (void)arg;
    uint8_t  fip[4]; uint16_t fport;
    static uint8_t tmp[512];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* THE BEAT COMES FIRST. There is no returning early here — this is a
         * task — so we WAIT for the window to pass before taking the bus. The
         * wait costs no precision: the packet's instant was already marked by
         * the ISR, and the W5500 holds beats for nearly ten seconds. Outside the
         * Ableton source the window is never open. */
        while (bsp_spi_in_quiet_window()) vTaskDelay(1);

        /* THE INSTANT: we read the variable the ISR wrote. The line runs after
         * the wake-up, but the VALUE is the one from the moment of the
         * interrupt, not from when we read it: the scheduling delay — which can
         * be milliseconds, if WiFi at priority 23 gets in first — does NOT enter
         * the timestamp.
         *
         * AN HONEST LIMIT: if another interrupt fires between the ISR that woke
         * us and this read, the variable has already been overwritten and we
         * read the instant of the FOLLOWING packet. That happens when the two
         * players emit their beats close together, and the error is worth the
         * gap between the two arrivals, which lies within the wake-up window.
         * Sub-millisecond, but not nil.
         *
         * It must be read BEFORE the acknowledgement, which being SPI can block
         * on the bus mutex for milliseconds if the display is flushing. */
        int64_t t_mark = s_int_last_us;

        /* Acknowledge BEFORE reading: a packet arriving during the read raises
         * INTn again and wakes us once more, instead of leaving us deaf. */
        w5500_net_int_ack(UDP_SN_BEAT);

        int32_t n;
        uint32_t head_w = s_rx_head;     /* working head: published at end of pass */
        for (;;) {
            n = w5500_net_recv(UDP_SN_BEAT, tmp, sizeof(tmp), fip, &fport);
            if (n <= 0) break;

            uint32_t h = head_w;
            if ((uint32_t)(h - s_rx_tail) >= RX_SLOTS) {   /* ring full */
                continue;                                   /* packet lost */
            }
            rx_slot_t *sl = &s_rx[h % RX_SLOTS];
            sl->t_arr   = t_mark;
            sl->len   = (uint16_t)n;
            memcpy(sl->ip, fip, 4);
            memcpy(sl->data, tmp, (n < RX_SLOTLEN) ? (size_t)n : RX_SLOTLEN);
            head_w = h + 1;             /* prepared, not yet published */
        }

        /* The LAST packet of the pass is the one that arrived last — the chip
         * delivers in FIFO order — so it is the one the most recent interrupt,
         * and therefore t_mark, really belongs to. The others carry a timestamp
         * late by the gap between arrivals.
         *
         * The head is published in one go AFTER marking: advancing it slot by
         * slot would let the consumer read one before we know its outcome. */
        if (head_w != s_rx_head) {
            s_rx_head = head_w;                             /* publish */
        }
    }
}

static void rx_interrupt_start(void)
{
    xTaskCreatePinnedToCore(int_rx_task, "w5rx", 4096, NULL, 20, &s_int_task, 0);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BSP_W5500_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* INTn is open-drain, active low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BSP_W5500_INT_GPIO, w5500_int_isr, NULL);

    w5500_net_int_enable_recv(UDP_SN_BEAT);     /* the beat socket only */
    /* From here on the beat socket is NOT polled by the main loop: its packets
     * arrive only by interrupt (see drain_beat). The other sockets stay on the
     * usual polling. */
    ESP_LOGI(TAG, "[int] probe active on GPIO%d: :50001 moves to the interrupt, "
                  "the other sockets stay on polling",
             BSP_W5500_INT_GPIO);
}


static void phase_on_master_beat(uint8_t b, int64_t T)
{
    double bpm = st.dj_bpm;
    if (bpm < 1.0) return;
    double beat_us = 60000000.0 / bpm;
    double bar_us  = LINK_QUANTUM * beat_us;

    /* A gap in the beats leaves the anchor without references: it must be
     * rebuilt. */
    if (T - s_phase_last_beat_us > PHASE_STALE_US) s_db_valid = false;

    /* A change of master, on the other hand, does NOT invalidate the anchor as
     * a matter of course: if the two players are in sync the audio timeline is
     * the same, only who narrates it has changed. Throwing the anchor away
     * would mean rebuilding it from a single raw sample of the new player,
     * taking in one go the difference between the two systematic delays — which
     * in the field is heard as a tug towards Ableton at every handover. The
     * decision is made below, once the deviation has been measured. */
    uint8_t prev_dev = s_phase_last_dev;
    double  prev_bpm = s_phase_last_bpm;
    bool    master_changed = (s_phase_last_dev != 0) && (st.dj_dev != s_phase_last_dev);
    s_phase_last_dev     = st.dj_dev;
    s_phase_last_bpm     = bpm;
    s_phase_last_beat_us = T;

    int64_t implied_db = T - (int64_t)((b - 1) * beat_us);   /* downbeat of this bar */
    if (!s_db_valid) {
        s_db_us = implied_db;
        s_db_valid = true;
    } else {
        double  n           = round((double)(implied_db - s_db_us) / bar_us);
        int64_t expected_db = s_db_us + (int64_t)(n * bar_us);
        int64_t err         = implied_db - expected_db;
        /* Master change: the deviation is now measured and the decision can be
         * made. Small means the two players describe the same timeline and the
         * difference is only their transmission delay: the anchor is kept and
         * left for the filter to absorb, without a tug. Large means they really
         * are playing at different points, and then the re-lock is needed. */
        bool master_jump = false;
        if (master_changed) {
            /* Two independent pieces of evidence that the players are running
             * together: the PHASE deviation just measured, and agreement on the
             * TEMPO. If the new master declares a different tempo it is not
             * locked to the previous one, and keeping the old anchor would make
             * no sense. */
            double dbpm = (prev_bpm > 0.0) ? fabs(bpm - prev_bpm) : 0.0;
            master_jump = (fabs((double)err) > PHASE_MASTER_KEEP_US) ||
                          (dbpm > PHASE_MASTER_KEEP_BPM);
            ESP_LOGI(TAG, "[phase] master %d -> %d: offset %+d us, dBPM %d.%02d -> %s",
                     prev_dev, st.dj_dev, (int)(err),
                     (int)dbpm, (int)(dbpm * 100) % 100,
                     master_jump ? "re-anchoring" : "keeping the anchor");
        }

        if (s_phase_reanchor) {
            /* The pitch has moved and we have just been told: a KNOWN
             * discontinuity, so it is not averaged — we restart from the fresh
             * downbeat. No force as a matter of course: the threshold decides,
             * but on a true number. */
            s_phase_reanchor = false;
            s_db_us = implied_db;
        } else if (master_jump || fabs((double)err) > beat_us / 2.0) {
            /* A bar-phase jump, for instance a hot cue with quantize: the
             * beat_in_bar is fresh and correct, so we take it as the new
             * downbeat and force the re-lock, exactly as on a resume. */
            s_db_us = implied_db;
            s_force_next = true;
        } else {
            s_db_us = expected_db + (int64_t)(PHASE_ALPHA * (double)err);   /* smooths jitter */
        }
    }

    /* lock: only when the bridge is on and the track is a rekordbox one */
    if (ui_clock_src() != CLOCK_SRC_DJLINK || s_master_mm == 2) return;

    int64_t now     = esp_timer_get_time();
    double  k       = round((double)(now - s_db_us) / bar_us);  /* downbeat nearest now */
    int64_t near_db = s_db_us + (int64_t)(k * bar_us);
    int64_t target  = near_db + (int64_t)ui_get_phase_offset_dj_us();

    /* Link's phase error against the target downbeat, SIGNED, for diagnostics */
    double ph  = linkbridge_phase_at(target, LINK_QUANTUM);     /* [0, quantum)      */
    double sgn = ph;
    if (sgn > LINK_QUANTUM / 2.0) sgn -= LINK_QUANTUM;           /* [-q/2, q/2)       */
    double dev_ms_signed = sgn * (60000.0 / bpm);               /* + = Link ahead    */
    double dev_ms        = fabs(dev_ms_signed);
    s_dev_ms = dev_ms_signed;   /* read by the corrector's feedback */

    /* Link is touched ONLY when needed: a diverging tempo, or a phase outside
     * the dead zone, or a resume. Otherwise complete silence — no pointless
     * commits in the steady state. The force is the only mechanism that really
     * moves Ableton; a request freezes the error.
     *
     * THE COMPARISON IS AGAINST WHAT LINK ACTUALLY IS (linkbridge_tempo()), not
     * against what we last wrote. In Ableton Link there is NO master, and ANY
     * peer — a Live, an iPad — can change the session tempo. Comparing against
     * our own last write would not notice: it would keep saying "nothing has
     * changed", and we would find out only indirectly, once the phase had
     * drifted past the dead zone. That is slowest precisely in the worst case:
     * a fine adjustment, 132 -> 132.05, could survive OVER TEN SECONDS before
     * being corrected.
     *
     * Comparing against Link itself, any divergence — from a peer, from the
     * bridge being switched back on, from anything unforeseen — is caught at
     * the next beat.
     *
     * The cost in the steady state is ZERO: if nobody touches anything, Link's
     * tempo matches ours and we do not write. The rule of silence is intact.
     * =========================================================================== */
    double link_now    = linkbridge_tempo();          /* what Link IS, not what I wrote */
    double bpm_w       = tempo_corrected(bpm);        /* what we want it to BE */
    bool   rekordbox   = (s_master_mm != 2);
    bool   tempo_changed = (fabs(bpm_w - link_now) > TEMPO_EPS) || s_corr_dirty;
    bool   offset_adj  = ui_offset_changed();   /* a manual adjustment is under way */
    bool   forced      = s_force_next;          /* for the log: was this lock requested? */
    s_force_next = false;

    /* End-of-manoeuvre lock: the anchor has just been rebuilt on the fresh
     * downbeat, so dev tells the truth. If the residual is already below a
     * millisecond there is nothing to lock — and a FORCE for nothing is not
     * free: it would invalidate the drift measurement window, which with
     * frequent manoeuvres is already the scarce resource. */
    bool   pitch_realign = s_pitch_realign && (dev_ms > PITCH_REALIGN_MIN_MS);
    s_pitch_realign = false;

    bool   do_phase    = rekordbox && (dev_ms > PHASE_DEAD_MS || forced ||
                                       offset_adj || pitch_realign);

    /* HAS SOMEBODY ELSE TOUCHED IT?
     *
     * If Link's current tempo is neither what WE wrote nor the DJ's, a peer has
     * changed it. That is not an error — it is the Link protocol, where all are
     * equal — but we want to SEE it: live, knowing that somebody is pulling the
     * other way is worth far more than a tempo quietly corrected. We put the
     * DJ's tempo back regardless: that is what the bridge is for. */
    /* Compared against bpm_w and not bpm, or our own correction would be
     * mistaken for a peer's intervention. */
    bool peer_touched = (s_last_set_bpm > 0.0) &&
                        (fabs(link_now - s_last_set_bpm) > 0.005) &&
                        (fabs(link_now - bpm_w) > 0.005);
    if (peer_touched)
        ESP_LOGI(TAG, "[link] a peer moved the tempo to %d.%02d (I had written "
                      "%d.%02d, the DJ is at %d.%02d): restoring the DJ's tempo",
                 (int)link_now, (int)(link_now * 100) % 100,
                 (int)s_last_set_bpm, (int)(s_last_set_bpm * 100) % 100,
                 (int)bpm, (int)(bpm * 100) % 100);

    const char *act = "idle";
    if (tempo_changed || do_phase) {
        linkbridge_set_tempo_align(bpm_w, target, 0.0, LINK_QUANTUM, do_phase, true);
        s_corr_dirty = false;
        corr_roundtrip_check(bpm_w);
        s_last_set_bpm = bpm_w;                 /* memory of WHAT WE WROTE: only
                                                 * to recognise a peer's
                                                 * intervention, no longer to decide */
        act = do_phase ? (pitch_realign ? "FORCE(pitch)"
                                        : (forced ? "FORCE(requested)" : "FORCE"))
                       : "tempo";
    }

    /* The log carries Link's REAL tempo beside the DJ's: if the two numbers
     * diverge for any length of time, something is wrong. */
    ESP_LOGI(TAG, "phase dev=%+dms dj=%d.%01d link=%d.%01d %s",
             (int)lround(dev_ms_signed),
             (int)bpm, (int)(bpm * 10) % 10,
             (int)link_now, (int)(link_now * 10) % 10, act);
}

static void update_clock(int64_t now)
{
    st.link_bpm = linkbridge_tempo();
    note_link_tempo(st.link_bpm, now);

    /* RISING EDGE OF THE BRIDGE: the user has just switched on DJ Link ->
     * Ableton.
     *
     * That is the moment they EXPLICITLY ask for synchronisation, so it makes no
     * sense to do nothing and wait for the phase to drift past the dead zone on
     * its own. Without this, in the worst case — the DJ's tempo unchanged since
     * the last time, the phase by chance still inside the dead zone — pressing
     * the button produced NOTHING for several seconds.
     *
     * Switching on therefore arms a force: at the master's first beat we lock
     * tempo and phase at once. That is the moment Ableton makes its most abrupt
     * jump — and rightly so, it is what the user asked for by pressing the
     * button. */
    static bool sync_prev = false;
    bool sync_ora = ui_clock_src() == CLOCK_SRC_DJLINK;
    if (sync_ora && !sync_prev) {
        s_force_next = true;
        ESP_LOGI(TAG, "[sync] DJ Link -> Ableton bridge ON: locking to the first beat");
        corr_discontinuity("bridge switched back on");
    }
    sync_prev = sync_ora;

    if (!sync_ora) {
        /* DJ LINK IS NOT THE SOURCE: we hold no opinion on the tempo. We do not write
         * — every write path is gated on the source being DJ Link —
         * ADOPT the session's tempo: that way the display shows the real one
         * and, more importantly, s_last_set_bpm does not keep a stale value
         * that could be proposed again at a later lock.
         *
         * The message tells the two cases apart: with ABLETON as the source,
         * the bridge is not off at all, it is working in the other direction —
         * and it is normal for it to follow the session tempo, because that is
         * what it must carry to the players. */
        double sess = linkbridge_tempo();
        if (sess > 20.0 && sess < 300.0 && fabs(sess - s_last_set_bpm) > 0.005) {
            if (linkbridge_num_peers() > 0)
                ESP_LOGI(TAG, "[link] %s: session tempo %d.%02d (was %d.%02d)",
                         ui_clock_src() == CLOCK_SRC_ABLETON ? "ableton source" : "no source",
                         (int)sess, (int)(sess * 100) % 100,
                         (int)s_last_set_bpm, (int)(s_last_set_bpm * 100) % 100);
            s_last_set_bpm = sess;
        }
        st.sync = UI_SYNC_OFF;
        return;
    }

    uint8_t m = master_current(now);
    if (m == 0) {
        /* no master: nothing is propagated, Ableton keeps its last tempo */
        st.sync = (st.beat_pkts == 0) ? UI_SYNC_NOSRC : UI_SYNC_NOMASTER;
        return;
    }

    /* Tempo and phase are set PER BEAT in phase_on_master_beat, in a single
     * transaction. Here only the UI state is updated. Without fresh beats we do
     * not touch Link at all, so it keeps tempo and phase — an implicit HOLD. */
    bool fresh = (st.beat_pkts > 0) && ((now - st.last_rx_us) < FRESH_US);
    st.sync = fresh ? UI_SYNC_ACTIVE
                    : (g_fallback == FALLBACK_HOLD ? UI_SYNC_HOLD : UI_SYNC_RELEASE);
}

/* States of the on-screen STATUS STRIP. They are computed by their own
 * functions, separate from those of the side LEDs even where the conditions
 * coincide: the two indicators must be able to change independently — see the
 * note on the enums in ui.h. The definitions live next to what they describe —
 * the first two by the LED states, the third by the OSC engine — so forward
 * declarations are needed here. */
static ui_lan_t  lan_badge_state(void);
static ui_wifi_t wifi_badge_state(void);
static bool      wifi_auth_badge(void);
static ui_osc_t  osc_badge_state(void);
static ui_midi_t midi_badge_state(void);   /* defined next to the OSC one */
static ui_cv_t   cv_badge_state(void);     /* idem */
static ui_sd_t   sd_badge_state(void);     /* idem */

static void publish_ui(int64_t now)
{
    uint8_t m = master_current(now);
    bool fresh = m && (st.beat_pkts > 0) && ((now - st.last_rx_us) < FRESH_US);

    /* If WE are the MASTER (Ableton source) we receive no packets to read the
     * master and the beat from: we generate them. The UI must therefore be fed
     * from OUR timeline, or it would show an empty master and a stopped beat
     * indicator. */
    bool we_are_master = (s_net_role == NET_MASTER);
    uint8_t  m_show    = we_are_master ? s_ka_device : m;
    uint8_t  beat_show = we_are_master ? (uint8_t)s_beat_bb : (fresh ? st.dj_beat : 0);
    /* Which tempo to display. With fresh beats we use those, the authoritative
     * source; if the master is stopped and sends none, we fall back on the last
     * tempo it DECLARED in its status. Better the loaded track's tempo than a
     * blank — and as soon as playback starts the beats take over again. */
    float    dj_show   = we_are_master ? (float)st.link_bpm
                                       : (fresh ? st.dj_bpm : st.dj_bpm_seen);

    ui_state_t us = {
        .dj_bpm       = dj_show,
        .link_bpm     = (float)st.link_bpm,
        .sync         = st.sync,
        .master_dev   = m_show,            /* 0 = no master; ourselves when Ableton is the source */
        .we_are_master = we_are_master,    /* green badge: the bridge is master */
        /* Is there a REAL TRACK to show?
         *
         * Not to be confused with "the DJ Link network is active", which is what
         * the LED watches: when WE are the master the network is very much
         * playing, but there is no track — our medium is a fiction. So a REAL
         * player is required, master, and playing.
         *
         * When false the UI HIDES the name but does not clear the data: if the
         * piece resumes there is no need to query the database again. */
        .track_playing = !we_are_master && master_current(now) && s_master_playing,
        /* The MASTER badge blinks amber while we NEGOTIATE to become it.
         *
         * Note that CLAIMING is not a master negotiation: it is only acquiring
         * an identity, and it happens with s_want_master false, so no blinking.
         * The real negotiation happens in HOLDING, while we ask the master
         * player to hand over. If the user selects the Ableton source during
         * the initial claim, that counts too — it is already a step towards
         * master. */
        .master_negotiating = (s_net_role == NET_CLAIMING || s_net_role == NET_HOLDING) && s_want_master,
        .master_failed      = s_master_failed_us &&
                              ((now - s_master_failed_us) < MASTER_FAIL_SHOW_US),
        .beat         = beat_show,
        .peers        = linkbridge_num_peers(),
        .lan          = lan_badge_state(),
        .wifi         = wifi_badge_state(),
        .osc          = osc_badge_state(),
        .midi         = midi_badge_state(),
        .cv           = cv_badge_state(),
        .sd           = sd_badge_state(),
        .wifi_auth    = wifi_auth_badge(),
        .wifi_ap      = !s_wifi_client_mode,
        .wifi_clients = wifi_ap_sta_count(),
        .wifi_rssi    = s_wifi_client_mode ? wifi_client_get_rssi() : 0,
    };
    ui_publish(&us);
}

static const char *sync_label(ui_sync_t s)
{
    switch (s) {
        case UI_SYNC_ACTIVE:   return "ACTIVE";
        case UI_SYNC_HOLD:     return "HOLD";
        case UI_SYNC_RELEASE:  return "RELEASE";
        case UI_SYNC_OFF:      return "OFF";
        case UI_SYNC_NOMASTER: return "NOMASTER";
        default:               return "NOSRC";
    }
}

static void open_djlink_sockets(void)
{
    if (w5500_net_open_udp(UDP_SN_BEAT, DJLINK_PORT_BEAT) < 0)
        ESP_LOGE(TAG, "UDP 50001 (beat) not opened");
    if (w5500_net_open_udp(UDP_SN_STATUS, DJLINK_PORT_STATUS) < 0)
        ESP_LOGE(TAG, "UDP 50002 (status) not opened");
    if (w5500_net_open_udp(UDP_SN_KEEPALIVE, DJLINK_PORT_KEEPALIVE) < 0)
        ESP_LOGE(TAG, "UDP 50000 (keep-alive) not opened");
    build_keepalive_buf();
}

/* --- Left-hand LEDs = the DJ Link chain; errors take priority --- */
#define DJLINK_LED_FIRST 5     /* first LED of the DJ Link side, on the left */
#define DJLINK_LED_COUNT 5
#define ABLETON_LED_FIRST 0    /* Ableton Link side (right-hand side) */
#define ABLETON_LED_COUNT 5
#define LED_LEVEL        100

/* Identity conflict: we could not take ANY free number, not even a virtual one
 * (7-15). Declared here because djlink_led_state uses it. */
static bool s_ident_ko = false;

typedef enum {
    DJL_CHIP_KO,      /* W5500 not detected         -> steady red        */
    DJL_NO_LINK,      /* RJ45 cable unplugged      -> blinking red      */
    DJL_NO_PIONEER,   /* link ok, no Pioneer       -> blinking blue     */
    DJL_IDLE,         /* Pioneer on the net, idle  -> steady blue       */
    DJL_PLAYING,      /* master playing            -> steady green      */
    DJL_IDENT_KO      /* identity CONFLICT         -> white, fast blink */
} djl_led_t;

static djl_led_t djlink_led_state(int64_t now)
{
    if (st.ver != 0x04)                                return DJL_CHIP_KO;
    if (!st.link_up)                                   return DJL_NO_LINK;
    /* Identity conflict: we could not take ANY free number, not even a virtual
     * one. This is a real fault and must be visible without the serial monitor:
     * white, fast blink — a colour and a rhythm used for nothing else, and
     * impossible to confuse with the blue or the red. */
    if (s_ident_ko)                                    return DJL_IDENT_KO;
    if ((now - s_last_djlink_us) > NET_FRESH_US)       return DJL_NO_PIONEER;
    /* If WE are the master (Ableton source) the DJ Link network is fully
     * operational: we are driving it. Without this the green fell back to blue,
     * because receiving no beats from a Pioneer master the condition was never
     * true. */
    if (s_net_role == NET_MASTER)                      return DJL_PLAYING;
    if (master_current(now) && s_master_playing)       return DJL_PLAYING;
    return DJL_IDLE;
}

/* --- Right-hand LEDs = Ableton Link / WiFi, same palette as the left side --- */
typedef enum {
    ABL_AP_KO,        /* SoftAP/STA failed to start -> steady red        */
    ABL_AUTH_KO,      /* network there but refusing us -> blinking red    */
    ABL_NO_CLIENT,    /* AP up, no client          -> blinking blue     */
    ABL_CLIENT_NOPEER,/* client but no Link peer   -> steady blue       */
    ABL_PEER          /* at least one peer in session -> steady green    */
} abl_led_t;

static abl_led_t ableton_led_state(void)
{
    if (s_wifi_client_mode) {
        /* Client mode. Colour semantics:
         *   red solid  (AP_KO)    = real error: STA not initialized/started
         *   red blink  (AUTH_KO)  = the network is there but refuses us: the
         *                           saved key is wrong. Distinct from "not
         *                           connected yet", which is a waiting state
         *                           and needs no action from the user.
         *   blue blink (NO_CLIENT)= not connected: searching, or the configured
         *                           network is not on the air
         *   blue solid (NOPEER)   = connected to the network
         *   green      (PEER)     = connected + an Ableton Link peer present   */
        wifi_client_state_t st = wifi_client_state();
        if (!wifi_client_started())        return ABL_AP_KO;        /* init error -> red */
        if (st != WIFI_CLIENT_CONNECTED) {
            /* Refused at authentication: a configuration error, to be told
             * apart from waiting — network absent, or attempt in progress. */
            if (wifi_client_last_fail() == WIFI_FAIL_AUTH) return ABL_AUTH_KO;
            return ABL_NO_CLIENT;                                   /* searching -> blue blink */
        }
        if (linkbridge_num_peers() < 1)    return ABL_CLIENT_NOPEER;/* connected -> blue solid */
        return ABL_PEER;                                            /* peer -> green */
    }
    /* SoftAP mode */
    if (!s_ap_ok)                      return ABL_AP_KO;
    if (wifi_ap_sta_count() < 1)        return ABL_NO_CLIENT;
    if (linkbridge_num_peers() < 1)    return ABL_CLIENT_NOPEER;
    return ABL_PEER;
}

/* ===========================================================================
 * STATES OF THE ON-SCREEN STATUS STRIP
 *
 * Twins of the two functions above, and entirely independent of them. They must
 * NOT be aligned "for consistency": they say different things on purpose.
 *
 *   the LEDs  tell the PROTOCOL story — are there Pioneers? is the master
 *             playing? is a Link peer present? — with six states and blinking;
 *   the BADGES tell the INFRASTRUCTURE story — does the chip answer? is the
 *             cable in? is the network up? — with three levels only: faulty,
 *             inert, operational.
 *
 * That is why these functions are much shorter than their LED counterparts, and
 * it is not a simplification waiting to be undone: it is the separation doing
 * its job. Protocol state stays on screen where it belongs — the MASTER badge,
 * the beat cells, the BPM — rather than duplicated at the bottom.
 * =========================================================================== */

static ui_lan_t lan_badge_state(void)
{
    if (st.ver != 0x04) return UI_LAN_CHIP_KO;   /* the W5500 does not answer */
    if (!st.link_up)    return UI_LAN_NO_CABLE;  /* present, but cable unplugged */
    return UI_LAN_UP;                            /* the physical chain is fine */
}

static ui_wifi_t wifi_badge_state(void)
{
    if (s_wifi_client_mode) {
        if (!wifi_client_started())                        return UI_WIFI_KO;
        if (wifi_client_state() != WIFI_CLIENT_CONNECTED)  return UI_WIFI_IDLE;
        return UI_WIFI_OK;
    }
    if (!s_ap_ok)                     return UI_WIFI_KO;
    if (wifi_ap_sta_count() < 1)      return UI_WIFI_IDLE;
    return UI_WIFI_OK;
}

/* The "AUTH" badge: the network is there but refuses the saved key. Client mode
 * only; in SoftAP it has no meaning. It is separate from the WiFi badge because
 * the two say different things and are read together: AUTH says WHY we are not
 * connected, while WIFI stays grey to say the adapter is healthy. */
static bool wifi_auth_badge(void)
{
    return s_wifi_client_mode
        && wifi_client_state() != WIFI_CLIENT_CONNECTED
        && wifi_client_last_fail() == WIFI_FAIL_AUTH;
}

/* --- State machine: our ROLE on the DJ Link network ------------------------
 *
 * The DESIRED state — the user's intent, that is the chosen source — is kept
 * separate from the CURRENT one. Any recovery (physical link down then up, or
 * network silence then traffic again) returns to CENSUS: from there the machine
 * walks the path by itself, readjusting the slot if necessary. Recovery from a
 * fault is automatic. */
#define CENSUS_WATCH_US 4000000            /* listen ~4s before deciding */

/* Claiming the device number, the CLAIMING state. */


/* THE USER'S CHOICE: stay a virtual device (7) even when a 1-4 number is free.
 *
 * Legitimate reasons to want it: not appearing as a player in the Pioneers'
 * LINK menu, or leaving all four slots free for real players.
 *
 * THE COST MUST BE STATED PLAINLY, because it is not small: without a real
 * identity there are NO metadata — track name, artist, and in future the
 * playlist — and Ableton CANNOT be the source, since driving tempo and phase means
 * becoming master, and one becomes master only with a real number. The
 * DJ Link -> Ableton bridge keeps working normally.
 *
 * The menu governs it (Clock Bridge > DJ Link ID): main follows the setting on
 * the slow loop and calls djb_set_force_virtual(), which renegotiates the
 * identity AT ONCE instead of waiting for a census that might never come. */
/* Defined further down with the rest of the master negotiation: the prototype
 * is needed here because switching AUTO/OBSERVER must be able to yield the role
 * before renegotiating the identity. */
static void master_handover(int64_t now);
static uint8_t s_virt_num = DJLINK_VCDJ_DEVICE;   /* the virtual number chosen (7-15) */

void djb_set_force_virtual(bool force)
{
    if (s_force_virtual == force) return;
    s_force_virtual = force;
    ESP_LOGI(TAG, "[ident] virtual identity %s, user's choice",
             force ? "FORCED" : "no longer forced");

    int64_t now = esp_timer_get_time();

    /* OBSERVER: the Ableton source is cleared AT ONCE. The user has declared
     * they do not want to be master, and letting them switch on a bridge that
     * requires exactly that would be a promise not kept.
     *
     * AUTO: it is RE-ENABLED, without waiting for the claim to succeed.
     *
     * Tying it to s_identity_real — "re-enable only once we really hold a 1-4
     * number" — would look stricter and is wrong: with the players SWITCHED OFF
     * the claim never even starts, s_identity_real stays false, and the button
     * would remain inert forever.
     *
     * The right rule is optimistic: in AUTO the Ableton source is possible until
     * something proves otherwise. And something does prove it — try_claim calls
     * ui_set_ableton_src_available(false) when all four numbers are taken, within
     * seconds of the players powering up. */
    ui_set_ableton_src_available(!force);

    /* --- IMMEDIATE RENEGOTIATION ------------------------------------------
     *
     * The choice takes effect NOW, not "from the next census": a census may
     * never arrive, and the device would keep the wrong identity indefinitely.
     *
     * AUTO -> OBSERVER: if we are MASTER the role must be yielded first —
     * vanishing while master would leave the network with nobody able to take
     * the role back, since the negotiation goes through the status packets.
     * master_handover() chooses who to yield to and the transition completes by
     * itself; the census starts once the role is back to OBSERVER.
     *
     * If instead we merely hold a number without being master, we re-enter the
     * census at once: the 1-4 number is freed for whoever needs it.
     *
     * OBSERVER -> AUTO: we re-enter the census to try for a player number. If
     * none is free we go back to being observers, which is the state we started
     * from — so the attempt cannot make things worse. */
    if (s_net_role == NET_MASTER) {
        if (force) {
            ESP_LOGI(TAG, "[ident] switching to OBSERVER: handing over the master first");
            master_handover(now);
        }
        /* With force false we are already as we want to be: nothing to do. */
    } else if (s_net_role != NET_CENSUS) {
        ESP_LOGI(TAG, "[ident] renegotiating the identity (%s)",
                 force ? "AUTO -> OBSERVER" : "OBSERVER -> AUTO");
        s_identity_real = false;
        net_set_role(NET_CENSUS, now);
    }
}

bool djb_force_virtual(void) { return s_force_virtual; }
static int     s_claim_step = 0;   /* 0..11: four stages x three sends */
static int     s_claim_tries = 0;  /* how many retries with another number */
#define CLAIM_MAX_TRIES 4          /* the four numbers at most, then we give up */
static int64_t s_claim_next_us = 0;

static const char *net_role_name(net_role_t r)
{
    switch (r) {
        case NET_OFFLINE:  return "OFFLINE";
        case NET_CENSUS:   return "CENSUS";
        case NET_OBSERVER: return s_identity_real ? "OBSERVER" : "OBSERVER(virtual)";
        case NET_CLAIMING: return "CLAIMING";
        case NET_HOLDING:  return "HOLDING";
        case NET_MASTER:   return "MASTER";
    }
    return "?";
}

static void net_set_role(net_role_t r, int64_t now)
{
    if (r == s_net_role) return;
    ESP_LOGI(TAG, "[net] %s -> %s", net_role_name(s_net_role), net_role_name(r));
    s_net_role = r;
    if (r == NET_OFFLINE) { ui_clear_track(); st.dj_bpm_seen = 0; }  /* link down: everything stale */
    if (r == NET_CENSUS) { s_census_start = now; s_claim_tries = 0; }
    /* CLAIMING is not a negotiation for the master role: it is simply the
     * acquisition of our IDENTITY on the network, and it happens even when we
     * have no wish to be master at all.
     *
     * The negotiation stopwatch must NOT be armed here. Doing so started it at
     * power-up, and when the user switched the bridge on — perhaps minutes later
     * — the 10 seconds had long since expired: instant timeout. */
    if (r == NET_HOLDING) {
        s_negotiation_start = now;      /* HERE the real negotiation begins */
        s_master_failed_us  = 0;
        ESP_LOGI(TAG, "[status] starting neutral status as device %d", s_claim_num);
    }
    if (r == NET_OBSERVER) s_negotiation_start = 0;   /* negotiation abandoned */
    if (r == NET_MASTER) {
        /* who was driving before us? we will need it to hand the baton back */
        if (st.master_dev && st.master_dev != s_ka_device && st.master_dev < DEVNUM_MAX)
            s_prev_master = st.master_dev;
        ESP_LOGI(TAG, "[master] ASSERTING tempo master as device %d", s_claim_num);
        s_negotiation_start = 0;        /* negotiation concluded successfully */
        s_master_failed_us  = 0;
    }
    if (r != NET_MASTER)  s_yield_to = 0;   /* no longer master: no handover pending */
    /* Once taken, the number is KEPT: it is our identity on the network, not a
     * temporary permit to be master. Going back to being observers we remain the
     * device we are, and the metadata keep working. */
    if (s_identity_real && s_claim_num) set_keepalive_device(s_claim_num);
    else if (r != NET_CLAIMING)         set_keepalive_device(s_virt_num);
}

/* Take a real player number (1-4), to be kept for the whole session. */
/* YIELDING THE MASTER ROLE when the user clears the Ableton source.
 *
 * Simply stopping is not enough: the baton must be HANDED OVER. Merely changing
 * role would leave the network without a master — or worse, still believing us
 * to be it — and the players would not know how to take it back.
 *
 * We reuse the handover mechanism that already works when it is a player asking
 * us: s_yield_to is set, our status packets carry Mh=recipient, and as soon as
 * he assumes the role we withdraw (see drain_status). */
static void master_handover(int64_t now)
{
    /* Yield to whom? Preferably to whoever was master before us: it is the
     * least surprising choice for the DJ. Otherwise to the first real player on
     * the network. */
    uint8_t to = 0;
    if (s_prev_master && s_prev_master != s_ka_device && devnum_used(s_prev_master, now))
        to = s_prev_master;
    else
        for (uint8_t d = 1; d < DEVNUM_MAX; d++) {
            if (d == s_ka_device) continue;
            if (devnum_used(d, now)) { to = d; break; }
        }

    if (!to) {                            /* no player on the network: nothing to yield */
        ESP_LOGI(TAG, "[master] bridge off and no player to hand over to: stopping");
        net_set_role(NET_OBSERVER, now);
        return;
    }
    s_yield_to    = to;
    s_yield_since = now;
    ESP_LOGI(TAG, "[master] bridge off: HANDING the master to device %d", to);
}

/* CONFLICT during the claim.
 *
 * A player can power up during those 3.6 seconds and take the same number, or
 * defend its own. The result would be TWO devices with the same number on the
 * same network: the kind of fault that confuses the whole of DJ Link, and that
 * would show itself in unintelligible ways.
 *
 * So if during the claim we hear somebody speaking with the number we are
 * taking, we stop and look for another. */
static void try_claim(int64_t now);   /* fwd */

static void claim_conflict(uint8_t who, int64_t now)
{
    ESP_LOGI(TAG, "[claim] CONFLICT: device %d already belongs to somebody else. "
                  "Giving it up and looking for another number.", who);
    devnum_seen(who);                     /* from now on we know it is taken */
    s_claim_num = 0;
    try_claim(now);                       /* retry: it will take the next free one */
}

static void try_claim(int64_t now)
{
    /* The user has chosen to stay virtual: we claim nothing, and knowingly
     * accept the consequences — no metadata, and Ableton cannot be the source. This is not a
     * failure, it is a choice. */
    uint8_t n = s_force_virtual ? 0 : devnum_pick_free(now);
    if (n && ++s_claim_tries > CLAIM_MAX_TRIES) {
        ESP_LOGI(TAG, "[claim] too many conflicts: giving up on a real number");
        n = 0;                            /* we give up: a virtual device */
    }
    if (n == 0) {
        /* We stay a virtual device (7): either because the user chose it, or
         * because the four slots are taken by real players. Either way we can
         * observe and run the DJ -> Ableton bridge, but NOT become master: no
         * Ableton as the source, no metadata. We say so plainly and do not keep
         * retrying for nothing. */
        s_identity_real = false;
        uint8_t v = devnum_pick_free_virtual(now);
        if (v == 0) {
            /* The whole 7..15 range is taken: a serious crowd of virtual
             * players (beat-link, rekordbox, other bridges...). We stay on 7,
             * but we WARN: the conflict is real and must be said, because the
             * symptoms would otherwise be unintelligible.
             *
             * With nine numbers available this case is far rarer than it was
             * with three. */
            v = DJLINK_VCDJ_DEVICE;
            s_ident_ko = true;                /* -> blinking white LED */
            ESP_LOGE(TAG, "[ident] every virtual number (7-15) is taken! "
                          "Using %d anyway, but there is a CONFLICT on the network: "
                          "turn off the other virtual player (beat-link? rekordbox? "
                          "another bridge?).", v);
        }
        else s_ident_ko = false;           /* a free number does exist */
        s_virt_num = v;
        set_keepalive_device(s_virt_num);
        ESP_LOGI(TAG, "[ident] staying virtual device %d (%s). Can observe and "
                      "bridge DJ Link -> Ableton, but NOT become master: "
                      "ableton cannot be the source and metadata are unavailable.",
                 s_virt_num,
                 s_force_virtual ? "user's choice"
                                 : "all four player numbers are taken");
        ui_set_ableton_src_available(false);
        net_set_role(NET_OBSERVER, now);
        return;
    }
    s_claim_num = n; s_claim_step = 0; s_claim_next_us = now;
    ESP_LOGI(TAG, "[ident] taking player number %d (kept for the session)", n);
    net_set_role(NET_CLAIMING, now);
}


/* --- Status packet send (HOLDING/MASTER states) ---------------------------- */
static uint32_t s_status_count = 0;
static int64_t  s_status_next  = 0;
/* STATUS CADENCE — [M] measured on two real XDJ-700s with Wireshark.
 *
 * A CDJ does NOT transmit at a fixed rate: at rest it sits around 150 ms
 * (6.6 Hz), but while the pitch is moving it drops to 64 ms (15.6 Hz), more
 * than twice as fast.
 *
 * THAT is what the instant responsiveness observed between two players rests
 * on: the synced player does not chase with a control loop, it READS the
 * effective pitch from the master's status and applies it — [M] measured,
 * values identical to the second decimal place, in the same instant. So its
 * promptness is exactly the cadence at which the master speaks.
 *
 * Sending at a FIXED 200 ms is slower than their idle rate and three times
 * slower than their transients. [M] The 46-238 ms of delay measured on the
 * players is precisely the uniform distribution of the wait for our next
 * status: they were not chasing slowly, they were waiting to be told. */
#define STATUS_INTERVAL_US      150000   /* at rest:    ~6.6 Hz, as they do   */
#define STATUS_FAST_US           64000   /* transient: ~15.6 Hz, as they do   */
/* How long the fast phase lasts after ONE change. [M] Measured on the players:
 * they return to the slow cadence AS SOON AS the pitch stops, with no tail — at
 * 13 s they were at 64 ms, at 14 s already at 136 ms. So the fast phase is not a
 * long timer, it is the state "something is changing".
 *
 * 250 ms is enough here to send three or four closely spaced status packets,
 * which also gives redundancy if one is lost; if the tempo keeps moving the
 * trigger repeats and the fast phase extends by itself, exactly as theirs
 * does. */
#define STATUS_FAST_HOLD_US     250000LL
static int64_t s_status_fast_until = 0;

/* Called when the tempo we impose changes: it sends a status AT ONCE and raises
 * the cadence for a moment. It is the difference between "the player finds out
 * within 200 ms" and "we tell it now". */
static void status_note_change(int64_t now)
{
    s_status_next       = 0;                        /* the next turn sends it */
    s_status_fast_until = now + STATUS_FAST_HOLD_US;
}

static void status_tick(int64_t now)
{
    /* WHOEVER HOLDS A PLAYER NUMBER BEHAVES LIKE A PLAYER: it sends status
     * packets, master or not. This is not cosmetic, it is the protocol.
     *
     * Sending them only from HOLDING/MASTER creates a ZOMBIE: device 4 stays
     * alive through its keep-alives but mute, with no status. And if we had been
     * master, the network goes on believing we still are (Mh=4) — we are driving
     * nothing, and the players cannot take the master role back, because the
     * handover is negotiated PRECISELY through the status packets we are not
     * sending. Complete deadlock.
     *
     * From an observer, the status simply tells the truth: "I am device 4, I am
     * NOT master". That way the players see us for what we are. */
    if (!s_identity_real) return;                     /* virtual device: we stay mute */
    if (s_net_role != NET_OBSERVER &&
        s_net_role != NET_HOLDING  && s_net_role != NET_MASTER) return;
    if (now < s_status_next) return;
    s_status_next = now + ((now < s_status_fast_until) ? STATUS_FAST_US
                                                       : STATUS_INTERVAL_US);

    bool master = (s_net_role == NET_MASTER);          /* HOLDING is neutral: not master */
    double bpm  = (st.link_bpm > 1.0) ? st.link_bpm : 120.0;
    uint8_t buf[DJLINK_STATUS_LEN];
    s_status_count++;
    /* We declare ourselves PLAYING — a stopped master does not drag the others'
     * phase, since "it is playing nothing, I leave my phase alone" — but the
     * status declares no medium or collection (see djlink_build_status), so that
     * nobody tries to browse our non-existent library. */
    /* The beat counter advances with the REAL BEAT, not with every status: it
     * is taken from the number of beat packets sent, so status and beat tell the
     * same story. */
    /* Counter aligned to the bar (see s_beat_ctr), never "beats sent", which
     * bears no relation to the position within the bar. */
    uint32_t beat_ctr = s_beat_ctr ? s_beat_ctr : 1;
    uint8_t mh = s_yield_to ? s_yield_to : 0xff;   /* Mh: announces the handover */
    djlink_build_status(buf, "aggeggino", s_ka_device, bpm, beat_ctr, (uint8_t)s_beat_bb,
                        s_sync_n, s_status_count, master, master, false, false, mh);
    /* Unicast to every fresh real player, on :50002, as dysentery describes. */
    for (int d = 1; d < DEVNUM_MAX; d++) {
        if (d == s_ka_device || !devnum_used((uint8_t)d, now)) continue;
        /* The same specialised path as the beat: 7 SPI transactions instead of
         * 23. Here the gain is twofold — the status becomes more punctual, and
         * it is the status that governs the players' convergence, and it stops
         * holding the bus for about 2.3 ms while a beat arrives.
         *
         * Unlike the beat this is UNICAST, so it goes through ARP: a failure is
         * not detected at the time, because we do not wait for confirmation, but
         * it lands in the 'timeout' counter of the [fasttx] line, read from the
         * Sn_IR register on the following send. */
        w5500_net_sendto_fast(UDP_SN_STATUS, buf, DJLINK_STATUS_LEN,
                              s_dev_ip[d], DJLINK_PORT_STATUS);
    }
}

/* --- RPC PORTMAPPER (:111): we answer "service not available" --------------
 *
 * [M] FOUND WITH A WIRESHARK CAPTURE: when we declare ourselves playing from a
 * USB slot, BOTH players ask us — by ONC-RPC PORTMAP GETPORT — where our MOUNT
 * service lives: they want to mount over NFS the stick we claim to have, to read
 * its tracks. Staying silent, they retried every second forever, and both showed
 * "E-8309 Link Access Error".
 *
 * We answer with a valid GETPORT reply and port 0, meaning "I do not have that
 * service": an honest answer, and above all NOT silence. */
static bool s_pm_open = false;
static uint32_t s_pm_replies = 0;   /* 'no' replies to the portmapper (in [hb]) */

static void portmap_tick(int64_t now)
{
    static int64_t retry = 0;
#if !DJB_PORTMAP_ON
    (void)now; (void)retry;
    return;                     /* TEST: socket 7 libero */
#else
    if (st.ver != 0x04) { s_pm_open = false; return; }
    if (!s_pm_open) {
        if (now < retry) return;
        retry = now + 2000000;
        if (w5500_net_open_udp(UDP_SN_PORTMAP, PORTMAP_PORT) < 0) {
            ESP_LOGE(TAG, "[rpc] socket :%d not opened", PORTMAP_PORT);
            return;
        }
        s_pm_open = true;
        ESP_LOGI(TAG, "[rpc] portmapper listening on UDP %d", PORTMAP_PORT);
        return;
    }

    uint8_t rx[256], fip[4]; uint16_t fport;
    int32_t n;
    while ((n = w5500_net_recv(UDP_SN_PORTMAP, rx, sizeof(rx), fip, &fport)) > 0) {
        if (n < 28) continue;
        uint32_t xid  = ((uint32_t)rx[0]<<24)|((uint32_t)rx[1]<<16)|((uint32_t)rx[2]<<8)|rx[3];
        uint32_t mtyp = ((uint32_t)rx[4]<<24)|((uint32_t)rx[5]<<16)|((uint32_t)rx[6]<<8)|rx[7];
        uint32_t prog = ((uint32_t)rx[12]<<24)|((uint32_t)rx[13]<<16)|((uint32_t)rx[14]<<8)|rx[15];
        uint32_t proc = ((uint32_t)rx[20]<<24)|((uint32_t)rx[21]<<16)|((uint32_t)rx[22]<<8)|rx[23];
        if (mtyp != 0 || prog != 100000) continue;   /* not a CALL to the portmapper */

        /* GETPORT (proc 3) -> we answer port 0, meaning the program is not
         * registered here. NULL (proc 0) is answered too, out of courtesy. */
        uint8_t tx[28]; int i = 0;
        tx[i++]=(uint8_t)(xid>>24); tx[i++]=(uint8_t)(xid>>16);
        tx[i++]=(uint8_t)(xid>>8);  tx[i++]=(uint8_t)xid;      /* identical XID */
        tx[i++]=0; tx[i++]=0; tx[i++]=0; tx[i++]=1;            /* REPLY         */
        tx[i++]=0; tx[i++]=0; tx[i++]=0; tx[i++]=0;            /* MSG_ACCEPTED  */
        tx[i++]=0; tx[i++]=0; tx[i++]=0; tx[i++]=0;            /* verifier: AUTH_NULL */
        tx[i++]=0; tx[i++]=0; tx[i++]=0; tx[i++]=0;            /*   length 0    */
        tx[i++]=0; tx[i++]=0; tx[i++]=0; tx[i++]=0;            /* accept: SUCCESS */
        if (proc == 3) {                                        /* GETPORT       */
            tx[i++]=0; tx[i++]=0; tx[i++]=0; tx[i++]=0;        /* porta = 0     */
        }
        w5500_net_sendto(UDP_SN_PORTMAP, tx, (size_t)i, fip, fport);
        /* The players ask every second, forever: no log line per request, which
         * would flood the monitor. We count them and report in the heartbeat.
         * The FIRST one is logged, so the mechanism can be seen to start. */
        if (s_pm_replies == 0)
            ESP_LOGI(TAG, "[rpc] %d.%d.%d.%d asks for MOUNT/NFS -> replying "
                          "'service unavailable' (further ones not logged)",
                     fip[0], fip[1], fip[2], fip[3]);
        s_pm_replies++;
    }
#endif /* DJB_PORTMAP_ON */
}

/* --- dbserver SWITCHBOARD (port 12523) -------------------------------------
 *
 * This is not a probe and does not merely observe: it ANSWERS. Seeing us as a
 * player, a CDJ opens a TCP connection here and asks where our data service is;
 * we reply with two bytes — port 1051 — and it moves there.
 *
 * The expected query is: 00 00 00 0f "RemoteDBServer" 00, 19 bytes.
 *
 * ON E-8309: without an answer the player times out after about 20 s and shows
 * the error. MIND THIS THOUGH — the same error is also produced by an
 * unanswered RPC portmapper (see portmap_tick and the note on
 * DJB_DECLARE_MEDIA in djlink.h), which is an entirely independent route. Both
 * were needed; [U] we never isolated experimentally which of the two caused it
 * in the case observed, so do not assume that removing this one leaves the
 * error away. */
static bool s_db_listening = false;

static void dbserver_tick(int64_t now)
{
    static int64_t last = 0;
    if (st.ver != 0x04) { s_db_listening = false; return; }

    if (!s_db_listening) {
        static int64_t retry = 0;
        if (now < retry) return;
        retry = now + 2000000;                       /* retry every 2 s */
        int r = w5500_net_tcp_listen(TCP_SN_DBSERVER, DBSERVER_PORT);
        if (r == 0) {
            s_db_listening = true;
            ESP_LOGI(TAG, "[db] LISTENING on TCP %d (switchboard: answering with the data port)", DBSERVER_PORT);
        } else {
            ESP_LOGE(TAG, "[db] listen FAILED (r=%d) on sn=%d port=%d",
                     r, TCP_SN_DBSERVER, DBSERVER_PORT);
        }
        return;
    }

    if (now - last < 50000) return;      /* poll a 20Hz */
    last = now;

    uint8_t buf[64], peer[4] = {0};
    int32_t n = w5500_net_tcp_poll(TCP_SN_DBSERVER, buf, sizeof(buf), peer);
    if (n > 0) {
        /* Expected query: 00 00 00 0f "RemoteDBServer" 00, 19 bytes.
         * It must be answered with TWO bytes: the data service port, high then
         * low. Without an answer the player times out and shows E-8309. */
        if (n >= 18 && memcmp(&buf[4], "RemoteDBServer", 14) == 0) {
            uint8_t rsp[2] = { (uint8_t)(DBDATA_PORT >> 8), (uint8_t)(DBDATA_PORT & 0xff) };
            int32_t w = w5500_net_tcp_send(TCP_SN_DBSERVER, rsp, 2);
            ESP_LOGI(TAG, "[db] RemoteDBServer query from %d.%d.%d.%d -> answering port %d (tx=%ld)",
                     peer[0], peer[1], peer[2], peer[3], DBDATA_PORT, (long)w);
        } else {
            /* Same as above: the hex dump belonged to the days when we did
             * not know what arrived here. */
            ESP_LOGI(TAG, "[db] unexpected data from %d.%d.%d.%d: %ld bytes",
                     peer[0], peer[1], peer[2], peer[3], (long)n);
        }
    } else if (n == -2) {
        w5500_net_tcp_close(TCP_SN_DBSERVER, DBSERVER_PORT);
    }
}

/* --- INVESTIGATIVE PROBE: we query a REAL PLAYER ---------------------------
 *
 * Type 0x3d03 is documented nowhere. But the players ask each other for it and
 * know how to answer. So we act as the CLIENT: we send a real player the very
 * same request it sends us, and RECORD its answer. Then we imitate it. Asking
 * the hardware instead of guessing. It runs once, when we are master. */

/* Sending a dbserver reply: if the W5500 is busy (SOCK_BUSY -> 0) the reply does
 * NOT go out. We cannot retry cleanly from here — we are inside the handling of
 * a request — but at least we REPORT it rather than losing it in silence. In
 * practice it does not happen: between one request and the next there is a
 * whole network round, more than enough for the chip to finish. */
static void db_reply(uint8_t sn, const uint8_t *b, size_t n, const char *cosa)
{
    int32_t w = w5500_net_tcp_send(sn, b, n);
    if (w != (int32_t)n)
        ESP_LOGE(TAG, "[dbd] the '%s' reply did NOT go out (%ld/%u bytes)",
                 cosa, (long)w, (unsigned)n);
}

/* --- NOW PLAYING: reading from the master which track is playing -----------
 *
 * The foundation for an evening's playlist. The dialogue is the same one we
 * sustain from the server side:
 *   TCP :12523   "RemoteDBServer"   -> the player says which port to use
 *   TCP :<port>  5-byte handshake   -> echoed back
 *                setup (0x0000)     -> who we are
 *                metadata (0x2002)  -> "how many elements have you for track N?"
 *                render (0x3000)    -> it sends title, artist, tempo...
 *
 * We ask ONLY when the pair (master device, track id) changes: that covers both
 * a change of master and a change of track on the same player, without pestering
 * anybody with requests. */

static char     s_np_title[NP_TITLE_MAX]  = {0};
static char     s_np_artist[NP_TITLE_MAX] = {0};
static uint8_t  s_np_device = 0;          /* who HOLDS the medium of the known track */
static uint8_t  s_np_slot_k = 0;          /* that track's slot                       */
static uint8_t  s_np_type_k = 0;          /* that track's type                       */
static uint32_t s_np_id     = 0;          /* rekordbox id                        */
static uint8_t  s_np_want_dev = 0;        /* track to query, 0 = none */
static bool     s_np_for_playlist = false;  /* who asked: the display or the file */
static uint8_t  s_np_for_player   = 0;      /* which player PLAYS the track asked about */
static uint32_t s_np_want_id  = 0;
static uint8_t  s_np_want_slot = 0, s_np_want_type = 0;
static int64_t  s_np_retry_us = 0;
static uint32_t s_np_txid = 0x10000;

/* WHICH NUMBER DO WE PRESENT TO THE DATA SERVICE?
 *
 * OUR OWN, and nothing else. No disguise works — [M] all of them were tried in
 * the field:
 *   a free but unannounced number (3)   -> "0 elements"
 *   another real player's number (2)    -> no answer at all
 *   the target's own number (1)         -> "0 elements"
 *
 * The reason, and it explains all three: the player builds the menu PER
 * REQUESTING DEVICE, and serves only those it genuinely knows.
 *
 * Two constraints follow:
 *   1) [M] it must be a PLAYER number, 1 to 4. With our observer's 7 the player
 *      closes the connection without answering — on Wireshark the setup is byte
 *      for byte identical to a real player's except for the device number, and
 *      the reply is a FIN. [E] The Deep Symmetry analysis reports the same
 *      requirement, so this is not an artefact of our own hardware;
 *   2) [M] it must be a number PRESENT ON THE NETWORK, not merely free.
 *
 * AN OPEN QUESTION, recorded so that it is not mistaken for settled: CDJ-3000s
 * use 5 and 6 as legitimate player numbers, and how those obtain metadata under
 * constraint 1 is not known to us — we have never had one on the bench. It may
 * be that the real rule is constraint 2 alone and that 1..4 is what it reduces
 * to on the hardware everyone has measured, or that newer players use a
 * different dialogue. Either way, keeping to 1..4 is the cautious choice: it is
 * what is documented and what works.
 *
 * Hence the decision to take a real number at start-up and keep it. */
static uint8_t np_asking_device(uint8_t target, int64_t now, bool *cautious)
{
    (void)target; (void)now;
    if (cautious) *cautious = true;
    if (s_ka_device >= 1 && s_ka_device <= 4) return s_ka_device;
    return 0;                                        /* no real identity */
}

/* Detect a change of track on the master and schedule the query.
 *
 * IMPORTANT, [E] learned from beat-link: the metadata must be asked of the
 * player that OWNS THE MEDIUM — field Dr at 0x28 — not of the one that is
 * playing. If player 2 plays a track loaded over LINK from player 1's USB
 * stick, it is player 1 that holds the database and can describe it. They often
 * coincide, a player playing from its own USB, but not always.
 *
 * A track's identity is the QUADRUPLE: who holds the medium, which slot, which
 * type, which id. Two players can carry the same rekordbox id on different USB
 * sticks. [E] beat-link compares exactly these fields to decide whether the
 * track has changed. */
static void nowplaying_note(const djlink_status_t *s)
{
    /* Without a real identity (1-4) the data service does not answer us: [M]
     * verified in the field in every variant — free number, someone else's
     * number, the target's own number — giving "0 elements" or a closed
     * connection. No point spending cycles. */
    if (!s_identity_real) return;
    if (s_net_role == NET_MASTER) return;   /* WE are the master: no real track
                                             * to ask about, and nobody to ask */
    if (!s->is_master) return;              /* only the master interests us */
    if (s->track_type != 0x01 || s->track_id == 0) return;   /* solo rekordbox   */
    if (s->track_device == 0 || s->track_device >= DEVNUM_MAX) return;
    if (s->track_device == s_ka_device) return;     /* the medium is ours, and fake */

    /* the same track we already know, or one already queued? */
    if (s->track_device == s_np_device && s->track_slot == s_np_slot_k &&
        s->track_type == s_np_type_k  && s->track_id   == s_np_id) return;
    if (s->track_device == s_np_want_dev && s->track_slot == s_np_want_slot &&
        s->track_type == s_np_want_type && s->track_id   == s_np_want_id) return;

    /* ALREADY KNOWN? A track looked up for the playlist a moment ago, on a
     * player that has just become master, needs no second journey: the answer
     * goes on the display straight from the table. */
    if (meta_known(s->device, s)) {
        const track_meta_t *m = &s_meta[s->device];
        strncpy(s_np_title,  m->title,  sizeof(s_np_title)-1);
        strncpy(s_np_artist, m->artist, sizeof(s_np_artist)-1);
        s_np_device = m->dev;   s_np_id     = m->id;
        s_np_slot_k = m->slot;  s_np_type_k = m->type;
        ui_set_track(s_np_title, s_np_artist);
        ESP_LOGI(TAG, "[now] device %d plays track %lu, title already known",
                 s->device, (unsigned long)s->track_id);
        return;
    }

    /* A new track: the old title leaves the display AT ONCE. Reading the
     * metadata is quick, around 150 ms, but showing the previous piece's title
     * over what is already playing would be worse than showing nothing. */
    ui_clear_track();
    s_np_want_dev  = s->track_device;      /* whoever HOLDS the medium is who we ask */
    s_np_for_playlist = false;             /* this one is for the display */
    s_np_for_player   = s->device;         /* who PLAYS it, for the table    */
    s_np_want_slot = s->track_slot;
    s_np_want_type = s->track_type;
    s_np_want_id   = s->track_id;
    s_np_retry_us  = 0;
    ESP_LOGI(TAG, "[now] the master (device %d) plays track %lu from device %d "
                  "(slot %d): asking for the metadata",
             s->device, (unsigned long)s->track_id, s->track_device, s->track_slot);
}

/* A NON-BLOCKING STATE MACHINE.
 * No waiting: we send a request and go back to work. When the answer arrives we
 * examine it and advance a state — exactly as we already do on the server side.
 * If it does not arrive in time, the timeout returns us to idle and we try
 * again. The loop NEVER stops: beats fall every 470 ms or so. */
typedef enum {
    NP_IDLE = 0,
    NP_DIR_CONNECT,   /* connecting to the "switchboard" :12523 */
    NP_DIR_WAIT,      /* waiting for the data service port      */
    NP_DB_CONNECT,    /* connecting to the data service         */
    NP_DB_HELLO,      /* 5-byte handshake                       */
    NP_DB_SETUP,      /* context setup                          */
    NP_DB_META,       /* "how many elements for track N?"       */
    NP_DB_RENDER,     /* "send them to me"                      */
} np_state_t;

static np_state_t s_np    = NP_IDLE;
static int64_t    s_np_deadline = 0;
static uint16_t   s_np_port = 0;
static uint32_t   s_np_count = 0;
static uint8_t    s_np_buf[1024];
static int32_t    s_np_len = 0;
static uint8_t    s_np_dev = 0, s_np_slot = 0, s_np_type = 0;
static uint8_t    s_np_asking = 0;   /* number (1-4) we present ourselves with */
static bool       s_np_no_slot = false;  /* 1-4 all taken: metadata impossible */
static uint32_t   s_np_id_q = 0;

#define NP_STEP_TIMEOUT_US  2000000     /* 2s per step: beyond that, give up */

/* SENDING, WITH RETRY.
 *
 * The ioLibrary's send() returns SOCK_BUSY — which is 0! — when the W5500 has
 * not finished transmitting the previous packet, its SENDOK flag not yet
 * raised. That is NOT an error: it means "try again".
 *
 * Ignoring the return value and advancing a state means believing we have sent
 * while the W5500 transmitted nothing — [M] verified with Wireshark: a 0-byte
 * segment, sequence number unchanged — and then waiting for an impossible
 * answer until the timeout. So a send stays PENDING until it has really gone
 * out. */
static uint8_t    s_np_out[160];
static size_t     s_np_out_len = 0;
static uint32_t   s_np_busy    = 0;   /* consecutive passes with SOCK_BUSY */
static np_state_t s_np_after   = NP_IDLE;

static void np_send(const uint8_t *b, size_t n, np_state_t next)
{
    if (n > sizeof(s_np_out)) n = sizeof(s_np_out);
    memcpy(s_np_out, b, n);
    s_np_out_len = n;
    s_np_after   = next;
}

static void np_abort(const char *why)
{
    if (s_np != NP_IDLE) w5500_net_tcp_disconnect(TCP_SN_PROBE);
    if (why) ESP_LOGI(TAG, "[now] giving up (%s), retrying in 10s", why);
    s_np = NP_IDLE;
    s_np_len = 0;
    s_np_out_len = 0;
    s_np_busy = 0;
    s_np_retry_us = esp_timer_get_time() + 10000000;

    /* The channel is free again, and the next request may come from the other
     * user: a flag left set would send its answer to the wrong place. */
    s_np_for_playlist = false;
}

static void np_go(np_state_t next)
{
    s_np = next;
    s_np_deadline = esp_timer_get_time() + NP_STEP_TIMEOUT_US;
}

static void nowplaying_tick(int64_t now)
{
    /* a track to ask about, and we are idle? then start. */
    if (s_np == NP_IDLE) {
        if (!s_np_want_dev || now < s_np_retry_us) return;
        const uint8_t *ip = s_dev_ip[s_np_want_dev];
        if (!ip[0]) return;
        s_np_dev  = s_np_want_dev;  s_np_id_q = s_np_want_id;
        s_np_slot = s_np_want_slot; s_np_type = s_np_want_type;
        s_np_len  = 0; s_np_port = 0;
        bool cautious = true;
        s_np_asking = np_asking_device(s_np_dev, now, &cautious);
        if (!s_np_asking) {
            /* All four 1-4 slots are taken by real players: there is no number
             * with which to present ourselves to the data service, so the
             * metadata are IMPOSSIBLE, not "failed". It is a limit of the
             * Pioneer protocol, not a fault of ours.
             *
             * There is no point retrying in a burst: while the network stays like
             * this the outcome will be the same. We say it ONCE, give up
             * about this track, and we will only try again when it changes —
             * by which time a player may have been switched off and a number
             * freed. */
            if (!s_np_no_slot) {
                s_np_no_slot = true;
                ESP_LOGI(TAG, "[now] no usable player number (1-4) to "
                              "present to device %d: no metadata for now. "
                              "The bridge works all the same.", s_np_dev);
            }
            /* We do NOT give up on the track: it is kept pending and checked
             * again every 30 s. If a player is switched off a number frees up
             * and we resume on our own, without needing the track to change. */
            s_np_retry_us = now + 30000000;
            return;
        }
        if (s_np_no_slot) {
            s_np_no_slot = false;
            ESP_LOGI(TAG, "[now] a usable number is available now: resuming "
                          "metadata reads");
        }
        ESP_LOGI(TAG, "[now] querying device %d, presenting as device %d%s",
                 s_np_dev, s_np_asking,
                 (s_np_asking == s_np_dev) ? "  <-- TRYING: its own number"
                                           : (cautious ? "" : "  (second choice)"));
        if (w5500_net_tcp_connect_start(TCP_SN_PROBE, ip, DBSERVER_PORT) != 0) {
            np_abort("connection to the switchboard failed"); return;
        }
        np_go(NP_DIR_CONNECT);
        return;
    }

    if (now > s_np_deadline) {
        uint8_t sr, ir; uint16_t txf, txm, rxa, rxm;
        w5500_net_tcp_diag(TCP_SN_PROBE, &sr, &ir, &txf, &txm, &rxa, &rxm);
        ESP_LOGE(TAG, "[now] STALLED in state %d | SR=0x%02x IR=0x%02x "
                      "(RECV=%d SENDOK=%d) | RX_pending=%u RX_max=%u "
                      "| TX_free=%u TX_max=%u",
                 (int)s_np, sr, ir, (ir & 0x04) ? 1 : 0, (ir & 0x10) ? 1 : 0,
                 rxa, rxm, txf, txm);
        np_abort("timeout");
        return;
    }

    /* A pending send? Retry until the W5500 really takes it. */
    if (s_np_out_len) {
        int32_t r = w5500_net_tcp_send(TCP_SN_PROBE, s_np_out, s_np_out_len);
        ESP_LOGI(TAG, "[now] state %d: sending %u bytes -> result %ld",
                 (int)s_np, (unsigned)s_np_out_len, (long)r);
        if (r == (int32_t)s_np_out_len) {       /* the chip has taken it */
            s_np_out_len = 0;
            s_np_busy    = 0;
            np_go(s_np_after);
        } else if (r < 0) {
            np_abort("send failed");
        } else {
            s_np_busy++;                        /* SOCK_BUSY: we retry */
        }
        return;
    }

    uint8_t  msg[160];
    size_t   len;
    int32_t  n;
    const uint8_t *ip = s_dev_ip[s_np_dev];

    switch (s_np) {
    case NP_DIR_CONNECT: {
        int r = w5500_net_tcp_connect_poll(TCP_SN_PROBE);
        if (r < 0) { np_abort("switchboard unreachable"); return; }
        if (r == 0) return;                       /* still in progress: come back later */
        static const uint8_t q[19] = { 0x00,0x00,0x00,0x0f,
            'R','e','m','o','t','e','D','B','S','e','r','v','e','r', 0x00 };
        np_send(q, sizeof(q), NP_DIR_WAIT);
        return;
    }
    case NP_DIR_WAIT:
        n = w5500_net_tcp_recv(TCP_SN_PROBE, s_np_buf, sizeof(s_np_buf));
        if (n < 0) { np_abort("the switchboard closed"); return; }
        if (n < 2) return;
        s_np_port = ((uint16_t)s_np_buf[0] << 8) | s_np_buf[1];
        w5500_net_tcp_disconnect(TCP_SN_PROBE);
        if (!s_np_port) { np_abort("invalid port"); return; }
        if (w5500_net_tcp_connect_start(TCP_SN_PROBE, ip, s_np_port) != 0) {
            np_abort("connection to the data service failed"); return;
        }
        np_go(NP_DB_CONNECT);
        return;

    case NP_DB_CONNECT: {
        int r = w5500_net_tcp_connect_poll(TCP_SN_PROBE);
        if (r < 0) { np_abort("data service unreachable"); return; }
        if (r == 0) return;
        static const uint8_t hello[5] = { 0x11, 0x00, 0x00, 0x00, 0x01 };
        np_send(hello, 5, NP_DB_HELLO);
        return;
    }
    case NP_DB_HELLO: {
        n = w5500_net_tcp_recv(TCP_SN_PROBE, s_np_buf, sizeof(s_np_buf));
        if (n < 0) { np_abort("handshake failed"); return; }
        if (n <= 0) return;
        ESP_LOGI(TAG, "[now] handshake: %ld bytes received -> sending the setup", (long)n);
        if (!s_np_asking) { np_abort("no usable number 1-4"); return; }
        len = db_req_setup(msg, s_np_asking);   /* TxID reserved, handled inside */
        np_send(msg, len, NP_DB_SETUP);
        return;
    }

    case NP_DB_SETUP:
        n = w5500_net_tcp_recv(TCP_SN_PROBE, s_np_buf, sizeof(s_np_buf));
        if (n < 0) { np_abort("setup refused"); return; }
        if (n <= 0) return;
        ESP_LOGI(TAG, "[now] setup: %ld-byte reply -> asking for the metadata", (long)n);
        len = db_req_metadata(msg, ++s_np_txid, s_np_asking,
                              s_np_slot, s_np_type, s_np_id_q);
        np_send(msg, len, NP_DB_META);
        return;

    case NP_DB_META: {
        n = w5500_net_tcp_recv(TCP_SN_PROBE, s_np_buf, sizeof(s_np_buf));
        if (n < 0) { np_abort("no metadata"); return; }
        if (n < 42) return;

        /* [E] Message structure, per dysentery: a 32-byte header, then
         * arguments of 5 bytes each — one tag byte 0x11 plus four of value:
         *   arg1: tag at 32, value at 33..36  -> the type of the request
         *   arg2: tag at 37, value at 38..41  -> HOW MANY elements there are
         * Mind the tag byte: reading from 37 takes the TAG as the high byte of
         * the number. */
        uint16_t kind = ((uint16_t)s_np_buf[11] << 8) | s_np_buf[12];
        if (kind == 0x4003) {          /* "requested media not available" */
            np_abort("the player says: media not available");
            return;
        }
        if (kind != 0x4000) {
            ESP_LOGI(TAG, "[now] unexpected reply 0x%04x to the metadata", kind);
            np_abort("unexpected reply");
            return;
        }
        s_np_count = ((uint32_t)s_np_buf[38]<<24)|((uint32_t)s_np_buf[39]<<16)|
                     ((uint32_t)s_np_buf[40]<<8) | s_np_buf[41];
        ESP_LOGI(TAG, "[now] the player has %lu items for this track",
                 (unsigned long)s_np_count);
        if (!s_np_count || s_np_count > 64) { np_abort("absurd count"); return; }
        len = db_req_render(msg, ++s_np_txid, s_np_asking,
                            s_np_slot, s_np_type, s_np_count);
        s_np_len = 0;
        np_send(msg, len, NP_DB_RENDER);
        return;
    }

    case NP_DB_RENDER:
        /* We accumulate: the reply is made of several messages, which may
         * arrive in several packets. Extraction is attempted on every pass. */
        n = w5500_net_tcp_recv(TCP_SN_PROBE, &s_np_buf[s_np_len],
                               (int32_t)sizeof(s_np_buf) - s_np_len);
        if (n > 0) {
            s_np_len += n;
            s_np_deadline = now + NP_STEP_TIMEOUT_US;   /* data is arriving: be patient */
        } else if (n < 0 && s_np_len == 0) {
            np_abort("the player closed the connection"); return;
        }
        if (s_np_len > 0) {
            char t[NP_TITLE_MAX] = {0}, a[NP_TITLE_MAX] = {0};
            if (db_extract_item(s_np_buf, s_np_len, DB_ITEM_TITLE, t, sizeof(t))) {
                db_extract_item(s_np_buf, s_np_len, DB_ITEM_ARTIST, a, sizeof(a));

                /* WHOSE ANSWER IS THIS? The channel serves two users and they
                 * must not overwrite each other.
                 *
                 * s_np_title and its companions mean "the track the MASTER is
                 * playing": they feed the display and they are what
                 * nowplaying_note compares against to decide whether to ask
                 * again. Filling them with a title fetched for the playlist —
                 * for some other player's track — makes that comparison fail,
                 * so the master's title is requested afresh, which displaces
                 * them again: the two users ask in turn, for ever. */
                /* INTO THE TABLE FIRST, for whichever player is playing this
                 * track: that is the copy both users read afterwards. */
                if (s_np_for_player && s_np_for_player < META_SLOTS) {
                    track_meta_t *m = &s_meta[s_np_for_player];
                    m->id   = s_np_id_q;  m->dev  = s_np_dev;
                    m->slot = s_np_slot;  m->type = s_np_type;
                    strncpy(m->title,  t, sizeof(m->title)-1);
                    strncpy(m->artist, a, sizeof(m->artist)-1);
                }

                /* The playlist gets it whoever asked: a line may be waiting for
                 * this very track. */
                sdcard_note_metadata(s_np_id_q, t, a);
                /* And to OSC, whose custom profile may carry the title. It
                 * filters repetitions itself: the same track can arrive here
                 * more than once. */
                osc_out_note_track(s_np_for_player, s_np_id_q, t, a);

                if (s_np_for_playlist) {
                    ESP_LOGI(TAG, "[now] playlist: player %u track %lu is \"%s\"%s%s",
                             (unsigned)s_np_for_player, (unsigned long)s_np_id_q,
                             t, a[0] ? " - " : "", a);
                } else {
                    /* The display's own copy. It means "what the MASTER plays",
                     * and only a request made for the display may set it. */
                    strncpy(s_np_title,  t, sizeof(s_np_title)-1);
                    strncpy(s_np_artist, a, sizeof(s_np_artist)-1);
                    s_np_device = s_np_dev;   s_np_id     = s_np_id_q;
                    s_np_slot_k = s_np_slot;  s_np_type_k = s_np_type;
                    ESP_LOGI(TAG, "[now] device %d plays: \"%s\"%s%s",
                             s_np_dev, s_np_title,
                             s_np_artist[0] ? " - " : "", s_np_artist);
                    ui_set_track(s_np_title, s_np_artist);
                }
                if (s_np_want_dev == s_np_dev && s_np_want_id == s_np_id_q)
                    s_np_want_dev = 0;                  /* request satisfied */
                s_np_for_playlist = false;
                w5500_net_tcp_disconnect(TCP_SN_PROBE);
                s_np = NP_IDLE;
                return;
            }
        }
        if (n < 0) { np_abort("closed with no title"); return; }
        return;

    default:
        s_np = NP_IDLE;
        return;
    }
}

/* --- dbserver: we answer metadata for our virtual track --------------------
 *
 * We declare Mv=0x8000, "rekordbox track loaded", because [M] without it the
 * players do not follow our tempo. But then we must also be able to DESCRIBE
 * that track, or the result is E-8309 Link Access Error and a frozen UI. We
 * answer with a track that identifies the bridge. */
#define DB_TRACK_TITLE   "Ableton Live"
#define DB_TRACK_ARTIST  "aggeggino"
#define DB_ITEM_COUNT    3          /* title, artist, tempo */

static bool db_handle_message(const uint8_t *b, int32_t n)
{
    db_request_t req;
    if (!db_parse_message(b, n, &req)) {
        ESP_LOGE(TAG, "[dbd] message not recognised (%ld bytes)", (long)n);
        return false;
    }

    uint8_t out[256];
    size_t len;

    switch (req.type) {
        case DB_REQ_SETUP:            /* "here I am": we answer with our number */
            len = db_msg_success(out, req.txid, 0x0000, s_ka_device);
            db_reply(TCP_SN_DBDATA, out, len, "setup");
            ESP_LOGI(TAG, "[dbd] setup -> I am device %d", s_ka_device);
            break;

        case DB_REQ_METADATA:         /* "how much metadata do you have?" -> we have 3 */
            len = db_msg_success(out, req.txid, DB_REQ_METADATA, DB_ITEM_COUNT);
            db_reply(TCP_SN_DBDATA, out, len, "metadata");
            ESP_LOGI(TAG, "[dbd] metadata -> %d items available", DB_ITEM_COUNT);
            break;

        case DB_REQ_RENDER_MENU: {
            /* IMPORTANT: header, items and footer must go in ONE TCP send.
             * Sending them separately splits them across several network
             * packets, and [E] the players — and even rekordbox — have known
             * bugs with dbserver messages divided between packets: beat-link
             * "goes to some lengths to avoid having them split". So everything
             * is accumulated in a single buffer. */
            static uint8_t big[768];
            size_t t = 0;
            double bpm = (st.link_bpm > 1.0) ? st.link_bpm : 120.0;
            char tempo[16];
            snprintf(tempo, sizeof(tempo), "%.2f", bpm);

            t += db_msg_menu_header(&big[t], req.txid);
            t += db_msg_menu_item(&big[t], req.txid, DB_TRACK_TITLE,  1, DB_ITEM_TITLE);
            t += db_msg_menu_item(&big[t], req.txid, DB_TRACK_ARTIST, 1, DB_ITEM_ARTIST);
            t += db_msg_menu_item(&big[t], req.txid, tempo,
                                  (uint32_t)(bpm * 100.0 + 0.5), DB_ITEM_TEMPO);
            t += db_msg_menu_footer(&big[t], req.txid);

            w5500_net_tcp_send(TCP_SN_DBDATA, big, t);
            break;
        }

        default:
            /* A 0x4003 reply with ONE argument: the type of the request.
             * [M] The exact shape was captured from a real player with the
             * client probe. Asking the hardware instead of guessing. */
            len = db_msg_ack(out, req.txid, req.type);
            w5500_net_tcp_send(TCP_SN_DBDATA, out, len);
            ESP_LOGI(TAG, "[dbd] type=0x%04x -> ack 0x4003 (as a real player does)", req.type);
            break;
    }
    return true;
}

/* --- THE DATA SERVICE (port 1051) ------------------------------------------
 *
 * This is not a probe: it is the real service. After the switchboard the player
 * opens a SECOND TCP connection here and speaks the dbserver protocol, and we
 * answer the whole dialogue (see the dbserver component):
 *
 *   SETUP        -> "I am device N"
 *   METADATA     -> "I have DB_ITEM_COUNT elements"
 *   RENDER_MENU  -> title, artist and the real tempo, in a single send
 *   any other type -> a 0x4003 ack, as a real player would
 *
 * The purpose is not diagnostic: it is to make our track appear on the PLAYERS'
 * DISPLAY when they look at the master. Whether it truly succeeds can only be
 * checked by looking at the players, not from this log. */
static bool s_dbd_listening = false;

static void dbdata_tick(int64_t now)
{
    static int64_t last = 0, retry = 0;
    if (st.ver != 0x04) { s_dbd_listening = false; return; }

    if (!s_dbd_listening) {
        if (now < retry) return;
        retry = now + 2000000;
        if (w5500_net_tcp_listen(TCP_SN_DBDATA, DBDATA_PORT) == 0) {
            s_dbd_listening = true;
            ESP_LOGI(TAG, "[dbd] LISTENING on TCP %d (data service)", DBDATA_PORT);
        } else {
            ESP_LOGE(TAG, "[dbd] listen FAILED on port %d", DBDATA_PORT);
        }
        return;
    }

    if (now - last < 50000) return;
    last = now;

    uint8_t buf[128], peer[4] = {0};
    int32_t n = w5500_net_tcp_poll(TCP_SN_DBDATA, buf, sizeof(buf), peer);
    if (n > 0) {
        /* NOT DUMPED. Printing each request as hex and ASCII costs two
         * buffers filled on every packet, and the protocol is understood: see
         * the dbserver component for what these messages carry. */

        /* HANDSHAKE: the 5-byte setup packet must be echoed back unchanged. */
        if (n == 5 && buf[0] == 0x11) {
            w5500_net_tcp_send(TCP_SN_DBDATA, buf, 5);
        } else {
            db_handle_message(buf, n);   /* a dbserver message -> a polite answer */
        }
    } else if (n == -2) {
        w5500_net_tcp_close(TCP_SN_DBDATA, DBDATA_PORT);
    }
}

/* EVERYTHING THAT MUST BE REDONE WHEN THE ADDRESS CHANGES.
 *
 * The DJ Link sockets are reopened by open_djlink_sockets, but three more sit
 * outside it — the portmapper, the dbserver switchboard and the data service —
 * because each opens itself on its first tick. Clearing their flags is what
 * makes them do it again.
 *
 * [U] IT MAY WELL NOT BE NECESSARY. On the W5500 SIPR is a GLOBAL register, not
 * a property of a socket: nothing stores the address, it is read when a packet
 * is built or filtered, so an open socket should simply follow. But that has
 * never been exercised — unplugging the cable used to bring back the SAME
 * address, so nothing had to adapt — and the symptom of being wrong would be a
 * dbserver that stops answering after a DHCP lease arrives, with nobody
 * connecting the two events. Reopening costs three sockets' worth of setup,
 * once, and removes the question. */
static void reopen_after_address_change(void)
{
    s_pm_open       = false;
    s_db_listening  = false;
    s_dbd_listening = false;
    open_djlink_sockets();
}

/* --- Beat packet engine (Ableton source) -----------------------------------
 *
 * ONE SINGLE INSTANT OF TRUTH: T_send = T_beat(Link) + offset_rev - tx_lead.
 * BOTH the arming of the timer AND the SPI quiet window derive from T_send, so
 * they cannot fall out of step. The timer wakes a high-priority task that does
 * the sending — no SPI in the timer's context. */
/* The SPI quiet window. MIND THIS: [M] a display flush, measured again with the
 * full UI, lasts up to about 11 ms — not the 3.1 ms of the first sizing. With a
 * 5 ms window a long flush started just beforehand did NOT have time to finish,
 * and the beat waited for the bus: actual departure +2.7 to +4.2 ms. 14 ms
 * covers the longest flush observed, with margin. */
#define BEAT_QUIET_US   14000
/* Tail of the window after T_send. It is NOT this value that decides when the
 * protection ends: beat_task closes it as soon as the packet has gone out. This
 * remains only as a SAFETY NET for the case where that task does not run at all
 * — otherwise the window would stay open forever and the display would stop
 * drawing anything.
 *
 * It must not be tight: [M] the task's wake-up was measured up to 1772 us after
 * T_send, and the packet's actual departure up to 3870 us. With 1000 us the
 * protection expired while the beat had not yet left, and LVGL could
 * legitimately take the bus — which accounts for the 2 ms of waiting the beat
 * was paying. */
#define BEAT_QUIET_TAIL_US 5000
#define BEAT_TX_LEAD_US  100     /* lead: [M] W5500 send measured at 55-115 us  */


static void beat_timer_cb(void *arg)            /* timer context: notify only */
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(s_beat_task, &hpw);
    portYIELD_FROM_ISR(hpw);
}

static void beat_task(void *arg)                /* high priority: it sends */
{
    (void)arg;
    uint8_t buf[DJLINK_BEATPKT_LEN];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if DJB_DEVELOPER
        /* A single sample of the wake-up instant, reused further down for
         * measure the duration of the send ON THE SAME beat. */
        int64_t t_wake = esp_timer_get_time();
        /* WAKE-UP jitter of the task: timer -> task scheduled */
        int32_t jit = (int32_t)(t_wake - s_beat_target);
        if (jit > s_beat_jit_max) s_beat_jit_max = jit;
        if (jit < s_beat_jit_min) s_beat_jit_min = jit;
#endif
        djlink_build_beat(buf, "aggeggino", s_ka_device, s_beat_bpm, (uint8_t)s_beat_bb);
#if DJB_DEVELOPER
        bsp_spi_watch_begin();      /* measures the bus wait of THE BEAT ALONE */
#endif
        /* The specialised path: 7 SPI transactions instead of 23, and no wait
         * for the transmission confirmation. The beat is a periodic broadcast
         * every 500 ms: the confirmation serves no purpose and cost about six
         * register reads. */
        w5500_net_sendto_fast(UDP_SN_BEAT, buf, DJLINK_BEATPKT_LEN,
                              DJB_BCAST, DJLINK_PORT_BEAT);
#if DJB_DEVELOPER
        {
            uint32_t wsum, wmax, wn; bool wdisp;
            bsp_spi_watch_end(&wsum, &wmax, &wn, &wdisp);
            if (wsum > s_beat_busw_sum_max) {
                s_beat_busw_sum_max = wsum;      /* the unluckiest beat */
                s_beat_busn_max     = wn;
            }
            if (wmax > s_beat_busw_max) { s_beat_busw_max = wmax; s_beat_busw_disp = wdisp; }
        }
#endif

        /* THE PACKET HAS GONE OUT: the bus reservation is no longer needed.
         *
         * The window closes HERE, not when a timer expires. Its opening edge is
         * anchored to a computed instant (T_send), but the actual departure is
         * an asynchronous event that can arrive milliseconds later: with a
         * purely time-based expiry there was an interval in which the protection
         * had already ended and the beat had not yet left, and LVGL could take
         * the bus exactly there. Now the two edges are consistent: one anchored
         * to the intended instant, the other to the real event.
         *
         * MANDATORY ORDER: close the window FIRST, THEN clear s_beat_armed_us.
         * The main loop schedules the next beat — and with it the next window —
         * as soon as it sees s_beat_armed_us at zero: swapping the two lines
         * would risk cancelling a window just opened for the following beat. */
        bsp_spi_set_quiet_window(0, 0);
#if DJB_DEVELOPER
        /* The REAL jitter: when the packet ACTUALLY went out. It includes the
         * SPI bus wait, and it is this that ends up in the Pioneers' phase, not
         * the task's wake-up. If the quiet window is working the two numbers are
         * similar; if they diverge, the beat is waiting for the display. */
        int64_t t_out = esp_timer_get_time();
        int32_t real = (int32_t)(t_out - s_beat_target);
        if (real > s_beat_out_max) s_beat_out_max = real;
        int32_t txdur = (int32_t)(t_out - t_wake);     /* paired: the same beat */
        if (txdur > s_beat_tx_max) s_beat_tx_max = txdur;
        if (real < s_beat_out_min) s_beat_out_min = real;
#endif
        s_beat_tx_count++;
        s_beat_armed_us = 0;        /* gone: the loop may schedule the next one */
    }
}

/* ===========================================================================
 * TEMPORARY PRIORITY ON THE NETWORK BLOCKS (priority ceiling)
 *
 * The SPI lock is taken and released for EVERY elementary transaction of the
 * ioLibrary: [M] measured at 1700-3200 acquisitions per second, averaging 100 us
 * — very close to the ~81 us theoretical. So the typical case is healthy.
 *
 * But the measured MAXIMUM is 9-15 ms, and the culprit is always main(p1): the
 * lowest-priority task in the system is suspended by the scheduler WHILE it
 * holds the lock, and goes on holding it while doing nothing with it. [M] Some
 * 20 events in 30 s, 0.8% of the time: rare, but every so often they land right
 * on the beat.
 *
 * The remedy: for the duration of the blocks that use the network, main raises
 * its own priority. NOT at every acquisition — 3000 times a second would be
 * unsustainable — but twice per turn of the loop, about 400 times a second,
 * which also covers the pauses BETWEEN one transaction and the next, where the
 * suspension actually happens.
 *
 * THE VALUE 22 IS CHOSEN, not arbitrary: above taskLVGL (2) and w5rx (20), which
 * therefore no longer interrupt main while it holds the bus; and BELOW beat_tx
 * (23), which keeps absolute precedence — raising main to 23 would have delayed
 * the beat, the very opposite of the purpose.
 *
 * A side effect worth knowing: w5rx can no longer interrupt the drains. It does
 * not falsify the phase measurements, since the instant is marked by the ISR,
 * but it delays the emptying of the sockets; the W5500's buffers cover hundreds
 * of milliseconds.
 * =========================================================================== */
#define NET_PRIO_CEILING  22

static UBaseType_t s_main_prio_base = 0;

static inline void net_prio_raise(void)
{
    if (s_main_prio_base == 0) s_main_prio_base = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, NET_PRIO_CEILING);
}

static inline void net_prio_restore(void)
{
    if (s_main_prio_base) vTaskPrioritySet(NULL, s_main_prio_base);
}

/* ===========================================================================
 * MIDI CLOCK ENGINE
 *
 * The engine itself lives in components/midi_clock: here are only the three
 * hooks that tie it to the bridge — the phase source, the transport, and the
 * sink.
 *
 * The sink counts the bytes by type and measures how long passes between the
 * INTENDED instant and actual delivery, then forwards each byte to whichever
 * transports are enabled: DIN, USB, network. Keeping the counters even now that
 * it transmits is what makes the [midi] line able to say whether the 24-pulse
 * grid is holding, independently of any one transport.
 *
 * It runs on CORE 1 like OSC, at priority 9 — one below OSC. Both events are
 * short, but OSC's is 24 times rarer: preferring it costs less overall delay.
 * =========================================================================== */
#if DJB_MIDI_CLOCK

#define MIDI_TASK_PRIO  10       /* see the note on OSC_TASK_PRIO          */
#define MIDI_TASK_STACK  3072

static volatile uint32_t s_midi_tick, s_midi_start, s_midi_stop;
static volatile int32_t  s_midi_lat_max;   /* delivery - intended instant */

#if DJB_MIDI_NET
/* A WiFi transition is under way: we are waiting for the address on the new
 * interface. Declared HERE, rather than among the WiFi variables, because
 * midi_net_poll() — which lives higher up the file — reads it too. */
static volatile bool s_link_wait_got_ip = false;   /* Link switched back on at GOT_IP */

/* --- transport: Network MIDI 2.0 -------------------------------------------
 *
 * Our role is HOST: we announce ourselves on _midi2._udp and wait to be invited
 * by the software — a DAW, a lighting engine, macOS's "MIDI UMP". We chase
 * nobody, which is consistent with a device that GIVES the tempo.
 *
 * FEC and retransmission are DELIBERATELY OFF. The standard provides them
 * because UDP loses packets, but they are meant for note data: a clock pulse
 * recovered late has no value left, and if the receiver plays it, it introduces
 * a phase error. Better to lose it. */
#define MNET_GROUP        0        /* UMP group 0                             */
#define MNET_TASK_PRIO    8        /* below midi_clk(10) and osc_tx(9)        */
#define MNET_TASK_STACK   4096

static midi2_net_host_handle_t s_mnet = NULL;
static char s_mnet_id[24];
static volatile uint32_t s_mnet_sent = 0, s_mnet_full = 0;

/* Which value of Announce the running host was started with, and from when a
 * retry is allowed after a failure. Declared here because both midi_net_start()
 * and midi_net_poll() use them. */
static bool    s_mnet_mdns_on  = false;
static int64_t s_mnet_retry_us = 0;

static void midi_net_start(void)
{
    if (s_mnet) return;
    if (!settings_get_midi_net_enabled()) {
        ESP_LOGI(TAG, "[midinet] off by settings (can be switched back on from the menu)");
        return;
    }

    /* Product identifier: printable ASCII, unique per device. It is derived
     * from the MAC because two bridges on the same network must be
     * distinguishable by whoever invites them. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_mnet_id, sizeof(s_mnet_id), "aggeggino-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    midi2_net_host_config_t cfg;
    midi2_net_host_config_default(&cfg);
    cfg.endpoint_name       = "aggeggino";
    cfg.product_instance_id = s_mnet_id;
    cfg.enable_mdns         = settings_get_midi_net_mdns();
    cfg.act_as_host         = true;
    cfg.max_clients         = 2;      /* a DAW and a lighting engine together */
    cfg.fec_repeats         = 0;      /* see the note above              */
    cfg.request_retransmit  = false;
    /* Without this the client connects but sees NO MIDI port at all: it is the
     * in-band discovery macOS sends immediately after the session opens. */
    cfg.respond_to_stream_discovery = true;
    cfg.function_block_name = "aggeggino clock";
    cfg.task_priority       = MNET_TASK_PRIO;
    cfg.task_stack_size     = MNET_TASK_STACK;
    cfg.task_core_id        = 1;      /* core 0 stays the real-time island */

    esp_err_t e = midi2_net_host_start(&cfg, &s_mnet);
    if (e != ESP_OK) {
        s_mnet = NULL;
        ESP_LOGW(TAG, "[midinet] start failed (%s): no network output "
                      "- most likely cause: lwIP sockets exhausted",
                 esp_err_to_name(e));
        return;
    }
    /* Recorded HERE, with which Announce the host started. Keeping it elsewhere
     * meant that on the first pass the comparison found a difference that did
     * not exist and restarted the host just started — visible in the boot log as
     * "Announce changed: restarting the host" two seconds after power-on. */
    s_mnet_mdns_on = cfg.enable_mdns;
    ESP_LOGI(TAG, "[midinet] host '%s' (%s) su port %u, core 1",
             "aggeggino", s_mnet_id, (unsigned)midi2_net_host_port(s_mnet));
}

/* A request to restart the host, raised by a touch in the settings and served by
 * the main loop. The touch does NOT restart it directly: that would run on the
 * UI task, which would stay blocked for as long as closing sockets, tasks and
 * the mDNS announcement takes. */
static volatile bool s_mnet_restart = false;
/* A copy of the last value handed to the UI. It exists only so the log can say
 * WHY the phase selector shows MIDI or does not, instead of leaving it to be
 * deduced. */
static bool s_midi_up_seen = false;

static void on_midinet_settings_changed(void)
{
    s_mnet_restart = true;
}

static void midi_net_stop(void)
{
    if (!s_mnet) return;
    midi2_net_host_stop(s_mnet);
    s_mnet = NULL;
    ESP_LOGI(TAG, "[midinet] host stopped");
}

/* The Announce value the currently running host was started with. */

/* Follows the setting, exactly as midi_din_poll() does for the DIN.
 *
 * ONLY WHAT REALLY CHANGED IS TOUCHED. A blind "stop and restart" on every MIDI
 * setting change — the DIN included — meant that switching the DIN off tore
 * down and rebuilt a network host that was working; and if the rebuild failed
 * (port still busy, mDNS state) the host stayed down with its setting at ON: a
 * permanent red badge, for an output nobody had touched.
 *
 * The ONE case that requires a restart with the setting unchanged is Announce,
 * because the component reads that parameter once at start-up. */
static void midi_net_poll(void)
{
    /* A WiFi TRANSITION IS UNDER WAY: nothing is touched.
     *
     * Moving to client mode the stop is synchronous but the start happens
     * LATER, on the got-IP event — "only now is there an address to announce
     * ourselves on". Without this guard the poll would restart the host midway,
     * on an interface that has no address yet.
     *
     * Ableton Link's flag is reused because it means exactly the same thing —
     * "waiting for the new interface" — and two flags with the same meaning
     * sooner or later diverge. */
    if (s_link_wait_got_ip) return;

    bool want = settings_get_midi_net_enabled();
    bool mdns = settings_get_midi_net_mdns();

    if (s_mnet && want && mdns != s_mnet_mdns_on) {
        ESP_LOGI(TAG, "[midinet] Announce changed: restarting the host");
        midi_net_stop();
    }
    if (want && !s_mnet) {
        /* A wait after a failure. Without it the poll retries every 50 ms:
         * twenty failed allocations a second and twenty log lines, drowning the
         * very message needed to understand what is wrong. */
        int64_t now = esp_timer_get_time();
        if (now < s_mnet_retry_us) return;
        midi_net_start();
        if (!s_mnet) s_mnet_retry_us = now + 5000000;   /* retry in 5 s */
    }
    if (!want && s_mnet) midi_net_stop();
}
#endif /* DJB_MIDI_NET */

#if DJB_MIDI_DIN
/* --- transport: MIDI DIN (M5Stack Unit MIDI U187, Port C) ------------------
 *
 * The simplest of the three sinks, for a structural reason: the UART has its
 * OWN FIFO and shift register. Writing a byte is a register access of a few
 * hundred nanoseconds, after which the byte leaves by itself at wire speed. No
 * locks, no quiet window, no contention — the exact opposite of the W5500 on
 * the shared SPI.
 *
 * At 135 BPM that is 54 bytes per second of 320 us each: 1.7% occupancy. The
 * FIFO is always empty by the time the next pulse arrives.
 *
 * ============================================================================
 * THE U187'S SWITCH MUST BE ON "SEPARATE". THIS IS NOT A DETAIL.
 * ============================================================================
 *
 * [E] The M5Stack documentation, read in full, says this — and the pin names
 * are from the UNIT's point of view, not the controller's:
 *
 *   SEPARATE  the unit's RX (= the controller's TX) -> SAM2695 synth AND OUTPUT
 *             the unit's TX (= the controller's RX) -> INPUT, but floating
 *
 *   BYPASS    the unit's RX (= the controller's TX) -> the SAM2695 synth ONLY
 *             the unit's TX (= the controller's RX) -> receives from INPUT, and
 *             INPUT and OUTPUT are wired directly together
 *
 * So in BYPASS our clock reaches only the internal synthesiser, and the DIN OUT
 * carries whatever comes in at the DIN IN: the firmware drives nothing. In
 * SEPARATE our TX reaches both the synth and the output, and it is the only
 * position in which this code makes any sense.
 *
 * A secondary consequence: the internal synth receives our stream regardless.
 * Harmless for a clock — System Real Time messages produce no sound — but worth
 * remembering the day notes are sent.
 *
 * A future possibility, for free: in BYPASS the unit's TX carries what arrives
 * at the DIN IN. With the RX pin already wired, reading incoming MIDI — and so
 * using an external MIDI clock as a source — needs no further hardware.
 *
 * [U] WHICH OF THE TWO GPIOs IS THE TX REMAINS OPEN: the CoreS3's Port C is
 * G17/G18 and the documentation does not say. If the probe produces nothing,
 * swapping these two numbers and recompiling is the first thing to try. */
/* --- DETECTING THE MODULE: G18, independent of the UART --------------------
 *
 * [M] MEASURED, not deduced. With the internal pull-down and the output ACTIVE:
 *   module connected      pull-down=1 pull-up=1  -> held HIGH by the module
 *   module disconnected   pull-down=0 pull-up=1  -> floating
 *
 * So a permanent pull-down and a level read are enough: 1 present, 0 absent.
 *
 * WHY IT IS BETTER THAN G17. The M5Stack documentation calls G18 "floating" in
 * SEPARATE mode, and that is true only in the sense that it carries no MIDI
 * data: the module holds it high anyway, almost certainly through the pull-up
 * on the optocoupler output of the input stage.
 *
 * The practical advantage is large: G18 does NOT belong to the UART, so it can
 * be measured AT ANY TIME — even while transmitting. With G17 the measurement
 * was only possible with the output off, because that pin is owned by the
 * serial driver, and a hot disconnection would have gone unseen.
 *
 * G17 stays dedicated to transmission alone. */
#define MDIN_TX_GPIO     17      /* transmit only */
#define MDIN_SENSE_GPIO  18      /* detection only, never the UART's */

#define MDIN_UART     UART_NUM_1      /* UART0 stays with the console */
#define MDIN_BAUD     31250

static bool     s_din_up = false;
static volatile uint32_t s_din_bytes = 0;

/* DETECTING THE MODULE — electrical, continuous, with no dedicated task.
 *
 * [M] VALIDATED IN THE FIELD: seven readings in three clean blocks — present,
 * absent, present — with the two changes of verdict falling exactly on the two
 * longest pauses between touches, which are when the cable was pulled and
 * plugged back in. No isolated reading, no oscillation within a block.
 *
 * WHAT IT MEASURES, precisely: not "the module is connected" but "the module is
 * connected AND in SEPARATE mode". In bypass the same measurement reported a
 * free line with the module present — and that is correct for us, because in
 * bypass our TX reaches only the internal synthesiser and the DIN OUT cannot be
 * driven. The test answers the question we actually care about.
 *
 * WHY THE RX PIN IS NOT GIVEN TO THE UART.
 * The measurement lives on a permanent pull-down. If that pin were also the
 * serial RX, with the module disconnected the line would sit steadily LOW — and
 * for a UART the idle state is HIGH: it would be read as a continuous start
 * bit, producing a storm of framing errors and 0x00 bytes in the FIFO.
 *
 * So rx_io_num = UART_PIN_NO_CHANGE is passed, and the driver (uart.c line 225
 * of ESP-IDF v5.5) ties the internal RX signal to the GPIO matrix's CONSTANT
 * ONE: the serial port sees a line permanently idle and never receives
 * anything.
 *
 * THE PRICE OF THE CHOICE: MIDI IN is given up. Reading an incoming MIDI clock
 * would require handing the pin back to the UART, and then this detection would
 * have to be abandoned. They are alternatives, not cumulative.
 *
 * WHY POLLING AND NOT AN INTERRUPT: gpio_get_level() is a register read. On the
 * slow loop that already exists it is twenty reads a second, not measurable. An
 * interrupt would cost the ISR, the contact bounce — a connector going in
 * produces dozens of edges — and state shared between ISR and main loop.
 * Reading the SETTLED level makes the bounce disappear by itself. */
#define MDIN_HW_STABLE  3        /* agreeing reads before believing a change */

static bool s_din_hw   = false;  /* module detected, at the settled level      */
static int  s_din_hw_n = 0;      /* consecutive disagreeing reads              */

/* Prepare the pin for the MEASUREMENT. See the extended note on midi_din_poll(). */
static void midi_din_pin_init(void)
{
    const gpio_config_t g = {
        .pin_bit_mask = 1ULL << MDIN_SENSE_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* permanent: it IS the measurement */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&g);
    ESP_LOGI(TAG, "[din] continuous detection on GPIO%d (permanent pull-down)",
             MDIN_SENSE_GPIO);
}

static void midi_din_stop(void)
{
    if (!s_din_up) return;
    uart_driver_delete(MDIN_UART);
    s_din_up = false;
    /* No reconfiguration: the detection pin was never the UART's, so there is
     * nothing to take back. */
    ESP_LOGI(TAG, "[din] output off");
}

/* Switches the UART on. It does NOT decide WHETHER to switch it on: that is
 * midi_din_poll()'s job, the one place where the user's wish and the module's
 * presence meet. */
static void midi_din_start(void)
{
    if (s_din_up) return;
    const uart_config_t c = {
        .baud_rate  = MDIN_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Receive buffer at the smallest allowed: we receive nothing, but the
     * driver will not accept zero. Transmission WITHOUT a ring:
     * uart_write_bytes copies straight into the hardware FIFO and returns,
     * instead of queueing and delegating to an ISR. Less latency and, above
     * all, less jitter — which on a clock is what matters. */
    if (uart_driver_install(MDIN_UART, 256, 0, 0, NULL, 0) != ESP_OK)
        return;
    uart_param_config(MDIN_UART, &c);
    /* TX ONLY: the RX pin stays the detection GPIO, see above. */
    uart_set_pin(MDIN_UART, MDIN_TX_GPIO, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_din_up = true;
    ESP_LOGI(TAG, "[din] output ON: UART%d TX=GPIO%d @ %d baud "
                  "| the U187 switch must be set to SEPARATE",
             (int)MDIN_UART, MDIN_TX_GPIO, MDIN_BAUD);
}

/* --- TEST GENERATION ON THE SAM2695, for CV measurement on a scope ---------
 *
 * WHAT IT IS FOR. The U187 module contains a Dream SAM2695 synthesiser which in
 * SEPARATE mode receives everything we send it anyway. Its audio output is a
 * jack on the module: if a percussive sound produced a voltage pulse with a
 * steep edge and little ringing, that jack would become a CV clock source for
 * analogue machines, with no added hardware.
 *
 * But three things must first be MEASURED on a scope, and none is a foregone
 * conclusion:
 *   amplitude - it is a LINE output, so fractions of a volt; an analogue clock
 *               input typically wants five
 *   width     - and the coupling: audio outputs are almost always AC, so the
 *               pulse returns to zero and "rings" below it
 *   latency   - between the byte on the wire and the sound; if it is constant
 *               it can be compensated with the phase offset we already have, if
 *               it wanders no clock can be built on it
 *
 * WHAT IT GENERATES: one percussive note on channel 10 ON THE BEAT, that is
 * every 24 clock pulses. Not on every tick: at 132 BPM that would be 53 notes a
 * second — the synth would not separate them and neither would the scope.
 *
 * THE NOTE CHOSEN: 37, Side Stick in General MIDI, the driest hit on the map —
 * a steep transient and little ringing tail, which is exactly what an edge
 * needs. Should the waveform not convince, the next candidates are 42 (Closed
 * Hi-Hat) and 39 (Hand Clap): only the constant changes.
 *
 * COST: 3 bytes per beat on a UART at 31250 baud already 1.7% occupied.
 * Irrelevant, and above all it does NOT disturb the clock stream — the notes
 * interleave between one 0xF8 and the next, which is exactly what MIDI provides
 * for with System Real Time messages. */
/* --- CV SOURCE FROM THE SYNTHESISER ----------------------------------------
 *
 * THE IDEA: the SAM2695's line output as a source of clock pulses for modular
 * instruments. Using an audio output as CV is documented practice, and here the
 * hardware is already present — no extra board.
 *
 * THE STRUCTURAL LIMIT, worth knowing before optimising anything: the output is
 * almost certainly AC-COUPLED, that is through a capacitor that removes the DC
 * component. That rules out GATES — a sustained level would decay to zero — but
 * NOT TRIGGERS: of a pulse, the edge gets through, and the edge is exactly what
 * a clock input samples. The project makes sense for clock and for nothing
 * else.
 *
 * WHAT WE ASK OF THE SOUND. Not a timbre: an IMPULSE.
 *   - instantaneous attack, because the edge must be steep
 *   - very short decay, so pulses do not overlap at high tempos
 *   - little ringing: a tonal hit at 200 Hz would read as 200 pulses, not one
 *   - a single initial excursion: "multiple" sounds — a hand clap has three or
 *     four — would give spurious triggers
 *
 * THE CHOICE MUST BE MEASURED, not settled here. That is why the candidate
 * sounds live in a table that can be scrolled from the Developer menu: connect
 * the scope and move from one to the next without reflashing. */
#define SAM_CV_VEL    127      /* maximum: we need amplitude, not musicality */

/* Envelope parameters, standard GS NRPNs (verified against the official
 * M5_SAM2695 library):
 *   01 63  attack time     01 64  decay     01 66  release
 *   01 20  filter cutoff   01 21  resonance
 * The value 64 is neutral: 0 is minimum, 127 maximum. */
#define SAM_ENV_ATTACK    0      /* as fast as possible        */
#define SAM_ENV_DECAY     0      /* as short as possible       */
#define SAM_ENV_RELEASE   0      /* idem                       */
#define SAM_TVF_CUTOFF  127      /* filter wide open: keeps the transient */
#define SAM_TVF_RESON     0      /* no resonance: it would add ringing */

/* Candidates, from the most promising to the least. The first four are dry hits,
 * almost atonal; the last two serve as a CONTRAST — it is useful to see
 * on a scope what a pulse must NOT look like. */
typedef struct { uint8_t note; const char *name; } sam_snd_t;
static const sam_snd_t s_sam_snd[] = {
    { 75, "Claves"          },   /* the shortest and cleanest on the GM map */
    { 76, "Hi Wood Block"   },
    { 37, "Side Stick"      },   /* the previous default                    */
    { 31, "Sticks"          },
    { 42, "Closed Hi-Hat"   },   /* noise: good edge, long tail             */
    { 39, "Hand Clap"       },   /* CONTRAST: more transients, spurious triggers */
    { 36, "Bass Drum"       },   /* CONTRAST: slow rise, little edge        */
};
#define SAM_SND_N  ((int)(sizeof(s_sam_snd) / sizeof(s_sam_snd[0])))

/* The index lives in NVS, not here: the choice is the user's and must survive a
 * power cycle. The bounds check is not pedantry — if the table were ever
 * shortened, a larger saved index would read past its end. */
static inline int sam_idx(void)
{
    int i = (int)settings_get_cv_sound();
    return (i >= 0 && i < SAM_SND_N) ? i : 0;
}
#define SAM_CV_NOTE   (s_sam_snd[sam_idx()].note)

/* Given to settings_ui, which then shows the sound's name without knowing la
 * table, and without waiting for the loop to come round. */
static const char *sam_sound_name(int idx)
{
    return (idx >= 0 && idx < SAM_SND_N) ? s_sam_snd[idx].name : "-";
}
    /* Channel 10 in the 1-based MIDI numbering, which in the messages is 9. In
     * General MIDI that is the percussion channel. */
#define SAM_CV_CH     9
    /* Program 118 = SynthDrum. [E] Verified in the official M5_SAM2695
     * library's enum (M5_Synth_Def.h), which corresponds to GM 119 in the
     * 1-based numbering. The drum.ino example loads it exactly this way. */
#define SAM_CV_PROG   118

/* The CV output is active. TWO conditions: the user's setting AND the DIN
 * transmitting, because the pulse travels down that wire. */
static bool s_cv_on = false;
static volatile uint32_t s_cv_pulses = 0;

/* THE SAM2695 MAKES NO SOUND WITHOUT THIS.
 *
 * Sending a Note On directly produces nothing at all. [E] The official
 * M5_SAM2695 library explains it through its drum.ino example, which BEFORE
 * every note does:
 *
 *     midi.setInstrument(0, 9, SynthDrum);
 *
 * that is, Bank Select MSB at 0 followed by a Program Change on the channel.
 * Without it the channel has no timbre loaded and the Note Ons fall into the
 * void — the synth receives and ignores them, which is exactly the symptom
 * observed: no sound at the audio output, and no error anywhere.
 *
 * It must be sent again every time the test is switched back on: the module may
 * have been unplugged and reconnected meanwhile, and would have lost its state.
 * It costs three bytes, and only on a touch. */
/* --- SCHEDULING THE CV PULSE, with an offset OF ITS OWN --------------------
 *
 * WHY A DEDICATED TIMER AND NOT THE SINK.
 *
 * The clock engine has ONE phase offset, and that function shifts the entire
 * 24 PPQN grid. MIDI DIN, USB, network and CV all leave from the same sink:
 * shifting the grid for CV would shift MIDI too.
 *
 * But CV needs an offset of ITS OWN, because its path is longer — it goes
 * through the synthesiser, which decodes MIDI and synthesises, adding a latency
 * the direct MIDI does not have. Compensating both with the same number means
 * putting one of the two out of phase.
 *
 * So the note no longer starts from the sink: the sink merely ARMS a one-shot
 * for the right instant.
 *
 * HOW THE INSTANT IS COMPUTED. The sink receives `due_us`, the EXPECTED instant
 * of the tick — not the real one, hence immune to delivery jitter. On a beat
 * boundary the period is measured from the distance between two consecutive
 * beats, and the arming is for the NEXT beat:
 *
 *     target = due_us + period + offset
 *
 * Arming from the previous beat gives a whole beat of lead, so NEGATIVE offsets
 * work too — the pulse ahead of the beat, which is precisely the case needed to
 * compensate the synth's latency.
 *
 * WHERE IT RUNS: in the esp_timer task, priority 22, above everything living on
 * core 1. For punctuality that is BETTER than the clock task at 10, not
 * worse. */
/* --- CV PULSE ON A GPIO ----------------------------------------------------
 *
 * THE PIN. G8 is the serial console's RX on Port B. Logging is ONE-WAY — we only
 * write — so that pin serves no purpose, and it is reconfigured as an output.
 * It is the same reclamation already done on the MIDI module, where the RX
 * receives nothing in SEPARATE mode.
 *
 * WHY IT IS BETTER THAN THE NOTE ON THE SAM. The note crosses the UART, MIDI
 * decoding and synthesis before reaching a LINE output, AC-coupled at that —
 * hence triggers only, never gates, and with an amplitude that depends on
 * duration. The GPIO instead is driven by the same timer as the beat: a clean
 * edge, DC coupling, and a width we choose.
 *
 * AND IT WORKS WITHOUT THE DIN MODULE, which was the limit of the other route.
 *
 * LEVEL: 3.3 V. Many eurorack clock inputs trigger above 1 to 2.5 V, so this
 * may well be enough. If it is not, a 5 V stage is needed — a transistor and two
 * resistors — but that is a decision to take AFTER the measurement.
 *
 * POLARITY: idle LOW, pulse HIGH. It is the commonest convention for triggers
 * and gates; should the opposite ever be needed, it is a parameter. */
#define CV_GPIO        8

/* --- PARAMETERS, from the menu -------------------------------------------
 *
 * WIDTH in milliseconds. Fixed values, because the inputs do not agree: some
 * want a minimum of 1 ms, others prefer wider pulses.
 *
 * DIVISION in PPQN — pulses per quarter note. This is the CRITICAL parameter:
 * get it wrong and everything plays at the wrong speed.
 *
 *     1   the eurorack convention, the commonest
 *     2   eighths
 *     4   sixteenths
 *     24  the full MIDI clock grid
 *
 * They are all submultiples of the engine's 24 ticks per quarter, so they can
 * be derived by simple division.
 *
 * POLARITY: idle low is the convention, but some devices want the opposite and
 * offer no alternative. */
static const uint16_t CV_WIDTH_MS[]  = { 1, 2, 5, 10, 20 };
static const uint8_t  CV_PPQN_VAL[]  = { 1, 2, 4, 24 };
#define CV_WIDTH_N  ((int)(sizeof(CV_WIDTH_MS)/sizeof(CV_WIDTH_MS[0])))
#define CV_PPQN_N   ((int)(sizeof(CV_PPQN_VAL)/sizeof(CV_PPQN_VAL[0])))

static inline int cv_width_us(void)
{
    uint8_t i = settings_get_cv_width();
    if (i >= CV_WIDTH_N) i = 3;
    return CV_WIDTH_MS[i] * 1000;
}
static inline int cv_ppqn(void)
{
    uint8_t i = settings_get_cv_ppqn();
    if (i >= CV_PPQN_N) i = 0;
    return CV_PPQN_VAL[i];
}
/* Idle level. The edge is always its opposite. */
static inline int cv_idle(void) { return settings_get_cv_polarity() ? 1 : 0; }

static esp_timer_handle_t s_cv_timer  = NULL;   /* rising edge  */
static esp_timer_handle_t s_cv_fall   = NULL;   /* falling edge */
static bool               s_cv_pin_ok = false;
/* The ACTUAL pulse width, computed when the one-shot is armed: la
     * the user's choice, bounded by the period. See the note in
     * sam_cv_tick. */
static int                s_cv_width_us = 0;

static void cv_fall_cb(void *arg)
{
    (void)arg;
    gpio_set_level(CV_GPIO, cv_idle());
}

/* Takes the pin from the console and makes it an output. Once only. */
static void cv_pin_init(void)
{
    if (s_cv_pin_ok) return;
    const gpio_config_t g = {
        .pin_bit_mask = 1ULL << CV_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&g) != ESP_OK) {
        ESP_LOGE(TAG, "[cv] GPIO%d cannot be configured: no pulse output",
                 CV_GPIO);
        return;
    }
    gpio_set_level(CV_GPIO, cv_idle());
    s_cv_pin_ok = true;
}


static void cv_pulse_cb(void *arg)
{
    (void)arg;
    if (!s_cv_on) return;

        /* THE EDGE FIRST, then the note: the GPIO is the precise output and
         * must not wait behind three MIDI bytes at 31250 baud, which would show
         * on the edge. */
    if (s_cv_pin_ok) {
        gpio_set_level(CV_GPIO, !cv_idle());
        if (s_cv_fall && s_cv_width_us > 0) {
            esp_timer_stop(s_cv_fall);
            esp_timer_start_once(s_cv_fall, (uint64_t)s_cv_width_us);
        }
    }
    s_cv_pulses++;

        /* The note on the SAM is an EXTRA and needs the module: without it the
         * CV still leaves from the GPIO. */
    if (!s_din_up) return;
    /* Note On ONLY, as the official example does: GM percussion is one-shot by
         * single, and a Note Off is unnecessary. Dropping it halves the bytes
         * and, above all, removes a message that on some synths shortens the
         * sound's tail — the opposite of what we want to measure. */
    const uint8_t msg[3] = { 0x90 | SAM_CV_CH, SAM_CV_NOTE, SAM_CV_VEL };
    uart_write_bytes(MDIN_UART, msg, sizeof(msg));
}

    /* One NRPN parameter: the MSB/LSB selector, then the datum. */
static void sam_nrpn(uint8_t lsb, uint8_t val)
{
    const uint8_t m[7] = { 0xB0 | SAM_CV_CH, 0x63, 0x01,
                           0xB0 | SAM_CV_CH, 0x62, lsb,
                           0x06 };
    /* The Data Entry wants the value immediately after: it is sent in two
     * pieces, so as not to build a variable-length array. */
    uart_write_bytes(MDIN_UART, m, sizeof(m));
    uart_write_bytes(MDIN_UART, &val, 1);
}

static void sam_cv_program(void)
{
    const uint8_t msg[8] = {
        0xB0 | SAM_CV_CH, 0x00, 0x00,          /* Bank Select MSB = 0      */
        0xC0 | SAM_CV_CH, SAM_CV_PROG,       /* Program Change           */
        /* Channel volume at maximum (Control Change 7). Not optimism: it rules
         * out the most banal hypothesis — that the channel is simply turned
         * down — before measuring the real amplitude on a scope. And for a CV
         * pulse, amplitude is everything.
         *
         * MASTER volume is a different matter, done through a universal SysEx
         * (F0 7F 7F 04 01 00 <lvl> F7). We do not touch it: it acts on the whole
         * synth, and should it ever be needed it is a message of its own. */
        0xB0 | SAM_CV_CH, 0x07, 0x7F,          /* Channel Volume = 127     */
    };
    uart_write_bytes(MDIN_UART, msg, sizeof(msg));

    /* Shaping for CV: envelope at minimum, filter wide open. */
    sam_nrpn(0x63, SAM_ENV_ATTACK);
    sam_nrpn(0x64, SAM_ENV_DECAY);
    sam_nrpn(0x66, SAM_ENV_RELEASE);
    sam_nrpn(0x20, SAM_TVF_CUTOFF);
    sam_nrpn(0x21, SAM_TVF_RESON);
}

/* Follows the CV settings, as midi_net_poll and midi_din_poll do for their own
 * transports. Called on the slow turn.
 *
 * Three things to reapply, each for a different reason:
 *   - SWITCHING ON needs the program change, the volume and the envelope,
 *     because the module may have been reconnected and lost its state
 *   - CHANGING SOUND needs them again: it is another note, and the envelope is
 *     a channel parameter that must be reasserted
 *   - SWITCHING OFF needs nothing: we simply stop arming the timer */
static void cv_poll(void)
{
    static int p_idx = -1;

    /* CV does NOT need the DIN module: the pulse leaves from the GPIO. The
     * module serves only the note on the synthesiser, which is an addition. */
    bool want = settings_get_cv_enabled();
    int  idx  = sam_idx();

    /* The POLARITY is applied to the pin at once, not at the next pulse:
     * changing it with the output on means the resting level must change now, or
     * the device downstream would see a gate held open until the next beat. */
    if (s_cv_on && s_cv_pin_ok) {
        static int p_idle = -1;
        int idle = cv_idle();
        if (idle != p_idle) { gpio_set_level(CV_GPIO, idle); p_idle = idle; }
    }

    if (want && (!s_cv_on || idx != p_idx)) {
        cv_pin_init();
        if (!s_cv_fall) {
            const esp_timer_create_args_t fa = {
                .callback = cv_fall_cb, .name = "cv_fall",
                .dispatch_method = ESP_TIMER_TASK,
            };
            if (esp_timer_create(&fa, &s_cv_fall) != ESP_OK) s_cv_fall = NULL;
        }
        if (!s_cv_timer) {
            const esp_timer_create_args_t a = {
                .callback = cv_pulse_cb,
                .name     = "cv_pulse",
                /* A TASK and not an ISR: writing to the UART can block when the
                 * FIFO is full, and in ISR context that would be unacceptable. */
                .dispatch_method = ESP_TIMER_TASK,
            };
            if (esp_timer_create(&a, &s_cv_timer) != ESP_OK) {
                ESP_LOGE(TAG, "[cv] timer not created: no pulse output");
                s_cv_timer = NULL;
                return;
            }
        }
        s_cv_on = true;
        if (s_din_up) sam_cv_program();   /* only if the module is there */
        ESP_LOGI(TAG, "[cv] output ACTIVE | GPIO%d: %d PPQN, %d ms, idle %s "
                      "| SAM note: %s (%s)",
                 CV_GPIO, cv_ppqn(), cv_width_us() / 1000,
                 cv_idle() ? "ALTO" : "basso",
                 s_din_up ? "yes" : "NO (the DIN module is missing)",
                 s_sam_snd[idx].name);
        p_idx = idx;
    } else if (!want && s_cv_on) {
        s_cv_on = false;
        if (s_cv_timer) esp_timer_stop(s_cv_timer);
        if (s_cv_fall)  esp_timer_stop(s_cv_fall);
        if (s_cv_pin_ok) gpio_set_level(CV_GPIO, 0);   /* never leave it high */
        ESP_LOGI(TAG, "[cv] output off");
    }
}

/* Called by the sink on every pulse: at the beat boundary it arms the one-shot. */
static inline void sam_cv_tick(uint8_t status, int64_t due_us)
{
    static uint8_t n = 0;
    static int64_t prev = 0;

    if (!s_cv_on || !s_cv_timer) { n = 0; prev = 0; return; }
    if (status == MIDI_CLOCK_START) { n = 0; prev = 0; return; }
    if (status != MIDI_CLOCK_TICK)  return;

    /* One pulse every 'every' ticks: 24 for 1 PPQN, 1 for 24 PPQN. The engine
     * gives 24 ticks per quarter, and every division we want is a submultiple
     * of that. */
    int every = 24 / cv_ppqn();
    if (n % every == 0) {
        if (prev) {
            int64_t period = due_us - prev;      /* between two pulses, not two beats */
            /* Guard: from ~1 ms (24 PPQN at 2500 BPM) to 3 s. Outside that
             * range it is a re-lock, and arming on an absurd number would give
             * a pulse at random. */
            if (period > 1000 && period < 3000000) {
            /* THE WIDTH MUST BE BOUNDED BY THE PERIOD, and this is the
             * delicate point: a pulse wider than the period would NEVER return
             * low — the pin would stay high and the clock would vanish, with an
             * unintelligible symptom.
                 *
                 * 45% leaves a clean edge and never comes close to touching
                 * the next pulse. */
                int wmax = (int)(period * 45 / 100);
                int w    = cv_width_us();
                s_cv_width_us = (w > wmax) ? wmax : w;

                int64_t target = due_us + period + (int64_t)ui_get_phase_offset_cv_us();
                int64_t delay  = target - esp_timer_get_time();
                if (delay > 0) {
                    esp_timer_stop(s_cv_timer);
                    esp_timer_start_once(s_cv_timer, (uint64_t)delay);
                }
            }
        }
        prev = due_us;
    }
    if (++n >= 24) n = 0;
}

/* Reconciles the desired state with the possible one. Called on the slow loop.
 *
 * The user's setting is NEVER touched: if they asked for the DIN to be on and
 * the module is unplugged, the transport stops but the wish remains — and on
 * reconnection it starts again by itself, without anybody touching the menu.
 * That is why the menu shows the INTENDED state even when the hardware is
 * absent. */
static void midi_din_poll(void)
{
    /* A CONTINUOUS measurement, even while transmitting: the detection pin
     * (G18) does not belong to the UART, which lives on G17. */
    bool lvl = (gpio_get_level(MDIN_SENSE_GPIO) != 0);
    if (lvl == s_din_hw) {
        s_din_hw_n = 0;
    } else if (++s_din_hw_n >= MDIN_HW_STABLE) {
        s_din_hw   = lvl;
        s_din_hw_n = 0;
        settings_ui_set_din_detected(s_din_hw);
        ESP_LOGI(TAG, "[din] module %s", s_din_hw ? "CONNECTED" : "NOT CONNECTED");
    }

    bool want = settings_get_midi_din_enabled() && s_din_hw;
    if (want && !s_din_up) midi_din_start();
    if (!want && s_din_up) midi_din_stop();
}

#endif /* DJB_MIDI_DIN */

#if DJB_MIDI_USB
/* The USB output is transmitting. TWO conditions, both required:
 *   - the user's setting
 *   - the host has ENUMERATED us (tud_midi_mounted)
 * Without the second we would write into an endpoint nobody reads, and TinyUSB
 * would accumulate in its transmit buffer until it filled.
 *
 * The USB device stays up regardless: switching the output off in the settings
 * does NOT unmount it, because the descriptors are read once at enumeration and
 * redoing that would mean unplugging the cable. The setting governs the FLOW,
 * not the presence of the device — and that differs from network and DIN. */
static bool     s_usb_out_on = false;
static volatile uint32_t s_usb_bytes = 0, s_usb_drop = 0;

/* --- DEFERRED start of the USB device --------------------------------------
 *
 * WHY NOT AT BOOT. USB enumeration has tight timing constraints: the host sends
 * GET_DESCRIPTOR and expects an answer within milliseconds. If the device
 * presents itself while the bridge is still building LVGL, starting the LEDs
 * and the audio, the TinyUSB task on core 1 may not be scheduled in time, and
 * the host declares failure.
 *
 * Deferring by a few seconds FROM ENTERING THE LOOP — not merely moving the
 * call to the end of app_main — guarantees the system is not just initialised
 * but actually RUNNING: the tasks are going, the display has done its first
 * flushes, the network is up.
 *
 * The price is nil: nobody expects a USB device to appear instantly at power-on,
 * and whoever plugs the cable in does so afterwards anyway.
 *
 * ----------------------------------------------------------------------------
 * WHY THE DEVICE FIRST APPEARS AS A SERIAL PORT
 *
 * It is not a fault of ours: on the ESP32-S3 the internal USB PHY is connected
 * by default to the USB-Serial-JTAG peripheral, and the ROM does that before our
 * firmware exists. TinyUSB switches the PHY to USB-OTG when it is installed —
 * that is what the phy field of its configuration is for — and from there the
 * host must RE-ENUMERATE the device, this time as MIDI.
 *
 * So the sequence seen by the host is, by construction:
 *   1. an Espressif serial device   (from the ROM)
 *   2. disappearance                (the PHY switching)
 *   3. Aggeggino, a MIDI device     (TinyUSB)
 *
 * Shortening the deferral reduces the chance the host settles on the first one,
 * which is why 2000 is better than 4000. It does not remove it.
 *
 * The ONLY way to eliminate the first step is to burn the USB_PHY_SEL eFuse,
 * which connects the PHY straight to USB-OTG from the ROM onwards. That is a
 * PERMANENT AND IRREVERSIBLE operation, and must be weighed carefully because it
 * also changes how the device is programmed. We do not do it: here the window is
 * merely shortened. */
#define MUSB_DEFER_MS   2000

static void midi_usb_deferred_start(int64_t now)
{
    static bool done = false;
    if (done || now < (int64_t)MUSB_DEFER_MS * 1000) return;
    done = true;

/* The name lives here and not in the settings because the USB descriptors are
 * read once at enumeration: changing it at runtime would mean unplugging and
 * replugging the cable. */
    midi_usb_cfg_t uc = {
/* Aligned with the rest of the firmware: the name is the same everywhere — log
 * tag, NVS namespace, DJ Link name, SoftAP SSID. */
        .product      = "Aggeggino",
        .manufacturer = "KTEK Labs",
        .port_name    = NULL,      /* -> "Aggeggino clock" */
        .serial       = NULL,      /* -> derived from the MAC */
        .task_core    = 1,
        .task_prio    = 0,         /* the component's default: 5   */
        .task_stack   = 0,         /* default: 4096, to be tuned from the logs */
    };
    ESP_LOGI(TAG, "[usb] start deferred to %lld ms after boot", now / 1000);
    midi_usb_start(&uc);
}
#endif /* DJB_MIDI_USB */

/* The clock engine's sink. It does TWO things, and the measurement stays even
 * when a transport is off: it is what validated the 24 PPQN grid, and is not
 * to be lost by adding a real output. */
static void midi_sink(uint8_t status, int64_t due_us, void *ctx)
{
    (void)ctx;
    int32_t lat = (int32_t)(esp_timer_get_time() - due_us);
    if (lat > s_midi_lat_max) s_midi_lat_max = lat;

    switch (status) {
    case MIDI_CLOCK_TICK:  s_midi_tick++;  break;
    case MIDI_CLOCK_START: s_midi_start++; break;
    case MIDI_CLOCK_STOP:  s_midi_stop++;  break;
    default: break;
    }

#if DJB_MIDI_DIN
    /* A single byte: System Real Time messages carry no data and do not
     * interleave. Before the network path, because this one has the least
     * latency — at equal instants it is better off leaving first. */
    if (s_din_up) {
        uint8_t b = status;   /* 0xF8 clock, 0xFA start, 0xFC stop */
        uart_write_bytes(MDIN_UART, &b, 1);
        s_din_bytes++;
    }
    /* AFTER the clock byte: the CV note must never delay it. */
    sam_cv_tick(status, due_us);
#endif
#if DJB_MIDI_USB
    /* BEFORE the network branch, which returns early when there is no host.
     * Putting USB after it would mean never transmitting with the network off. */
    if (s_usb_out_on) {
        if (midi_usb_send_rt(status)) s_usb_bytes++;
        else                          s_usb_drop++;
    }
#endif
#if DJB_MIDI_NET
    if (!s_mnet) return;
    uint32_t w[1];
    size_t n = midi2_ump_system_rt(w, MNET_GROUP, status);
    if (!n) return;
    esp_err_t e = midi2_net_host_send_ump(s_mnet, w, n);
    if (e == ESP_OK)                        s_mnet_sent++;
    else if (e != ESP_ERR_INVALID_STATE)    s_mnet_full++;
        /* ESP_ERR_INVALID_STATE means no session is open. Not an error: it is
         * the normal case until somebody invites us, and must not be counted. */
#endif
}

/* The MIDI transport follows the ACTIVE bridge, with the same source choice
 * as clock_next_beat(): taking the phase from one direction and the running
 * state from the other would make no sense. */
static bool midi_transport_playing(int64_t now)
{
    if (ui_clock_src() == CLOCK_SRC_ABLETON) return linkbridge_is_playing();
    if (ui_clock_src() == CLOCK_SRC_DJLINK)    return transport_any_playing(now);
    return false;
}

/* Which transport state to hand the MIDI engine, passed through the TWO FILTERS
 * in Settings -> Bridge.
 *
 * "Start Sync" and "Stop Sync" are relative to the bridge's direction: in the
 * forward one they govern the fader start towards the players, in the reverse
 * one the play/stop towards Ableton. MIDI is a third output and must obey the
 * same two switches, or one would get the absurdity of a DAW receiving START
 * while the players have not been started.
 *
 * THE FILTER ACTS ON TRANSITIONS, not on the state: the state handed to the
 * engine changes only through a permitted passage. An intended behaviour
 * follows — with "Stop Sync" off the clock keeps running when the source stops,
 * exactly as the bridge does not propagate the stop.
 *
 * THE BRIDGE BEING OFF IS A SEPARATE CASE and does not pass through the filter:
 * it is not a transport "stop", it is the absence of a source to follow.
 * Filtering it would leave the engine declaring itself running with no clock
 * left to take its phase from. */
static bool midi_transport_wanted(int64_t now)
{
    static bool cur = false;      /* the state already handed to the engine */

    if (ui_clock_src() == CLOCK_SRC_NONE) {
        if (cur) ESP_LOGI(TAG, "[midi] transport STOP (bridge off)");
        cur = false;
        return false;
    }

    bool playing = midi_transport_playing(now);
    if (playing != cur) {
        bool allowed = playing ? settings_get_start_sync()
                               : settings_get_stop_sync();
        /* The blocking message is emitted once per SITUATION, not once and for
         * all: it remembers which transition it was blocking, and speaks again
         * if that changes or if something got through meanwhile. With a plain
         * "already said" flag, re-enabling the filter and retrying would have
         * produced silence. */
        static bool blk_attivo = false, blk_dir = false;
        if (allowed) {
            cur = playing;
            blk_attivo = false;
            ESP_LOGI(TAG, "[midi] transport %s", cur ? "START" : "STOP");
        } else {
            if (!blk_attivo || blk_dir != playing) {
                ESP_LOGI(TAG, "[midi] %s not sent: %s filter off",
                         playing ? "START" : "STOP",
                         playing ? "Start Sync" : "Stop Sync");
                blk_attivo = true;
                blk_dir    = playing;
            }
            return cur;
        }
    }
    return cur;
}

/* MIDI phase offset, adjusted from the dashboard's cyclic selector and kept in
 * NVS per clock source. The engine re-reads it at every beat, so an adjustment
 * is heard at the next one. */
static int midi_offset_us(void) { return ui_get_phase_offset_midi_us(); }

#endif /* DJB_MIDI_CLOCK */

/* ===========================================================================
 * UNIFIED PHASE SOURCE  (for the auxiliary outputs: OSC, then MIDI)
 *
 * The bridge has two phase references, depending on its direction:
 *
 *   forward (DJ Link -> Ableton): the anchor s_db_us, derived from the players'
 *                                 beat packets and kept by the phase filter
 *   reverse (Ableton -> DJ Link): Ableton Link's own timeline
 *
 * The auxiliary outputs must follow the same phase as the active bridge, but
 * with an emission instant OF THEIR OWN — they each have their own offset. This
 * function says where the next beat falls according to the source in use,
 * without the caller needing to know which one that is.
 *
 * It returns false when there is no reliable phase: no active bridge, an anchor
 * not yet locked, an implausible tempo. Callers must then emit nothing — never
 * invent an instant.
 * =========================================================================== */
bool clock_next_beat(int64_t now, int64_t *t_beat, uint8_t *bb, double *bpm_out)
{
    double bpm;
    int64_t tb;
    int nb;

    if (ui_clock_src() == CLOCK_SRC_ABLETON) {
        /* Ableton Link source: the phase comes from the session's timeline. */
        bpm = linkbridge_tempo();
        if (bpm < 20.0 || bpm > 300.0) return false;
        double beat_us = 60000000.0 / bpm;
        double ph   = linkbridge_phase_at(now, LINK_QUANTUM);   /* [0,4) */
        double frac = ph - floor(ph);
        tb = now + (int64_t)((1.0 - frac) * beat_us);
        /* Same convention as beat_schedule_next: LINK_QUANTUM is a double
         * (4.0), so the modulo needs the cast. The 0..3 result becomes 1..4
         * with the final +1, as in the DJ Link beat packet. */
        nb = ((int)floor(ph) + 1) % (int)LINK_QUANTUM + 1;      /* 1..4 */
    } else if (ui_clock_src() == CLOCK_SRC_DJLINK) {
        /* DJ Link source: the phase anchor, valid only while we are receiving
         * beats from a rekordbox master. */
        if (!s_db_valid || s_master_mm == 2) return false;
        bpm = st.dj_bpm;
        if (bpm < 20.0 || bpm > 300.0) return false;
        double beat_us = 60000000.0 / bpm;
        /* how many whole beats separate the last estimated downbeat from the next */
        double since  = (double)(now - s_db_us);
        double nbeats = floor(since / beat_us) + 1.0;
        tb = s_db_us + (int64_t)(nbeats * beat_us);
        /* fmod takes doubles, so LINK_QUANTUM needs no cast here */
        double inbar = fmod(nbeats, LINK_QUANTUM);
        if (inbar < 0) inbar += LINK_QUANTUM;
        nb = (int)inbar + 1;                                    /* 1..4 */
    } else {
        return false;                       /* no active source */
    }

    if (t_beat)  *t_beat  = tb;
    if (bb)      *bb      = (uint8_t)nb;
    if (bpm_out) *bpm_out = bpm;
    return true;
}

/* ===========================================================================
 * OSC OUTPUT - timing engine
 *
 * It lives on CORE 1, where only the UI (p2) and the audio (p3) are: core 0
 * intact, with main, w5rx and beat_tx and the balance between them, which cost
 * too much work to risk.
 *
 * It does not go through the W5500 but through the lwIP stack on WiFi, so it
 * does not touch the SPI bus and has nothing to do with the beat's quiet
 * window.
 *
 * A TIMER OF ITS OWN, not shared with the DJ Link beat: OSC's phase offset is
 * independent, so its emission instant is a different one. Sharing the timer
 * would have tied the two paths together exactly where they need to stay
 * apart.
 * =========================================================================== */
/* PRIORITIES ON CORE 1.
 *
 * The criterion: at equal core, whoever has more EVENTS and less TOLERANCE sits
 * higher.
 *
 *   midi_clk   24 pulses per beat, tolerance about 1 ms  -> 10
 *   osc_tx      1 packet per beat, wide tolerance        ->  9
 *
 * The cost of the ordering is asymmetric: the MIDI sink does microsecond work —
 * one FIFO write, one enqueue — so preempting OSC costs it almost nothing; the
 * other way round costs milliseconds, because a sendto on lwIP can block while
 * the WiFi stack is busy.
 *
 * [M] THE EVIDENCE: with OSC switched off in the settings, the maximum [midi]
 * wake-up fell from about 6200 us to about 4600, with a median of 2600. Not a
 * proof — the two sessions differed in other ways too — but the first datum in
 * favour, and in the predicted direction.
 *
 * [U] STILL TO MEASURE: [midi] wake-up with OSC ON. If it falls towards the
 * OSC-off values the ordering has done its job; if [osc] wake-up worsens
 * appreciably, the price is higher than expected and this needs revisiting. */
#define OSC_TASK_PRIO    9       /* below midi_clk, see above               */
#define OSC_TASK_STACK  3072
#define OSC_LEAD_US     100      /* lead: as for the DJ Link beat            */

static TaskHandle_t     s_osc_task  = NULL;
static esp_timer_handle_t s_osc_timer = NULL;
static volatile int64_t s_osc_target = 0;   /* the instant it is armed for  */
/* The timer rearms even when the phase is NOT available, so as to retry shortly.
 * Those wake-ups are not beats: they must not be counted and, above all, must
 * not be measured — s_osc_target was not updated in that case and the comparison
 * would give meaningless numbers. At start-up it is zero, so the "jitter" came
 * out equal to the time since boot. */
static volatile bool    s_osc_armed_real = false;
static volatile uint8_t s_osc_bb     = 1;   /* beat within the bar          */
static volatile double  s_osc_bpm    = 0.0;

/* diagnostics: how punctual the wake-up is */
static volatile uint32_t s_osc_sent   = 0;
static volatile int32_t  s_osc_jit_max = 0, s_osc_jit_min = 0;

static void osc_schedule_next(int64_t now);   /* fwd */

/* State of the strip's OSC badge. There is no matching LED, so there is no twin
 * to keep aligned here: this is the only place it is decided.
 *
 * THE GREEN IS TIED TO THE BRIDGE, not to packets actually sent. A deliberate
 * choice: tying it to real output left the badge grey during the seconds when
 * the phase anchor has not yet locked, and a user who has just switched the
 * bridge on reads that grey as a fault. The badge answers "will OSC receive
 * data?", not "did a packet go out in the last three seconds?".
 *
 * The RED was not in the three-state specification, but it stays: every other
 * badge on the strip has a red meaning "faulty", and an OSC switched on with a
 * dead socket or an invalid address is exactly that case — and the only one the
 * user can correct from the menu. */
/* State of the MIDI badge. The same scheme as OSC, but applied to TWO
 * transports: there is a single badge because the strip says whether MIDI is
 * going out, not from which port.
 *
 * The FAULT case is the one worth having: a transport REQUESTED in the settings
 * that did not start. Without it, a network host that fails or a UART that will
 * not open would look identical to MIDI simply switched off — and the
 * difference between "I did not turn it on" and "I turned it on and it does not
 * work" is exactly what one wants to see at a glance. */
static ui_midi_t midi_badge_state(void)
{
    bool want_net = settings_get_midi_net_enabled();
    bool up_net   = (s_mnet != NULL);
#if DJB_MIDI_DIN
    bool want_din = settings_get_midi_din_enabled();
    bool up_din   = s_din_up;
#else
    bool want_din = false, up_din = false;
#endif
#if DJB_MIDI_USB
    bool want_usb = settings_get_midi_usb_enabled();
    bool up_usb   = s_usb_out_on;
#else
    bool want_usb = false, up_usb = false;
#endif
    if (!want_net && !want_din && !want_usb)           return UI_MIDI_OFF;
    if ((want_net && !up_net) || (want_din && !up_din) ||
        (want_usb && !up_usb))                         return UI_MIDI_FAULT;
    if (ui_clock_src() == CLOCK_SRC_NONE)   return UI_MIDI_IDLE;
    return UI_MIDI_ON;
}

#if DJB_DEVELOPER
/* A screenshot was asked for. The write takes the SPI bus for a moment — 150 KB
 * at a row a time — and is done here rather than in the touch handler for the
 * same reason a format is: the handler should return. */
static void save_screenshot(void)
{
    sdcard_save_screenshot();
}
#endif


/* State of the SD badge. A direct translation of what the component reports:
 * the two enums say the same things and the mapping keeps the UI free of any
 * dependency on the card. */
static ui_sd_t sd_badge_state(void)
{
    switch (sdcard_state()) {
    case SDCARD_ERROR:   return UI_SD_ERROR;
    case SDCARD_IDLE:    return UI_SD_IDLE;
    case SDCARD_WRITING: return UI_SD_WRITING;
    default:             return UI_SD_ABSENT;
    }
}


/* State of the CV badge.
 *
 * FAULT is now rare: the pulse leaves from the GPIO and does not depend on
 * external hardware, so the only way to ask for it without having it is if the
 * pin could not be configured. The note on the SAM, which does need the DIN
 * module, is an extra, and its absence is NOT a fault. */
static ui_cv_t cv_badge_state(void)
{
#if DJB_MIDI_DIN
    if (!settings_get_cv_enabled())                    return UI_CV_OFF;
    if (!s_cv_on)                                      return UI_CV_FAULT;
    if (ui_clock_src() == CLOCK_SRC_NONE)   return UI_CV_IDLE;
    return UI_CV_ON;
#else
    return UI_CV_OFF;
#endif
}

static ui_osc_t osc_badge_state(void)
{
    if (!settings_get_osc_enabled())                   return UI_OSC_OFF;
    if (!osc_out_socket_ok() || !osc_out_dest_ok())    return UI_OSC_FAULT;
    if (ui_clock_src() == CLOCK_SRC_NONE)   return UI_OSC_IDLE;
    return UI_OSC_ON;
}

/* ===========================================================================
 * THE OSC CUSTOM PROFILE: between NVS and the output
 *
 * main is the one place that sees both sides — settings keeps opaque bytes,
 * osc_out knows the shape — so the translation lives here, as it does for every
 * other setting that crosses two components.
 *
 * THE LENGTH IS THE VERSION CHECK. There is no version byte: a blob whose size
 * is not an exact number of messages comes from a firmware whose layout
 * differed, and is discarded. Reading it anyway would mean paths and values
 * sliced at the wrong offsets — strings that look like text and are not, sent
 * to somebody's lighting desk.
 * ==========================================================================*/
static osc_custom_msg_t s_osc_cust[OSC_CUSTOM_MAX];
static int              s_osc_cust_n = 0;

static void osc_custom_load(void)
{
    size_t n = settings_get_osc_custom(s_osc_cust, sizeof(s_osc_cust));
    if (n == 0 || (n % sizeof(s_osc_cust[0])) != 0) {
        if (n) ESP_LOGW(TAG, "[osc] custom profile of %u bytes: not a whole "
                              "number of messages, discarding it", (unsigned)n);
        s_osc_cust_n = 0;
        memset(s_osc_cust, 0, sizeof(s_osc_cust));
    } else {
        s_osc_cust_n = (int)(n / sizeof(s_osc_cust[0]));
        ESP_LOGI(TAG, "[osc] custom profile: %d messages read", s_osc_cust_n);
    }
    osc_out_set_custom(s_osc_cust, s_osc_cust_n);
}

/* Saves and applies in one go: the two must never diverge, or the output would
 * carry something other than what the menu shows. */
static void osc_custom_save(void)
{
    settings_set_osc_custom(s_osc_cust,
                            (size_t)s_osc_cust_n * sizeof(s_osc_cust[0]));
    osc_out_set_custom(s_osc_cust, s_osc_cust_n);
}


/* Carries the user's settings to the OSC module. Called on every slow turn of
 * the loop: the settings can change from the menu at any moment, and re-reading
 * them costs one cached NVS read. */
static void osc_apply_settings(void)
{
    static uint8_t  p_en = 0xff, p_preset = 0xff;
    static uint16_t p_port = 0;
    static char     p_host[20] = { 0 };

    char host[20];
    settings_get_osc_host(host, sizeof(host));
    uint16_t port   = settings_get_osc_port();
    uint8_t  preset = settings_get_osc_preset();
    uint8_t  en     = settings_get_osc_enabled() ? 1 : 0;

    if (en == p_en && preset == p_preset && port == p_port &&
        strncmp(host, p_host, sizeof(p_host)) == 0)
        return;                                   /* nothing changed */

    /* The socket is managed by configure() itself, according to 'enabled':
     * opening and closing it from here produced an asymmetry — it closed and
     * never reopened. */
    osc_out_configure(host, port, (osc_out_preset_t)preset, en != 0);
    p_en = en; p_preset = preset; p_port = port;
    snprintf(p_host, sizeof(p_host), "%s", host);
}

static void osc_timer_cb(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_osc_task) vTaskNotifyGiveFromISR(s_osc_task, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void osc_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_osc_armed_real) {
            int32_t jit = (int32_t)(esp_timer_get_time() - s_osc_target);
            if (jit > s_osc_jit_max) s_osc_jit_max = jit;
            if (jit < s_osc_jit_min) s_osc_jit_min = jit;
            s_osc_sent++;

            osc_out_send_beat(s_osc_bpm, s_osc_bb);
        }

        osc_schedule_next(esp_timer_get_time());
    }
}

/* Arms the timer on the active source's next beat. If the phase is unavailable
 * we retry shortly rather than stopping: the bridge may lock at any moment. */
static void osc_schedule_next(int64_t now)
{
    if (!s_osc_timer) return;
    esp_timer_stop(s_osc_timer);

    int64_t t_beat; uint8_t bb; double bpm;
    if (!clock_next_beat(now, &t_beat, &bb, &bpm)) {
        s_osc_armed_real = false;                    /* not a beat */
        esp_timer_start_once(s_osc_timer, 100000);   /* retry in 100 ms */
        return;
    }

    int64_t t_send = t_beat + (int64_t)ui_get_phase_offset_osc_us() - OSC_LEAD_US;
    int64_t dt = t_send - now;
    /* If the instant has already passed — a strongly negative offset, or a late
     * wake-up — we skip to the next beat rather than firing at once: better to
     * lose a beat than to emit one out of place. */
    while (dt < 1000) { t_send += (int64_t)(60000000.0 / bpm); dt = t_send - now; }

    s_osc_target     = t_send;
    s_osc_bb         = bb;
    s_osc_bpm        = bpm;
    s_osc_armed_real = true;
    esp_timer_start_once(s_osc_timer, (uint64_t)dt);
}

/* The tempo with which the currently armed beat was computed. */
static double s_beat_sched_bpm = 0.0;

/* Schedules the next beat: computes T_send from Link's timeline, arms the timer
 * and opens the quiet window ANCHORED to that same T_send. */
static void beat_schedule_next(int64_t now)
{
    double bpm = linkbridge_tempo();
    if (bpm < 1.0) return;
    double beat_us = 60000000.0 / bpm;

    /* Current phase within the bar (quantum 4): how far to the next beat. */
    double ph      = linkbridge_phase_at(now, LINK_QUANTUM);   /* [0,4) beat */
    double frac    = ph - floor(ph);                            /* [0,1) of the beat */
    int64_t to_next = (int64_t)((1.0 - frac) * beat_us);
    int64_t t_beat  = now + to_next;                            /* the beat on Link */

    /* THE INSTANT OF TRUTH */
    int64_t t_send = t_beat + (int64_t)ui_get_phase_offset_abl_us() - BEAT_TX_LEAD_US;
    int64_t delay  = t_send - esp_timer_get_time();

    /* Too close to arm safely? Skip to the next beat. */
    if (delay < BEAT_QUIET_US) {
        t_send += (int64_t)beat_us;
        t_beat += (int64_t)beat_us;
        delay  += (int64_t)beat_us;
        ph     += 1.0;
    }

    /* beat-in-bar of the beat we are about to send */
    int nb = ((int)floor(ph) + 1) % (int)LINK_QUANTUM;          /* 0..3 */
    s_beat_bb  = (uint32_t)(nb + 1);                            /* 1..4 */
    s_beat_bpm = bpm;

    /* Beat counter aligned to the bar: it must satisfy
     *     ((s_beat_ctr - 1) % 4) + 1 == s_beat_bb
     * or the player sees a status that contradicts itself — saying "beat 2 of
     * the track" but "3rd beat of the bar". We advance until the congruence
     * holds again. */
    uint32_t want = s_beat_bb % 4;                  /* 1,2,3,0 */
    s_beat_ctr++;
    for (int guard = 0; (s_beat_ctr % 4) != want && guard < 4; guard++) s_beat_ctr++;

    s_beat_armed_us  = t_send;
    s_beat_sched_bpm = bpm;       /* the tempo t_send was computed with */
    s_chase_our_beat_us = t_beat;   /* reference for the players' offset  */
#if DJB_DEVELOPER
    s_beat_target = t_send;
#endif
    bsp_spi_set_quiet_window(t_send - BEAT_QUIET_US, t_send + BEAT_QUIET_TAIL_US);
    esp_timer_start_once(s_beat_timer, (uint64_t)delay);
}

static void beat_engine_tick(int64_t now)
{
    if (s_net_role != NET_MASTER || !s_want_master) {
        if (s_beat_armed_us) {                 /* leaving master: disarm */
            esp_timer_stop(s_beat_timer);
            s_beat_armed_us = 0;
            bsp_spi_set_quiet_window(0, 0);
        }
        return;
    }
    /* A beat already armed carries the instant computed with the tempo of THAT
     * moment. If Link has changed meanwhile, that instant is no longer the real
     * beat: the timeline has changed pace, and the beat falls early or late.
     * [M] On a 10 BPM jump the error reaches 38 ms, injected exactly when the
     * players are deciding how to realign — a phase step of our own making.
     *
     * So it is rescheduled. The IMMINENT beat is NOT touched: inside the quiet
     * window the timer is about to fire and the bus is already reserved, so
     * disarming it would introduce a risk worse than the millisecond gained.
     * That one is let go, and the correction starts from the next. */
    if (s_beat_armed_us) {
        double bpm_now = linkbridge_tempo();
        if (bpm_now > 1.0 && fabs(bpm_now - s_beat_sched_bpm) > TEMPO_EPS &&
            (s_beat_armed_us - now) > BEAT_QUIET_US) {
            esp_timer_stop(s_beat_timer);
            s_beat_armed_us = 0;
            bsp_spi_set_quiet_window(0, 0);   /* rescheduling will reopen it */
        }
    }

    /* One beat in flight at a time: the next is scheduled when the previous has
     * gone — the task clears s_beat_armed_us — or when we disarmed it above. */
    if (s_beat_armed_us == 0) beat_schedule_next(now);
}

/* --- Handing over the master role, from HOLDING towards MASTER ------------- */
static int64_t s_takeover_next = 0;

/* LOSING THE MASTER ROLE WITHOUT NEGOTIATION.
 *
 * When the DJ sets the master on a player, that player ASKS us for it: we yield
 * (s_yield_to), it takes the role, and we withdraw, clearing the source
 * off. That path works and is the intended behaviour: if the DJ wants the master
 * on a player, the bridge steps aside.
 *
 * But there is a case that path does not cover: a device declaring itself master
 * WITHOUT going through the negotiation. [M] Observed in the field — after two
 * minutes as master a player took the role, s_yield_to was zero and so nobody
 * reacted: we went on declaring ourselves master and transmitting beats for two
 * and a half minutes, while the players followed it.
 *
 * The symptom was an offset growing from 10 to 15 ms: not our crystal's drift,
 * but two disconnected timelines diverging freely.
 *
 * The remedy does exactly what the negotiated handover does: it takes note,
 * clears the source and returns to being an observer. We do NOT
 * try to take the role back: if somebody has taken it, they have a reason — and
 * a bridge that grabs it back on its own would be a tug of war, not a solution.
 *
 * The hysteresis is needed because during a negotiation there are transients in
 * which two devices declare themselves master for a few packets. */
#define MASTER_LOST_CONFIRM_US 3000000LL
static int64_t s_master_lost_since = 0;

static void master_watch(int64_t now)
{
    if (s_net_role != NET_MASTER) { s_master_lost_since = 0; return; }
    if (s_yield_to) { s_master_lost_since = 0; return; }  /* a handover is under way */

    uint8_t cur = master_current(now);
    if (cur == 0 || cur == s_ka_device) {      /* nobody, or us: all is well */
        s_master_lost_since = 0;
        return;
    }
    if (!s_master_lost_since) { s_master_lost_since = now; return; }
    if (now - s_master_lost_since < MASTER_LOST_CONFIRM_US) return;

    s_master_lost_since = 0;
    ESP_LOGI(TAG, "[master] device %d took the master without negotiating: "
                  "withdrawing and clearing the source", cur);
    s_sync_n = s_sync_n_max + 1;               /* as in the negotiated handover */
    ui_set_clock_src(CLOCK_SRC_NONE);
    net_set_role(NET_OBSERVER, now);
}

static void master_tick(int64_t now)
{
    master_watch(now);                          /* first: are we still master? */
    if (s_net_role != NET_HOLDING || !s_want_master) return;

    /* TIMEOUT: if after 10 s we have not obtained the master role, we give up
     * and return to the neutral state, leaving nothing hanging. */
    if (s_negotiation_start && (now - s_negotiation_start) > MASTER_NEGOTIATION_TIMEOUT_US) {
        ESP_LOGI(TAG, "[master] TIMEOUT: no master obtained in %ds, giving up",
                 (int)(MASTER_NEGOTIATION_TIMEOUT_US / 1000000));
        s_master_failed_us  = now;              /* blinking red badge */
        s_negotiation_start = 0;
        ui_set_clock_src(CLOCK_SRC_NONE);          /* the source is cleared */
        net_set_role(NET_OBSERVER, now);        /* back to observer: the NUMBER
                                                 * stays ours, it is our identity */
        return;
    }

    uint8_t cur = master_current(now);
    if (cur == 0) {                                  /* no master: assert directly */
        ESP_LOGI(TAG, "[master] no master on the network: asserting directly");
        net_set_role(NET_MASTER, now);
        return;
    }
    if (cur == s_ka_device) { net_set_role(NET_MASTER, now); return; }

    if (s_master_mh == s_ka_device) {                /* the master is handing it to us */
        ESP_LOGI(TAG, "[master] device %d yields to us (Mh=%d): ASSERTING", cur, s_ka_device);
        net_set_role(NET_MASTER, now);
        return;
    }

    /* otherwise: a takeover request to the current master, about once a second */
    if (now >= s_takeover_next) {
        s_takeover_next = now + 1000000;
        uint8_t buf[DJLINK_TAKEOVER_LEN];
        djlink_build_takeover(buf, "aggeggino", s_ka_device);
        w5500_net_sendto(UDP_SN_BEAT, buf, DJLINK_TAKEOVER_LEN, s_dev_ip[cur], DJLINK_PORT_BEAT);
        ESP_LOGI(TAG, "[master] takeover request -> device %d (want to be %d, current Mh=%d)",
                 cur, s_ka_device, s_master_mh);
    }
}

/* Sends the sequence hello -> claim1 -> claim2 -> claim3 (4x3, 300 ms). Called
 * from the loop. */
static void claiming_tick(int64_t now)
{
    if (s_net_role != NET_CLAIMING || now < s_claim_next_us) return;

    if (s_claim_step >= 12) {                 /* completed with no defence */
        ESP_LOGI(TAG, "[claim] slot %d acquired (held with the keep-alive)", s_claim_num);
        /* The number is acquired: it is our identity, and from here on we keep
         * it. If we wanted the master role we go for it; otherwise we stay
         * observers WITH our own number, and the metadata keep working. */
        s_identity_real = true;
        s_ident_ko      = false;
        ui_set_ableton_src_available(true);
        ESP_LOGI(TAG, "[ident] I am device %d (real identity)", s_claim_num);
        net_set_role(s_want_master ? NET_HOLDING : NET_OBSERVER, now);
        return;
    }

    uint8_t buf[64], mac[6], ip[4];
    w5500_net_get_mac(mac); w5500_net_get_ip(ip);
    int stage = s_claim_step / 3;
    uint8_t ctr = (uint8_t)(s_claim_step % 3) + 1;
    size_t len = 0;
    const char *nm = "aggeggino";
    switch (stage) {
        case 0: len = djlink_build_hello (buf, nm); break;
        case 1: len = djlink_build_claim1(buf, mac, nm, ctr); break;
        case 2: len = djlink_build_claim2(buf, ip, mac, nm, s_claim_num, ctr, 1); break;
        case 3: len = djlink_build_claim3(buf, nm, s_claim_num, ctr); break;
    }
    w5500_net_sendto(UDP_SN_KEEPALIVE, buf, len, DJB_BCAST, DJLINK_PORT_KEEPALIVE);
    s_claim_step++;
    s_claim_next_us = now + 300000;
}

static void net_role_tick(int64_t now)
{
    static bool prev_alive = false;
    s_want_master = ui_clock_src() == CLOCK_SRC_ABLETON;        /* the user's intent, right toggle */
    bool online    = (st.ver == 0x04) && st.link_up;
    bool net_alive = (now - s_last_djlink_us) < NET_FRESH_US;

    if (!online) {
        net_set_role(NET_OFFLINE, now);          /* fault: back offline */
    } else if (s_net_role == NET_OFFLINE) {
        net_set_role(NET_CENSUS, now);           /* the link came back up */
    } else {
        /* Traffic returning after a silence — a switch, the players — so we
         * take the census again. */
        if (net_alive && !prev_alive && s_net_role != NET_CENSUS)
            net_set_role(NET_CENSUS, now);

        /* The census is over: we ALWAYS take a real player number, even when we
         * do not want to be master. It is our identity on the network, and the
         * metadata need it. */
        if (s_net_role == NET_CENSUS && (now - s_census_start) >= CENSUS_WATCH_US)
            try_claim(now);

        /* The user's intent has changed. We already hold the number: becoming
         * master now needs only the election, no claim. */
        if (s_want_master && s_net_role == NET_OBSERVER) {
            if (s_identity_real) net_set_role(NET_HOLDING, now);   /* -> election */
            else ui_set_clock_src(CLOCK_SRC_NONE);   /* impossible: no identity */
        }
        /* The user clears the Ableton source. */
        if (!s_want_master && s_net_role == NET_HOLDING)
            net_set_role(NET_OBSERVER, now);      /* not master yet: clear to go */

        if (!s_want_master && s_net_role == NET_MASTER && !s_yield_to)
            master_handover(now);                 /* we were master: HAND OVER, do not vanish */

        /* The handover does not complete — the recipient never takes the master
         * role? After a few seconds we let go anyway: our status packets will
         * say we are not master, and the players will reorganise by themselves.
         * Better a network with no master than one with a master that is not
         * driving. */
        if (s_yield_to && !s_want_master &&
            (now - s_yield_since) > YIELD_TIMEOUT_US) {
            ESP_LOGI(TAG, "[master] device %d did not take the master: "
                          "letting go anyway", s_yield_to);
            s_yield_to = 0;
            net_set_role(NET_OBSERVER, now);
        }
    }
    prev_alive = net_alive;
}

static void update_leds(int64_t now)
{
    bool on = ((now / 500000) % 2) == 0;    /* shared 1 Hz blink */

    /* --- the DJ Link side, on the left --- */
    uint8_t dr = 0, dg = 0, db = 0;
    switch (djlink_led_state(now)) {
        case DJL_CHIP_KO:    dr = LED_LEVEL; break;
        case DJL_NO_LINK:    if (on) dr = LED_LEVEL; break;
        case DJL_NO_PIONEER: if (on) db = LED_LEVEL; break;
        case DJL_IDLE:       db = LED_LEVEL; break;
        case DJL_PLAYING:    dg = LED_LEVEL; break;
        case DJL_IDENT_KO: {
            /* WHITE at 2.5 Hz: a colour and a rhythm used for nothing else.
             * 200 ms is an EXACT MULTIPLE of the sampling — update_leds runs at
             * 10 Hz — giving a clean 50/50 duty. With 150 ms it would have
             * aliased into a limping blink: 200 ms on, 100 ms off.
             * The computation lives INSIDE here: in the normal state it costs
             * nothing. */
            bool fast = ((now / 200000) % 2) == 0;
            if (fast) { dr = dg = db = LED_LEVEL; }
            break;
        }
    }

    /* --- Ableton Link side (right), same palette --- */
    uint8_t ar = 0, ag = 0, ab = 0;
    switch (ableton_led_state()) {
        case ABL_AP_KO:         ar = LED_LEVEL; break;
        case ABL_AUTH_KO:       if (on) ar = LED_LEVEL; break;   /* blinking red */
        case ABL_NO_CLIENT:     if (on) ab = LED_LEVEL; break;
        case ABL_CLIENT_NOPEER: ab = LED_LEVEL; break;
        case ABL_PEER:          ag = LED_LEVEL; break;
    }

    /* Change detection on BOTH sides: retransmit only if something changed.
     * It avoids pointless WS2812 rewrites — interrupt glitches and wasted
     * cycles. */
    static uint8_t pdr = 1, pdg = 1, pdb = 1, par = 1, pag = 1, pab = 1;
    if (dr == pdr && dg == pdg && db == pdb && ar == par && ag == pag && ab == pab) return;
    pdr = dr; pdg = dg; pdb = db; par = ar; pag = ag; pab = ab;

    for (int i = 0; i < leds_count(); i++) {
        if (i >= DJLINK_LED_FIRST && i < DJLINK_LED_FIRST + DJLINK_LED_COUNT)
            leds_set(i, dr, dg, db);       /* the DJ Link side */
        else
            leds_set(i, ar, ag, ab);       /* the Ableton side */
    }
    leds_show();
}

#if DJB_DEVELOPER
static TaskStatus_t s_task_arr[24];

/* Per-core occupancy over the given interval, derived from the IDLE tasks' time.
 * The run-time counter is in microseconds (esp_timer), so idle_delta divided by
 * the interval gives the idle share. */
#if DJB_DEVELOPER
static void cpu_usage_log(int64_t interval_us)
{
    static uint32_t prev_idle[2] = {0, 0};
    static bool first = true;

    uint32_t total;
    UBaseType_t n = uxTaskGetSystemState(s_task_arr,
                                         sizeof(s_task_arr) / sizeof(s_task_arr[0]),
                                         &total);
    uint32_t idle[2] = {0, 0};
    for (UBaseType_t i = 0; i < n; i++) {
        if      (strcmp(s_task_arr[i].pcTaskName, "IDLE0") == 0) idle[0] = s_task_arr[i].ulRunTimeCounter;
        else if (strcmp(s_task_arr[i].pcTaskName, "IDLE1") == 0) idle[1] = s_task_arr[i].ulRunTimeCounter;
    }
    if (first) { prev_idle[0] = idle[0]; prev_idle[1] = idle[1]; first = false; return; }

    uint32_t d0 = idle[0] - prev_idle[0];   /* uint32 arithmetic: wrap-safe */
    uint32_t d1 = idle[1] - prev_idle[1];
    prev_idle[0] = idle[0]; prev_idle[1] = idle[1];

    int use0 = 100 - (int)(((uint64_t)d0 * 100) / (uint64_t)interval_us);
    int use1 = 100 - (int)(((uint64_t)d1 * 100) / (uint64_t)interval_us);
    if (use0 < 0)   use0 = 0;
    if (use0 > 100) use0 = 100;
    if (use1 < 0)   use1 = 0;
    if (use1 > 100) use1 = 100;
    /* The OSC engine's punctuality: the same measurement we make on the DJ Link
     * beat, except that this path does not go through the SPI bus. */
    if (s_osc_sent) {
        uint32_t tx = 0, er = 0;
        osc_out_stats(&tx, &er);
        ESP_LOGI(TAG, "[osc] beat=%u pkt=%u err=%u | wake-up: %+d..%+dus",
                 (unsigned)s_osc_sent, (unsigned)tx, (unsigned)er,
                 (int)s_osc_jit_min, (int)s_osc_jit_max);
        s_osc_jit_max = 0; s_osc_jit_min = 0;
    }
    ESP_LOGI(TAG, "[cpu] core0=%d%% core1=%d%%  (real-time / UI)", use0, use1);

    /* Receiving the status packets: 'read' is what socket :50002 delivered
     * BEFORE any filtering, which separates a reception problem from a parsing
     * one. 'deferred' counts the drains skipped by the quiet window: if it grows
     * much, the window is starving the polling. */
    {
        char line[160]; line[0] = '\0';
        size_t off = 0;
        for (int d = TRANSPORT_DEV_MIN; d <= TRANSPORT_DEV_MAX && d < DEVNUM_MAX; d++) {
            if (d != s_ka_device && (s_st_rx_n[d] || s_st_rx_last[d]) &&
                off < sizeof(line) - 1) {
                int w = snprintf(line + off, sizeof(line) - off, " | %d:n=%u gap=%ums",
                                 d, (unsigned)s_st_rx_n[d], (unsigned)s_st_gap_ms[d]);
                /* snprintf returns what it WOULD have written: without the
                 * ceiling, off would exceed the buffer and (sizeof - off) would
                 * underflow. */
                if (w > 0) off += (size_t)w;
                if (off >= sizeof(line)) off = sizeof(line) - 1;
            }
            /* the reset happens regardless, even if the line was full */
            s_st_rx_n[d] = 0; s_st_gap_ms[d] = 0;
        }
        ESP_LOGI(TAG, "[rx50002] read=%u deferred=%u%s",
                 (unsigned)s_st_drained, (unsigned)s_st_quiet_skip, line);
        s_st_drained = 0; s_st_quiet_skip = 0;
    }

    /* OUR keep-alive. 'max gap' is the number that matters: it must stay around
     * 2000 ms. If it climbs towards 4000, an announcement was lost and the
     * players may have dropped us from their list — which switches off every
     * unicast towards us (status, NFS queries), leaving only the broadcasts.
     *
     * 'deferred' counts the attempts that fell inside the quiet window: those
     * are RETRIED on the next turn, so they can be non-zero without the gap
     * growing. If the two grow together, the retry is not enough. */
    ESP_LOGI(TAG, "[ka  ] sent=%u deferred=%u | max gap=%ums (every %dms)",
             (unsigned)s_ka_sent, (unsigned)s_ka_defer,
             (unsigned)s_ka_gap_max, (int)KEEPALIVE_MS);
    s_ka_sent = s_ka_defer = s_ka_gap_max = 0;

#if DJB_MIDI_DIN
    if (s_din_up) {
        ESP_LOGI(TAG, "[din] bytes sent=%u", (unsigned)s_din_bytes);
        s_din_bytes = 0;
    }
    if (s_cv_on) {
        /* 'pulses' must equal the beats in the window: if it is fewer, the
         * one-shot was armed late and some beats were skipped. */
        ESP_LOGI(TAG, "[cv] pulses=%u | offset=%+d us | GPIO%d=%s note=%s",
                 (unsigned)s_cv_pulses, ui_get_phase_offset_cv_us(),
                 CV_GPIO, s_cv_pin_ok ? "active" : "NOT AVAILABLE",
                 s_din_up ? "yes" : "no");
        s_cv_pulses = 0;
    }

#endif
#if DJB_MIDI_USB
    /* 'mounted' is the first thing to read: it says whether a host has
     * enumerated and configured the interface. At 0 with the cable plugged in,
     * the device was not recognised and the rest means nothing. */
    /* With the device not yet started the line does not appear: in the first
     * seconds that is the intended wait, not a fault. */
    if (midi_usb_running()) {
        /* 'dropped' must stay at ZERO: it means tud_midi_stream_write refused
         * the byte, that is, the transmit buffer is full because the host is
         * not consuming. */
        ESP_LOGI(TAG, "[usb] mounted=%d output=%s | bytes sent=%u dropped=%u",
                 (int)midi_usb_mounted(), s_usb_out_on ? "ON" : "OFF",
                 (unsigned)s_usb_bytes, (unsigned)s_usb_drop);
        s_usb_bytes = s_usb_drop = 0;
    }
#endif
#if DJB_MIDI_NET
    /* MIDI output over the network. 'sessions' is how many clients have invited
     * us: while it is 0 nothing goes out, and that is normal. 'queue full' must
     * stay at zero — if it moves, the transport task is not keeping up with 24
     * pulses per beat and MIDI2_NET_TX_QUEUE_LEN must be raised. */
    if (s_mnet) {
        ESP_LOGI(TAG, "[midi-ui] transport on=%d (net host=%d, DIN=%d) "
                      "| Enabled setting=%d",
                 (int)s_midi_up_seen, (int)(s_mnet != NULL),
#if DJB_MIDI_DIN
                 (int)s_din_up,
#else
                 0,
#endif
                 (int)settings_get_midi_net_enabled());
        ESP_LOGI(TAG, "[midinet] sessions=%u port=%u | sent=%u queue full=%u",
                 (unsigned)midi2_net_host_session_count(s_mnet),
                 (unsigned)midi2_net_host_port(s_mnet),
                 (unsigned)s_mnet_sent, (unsigned)s_mnet_full);
        s_mnet_sent = s_mnet_full = 0;
    }
#endif

    /* The specialised send path, for beat and status. These two counters are the
     * only failures that path cannot report at once, and both must stay at ZERO:
     * 'timeout' is a failed ARP on a unicast, that is a status that never
     * reached the player; 'no space' should never happen at all — 2 KB of ring
     * against 284 bytes. */
    {
        uint32_t fs, ft, fn;
        w5500_net_fast_stats(&fs, &ft, &fn);
        if (fs || ft || fn)
            ESP_LOGI(TAG, "[fasttx] sent=%u | ARP failed=%u | no room=%u",
                     (unsigned)fs, (unsigned)ft, (unsigned)fn);
    }

    /* The specialised receive path. 'absurd headers' must stay at ZERO: it is
     * the sign that the path is reading the ring at the wrong point — the same
     * misalignment that once cost days of investigation, except that this time
     * it is visible at once instead of being deduced from gaps in the status
     * packets. */
    {
        uint32_t rn, rb, rt;
        w5500_net_fast_recv_stats(&rn, &rb, &rt);
        if (rn || rb || rt)
            ESP_LOGI(TAG, "[fastrx] read=%u | absurd headers=%u | truncated=%u",
                     (unsigned)rn, (unsigned)rb, (unsigned)rt);
    }

#if DJB_MIDI_CLOCK
    /* The 24 PPQN grid. What to look at, in order of importance:
     *   wake-up  the task's punctuality, comparable with [osc]'s
     *   error    the grid's deviation from the anchor: it must oscillate around
     *            zero, not wander away from it
     *   resync   HARD re-locks. In the steady state they must be ZERO: if they
     *            appear without the bridge or the track having changed, the grid
     *            is not holding
     *   skipped  pulses lost because the instant had already passed. Always
     *            zero, or the engine is struggling
     *
     * 'corr' are the gentle corrections, one per beat: a counter of normality,
     * not an alarm. */
    {
        midi_clock_stats_t m;
        midi_clock_stats(&m);
        if (m.ticks || m.locked) {
            ESP_LOGI(TAG, "[midi] %s tick=%u corr=%u resync=%u skipped=%u | "
                          "wake-up: %+d..%+dus | error=%+dus | delivery max=%+dus",
                     m.locked ? "locked" : "WAITING",
                     (unsigned)m.ticks, (unsigned)m.beats,
                     (unsigned)m.resyncs, (unsigned)m.skips,
                     (int)m.jit_min_us, (int)m.jit_max_us,
                     (int)m.err_last_us, (int)s_midi_lat_max);
            s_midi_lat_max = 0;
        }
    }
#endif

    /* --- SPI contention and send time: they set the precision with which we can
     * send the beat packets, since jitter becomes phase error in the
     * Pioneers. --- */
    bsp_spi_stats_t sp;
    bsp_spi_stats(&sp);
    uint32_t win_us = (uint32_t)(HB_PERIOD_US / 1000);          /* window in ms */
    uint32_t occup  = win_us ? (sp.disp_hold_tot / 10 / win_us) : 0;  /* per mille -> % with 1 dec */

    /* WHO HOLDS THE BUS. The network is what PAYS: every microsecond the W5500
     * spends waiting goes straight into the beats' jitter. */
    ESP_LOGI(TAG, "[spi] network: wait max=%uus avg=%uus | held max=%uus"
                  " | the longest wait was behind: %s | over 500us: %u (display: %u)",
             (unsigned)sp.net_wait_max, (unsigned)sp.net_wait_avg,
             (unsigned)sp.net_hold_max,
             sp.net_wait_max_by_display ? "DISPLAY" : "network",
             (unsigned)sp.net_wait_long_n, (unsigned)sp.net_wait_long_by_display);
    ESP_LOGI(TAG, "[spi] display: %u flush | held max=%uus tot=%ums (%u.%u%% of the time)"
                  " | area max=%upx",
             (unsigned)sp.disp_n, (unsigned)sp.disp_hold_max,
             (unsigned)(sp.disp_hold_tot / 1000),
             (unsigned)(occup / 10), (unsigned)(occup % 10),
             (unsigned)sp.disp_px_max);
    if (sp.disp_defer_n)
        ESP_LOGI(TAG, "[spi] quiet window: %u flush deferred (max %uus)",
                 (unsigned)sp.disp_defer_n, (unsigned)sp.disp_defer_max);
    if (s_tx_n) {
        ESP_LOGI(TAG, "[tx ] W5500 send: max=%uus avg=%uus (n=%u)",
                 (unsigned)s_tx_max_us, (unsigned)(s_tx_sum_us / s_tx_n), (unsigned)s_tx_n);
        s_tx_max_us = s_tx_sum_us = s_tx_n = 0;
    }
    if (s_beat_tx_count) {
        ESP_LOGI(TAG, "[beat] sent=%u | wake-up: %+d..%+dus | ACTUAL OUT: %+d..%+dus"
                      " | send(paired) max=%dus"
                      " | of which bus: tot=%uus in %u takes, worst %uus behind %s",
                 (unsigned)s_beat_tx_count,
                 (int)s_beat_jit_min, (int)s_beat_jit_max,
                 (int)s_beat_out_min, (int)s_beat_out_max, (int)s_beat_tx_max,
                 (unsigned)s_beat_busw_sum_max, (unsigned)s_beat_busn_max,
                 (unsigned)s_beat_busw_max, s_beat_busw_disp ? "DISPLAY" : "rete");
        s_beat_jit_max = s_beat_jit_min = 0;
        s_beat_out_max = s_beat_out_min = 0;
        s_beat_tx_max  = 0;
        s_beat_busw_max = s_beat_busw_sum_max = s_beat_busn_max = 0;
        s_beat_busw_disp = false;
    }

    /* The genuinely free stack (high-water mark) of EVERY task: stacks come from
     * the internal heap, which is the scarce resource. Printing only two of them
     * left ours — w5rx, beat_tx, audio — as 10 KB we knew nothing about. One
     * compact line, so as not to lengthen the heartbeat. */
    {
        char line[192]; int off = 0;
        for (UBaseType_t i = 0; i < n && off < (int)sizeof(line) - 24; i++) {
            unsigned freeb = (unsigned)(s_task_arr[i].usStackHighWaterMark * sizeof(StackType_t));
            int w = snprintf(line + off, sizeof(line) - off, "%s%.7s=%u",
                             off ? " " : "", s_task_arr[i].pcTaskName, freeb);
            /* snprintf returns what it WOULD have written: if it truncated, we
             * stop here rather than letting the index overrun. */
            if (w < 0 || w >= (int)(sizeof(line) - off)) break;
            off += w;
        }
        ESP_LOGI(TAG, "[stack] free: %s", line);
    }
}
#else
static inline void cpu_usage_log(int64_t interval_us) { (void)interval_us; }
#endif
#endif

/* ============================================================================
 * WiFi hot-switch: SoftAP <-> Client, coordinated with Ableton Link.
 *
 * Sequence (direct approach): stop Link discovery, tear down the current mode,
 * bring up the new one, re-enable Link. For AP->Client, Link is re-enabled only
 * after the client gets an IP (GOT_IP), not before - so a flag marks "waiting
 * for client IP to re-enable Link".
 * ==========================================================================*/
static volatile int  s_wifi_mode_request   = -1;    /* -1 none, 0 AP, 1 client (set by UI) */

/* Called from settings_ui (LVGL task) when the mode toggle settles. We do NOT
 * run the transition here - esp_wifi_stop/start can block and would freeze the
 * UI. Instead we record a request; the main loop executes it off the LVGL task. */
static void request_wifi_mode(bool client)
{
    s_wifi_mode_request = client ? 1 : 0;
}

/* pending connect request (SSID+password) from the password screen */
static volatile bool s_connect_request = false;
static char s_connect_ssid[33] = {0};
static char s_connect_pass[65] = {0};

/* pending AP-restart request (SoftAP name/password changed at runtime) */
static volatile bool s_ap_restart_request = false;

/* Called from settings_ui (LVGL task) when the SoftAP name or password changes.
 * esp_wifi stop/start can block, so we defer the actual restart to the main
 * loop, off the LVGL task. */
static void request_ap_restart(void)
{
    s_ap_restart_request = true;
}

/* Called from settings_ui when the user confirms a password (or picks an open
 * network). Saves the credentials and requests a connection, executed off the
 * LVGL task by the main loop. */
static void request_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid) return;
    strncpy(s_connect_ssid, ssid, sizeof(s_connect_ssid) - 1);
    s_connect_ssid[sizeof(s_connect_ssid) - 1] = 0;
    strncpy(s_connect_pass, pass ? pass : "", sizeof(s_connect_pass) - 1);
    s_connect_pass[sizeof(s_connect_pass) - 1] = 0;
    s_connect_request = true;
}

/* called from the wifi_client GOT_IP path (via poll below) to re-enable Link */
static void wifi_on_client_got_ip(void)
{
    if (s_link_wait_got_ip) {
        s_link_wait_got_ip = false;
        linkbridge_set_enabled(true);   /* Link resumes discovery on the new subnet */
#if DJB_MIDI_NET
        midi_net_start();               /* only now is there an IP to announce on */
#endif
        ESP_LOGI(TAG, "hot-switch: client got IP, Link re-enabled");
    }
}

/* the mode handler registered into settings_ui; runs on the LVGL task context */
static void apply_wifi_mode(bool client)
{
    if (client == s_wifi_client_mode) return;   /* already in that mode */
    ESP_LOGI(TAG, "hot-switch: -> %s", client ? "CLIENT" : "SoftAP");

    /* 1. stop Link before touching the network interface */
    linkbridge_set_enabled(false);
#if DJB_MIDI_NET
        /* Same reason: the host holds an lwIP socket and an mDNS announcement
         * bound to the interface that is about to disappear. */
    midi_net_stop();
#endif

    if (client) {
        /* AP -> Client */
        wifi_ap_stop();
        s_ap_ok = false;
        wifi_client_start();
        /* read saved credentials; connect if we have an SSID */
        char ssid[33] = {0}, pass[65] = {0};
        settings_get_wifi_ssid(ssid, sizeof(ssid));
        settings_get_wifi_password(pass, sizeof(pass));
        if (ssid[0]) {
            s_link_wait_got_ip = true;      /* Link re-enabled at GOT_IP */
            wifi_client_connect(ssid, pass);
        } else {
            /* no saved network: stay client-but-idle; Link stays off until a
             * network is chosen and connected. Status row will show red. */
            ESP_LOGW(TAG, "hot-switch: client mode with no saved network");
        }
    } else {
        /* Client -> AP */
        wifi_client_stop();
        s_ap_ok = (wifi_ap_start() == ESP_OK);
        linkbridge_set_enabled(true);       /* AP is immediate, no DHCP wait */
#if DJB_MIDI_NET
        midi_net_start();
#endif
    }

    s_wifi_client_mode = client;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== M4c + master tracking ===");
    memset(&st, 0, sizeof(st));

    ESP_ERROR_CHECK(bsp_power_init());

    /* The internal temperature sensor is not critical: if it fails we carry on,
     * and the Info page shows a dash instead of the value. */
    if (bsp_temp_init() != ESP_OK)
        ESP_LOGW(TAG, "[temp] internal sensor not available");
    ESP_ERROR_CHECK(settings_init());   /* NVS: the UI reads the offset in ui_init */
    ESP_ERROR_CHECK(bsp_spi_bus_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(w5500_net_init());
    st.ver = w5500_net_version();
    open_djlink_sockets();
    ESP_ERROR_CHECK(wifi_core_init());        /* global WiFi init, once */
    /* start in the persisted mode (default AP). Link starts after, on whichever
     * interface is up. */
    bool want_client = (settings_get_wifi_mode() == WIFI_MODE_PREF_CLIENT);
    if (want_client) {
        char ssid[33] = {0}, pass[65] = {0};
        settings_get_wifi_ssid(ssid, sizeof(ssid));
        settings_get_wifi_password(pass, sizeof(pass));
        wifi_client_start();
        s_wifi_client_mode = true;
        s_ap_ok = false;
        if (ssid[0]) wifi_client_connect(ssid, pass);
        ESP_LOGI(TAG, "boot in CLIENT mode (ssid '%s')", ssid[0] ? ssid : "<none>");
    } else {
        s_ap_ok = (wifi_ap_start() == ESP_OK);
        if (!s_ap_ok) ESP_LOGE(TAG, "SoftAP not started: Ableton side shows red");
    }
    /* apply the persisted TX power level (esp_wifi must be started first) */
    wifi_core_set_power((int)settings_get_wifi_power());
    ESP_ERROR_CHECK(linkbridge_start(INITIAL_BPM));
    /* Start/Stop Sync: apply the saved preference. With it OFF we do not touch
     * the session's transport at all, not even passively. */
    /* The library's start/stop channel must be opened if at least one of the two
     * filters is active; it is then the filters that decide what passes. */
    linkbridge_set_start_stop_sync(settings_get_start_sync() ||
                                   settings_get_stop_sync());
    linkbridge_set_start_stop_callback(on_link_transport);
    rx_interrupt_start();

    /* The beat packet engine, used when Ableton is the source: the timer wakes a
     * high-priority task on core 0, which does the sending. No SPI in the
     * timer's context. */
    xTaskCreatePinnedToCore(beat_task, "beat_tx", 3072, NULL, 23, &s_beat_task, 0);

    /* The OSC engine on CORE 1: core 0 is left as it is. */
    {
        osc_out_init();
        osc_apply_settings();
        osc_custom_load();
        const esp_timer_create_args_t a = { .callback = osc_timer_cb,
                                            .dispatch_method = ESP_TIMER_TASK,
                                            .name = "osc" };
        if (esp_timer_create(&a, &s_osc_timer) == ESP_OK &&
            xTaskCreatePinnedToCore(osc_task, "osc_tx", OSC_TASK_STACK, NULL,
                                    OSC_TASK_PRIO, &s_osc_task, 1) == pdPASS) {
            osc_schedule_next(esp_timer_get_time());
            ESP_LOGI(TAG, "[osc] engine started (core 1, prio %d)", OSC_TASK_PRIO);
        } else {
            ESP_LOGW(TAG, "[osc] start failed: no OSC output");
            s_osc_task = NULL;
        }
    }

#if DJB_MIDI_CLOCK
    {
        const midi_clock_config_t mc = {
            .src       = clock_next_beat,   /* the same phase as the other outputs */
            .sink      = midi_sink,
            .sink_ctx  = NULL,
            .offset_us = midi_offset_us,
            .core_id   = 1,                 /* core 0 stays the real-time island */
            .priority  = MIDI_TASK_PRIO,
            .stack     = MIDI_TASK_STACK,
        };
        if (midi_clock_start(&mc) != ESP_OK)
            ESP_LOGW(TAG, "[midi] start failed: no clock engine");
    }
#endif

#if DJB_MIDI_DIN
    /* The pin only: whether to switch the UART on is decided by midi_din_poll(),
     * when it sees the setting and the module together. */
    midi_din_pin_init();
#endif
#if DJB_MIDI_USB
    /* NOT started here: midi_usb_deferred_start() does it from the main loop,
     * once the system is running. See the note beside that function. */
#endif
#if DJB_MIDI_NET
    /* In SoftAP the interface is ready already; in client mode without an IP the
     * host starts anyway and mDNS announces itself when the address arrives —
     * or wifi_on_client_got_ip() restarts it. */
    midi_net_start();
#endif
    const esp_timer_create_args_t beat_timer_args = {
        .callback = beat_timer_cb, .name = "beat"
    };
    ESP_ERROR_CHECK(esp_timer_create(&beat_timer_args, &s_beat_timer));
    if (leds_init() != ESP_OK) ESP_LOGW(TAG, "LEDs not initialised");
    /* apply the persisted brightness levels (settings_init already ran) */
    {
        static const uint8_t led_pct[4] = { 0, 20, 40, 80 };   /* keep in sync with settings_ui */
        leds_set_brightness(led_pct[settings_get_led_brightness()]);
        bsp_set_backlight_level((int)settings_get_display_brightness());
    }
    /* Screen rotation BEFORE drawing anything: applied afterwards, the first
     * frame would appear the wrong way up and it would show.
     *
     * The two calls go together — image and touch — or the interface would
     * respond at the point opposite the one touched. */
    {
        bool flip = settings_get_screen_flipped();
        bsp_touch_set_flipped(flip);
        display_set_flipped(flip);
        if (flip) ESP_LOGI(TAG, "[ui] screen rotated by 180 degrees");
    }
    /* The card is mounted before the UI so the badge has a state to show from
     * the first frame. A missing card is not an error and is not checked: it
     * simply leaves the badge hidden. */
    sdcard_init();

    /* The WiFi mode BEFORE the dashboard is drawn: the badge carries a
     * different word in each, and telling it afterwards means changing it under
     * the eye a moment after the screen appears. */
    ui_set_wifi_ap_initial(!s_wifi_client_mode);

#if DJB_DEVELOPER
    /* The long press asks ui for a screenshot; ui asks whoever registered this.
     * The card is not ui's business, so the two are joined here. */
    ui_set_screenshot_fn(save_screenshot);

    /* The flush keeps a copy of the screen only with the option on. Read once
     * here, and set again by the menu when it changes: it cannot change by any
     * other route, so there is nothing to poll. */
    ui_set_screenshots(settings_get_screenshots());
    settings_ui_set_shots_handler(ui_set_screenshots);
#endif

    ESP_ERROR_CHECK(ui_init());

    /* Bridge memory: we resume as we were left.
     *
     * What it is for: if the device is switched off and on again DURING a set,
     * it comes back already active instead of with everything stopped.
     *
     * ABLETON MAY NOT BE RESTORED AS THE SOURCE: ui_set_clock_src_initial
     * filters it through the availability. If this time there were no 1-4
     * identity — four players on the network, or OBSERVER mode — it would stay
     * unset, and rightly so: better to start with no source than to show a
     * bridge that carries nothing. */
    if (settings_get_remember_bridge()) {
        bool dj = false, abl = false;
        settings_get_bridge_state(&dj, &abl);
        clock_src_t src = dj  ? CLOCK_SRC_DJLINK
                        : abl ? CLOCK_SRC_ABLETON
                              : CLOCK_SRC_NONE;
        if (src != CLOCK_SRC_NONE) {
            ui_set_clock_src_initial(src);
            ESP_LOGI(TAG, "[clock] source restored: %s",
                     src == CLOCK_SRC_DJLINK ? "DJ LINK" : "ABLETON");
        }
    }
    settings_ui_set_mode_handler(request_wifi_mode);   /* hot-switch on mode toggle */
    settings_ui_set_connect_handler(request_wifi_connect);  /* connect on password confirm */
    settings_ui_set_ap_restart_handler(request_ap_restart); /* recreate AP on credential change */
    settings_ui_set_drift_handler(on_drift_comp);           /* Drift Compensation toggle */
    /* The custom OSC list: the menu edits it in place and calls back to save.
     * It is handed over AFTER osc_custom_load, so what the menu shows is what
     * came out of NVS. */
    settings_ui_set_osc_custom(s_osc_cust, &s_osc_cust_n, osc_custom_save);
#if DJB_MIDI_NET
    settings_ui_set_midinet_handler(on_midinet_settings_changed);
#if DJB_MIDI_DIN
    settings_ui_set_sound_table(sam_sound_name, SAM_SND_N);
#endif
#endif
    s_corr_on = settings_get_drift_comp();                  /* persisted state at boot */
    if (s_corr_on) ESP_LOGI(TAG, "[drift] compensation ON (from settings)");


    /* Audio: the UI's sound feedback. Amplifier init plus a task on core 1. If
     * anything goes wrong the init disables itself — audio_ready() stays false
     * and audio_play becomes a no-op — and the rest of the firmware is
     * untouched. */
#if DJB_AUDIO_ON
    if (audio_init() == ESP_OK) {
        ESP_LOGI(TAG, "[audio] sound feedback active");
        audio_set_enabled(settings_get_audio_enabled());   /* apply saved on/off */
        audio_play(AUDIO_BOOT);   /* dedicated start-up chime (if enabled) */
    } else {
        ESP_LOGW(TAG, "[audio] init failed: carrying on without sound feedback");
    }
#endif

    int64_t last_clock = 0, last_led = 0, last_check = 0, last_hb = 0, last_ka = 0;

    /* st.link_up starts DOWN, and deliberately: the address is fetched on the
     * RISING EDGE of the link and nowhere else. A device booted with the cable
     * in sees that edge on the first turn of the loop, one booted without it
     * whenever the cable appears — one path for both, and no separate call at
     * start-up that would race the loop for the same job. */

    bool prev_alive = (st.ver == 0x04);

    while (1) {
        /* counter for the two-rate polling: see SLOW_POLL_DIV */
        static uint32_t poll_n = 0;
        bool slow_turn = (SLOW_POLL_DIV <= 1) || ((poll_n % SLOW_POLL_DIV) == 0);
        poll_n++;

        if (st.ver == 0x04) {
            /* Block 1: the drains. Inside the quiet window they return at once
             * by themselves, so the priority is not even raised. */
            if (!bsp_spi_in_quiet_window()) {
                net_prio_raise();
                drain_discovery();           /* :50000 keep-alive and media queries */
                drain_status();              /* :50002 status                       */
                drain_w5500();               /* :50001 beat                         */
                net_prio_restore();
            }
        }
        int64_t now = esp_timer_get_time();

        tempo_tick(now);        /* a ramp's final value gets through regardless */
        phase_pitch_tick(now);  /* manoeuvre over -> one lock, on a fresh measurement */

        /* The tempo we impose has changed: the status goes out AT ONCE instead
         * of waiting for the periodic slot, and for a moment we run at the fast
         * cadence. That is what a real CDJ does, and it is what the synced
         * player's promptness rests on: it reads the tempo from the status and
         * applies it, so its responsiveness is our cadence.
         *
         * Only as MASTER: in the forward direction we impose nothing and have
         * nothing to announce. */
        if (s_net_role == NET_MASTER) {
            static double last_imposed = 0.0;
            double b = linkbridge_tempo();
            if (last_imposed > 0.0 && fabs(b - last_imposed) > TEMPO_EPS)
                status_note_change(now);
            last_imposed = b;
        }

        if (now - last_clock >= (int64_t)CLOCK_MS * 1000) {
            update_clock(now);
            publish_ui(now);
            last_clock = now;
        }

        if (now - last_led >= 100000) {   /* 10 Hz: enough for the 1 Hz blink */
            update_leds(now);
            /* hot-switch: execute a pending mode-change request here (main task,
             * not LVGL) so the blocking WiFi calls don't freeze the UI */
            if (s_wifi_mode_request >= 0) {
                bool want_client = (s_wifi_mode_request == 1);
                s_wifi_mode_request = -1;
                apply_wifi_mode(want_client);
            }
            /* connect request from the password screen: save creds, ensure we're
             * in client mode, then connect */
            if (s_connect_request) {
                s_connect_request = false;
                settings_set_wifi_ssid(s_connect_ssid);
                settings_set_wifi_password(s_connect_pass);
                settings_set_wifi_mode(WIFI_MODE_PREF_CLIENT);
                if (!s_wifi_client_mode) {
                    apply_wifi_mode(true);   /* switch to client (will connect saved creds) */
                } else {
                    /* already client: just (re)connect to the new network */
                    s_link_wait_got_ip = true;
                    wifi_client_connect(s_connect_ssid, s_connect_pass);
                }
                ESP_LOGI(TAG, "connect request: '%s'", s_connect_ssid);
            }
            /* AP restart request: SoftAP credentials changed at runtime.
             * Recreate the AP so it picks up the new name/password. Only makes
             * sense in SoftAP mode; skip if we've since switched to client. */
            if (s_ap_restart_request) {
                s_ap_restart_request = false;
                if (!s_wifi_client_mode) {
                    ESP_LOGI(TAG, "restarting SoftAP with new credentials");
                    linkbridge_set_enabled(false);
                    wifi_ap_stop();
                    s_ap_ok = (wifi_ap_start() == ESP_OK);
                    if (s_ap_ok) linkbridge_set_enabled(true);
                    else ESP_LOGE(TAG, "SoftAP restart failed");
                }
            }
            /* re-enable Link once the client gets an IP after an AP->Client switch */
            if (s_link_wait_got_ip &&
                wifi_client_state() == WIFI_CLIENT_CONNECTED) {
                wifi_on_client_got_ip();
            }
            last_led = now;
        }

        /* Block 2: nearly all these ticks transmit on the W5500 (keep-alive,
         * status, handoff). The same protection as the drain block.
         *
         * beat_engine_tick stays INSIDE on purpose: it does not touch the bus —
         * it arms a timer and reserves the quiet window — and running at raised
         * priority means the reservation is recorded without risk of
         * be suspended halfway. */
        net_prio_raise();
        claiming_tick(now);   /* claim sequence (self-gated on CLAIMING)       */
        status_tick(now);     /* status send (self-gated on HOLDING/MASTER)    */
        transport_tick(now);       /* players aggregate -> Ableton (direct)   */
        link_transport_tick();     /* Ableton -> players via fader (reverse)  */
        master_tick(now);     /* master handoff (self-gated on HOLDING)         */
        beat_engine_tick(now); /* beat packet (self-gated on MASTER)           */
        net_prio_restore();

        /* Block 3: TCP services and the portmapper. They use the bus like the
         * others, and the TCP operations are the longest we perform — all the
         * more reason not to leave them at priority 1 while holding the lock. */
        net_prio_raise();
        if (slow_turn) osc_apply_settings();  /* the OSC settings can change */
#if DJB_MIDI_NET
            /* A MIDI setting has changed: it is applied NOW, stopping and
             * restarting the host. midi_net_start() re-reads both Enabled and
             * Announce by itself, so a single path serves both switches —
         * and if Enabled is OFF it simply does not restart. */
        if (s_mnet_restart) {
            s_mnet_restart = false;
            /* Nothing is ACTED on here any more: we only record what the user
             * asked for. Each transport has its own poll, and each touches only
             * what changed in its own transport. */
#if DJB_MIDI_DIN
            const char *din_want = settings_get_midi_din_enabled() ? "ON" : "OFF";
#else
            const char *din_want = "n/d";
#endif
            ESP_LOGI(TAG, "[midi] settings: net=%s (announce %s), DIN=%s",
                     settings_get_midi_net_enabled() ? "ON" : "OFF",
                     settings_get_midi_net_mdns()    ? "ON" : "OFF", din_want);
        }
            /* The cyclic phase selector shows MIDI only if the transport is
             * genuinely up: otherwise one would be adjusting the offset of an
             * output that does not exist. */
        if (slow_turn) {
#if DJB_MIDI_USB
            midi_usb_deferred_start(now);
            bool usb_mnt = midi_usb_mounted();
            s_usb_out_on = settings_get_midi_usb_enabled() && usb_mnt;
            settings_ui_set_usb_mounted(usb_mnt);
#endif
            /* DJ Link identity: AUTO or OBSERVER, from the menu.
             *
             * It is hooked in with the same pattern as the other settings — the
             * value is followed rather than reacted to on the touch, so it works
             * at first boot too, without a separate path.
             *
             * The choice does NOT snatch back a number already taken: it applies
             * from the next census, because changing identity while running
             * would break the metadata connections in progress. */
            djb_set_force_virtual(settings_get_djlink_observer());

            /* Bridge memory: on the RISING edge, the current state is
             * snapshotted.
             *
             * Without it, until the first touch on a bridge the old state would
             * stay saved, and a restart meanwhile would restore something the
             * user no longer has in front of them.
             *
             * The snapshot lives HERE and not in settings_ui because there it
             * would need ui_clock_src() == CLOCK_SRC_DJLINK, which lives in ui.h — and ui already
             * depends on settings_ui, so including it would be a cycle. main
             * sees both, and is the natural place. */
            {
                static int p_remember = -1;
                bool rem = settings_get_remember_bridge();
                if (rem && p_remember == 0)
                    settings_set_bridge_state(ui_clock_src() == CLOCK_SRC_DJLINK,
                                              ui_clock_src() == CLOCK_SRC_ABLETON);
                p_remember = rem ? 1 : 0;
            }

            midi_net_poll();       /* follows the network setting */
#if DJB_MIDI_DIN
            midi_din_poll();       /* continuous detection plus following */
#endif
#if DJB_MIDI_DIN
            /* CV goes out only if it is enabled AND the DIN is transmitting:
             * they share the same wire. */
            cv_poll();
            ui_set_cv_out_active(s_cv_on);
#endif
            bool midi_up = (s_mnet != NULL);
#if DJB_MIDI_DIN
            midi_up = midi_up || s_din_up;
#endif
#if DJB_MIDI_USB
            midi_up = midi_up || s_usb_out_on;
#endif
            ui_set_midi_out_active(midi_up);
            s_midi_up_seen = midi_up;   /* for the diagnostic line only */
        }
#endif
#if DJB_MIDI_CLOCK
            /* The MIDI transport follows the active bridge's. The slow turn is
             * enough: Start and Stop do not have the phase precision of the
             * pulses, and the engine aligns them to the grid anyway. */
        /* ONE CALL FOR BOTH: midi_transport_wanted remembers the state it last
         * handed over and logs the transitions, so asking it twice would mean
         * the second answer seeing a change the first had already consumed.
         *
         * OSC gets the SAME state as MIDI — the one filtered by Start Sync and
         * Stop Sync — so the two outputs cannot disagree about whether the
         * music is running. */
        if (slow_turn) {
            const bool playing = midi_transport_wanted(now);
            midi_clock_set_running(playing);
            osc_out_note_transport(playing);
        }
#endif
        /* THE PLAYLIST MAY WANT A TITLE, for a track on a player that is not the
         * master — the UI asks only for the master's, so those would go into
         * the file with their id alone.
         *
         * IT YIELDS TO THE DISPLAY. The query channel is one socket and one
         * state machine: if it is busy, or the UI has its own request queued,
         * the playlist waits. What the user is looking at now matters more than
         * a line that will be written in a few seconds, and the line goes in
         * without a title rather than not at all. */
        if (slow_turn && s_np == NP_IDLE && s_np_want_dev == 0) {
            uint8_t pl_player, d, sl, ty; uint32_t id;
            if (sdcard_wants_metadata(&pl_player, &d, &sl, &ty, &id)) {
                s_np_want_dev  = d;
                s_np_want_slot = sl;
                s_np_want_type = ty;
                s_np_want_id   = id;
                s_np_retry_us  = 0;
                s_np_for_playlist = true;
                s_np_for_player   = pl_player;
                ESP_LOGI(TAG, "[now] playlist wants the title of track %lu "
                              "(medium on device %u)", (unsigned long)id, (unsigned)d);
            }
        }


        if (slow_turn) portmap_tick(now);  /* :111 RPC: "I have no NFS service" */

        /* The DHCP client. While a lease is held this is one comparison against
         * T1 — no socket read, no SPI — so the slow turn is frequent enough
         * without costing anything. It returns true only when the address has
         * CHANGED, and then the sockets follow it. */
        if (slow_turn && dhcp_lite_tick(now)) reopen_after_address_change();
        /* THE CARD, and whether this is a moment to hold the bus for it.
         *
         * Two things must be true. The status cadence must be at rest: [M] a
         * synced player reads the pitch from our status and applies it, so its
         * promptness IS the rate at which we speak, and a card holding the bus
         * through a tempo change is that change arriving late in every player.
         *
         * And there must be ROOM BEFORE THE NEXT BEAT — not merely no beat in
         * flight. As master there is always one armed: the counter is cleared
         * when a beat leaves and the loop arms the next at once, so asking for
         * "none armed" is asking for an instant that barely exists. What
         * matters is the margin: [M] a real mount holds the bus for 81 ms, and
         * at 120 BPM the period is 500.
         *
         * When we are not master neither applies: we emit no beats, and a late
         * drain is absorbed by the W5500's buffers without touching the phase,
         * which the ISR marks. */
        if (slow_turn) {
            const bool status_calm = (now >= s_status_fast_until);
            const int64_t to_beat  = s_beat_armed_us ? (s_beat_armed_us - now) : 0;
            const bool beat_room   = (s_net_role != NET_MASTER) ||
                                     (to_beat > (int64_t)SD_BEAT_ROOM_US);
            sdcard_set_relaxed(status_calm && beat_room);
        }

        /* THE BRIDGE STATE, for the menus that must bar what would disturb it —
         * formatting the card holds the SPI bus for minutes.
         *
         * PUSHED EVERY SLOW TURN, not when the identity changes: the source is
         * switched from the dashboard at any moment, and a value refreshed only
         * on a network event would have the menu believing whatever was true
         * the last time the player number moved. */
        if (slow_turn) settings_ui_set_bridge_active(ui_clock_src() != CLOCK_SRC_NONE);
        nowplaying_tick(now);     /* which track is the master playing? non-blocking */
        dbserver_tick(now); /* switchboard :12523: we say where the data service is */
        dbdata_tick(now);   /* data service :1051: we answer the real dialogue   */
        net_prio_restore();

        /* A SILENT CENSUS.
         *
         * Until we have chosen our number we do NOT announce ourselves. First we
         * only listen: other devices' keep-alives are BROADCAST and carry the
         * device number — [M] verified on a capture — so the census can be taken
         * perfectly well while mute, covering both real and virtual players.
         *
         * Announcing device 7 from boot, BEFORE knowing whether it was free,
         * meant that a single beat-link running beside the bridge gave two
         * device 7s on the same network, silently. */
        bool announcing = (s_net_role != NET_OFFLINE && s_net_role != NET_CENSUS);
        static bool ka_said = false;
        if (st.ver == 0x04 && announcing &&
            now - last_ka >= (int64_t)KEEPALIVE_MS * 1000) {
            /* last_ka advances ONLY if the packet really went out: otherwise it
             * is retried on the next turn of the loop, that is within 5 ms. See
             * the note on send_keepalive(). */
            if (send_keepalive()) {
                if (!ka_said) {
                    ka_said = true;
                    ESP_LOGI(TAG, "[ident] starting to announce as device %d", s_ka_device);
                }
                last_ka = now;
            }
        }
        if (!announcing) ka_said = false;   /* re-census: we will say it again */

        if (now - last_check >= (int64_t)CHECK_MS * 1000) {
            uint8_t v = w5500_net_version();
            bool alive = (v == 0x04);
            if (alive && !prev_alive) {
                ESP_LOGW(TAG, "W5500 back: reconfiguring and reopening sockets");
                if (w5500_net_reinit() == ESP_OK) {
                    /* reinit runs the whole address sequence again — DHCP, then
                     * auto-ip — so the address is very likely a different one:
                     * everything that binds to it goes back up. */
                    reopen_after_address_change();
                    ESP_LOGI(TAG, "DJ Link restored");
                    prev_alive = true;
                }
            } else if (!alive && prev_alive) {
                ESP_LOGW(TAG, "W5500 gone (0x%02x)", v);
                prev_alive = false;
            }
            const bool link_before = st.link_up;
            st.link_up = alive ? w5500_net_link_up() : false;

            /* THE CABLE COMING AND GOING.
             *
             * A rise starts an acquisition; a fall is REPORTED rather than
             * waited out. Flapping means a faulty switch or cable, not bounce
             * to be filtered: there is no settling time that makes it right,
             * and a delay here would only slow down the ordinary case.
             *
             * What must not happen is a half-finished acquisition left standing,
             * or two tasks racing. The component handles both — a run in
             * progress gives up early, and address_stale() is true only when no
             * task is running — so this can simply act on what it is told. */
            if (!st.link_up && link_before) w5500_net_note_link_down();

            if ((st.link_up && !link_before) || w5500_net_address_stale()) {
                /* THE WHOLE SEQUENCE AGAIN, in the order of a power-on: wait
                 * for the link, DHCP, then link-local. [M] It is what the
                 * players do — they ask only at power-on and settle for good —
                 * and a cable coming back is our equivalent of one.
                 *
                 * It runs in a task, so this returns at once and the loop keeps
                 * turning. Sends are suspended for the duration, inside the send
                 * path itself: during the DHCP exchange SIPR is zero, and a
                 * keep-alive leaving from 0.0.0.0 would be worse than none. */
                w5500_net_acquire_address(reopen_after_address_change);
            }
            st.ver = v;
            net_role_tick(now);
            last_check = now;
        }

        if (now - last_hb >= HB_PERIOD_US) {
            ESP_LOGI(TAG, "[hb] up=%lus master=%d(Mh=%d) dj=%.2f link=%.2f sync=%s off=%+dus drift=%s peers=%d W5500=0x%02x heap=%u rpc_no=%lu",
                     (uint32_t)(now/1000000), master_current(now), s_master_mh, st.dj_bpm, st.link_bpm,
                     sync_label(st.sync), ui_get_phase_offset_dj_us(), corr_label(), linkbridge_num_peers(), st.ver,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)s_pm_replies);

            /* The DHCP state, only when it says something. On a switch-only
             * network the client settles into NONE and stays there, and a line
             * every heartbeat repeating "no server" would be noise. A held
             * lease is worth watching though: the seconds left tell whether the
             * renewal is working long before it fails. */
            switch (dhcp_lite_state()) {
            case DHCP_ST_BOUND: {
                uint8_t ip[4]; w5500_net_get_ip(ip);
                ESP_LOGI(TAG, "[dhcp] %u.%u.%u.%u lease %lus left",
                         ip[0], ip[1], ip[2], ip[3],
                         (unsigned long)dhcp_lite_lease_left_s(now));
                break;
            }
            case DHCP_ST_RENEWING:
                ESP_LOGI(TAG, "[dhcp] renewing");
                break;
            default:
                break;                     /* OFF: on link-local, nothing to say */
            }
        /* device number census: which of the slots 1-15 are in use */
            char used[64]; int u = 0;
            for (int d = 1; d < DEVNUM_MAX; d++)
                if (devnum_used((uint8_t)d, now)) u += snprintf(used + u, sizeof(used) - u, "%d ", d);
            if (u == 0) { used[0] = '-'; used[1] = 0; }
    /* With Ableton as the source: how closely the players really follow. Printed only when
     * WE are the master, the one case in which the question makes sense. */
    if (s_net_role == NET_MASTER) {
        char l[224]; int off = 0;
        for (int d = 1; d < DEVNUM_MAX && off < (int)sizeof(l) - 46; d++) {
            if (d == s_ka_device) continue;
            if (s_chase_dev_bpm[d] <= 1.0f && s_chase_dev_bpms[d] <= 1.0f) continue;
            int w = snprintf(l + off, sizeof(l) - off,
                             " %d:beat=%d.%02d st=%d.%02d sf%+ldms(max%+ld)", d,
                             (int)s_chase_dev_bpm[d],  (int)(s_chase_dev_bpm[d]  * 100) % 100,
                             (int)s_chase_dev_bpms[d], (int)(s_chase_dev_bpms[d] * 100) % 100,
                             (long)(s_chase_dev_off[d] / 1000), (long)(s_chase_dev_offmax[d] / 1000));
            s_chase_dev_offmax[d] = 0;
            if (w < 0 || w >= (int)(sizeof(l) - off)) break;
            off += w;
        }
        if (off) ESP_LOGI(TAG, "[rev] imposing %d.%02d | players (bpm/offset):%s",
                          (int)st.link_bpm, (int)(st.link_bpm * 100) % 100, l);
    }

    ESP_LOGI(TAG, "[devnum] role=%s in use: %s| free slot (1-4): %d | status_tx=%u",
                     net_role_name(s_net_role), used, devnum_pick_free(now), (unsigned)s_status_count);
#if DJB_DEVELOPER
            cpu_usage_log(now - last_hb);
#endif
            last_hb = now;
        }
        vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
    }
}
