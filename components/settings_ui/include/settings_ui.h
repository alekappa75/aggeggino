/* ============================================================================
 * settings_ui.h — the Settings screen.
 *
 * An ISOLATED component: it owns its own LVGL screen and its own navigation
 * state machine. It meets the dashboard (ui.c) at a handful of points:
 *   - settings_ui_init()    creates the screen, once, at start-up
 *   - settings_ui_enter()   enters settings, on a short tap in the centre
 *   - settings_ui_active()  true while settings are on screen, so the
 *                           dashboard knows to route input here
 *   - settings_ui_touch()   feeds it a touch event; the state machine decides.
 *                           Returns to the dashboard when settings close.
 *
 * Visually it belongs to the same family as the dashboard — dark background,
 * same palette, cyan and green accents — but with more air: here one reads and
 * chooses, rather than monitors.
 * ==========================================================================*/
#pragma once
#include <stdbool.h>
#include "osc_out.h"   /* osc_custom_msg_t: the menu edits that list */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the settings screen, hidden. Call once at start-up, after ui_init()
 * so that LVGL is already up. */
void settings_ui_init(void);

/* Info-page data that lives outside the UI: the MAC of the DJ Link interface
 * and the player number we are using. main pushes them when they change, so
 * settings_ui acquires no dependency on the network layer. */
void settings_ui_set_ident(const uint8_t mac[6], const uint8_t ip[4],
                           uint8_t djlink_device);

/* Whether a bridge is carrying tempo in either direction.
 *
 * PUSHED IN BY main RATHER THAN ASKED FOR: the answer lives in ui_clock_src(),
 * and ui already depends on this component — asking would close a cycle. It is
 * the same reason the bridge state is handed over rather than read.
 *
 * Used to bar the operations that would disturb a running bridge: formatting
 * the card holds the SPI bus for seconds, which is not something to do while
 * beats are going out. */
void settings_ui_set_bridge_active(bool active);

/* --- the OSC custom profile ------------------------------------------------
 *
 * The list LIVES IN main, which owns both its persistence and the output it
 * feeds. The menu is given a pointer to it and a way to say "I changed it":
 * copying it here would mean two versions of the same thing, and sooner or
 * later they diverge.
 *
 * save() is called after every edit. It writes to NVS and hands the list to the
 * output in one go, so what is on screen and what goes on the wire cannot
 * differ. */
void settings_ui_set_osc_custom(osc_custom_msg_t *msgs, int *count,
                                void (*save)(void));

#if DJB_DEVELOPER
/* Called when the screenshot option is switched, so whoever keeps a copy of it
 * can update theirs. Developer builds only. */
typedef void (*settings_ui_shots_fn)(bool on);
void settings_ui_set_shots_handler(settings_ui_shots_fn fn);
#endif

/* Enter settings and show the main menu. */
void settings_ui_enter(void);

/* True while settings are on screen. */
bool settings_ui_active(void);

/* Feed a touch event to the settings state machine.
 *   x, y     touch coordinates, meaningful only when pressed is true
 *   pressed  true while a touch is in progress, false on release
 * Hit zones — arrows at the bottom, taps on entries, centre tap for back — are
 * handled internally. Call ONLY while settings_ui_active() is true. */
void settings_ui_touch(uint16_t x, uint16_t y, bool pressed);

/* Forget the touch in progress: its release has been taken by somebody else
 * and will never arrive. Without this the next release, whatever it belongs
 * to, is read as the end of the abandoned press. */
void settings_ui_touch_cancel(void);

/* True while the About intro is playing. It reads the touch itself, on its own
 * task, so a long press there belongs to it alone — taken as a screenshot as
 * well, the two answers would sound one over the other. */
bool settings_ui_about_running(void);

/* --- handlers registered by main -------------------------------------------
 *
 * Everything below inverts a dependency: settings_ui knows WHEN something was
 * chosen, main knows WHAT to do about it. Without these the settings screen
 * would have to reach into the network, the clock and the storage layers. */

/* The user changed WiFi mode, after debounce. main runs the hot-switch.
 * Parameter: the new mode — true = client, false = SoftAP. */
typedef void (*settings_ui_mode_handler_t)(bool client);
void settings_ui_set_mode_handler(settings_ui_mode_handler_t fn);

/* The user confirmed a WiFi password. main stores the credentials and starts
 * the connection. */
typedef void (*settings_ui_connect_handler_t)(const char *ssid, const char *password);
void settings_ui_set_connect_handler(settings_ui_connect_handler_t fn);

/* The SoftAP name or password changed, and actually differs, so main can
 * recreate the access point at runtime with the new credentials. */
typedef void (*settings_ui_ap_restart_handler_t)(void);
void settings_ui_set_ap_restart_handler(settings_ui_ap_restart_handler_t fn);


/* Drift compensation was toggled. The persistent state lives in settings; this
 * only tells main that it changed NOW, so it can reset its estimate instead of
 * discovering it on the next pass. */
typedef void (*settings_ui_drift_handler_t)(bool on);
void settings_ui_set_drift_handler(settings_ui_drift_handler_t fn);

/* Network MIDI output: something changed (Enabled, or Announce).
 *
 * No value is passed, because the network host reads its WHOLE configuration
 * at start-up: the only way to apply a change is to restart it. The recipient
 * must not stop and restart it here — that would happen on the UI task, and
 * would freeze the interface for as long as closing sockets and tasks takes.
 * It should flag that a restart is due and let the loop that owns the host's
 * lifetime perform it. */
typedef void (*settings_ui_midinet_handler_t)(void);
void settings_ui_set_midinet_handler(settings_ui_midinet_handler_t fn);

/* Result of the last electrical detection of the MIDI DIN module, shown in the
 * menu.
 *
 * [M] The detection is validated in the field, and it measures something more
 * precise than "the module is plugged in": it reports whether the module is
 * present AND in SEPARATE mode, which is the only configuration in which our
 * TX actually reaches the DIN OUT. See midi_din_detect() in main.c. */
void settings_ui_set_din_detected(bool seen);

/* A USB host has enumerated the MIDI device. Shown in the menu. */
void settings_ui_set_usb_mounted(bool seen);

/* The CV sound table, owned by main, which knows the notes and the names.
 * settings_ui only receives a way to read it, so it can show the name matching
 * the stored index WITHOUT delay — the value appears the instant it is
 * touched, not on the next pass of the loop. */
typedef const char *(*settings_ui_sound_name_fn)(int idx);
void settings_ui_set_sound_table(settings_ui_sound_name_fn fn, int count);


#ifdef __cplusplus
}
#endif
