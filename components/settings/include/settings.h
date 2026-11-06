/*
 * settings.h — persistent preferences, stored in NVS.
 *
 * The device's configuration store: WiFi mode and credentials, SoftAP
 * identity, phase offsets for every output, transport filters, MIDI, CV and
 * OSC settings, brightness, and what to restore at boot.
 *
 * TWO CONVENTIONS RUN THROUGH THE WHOLE FILE.
 *
 * All OUTPUTS default to OFF. They switch on because somebody asked, never on
 * their own: a bridge that starts emitting clock into a live rig by itself is
 * worse than one that does nothing.
 *
 * Phase offsets are in MICROSECONDS, and there is one per destination — and
 * for OSC, MIDI and CV, one per clock SOURCE as well. That is not
 * over-engineering: the delay to compensate depends on both ends of the chain,
 * so switching bridge direction genuinely changes the right value.
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t settings_init(void);

/* Factory reset: erase the namespace. On the next boot the magic-value check
 * rewrites the complete set of defaults. Call, then RESTART. */
void     settings_erase_all(void);

/* --- bridge phase offsets --------------------------------------------------
 *
 * How much Ableton Link's phase is delayed (+) or advanced (-) relative to
 * DJ Link. Persistent, default 0. */
int  settings_get_phase_offset_dj_us(void);      /* source DJ Link  */
void settings_set_phase_offset_dj_us(int us);
int  settings_get_phase_offset_abl_us(void);     /* source Ableton  */
void settings_set_phase_offset_abl_us(int us);

/* --- WiFi ------------------------------------------------------------------ */

typedef enum {
    WIFI_MODE_PREF_AP = 0,     /* SoftAP: the bridge creates its own network (default) */
    WIFI_MODE_PREF_CLIENT = 1, /* Client: the bridge joins an existing network         */
} wifi_mode_pref_t;

wifi_mode_pref_t settings_get_wifi_mode(void);         /* default: AP */
void             settings_set_wifi_mode(wifi_mode_pref_t m);

/* Client credentials. Copy the saved SSID into out (size >= 33) and the
 * password into out (size >= 65); empty strings when unset. */
void settings_get_wifi_ssid(char *out, int out_size);
void settings_set_wifi_ssid(const char *ssid);
void settings_get_wifi_password(char *out, int out_size);
void settings_set_wifi_password(const char *password);

typedef enum {
    WIFI_POWER_LOW  = 0,
    WIFI_POWER_MID  = 1,       /* default */
    WIFI_POWER_HIGH = 2,
} wifi_power_t;

wifi_power_t settings_get_wifi_power(void);        /* default: MID */
void         settings_set_wifi_power(wifi_power_t p);

/* SoftAP identity, editable, falling back to the compiled defaults —
 * "aggeggino" for the name. Same buffer sizes as the client credentials. */
void settings_get_ap_name(char *out, int out_size);
void settings_set_ap_name(const char *name);
void settings_get_ap_password(char *out, int out_size);
void settings_set_ap_password(const char *password);

/* --- transport propagation -------------------------------------------------
 *
 * Start Sync and Stop Sync are PROPAGATION filters for transport, separate for
 * the two transitions and effective in BOTH directions (DJ Link <-> Ableton).
 *
 * The aggregate's internal state is kept up to date REGARDLESS of these
 * settings: what is decided here is only whether the command goes out.
 *
 * MIND THE SEMANTICS: with Stop Sync enabled, a stop coming from Ableton sends
 * the players back to their cue point, not into pause — that is what fader
 * start does, and it is potentially destructive during a set. */
bool settings_get_start_sync(void);   /* default: false */
void settings_set_start_sync(bool on);
bool settings_get_stop_sync(void);    /* default: false */
void settings_set_stop_sync(bool on);

/* Drift compensation: corrects the tempo written to Ableton Link for the rate
 * difference between our crystal and the master player's. See the DRIFT
 * COMPENSATION block in main.c for why a stored calibration constant is not
 * enough. */
bool settings_get_drift_comp(void);   /* default: false */
void settings_set_drift_comp(bool on);

/* Playlist on the SD card: whether tracks played past the threshold are written
 * to a CSV in the card's root. Off by default. */
bool settings_get_sd_playlist(void);
void settings_set_sd_playlist(bool on);

/* Seconds a track must play continuously before it goes in the playlist, and
 * also the silence that closes one file and starts the next. */
uint8_t settings_get_sd_threshold_s(void);
void    settings_set_sd_threshold_s(uint8_t s);

/* Whether a long press on the touch strip saves a screenshot instead of
 * locking the screen. A developer tool: off by default. */
bool settings_get_screenshots(void);
void settings_set_screenshots(bool on);

/* The OSC custom profile, kept as OPAQUE BYTES: the shape of a message lives
 * in osc_out.h, and this component does not know about it.
 *
 * get returns how many bytes were read, 0 if there is nothing or the buffer
 * is too small. The caller must check the length is the one it expects — a
 * firmware that changed the layout will find a blob of the wrong size, and
 * discarding it beats misreading it field by field.
 *
 * set with len 0 removes the key. */
size_t settings_get_osc_custom(void *out, size_t cap);
void   settings_set_osc_custom(const void *data, size_t len);

/* --- OSC -------------------------------------------------------------------
 *
 * A PRESET defines which messages are sent and in what form. This is not a
 * matter of dialect but of synchronisation METHOD:
 *
 *   GENERIC   one message on EVERY beat, plus tempo and position in the bar.
 *             The phase is ours, bar by bar. Several audio-analysis tools use
 *             this shape; it is not a formal standard, which is why it is
 *             called "generic" rather than named after one of them.
 *   RESOLUME  tempo only, as a float NORMALISED to 0..1 over 20-500 BPM, plus
 *             a resync on the downbeat. Resolume does not accept individual
 *             beats: it keeps the phase, we correct it.
 *   CUSTOM    configurable strings (not implemented yet).
 */
typedef enum {
    OSC_PRESET_GENERIC = 0,
    OSC_PRESET_RESOLUME,
    OSC_PRESET_CUSTOM,
    OSC_PRESET_COUNT
} osc_preset_t;

bool     settings_get_osc_enabled(void);          /* default: false   */
void     settings_set_osc_enabled(bool on);
void     settings_get_osc_host(char *out, size_t len);
void     settings_set_osc_host(const char *host);
uint16_t settings_get_osc_port(void);             /* default: 7000    */
void     settings_set_osc_port(uint16_t p);
uint8_t  settings_get_osc_preset(void);           /* default: GENERIC */
void     settings_set_osc_preset(uint8_t p);

/* src_link = true when the reference clock is Ableton Link, false when it is
 * Pioneer DJ Link. */
int      settings_get_phase_offset_osc_us(bool src_link);
void     settings_set_phase_offset_osc_us(bool src_link, int us);

/* --- MIDI ------------------------------------------------------------------
 *
 * The mDNS announcement is the one exception to the "outputs start off" rule,
 * because it is not an output but a parameter of the network one, and it only
 * matters once that is already on. */
bool     settings_get_midi_net_enabled(void);     /* default: false */
void     settings_set_midi_net_enabled(bool on);
bool     settings_get_midi_net_mdns(void);        /* default: true  */
void     settings_set_midi_net_mdns(bool on);
bool     settings_get_midi_din_enabled(void);     /* default: false */
void     settings_set_midi_din_enabled(bool on);

/* Enabling USB does NOT reinitialise the device — descriptors are read once at
 * enumeration — it only governs whether clock leaves through that port. */
bool     settings_get_midi_usb_enabled(void);     /* default: false */
void     settings_set_midi_usb_enabled(bool on);

int      settings_get_phase_offset_midi_us(bool src_link);
void     settings_set_phase_offset_midi_us(bool src_link, int us);

/* --- CV --------------------------------------------------------------------
 *
 * The CV offset is kept SEPARATE from the MIDI one because the path is a
 * different one: the pulse goes through the synthesiser, which adds a latency
 * of its own to compensate. */
bool     settings_get_cv_enabled(void);          /* default: false */
void     settings_set_cv_enabled(bool on);

/* Indices into tables that live in main; here they are just numbers. */
uint8_t  settings_get_cv_sound(void);            /* default: 0 */
void     settings_set_cv_sound(uint8_t idx);
uint8_t  settings_get_cv_width(void);            /* default: 3, meaning 10 ms  */
void     settings_set_cv_width(uint8_t i);
bool     settings_get_cv_polarity(void);         /* default: false, idle low   */
void     settings_set_cv_polarity(bool idle_high);
uint8_t  settings_get_cv_ppqn(void);             /* default: 0, meaning 1 PPQN */
void     settings_set_cv_ppqn(uint8_t i);

int      settings_get_phase_offset_cv_us(bool src_link);
void     settings_set_phase_offset_cv_us(bool src_link, int us);

/* --- device and boot behaviour --------------------------------------------- */

/* All sounds, including UI feedback. */
bool settings_get_audio_enabled(void);      /* default: true */
void settings_set_audio_enabled(bool on);

/* Remember the bridge state across reboots: when on, the device comes back up
 * as it was left. See the note in settings.c. */
bool     settings_get_remember_bridge(void);     /* default: true */
void     settings_set_remember_bridge(bool on);
void     settings_get_bridge_state(bool *fwd, bool *rev);
void     settings_set_bridge_state(bool fwd, bool rev);

/* DJ Link identity. false = AUTO (claims 1..4, may become master),
 * true = OBSERVER (7..15 only, read-only). See the note in settings.c. */
bool     settings_get_djlink_observer(void);     /* default: false, AUTO */
void     settings_set_djlink_observer(bool on);

/* 180-degree screen rotation, persistent across reboots. */
bool     settings_get_screen_flipped(void);      /* default: false */
void     settings_set_screen_flipped(bool on);

/* Display brightness: three levels only. There is deliberately no OFF — a
 * black screen would be unrecoverable without a reboot. */
typedef enum {
    BRIGHT_LOW  = 0,
    BRIGHT_MED  = 1,
    BRIGHT_HIGH = 2,
} brightness_t;

/* LED strip: the same levels plus OFF, since the strip can safely go dark. */
typedef enum {
    LED_BRIGHT_OFF  = 0,
    LED_BRIGHT_LOW  = 1,
    LED_BRIGHT_MED  = 2,       /* default */
    LED_BRIGHT_HIGH = 3,
} led_brightness_t;

brightness_t settings_get_display_brightness(void);   /* default: HIGH */
void         settings_set_display_brightness(brightness_t b);
led_brightness_t settings_get_led_brightness(void);       /* default: MED  */
void             settings_set_led_brightness(led_brightness_t b);

#ifdef __cplusplus
}
#endif
