/* sdcard — the microSD slot, and the playlist written to it.
 *
 * ============================================================================
 * IT SHARES THE SPI BUS WITH THE DISPLAY AND THE W5500
 *
 * That is the whole difficulty, and no amount of task priority fixes it: a
 * low-priority task holding the SPI lock blocks the beat exactly as a
 * high-priority one would. Worse, the duration is not ours to choose — a card
 * doing wear levelling holds the bus for as long as it takes, and [D] that can
 * reach hundreds of milliseconds against a beat jitter budget under 1.3 ms.
 *
 * So the lever is WHEN, not with what priority:
 *
 *   - never while the status cadence is fast. [M] A synced player does not
 *     chase the tempo with a control loop, it READS the pitch from the master's
 *     status and applies it — so its promptness is exactly the rate at which we
 *     speak, and blocking three status packets is three status packets of lag
 *     in every player following us
 *   - as master, only just after a beat has left, which gives the whole beat
 *     period before the next one is due
 *   - as anything else there is no constraint: we emit no beats, and a late
 *     drain is absorbed by the W5500's buffers without disturbing the phase,
 *     because the beat instant is marked by the ISR and not by the drain
 *
 * A playlist line is the least urgent thing this device does. Waiting for a
 * relaxed moment costs nothing: the track has already been playing for a
 * minute, and writing it down a few seconds later changes nothing.
 *
 * ============================================================================
 * WHAT GOES IN THE PLAYLIST
 *
 * A track counts as played once it has been in CONTINUOUS PLAYBACK past the
 * threshold set in the menu, which also decides when one playlist file ends: a
 * silence longer than that closes it, and the next track opens a new one.
 *
 * The criterion is the same idea Beat Link Trigger uses, and it works in both
 * bridge directions because it reads the players' status rather than asking
 * who is master.
 *
 * The state is kept PER PLAYER: each has its own stopwatch and remembers the
 * last track it wrote. Pausing and resuming the same track on the same player
 * gives ONE line; A, B, A gives two, because B displaced the memory; the same
 * track on two players gives two lines, and the device column says which.
 * ==========================================================================*/
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* What the dashboard badge shows. */
typedef enum {
    SDCARD_ABSENT = 0,   /* no card in the slot        -> badge hidden */
    SDCARD_ERROR,        /* card present, unusable     -> red          */
    SDCARD_IDLE,         /* mounted, nothing to do     -> grey         */
    SDCARD_WRITING,      /* mounted, playlist enabled  -> green        */
} sdcard_state_t;

/* Mounts the card if one is there. Safe to call with no card: it reports
 * SDCARD_ABSENT and nothing else happens. */
esp_err_t sdcard_init(void);

sdcard_state_t sdcard_state(void);

/* Tells the card's task whether this is a good moment to hold the SPI bus.
 *
 * The caller knows things this component does not — whether we are master,
 * whether a beat has just left, whether the status cadence is chasing a tempo
 * change. Call it from the slow loop; the task reads it when it needs to.
 *
 * IT IS NOT THE ONLY PROTECTION, and not the main one: every card operation
 * waits out the quiet window before taking the bus, exactly as a display flush
 * does. This only keeps the long operations — a mount attempt, later a write —
 * out of the moments when a tempo change is being propagated.
 *
 * NOTICING THE CARD GO AND COME BACK IS ASYMMETRIC:
 *
 *   removal    a CMD13 to a mounted card, a few bytes, [D] some 50 us. Cheap
 *              enough to ask every couple of seconds regardless
 *   insertion  THERE IS NO SEPARATE DETECTION. With no card-detect line, the
 *              only way to know a card is there is to talk to it, and talking
 *              to it means beginning to initialise it: the mount attempt IS the
 *              detection
 *
 * [M] AN EMPTY-SLOT ATTEMPT COSTS 23 ms, measured on hardware, and [M] it
 * barely touches the bus: with the card polling every 3 s, the longest wait
 * seen on the network path was 347 us and there were no waits over 500 us at
 * all. The time is spent waiting for a card that will not answer, not holding
 * the wire. */
void sdcard_set_relaxed(bool relaxed);

/* --- FORMATTING -------------------------------------------------------------
 *
 * ASKED FOR HERE, DONE IN THE TASK, because it takes SECONDS: run from a touch
 * handler it would block the interface for its whole duration, and the very
 * line telling the user what is happening would never be drawn.
 *
 * sdcard_request_format() returns at once, with the state already BUSY so the
 * next redraw shows it. sdcard_format_state() is what to put on screen.
 *
 * ONLY ON AN EXPLICIT REQUEST, and never as a way out of a failed mount. It
 * holds the SPI bus throughout — the caller must make sure no bridge is
 * carrying tempo, because nothing else can use the bus meanwhile. */
typedef enum {
    SDCARD_FMT_NONE = 0,   /* nothing to say                           */
    SDCARD_FMT_BUSY,       /* running: the bus is held                 */
    SDCARD_FMT_DONE,       /* finished; shown until the screen is left */
    SDCARD_FMT_FAILED,     /* the card is out of use until it is taken out */
} sdcard_format_t;

void sdcard_request_format(void);
sdcard_format_t sdcard_format_state(void);

/* The outcome has been seen: clears DONE or FAILED so the row goes back to
 * normal. Call it on leaving the screen that shows it — a format takes
 * minutes and whoever starts one goes away meanwhile, so a message that
 * expires on a timer is one nobody reads. */
void sdcard_format_ack(void);

#if DJB_DEVELOPER
/* Saves the screen to SCRN####.BMP. Returns false with no card, or on a write
 * failure.
 *
 * BMP BECAUSE THE FRAMEBUFFER IS ALREADY RGB565 and a 16-bit BMP takes those
 * bytes verbatim: a header and the pixels, no compression, no library. It
 * holds the bus for the length of one write — 150 KB, a row at a time. */
bool sdcard_save_screenshot(void);
#endif

/* Tells the playlist what a player is doing. Called wherever the players'
 * status is decoded, once per status packet.
 *
 * device      the player, 1..4
 * playing     is it playing right now
 * track_id    rekordbox id, 0 when unknown
 */
void sdcard_note_player(uint8_t device, bool playing, uint32_t track_id,
                        uint8_t media_dev, uint8_t media_slot,
                        uint8_t media_type, double bpm);

/* Is a title wanted, and for which track? Fills the four values and returns
 * true when the caller should go and ask.
 *
 * ASKED ONLY FOR TRACKS GOING INTO THE FILE, once each. Metadata are a TCP
 * conversation with a player — [M] some 200 ms — and asking for every track as
 * it is loaded would mean asking for everything a DJ auditions and drops. A
 * track past the threshold was actually played, and there is at most one of
 * those a minute per player.
 *
 * The answer comes back through sdcard_note_metadata; if it does not come at
 * all the line is written with its id alone. */
bool sdcard_wants_metadata(uint8_t *player, uint8_t *dev, uint8_t *slot,
                           uint8_t *type, uint32_t *id);

/* The title for a track, when it becomes known.
 *
 * [M] Metadata come some ten seconds after the status that announced the track,
 * and the threshold is a minute: they are known long BEFORE the line is
 * prepared. They are kept until it needs them, and a line already waiting is
 * filled in as well.
 *
 * Fetched for the master alone, so a second player's track goes in with its id
 * and no title — which still identifies it in rekordbox afterwards. */
void sdcard_note_metadata(uint32_t track_id, const char *title, const char *artist);
