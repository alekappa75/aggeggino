/*
 * linkbridge.h — wrapper around esp_abl_link (Ableton Link).
 *
 * The device joins the Link session as a peer. It lives on WiFi/lwIP (SoftAP),
 * entirely separate from the W5500, which runs its own hardware stack. With a
 * single lwIP interface, Link's multicast binding is unambiguous — the two
 * protocols genuinely live on two different networks, which is also why the
 * bridge has to sit on both.
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create and enable the Link peer with an initial tempo.
 *
 * An initial BPM is mandatory: abl_link has no constructor without one. The
 * instance starts disabled and silent on the network, so that tempo is not
 * imposed on anybody until it is enabled. */
esp_err_t linkbridge_start(double initial_bpm);

/* Enable or disable Link discovery and sync without destroying session state.
 * Used by the WiFi hot-switch: disable before changing network interface,
 * re-enable once the new one is up. */
void linkbridge_set_enabled(bool on);

/* Impose a tempo on the Link session. */
void linkbridge_set_tempo(double bpm);

/* Number of Link peers currently in session (Live, Push, ...). */
int linkbridge_num_peers(void);

/* Current session tempo, in BPM. */
double linkbridge_tempo(void);

/* Current beat position relative to the quantum, for displaying phase. */
double linkbridge_beat(double quantum);

/* --- phase --------------------------------------------------------------- */

/* Link's clock, in microseconds. Same value as esp_timer, but taken through
 * the library's API for consistency with the timestamps it returns. */
int64_t linkbridge_clock_now(void);

/* Session beat and phase at a given instant; phase is in [0, quantum). */
double  linkbridge_beat_at(int64_t time_us, double quantum);
double  linkbridge_phase_at(int64_t time_us, double quantum);

/* Align Link's grid so that a DOWNBEAT (phase 0) falls at the given instant.
 * force=false requests it politely, quantised; force=true imposes it on the
 * session, possibly with a discontinuity. */
void    linkbridge_align_downbeat(int64_t downbeat_time_us, double quantum, bool force);

/* Align Link's grid so that at 'time_us' the phase equals 'phase', in
 * [0, quantum). Same force/request distinction as above. */
void    linkbridge_align_beat(int64_t time_us, double phase, double quantum, bool force);

/* Set tempo AND phase in a SINGLE transaction, tempo first, so the two cannot
 * disturb each other. do_phase=false sets tempo only. */
void    linkbridge_set_tempo_align(double bpm, int64_t time_us, double phase,
                                   double quantum, bool do_phase, bool force);

/* --- transport (Start Stop Sync) -------------------------------------------
 *
 * Link's start/stop sync is opt-in on EVERY peer: transport state is shared
 * only among peers that enabled it (in Live it is the "Start Stop Sync"
 * checkbox). It is BIDIRECTIONAL by nature, so enabling it also delivers other
 * peers' events to us — which we cannot forward to the CDJs, because DJ Link
 * exposes no remote play. We therefore ignore them. */

/* Enable or disable sharing transport state with peers. When off, we do not
 * touch session transport at all, not even passively. */
void linkbridge_set_start_stop_sync(bool on);

/* True when start/stop sync is enabled in the library. */
bool linkbridge_start_stop_sync_enabled(void);

/* Start or stop the Link session transport.
 *
 * On START, align=true also asks for the indicated beat to fall on the start
 * instant — Link's "quantized launch", which avoids starting out of phase.
 * With align=false the state simply changes, keeping whatever offset the
 * session happens to have. */
void linkbridge_set_playing(bool playing, double quantum, bool align);

/* Current transport state of the Link session. */
bool linkbridge_is_playing(void);

/* Register a callback for transport changes coming from the session, that is
 * from Ableton or another peer.
 *
 * WARNING: it is invoked on a thread owned by Link, so it must not do heavy
 * work or touch peripherals. Signal from here, act from the main loop. */
void linkbridge_set_start_stop_callback(void (*cb)(bool is_playing));

#ifdef __cplusplus
}
#endif
