/*
 * ui.h — LVGL user interface, and the two-way data channel to the clock bridge.
 *
 *   bridge (core 0) -> UI:  ui_publish() posts the state; an lv_timer on core 1
 *                           reads it and updates the objects.
 *   UI (core 1) -> bridge:  ui_clock_src() says which protocol gives the tempo.
 *
 * The shared state is guarded by a spinlock. The critical sections are very
 * short, so the real-time side does not suffer.
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SYNC_OFF,      /* bridge disabled by the toggle            */
    UI_SYNC_NOSRC,    /* enabled, but no DJ Link ever received    */
    UI_SYNC_NOMASTER, /* enabled, but no player is tempo master   */
    UI_SYNC_ACTIVE,   /* fresh beats: we are imposing the tempo   */
    UI_SYNC_HOLD,     /* DJ source lost: holding the last tempo   */
    UI_SYNC_RELEASE   /* DJ source lost: releasing the tempo      */
} ui_sync_t;

/* ---------------------------------------------------------------------------
 * STATUS BAR BADGES
 *
 * These enums are DELIBERATELY separate from the ones driving the side LEDs
 * (djl_led_t / abl_led_t in main.c). The duplication is intentional: screen
 * and LEDs are two different indicators and must be free to change
 * independently. Today they no longer resemble each other at all:
 *
 *   the LEDs   tell the PROTOCOL story, with six states and blinking — are
 *              there Pioneers? is the master playing? is a Link peer present?
 *   the BADGES tell the INFRASTRUCTURE story, in three colours and without
 *              blinking — red faulty, grey inert, green operational.
 *
 * Blinking was removed from the badges after trying it on the device. A
 * blinking LED is a point of light at the edge of vision; a blinking badge is
 * a filled rectangle changing underneath text, inside the strip one is
 * actually reading. Same rate, very different irritation.
 *
 * Protocol state still reaches the screen, but where it earns its place:
 * the MASTER badge, the beat cells, the BPM.
 * ------------------------------------------------------------------------- */

/* "LAN" badge: the physical chain, W5500 plus cable. It says nothing about the
 * DJ Link protocol, on purpose. */
typedef enum {
    UI_LAN_CHIP_KO,     /* W5500 missing or faulty        -> red   */
    UI_LAN_NO_CABLE,    /* W5500 present, cable unplugged -> grey  */
    UI_LAN_UP,          /* W5500 present, cable in        -> green */
} ui_lan_t;

/* "WIFI" (client) or "WIFI AP" (SoftAP) badge. */
typedef enum {
    UI_WIFI_KO,         /* adapter or SoftAP faulty                     -> red   */
    UI_WIFI_IDLE,       /* up: AP with no clients, or STA not connected -> grey  */
    UI_WIFI_OK,         /* AP with at least one client, or STA joined   -> green */
} ui_wifi_t;

/* "OSC" badge: the auxiliary output.
 *
 * The grammar below is shared by OSC, MIDI and CV, for the same reason in each
 * case: an output switched off in the settings takes no room in the strip, one
 * switched on but unusable is a fault worth seeing, and one switched on with
 * no clock feeding it is merely idle. */
typedef enum {
    UI_OSC_OFF,         /* off in the settings          -> badge ABSENT */
    UI_OSC_FAULT,       /* on but unusable              -> red          */
    UI_OSC_IDLE,        /* on, no bridge feeding it     -> grey         */
    UI_OSC_ON,          /* on and fed by a bridge       -> green        */
} ui_osc_t;

/* MIDI badge. ONE badge covers every MIDI transport — network and DIN —
 * because the strip says whether MIDI is going out, not through which port. */
typedef enum {
    UI_MIDI_OFF,        /* no transport enabled          -> badge ABSENT */
    UI_MIDI_FAULT,      /* requested but failed to start -> red          */
    UI_MIDI_IDLE,       /* started, no bridge feeding it -> grey         */
    UI_MIDI_ON,         /* started and fed by a bridge   -> green        */
} ui_midi_t;

/* CV badge. */
typedef enum {
    UI_CV_OFF,          /* off in the settings        -> badge ABSENT */
    UI_CV_FAULT,        /* requested but unavailable  -> red          */
    UI_CV_IDLE,         /* ready, no bridge           -> grey         */
    UI_CV_ON,           /* ready and fed              -> green        */
} ui_cv_t;

/* The microSD slot. ABSENT hides the badge entirely: an empty slot is not a
 * fault and does not deserve a permanent grey box on the dashboard. */
typedef enum {
    UI_SD_ABSENT = 0,   /* no card                     -> badge ABSENT */
    UI_SD_ERROR,        /* card there but unusable     -> red          */
    UI_SD_IDLE,         /* mounted, playlist off       -> grey         */
    UI_SD_WRITING,      /* mounted, playlist on        -> green        */
} ui_sd_t;

typedef struct {
    float     dj_bpm;       /* BPM from the Pioneers    */
    float     link_bpm;     /* BPM of the Link session  */
    ui_sync_t sync;
    uint8_t   master_dev;
    bool      we_are_master;      /* the bridge is master        -> GREEN badge */
    bool      master_negotiating; /* handover under way -> blinking AMBER badge */
    bool      master_failed;      /* negotiation failed -> blinking RED badge   */

    /* A REAL player is playing a REAL track.
     *
     * Not the same thing as "the DJ Link network is active": when WE are the
     * master the network is running, but there is no track to display, because
     * our medium is a fiction. When false the name is HIDDEN rather than
     * cleared — the data stays in memory, so if the track resumes we do not
     * query the database again. */
    bool      track_playing;

    uint8_t   beat;         /* current beat 1..4, 0 = none */
    int       peers;        /* Link peers                  */
    ui_lan_t  lan;
    ui_wifi_t wifi;
    ui_osc_t  osc;
    ui_midi_t midi;
    ui_cv_t   cv;
    ui_sd_t   sd;

    /* Key rejected by the network, client mode only: raises a red "AUTH" badge
     * left of the WiFi one. It is a separate flag rather than a ui_wifi_t
     * state because the two badges coexist — "AUTH" says WHY we are not
     * connecting, while "WIFI" stays grey to say the adapter is healthy. */
    bool      wifi_auth;

    bool      wifi_ap;      /* true = SoftAP, false = client */
    int       wifi_clients; /* clients joined to the AP      */
    int       wifi_rssi;    /* dBm in client mode; 0 = not connected */
} ui_state_t;

esp_err_t ui_init(void);

/* Posted by the bridge (core 0) at about 10 Hz. */
void ui_publish(const ui_state_t *s);

/* WHICH PROTOCOL GIVES THE TEMPO.
 *
 * The bridge is bidirectional and the two directions are mutually exclusive:
 * either the players give the tempo and we carry it to Ableton, or Ableton
 * gives it and we carry it to the players. ONE VALUE, not two flags — a state
 * "both at once" does not exist and now cannot be represented.
 *
 * The source decides everything downstream: which phase reference is used, who
 * has to become master, and which offset applies to each auxiliary output. */
typedef enum {
    CLOCK_SRC_NONE = 0,   /* no bridge: we hold no opinion on the tempo       */
    CLOCK_SRC_DJLINK,     /* the players give it -> carried to Ableton        */
    CLOCK_SRC_ABLETON,    /* Ableton gives it    -> carried to the players    */
} clock_src_t;

/* Read by the bridge; driven by the UI on core 1. */
clock_src_t ui_clock_src(void);

/* Written by the bridge, which clears the source after yielding the master
 * role. Asking for CLOCK_SRC_ABLETON when it is unavailable leaves NONE. */
void ui_set_clock_src(clock_src_t src);

/* THE SOURCE AT START-UP.
 *
 * By default there is none: the user chooses it, deliberately. This is the
 * single entry point for changing that default — the settings module reads the
 * saved value from NVS and restores it by calling this ONCE, right after
 * ui_init() and before the bridge starts reading. Keeping the restore here,
 * rather than scattered, prevents the start-up state from being decided in two
 * different places. */
void ui_set_clock_src_initial(clock_src_t src);

/* The WiFi mode as it is at start-up, before ui_init() draws anything.
 *
 * The badge carries a different word in each mode — "WIFI" or "WIFI AP" — and
 * therefore a different width. Created for one and corrected on the first
 * refresh, it changes under the eye fifty milliseconds after the dashboard
 * appears. Told beforehand, it is drawn right the first time. */
void ui_set_wifi_ap_initial(bool ap);

#if DJB_DEVELOPER
/* Called when a long press asks for a screenshot, with the setting on.
 *
 * A CALLBACK, so ui need not know about the card: it sees the gesture, main
 * decides what saving means. Present only in developer builds — at
 * DJB_DEVELOPER 0 the gesture goes back to locking and this disappears. */
typedef void (*ui_screenshot_fn)(void);
void ui_set_screenshot_fn(ui_screenshot_fn fn);

/* Whether the flush should keep a copy of the screen for screenshots.
 *
 * TOLD, NOT ASKED. The flush runs on every frame and reading the setting from
 * NVS there would cost more than the copy itself, while polling it would be
 * asking a question nobody has changed the answer to. It is set at start-up
 * and again by whoever changes the option. */
void ui_set_screenshots(bool on);

#endif

/* TRACK PLAYING on the master, read from the players' data service.
 *
 * The UI alternates title and artist: there is little room at the top of the
 * display, and scrolling would cost cycles on a core already contended by the
 * SPI bus — not worth a flourish. Pass NULL or an empty string to clear. */
void ui_set_track(const char *title, const char *artist);
void ui_clear_track(void);

/* CAN ABLETON BE THE SOURCE?
 *
 * With four players already on the network no number in 1..4 is left to take,
 * and without a real identity we cannot become master — so taking the tempo
 * from Ableton is IMPOSSIBLE, not merely unused. The toggle is then switched
 * off and made inert (grey, not clickable): better to say so plainly than to
 * leave a button that cannot work. */
void ui_set_ableton_src_available(bool avail);
bool ui_ableton_src_available(void);

/* --- phase offsets ---------------------------------------------------------
 *
 * Owned by the UI, adjusted with the silkscreened buttons. They are separate
 * values rather than one, because each compensates a different chain. */

int  ui_get_phase_offset_dj_us(void);   /* source DJ Link: delays Ableton  */
int  ui_get_phase_offset_abl_us(void);  /* source Ableton: delays the players */

/* OSC output offset: compensates the latency of whatever sits downstream, so
 * it is independent of the bridge offsets. */
int  ui_get_phase_offset_osc_us(void);

/* MIDI output offset. */
int  ui_get_phase_offset_midi_us(void);

/* main tells us when a MIDI transport is up — network host started, or DIN
 * connected. It governs whether MIDI appears in the phase selector: without
 * it, one would be adjusting the offset of an output that does not exist. */
void ui_set_midi_out_active(bool on);

/* CV offset, kept apart from the MIDI one: CV goes through the synthesiser,
 * which adds a latency of its own to compensate. */
int  ui_get_phase_offset_cv_us(void);
void ui_set_cv_out_active(bool on);

/* Consume the "an offset was just changed" flag. */
bool ui_offset_changed(void);

#ifdef __cplusplus
}
#endif
