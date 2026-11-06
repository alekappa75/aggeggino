/*
 * linkbridge.c — see linkbridge.h. Uses the official abl_link C API.
 *
 * MEMORY, AND WHY THE SESSION STATE IS CREATED ONCE. It is created a single
 * time and reused. An earlier version created and destroyed it on every call:
 * with update_clock at 20 Hz plus one call per beat, that was over twenty
 * create/destroy cycles per second. Besides being wasteful, [M] it exposed
 * heap consumption that grew whenever a peer was in session — heap fell from
 * about 54k to 18k across two Link on/off cycles. With a persistent session
 * state we only capture and commit, with no repeated allocation.
 *
 * Access to the shared session state is serialised by a mutex.
 */
#include "linkbridge.h"
#include "abl_link.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>

static const char *TAG = "linkbridge";
static abl_link               s_link;
static abl_link_session_state s_ss;          /* reused, never destroyed */
static SemaphoreHandle_t      s_lock;

static inline void lb_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void lb_unlock(void) { xSemaphoreGive(s_lock); }

esp_err_t linkbridge_start(double initial_bpm)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_link = abl_link_create(initial_bpm);
    s_ss   = abl_link_create_session_state();   /* ONCE, for the whole life */
    abl_link_enable(s_link, true);              /* begin discovery and sync */
    ESP_LOGI(TAG, "Ableton Link enabled (initial tempo %.2f BPM)", initial_bpm);
    return ESP_OK;
}

void linkbridge_set_enabled(bool on)
{
    if (!s_lock) return;   /* not started yet (s_lock is created in _start) */
    /* Toggle discovery/sync without touching the session state (kept alive for
     * the whole life to avoid the memory churn of recreating it). */
    lb_lock();
    abl_link_enable(s_link, on);
    lb_unlock();
    ESP_LOGI(TAG, "Ableton Link %s", on ? "enabled" : "disabled (WiFi transition)");
}

void linkbridge_set_tempo(double bpm)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    abl_link_set_tempo(s_ss, bpm, esp_timer_get_time());
    abl_link_commit_app_session_state(s_link, s_ss);
    lb_unlock();
}

int linkbridge_num_peers(void)
{
    return (int)abl_link_num_peers(s_link);
}

double linkbridge_tempo(void)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    double t = abl_link_tempo(s_ss);
    lb_unlock();
    return t;
}

double linkbridge_beat(double quantum)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    double b = abl_link_beat_at_time(s_ss, esp_timer_get_time(), quantum);
    lb_unlock();
    return b;
}

int64_t linkbridge_clock_now(void)
{
    return abl_link_clock_micros(s_link);
}

/* ABSOLUTE beat at an instant of our choosing. It exists to measure the rate
 * at which the session timeline advances against OUR clock: sampling it at two
 * instants yields Link's effective BPM, which with peers connected does not
 * necessarily match the one that was set — the library compensates for the
 * drift between the participants' clocks. */
double linkbridge_beat_at(int64_t time_us, double quantum)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    double b = abl_link_beat_at_time(s_ss, time_us, quantum);
    lb_unlock();
    return b;
}

double linkbridge_phase_at(int64_t time_us, double quantum)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    double p = abl_link_phase_at_time(s_ss, time_us, quantum);
    lb_unlock();
    return p;
}

void linkbridge_align_downbeat(int64_t downbeat_time_us, double quantum, bool force)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    /* Nearest beat that is a multiple of the quantum: phase 0 at that instant. */
    double cur    = abl_link_beat_at_time(s_ss, downbeat_time_us, quantum);
    double target = round(cur / quantum) * quantum;
    if (force) abl_link_force_beat_at_time(s_ss, target, downbeat_time_us, quantum);
    else       abl_link_request_beat_at_time(s_ss, target, downbeat_time_us, quantum);
    abl_link_commit_app_session_state(s_link, s_ss);
    lb_unlock();
}

void linkbridge_align_beat(int64_t time_us, double phase, double quantum, bool force)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    double cur    = abl_link_beat_at_time(s_ss, time_us, quantum);
    /* Beat near 'cur' whose phase against the quantum equals 'phase'. */
    double target = round((cur - phase) / quantum) * quantum + phase;
    if (force) abl_link_force_beat_at_time(s_ss, target, time_us, quantum);
    else       abl_link_request_beat_at_time(s_ss, target, time_us, quantum);
    abl_link_commit_app_session_state(s_link, s_ss);
    lb_unlock();
}

void linkbridge_set_tempo_align(double bpm, int64_t time_us, double phase,
                                double quantum, bool do_phase, bool force)
{
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    int64_t now = abl_link_clock_micros(s_link);
    abl_link_set_tempo(s_ss, bpm, now);                 /* 1) tempo */
    if (do_phase) {                                      /* 2) phase, after tempo */
        double cur    = abl_link_beat_at_time(s_ss, time_us, quantum);
        double target = round((cur - phase) / quantum) * quantum + phase;
        if (force) abl_link_force_beat_at_time(s_ss, target, time_us, quantum);
        else       abl_link_request_beat_at_time(s_ss, target, time_us, quantum);
    }
    abl_link_commit_app_session_state(s_link, s_ss);     /* 3) single commit */
    lb_unlock();
}

/* ============================================================================
 * TRANSPORT (Start Stop Sync)
 * ==========================================================================*/

void linkbridge_set_start_stop_sync(bool on)
{
    if (!s_lock) return;            /* not started yet */
    /* Idempotent: the two filters, Start Sync and Stop Sync, open the SAME
     * channel, so this is also called when nothing changes. The real state is
     * read back from the library rather than mirrored locally, and action is
     * taken only when needed — that way the log reflects genuine changes. */
    lb_lock();
    bool changed = (abl_link_is_start_stop_sync_enabled(s_link) != on);
    if (changed) abl_link_enable_start_stop_sync(s_link, on);
    lb_unlock();
    if (changed) ESP_LOGI(TAG, "Start Stop Sync %s", on ? "enabled" : "disabled");
}

bool linkbridge_start_stop_sync_enabled(void)
{
    if (!s_lock) return false;
    lb_lock();
    bool on = abl_link_is_start_stop_sync_enabled(s_link);
    lb_unlock();
    return on;
}

void linkbridge_set_playing(bool playing, double quantum, bool align)
{
    if (!s_lock) return;
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    int64_t now = abl_link_clock_micros(s_link);
    if (playing && align) {
        /* Aligned start: the library starts transport and maps the given beat
         * onto the start instant in a single transaction. We ask for beat 0,
         * the DOWNBEAT — the same convention linkbridge_align_downbeat uses —
         * so Ableton starts on the beat rather than with whatever offset the
         * session happens to have. */
        abl_link_set_is_playing_and_request_beat_at_time(s_ss, true, now, 0.0, quantum);
    } else {
        abl_link_set_is_playing(s_ss, playing, now);
    }
    abl_link_commit_app_session_state(s_link, s_ss);
    lb_unlock();
}

bool linkbridge_is_playing(void)
{
    if (!s_lock) return false;
    lb_lock();
    abl_link_capture_app_session_state(s_link, s_ss);
    bool p = abl_link_is_playing(s_ss);
    lb_unlock();
    return p;
}

/* Bridge between the library's C callback, which carries a void* context, and
 * our simplified signature. The callback runs on a Link thread: whoever
 * registers one must do nothing but signal the event. */
static void (*s_ss_cb)(bool) = NULL;

static void ss_trampoline(bool is_playing, void *context)
{
    (void)context;
    if (s_ss_cb) s_ss_cb(is_playing);
}

void linkbridge_set_start_stop_callback(void (*cb)(bool is_playing))
{
    if (!s_lock) return;
    s_ss_cb = cb;
    abl_link_set_start_stop_callback(s_link, cb ? ss_trampoline : NULL, NULL);
}
