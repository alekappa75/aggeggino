/*
 * osc_out.h — OSC (Open Sound Control) output over UDP/WiFi.
 *
 * OUTPUT ONLY: the bridge sends, it never listens. The source is whichever
 * clock the active bridge direction provides (Pioneer DJ Link or Ableton
 * Link); that choice is made upstream by the caller.
 *
 * This module knows nothing about phase or timers. It is told "a beat is
 * happening now, at this tempo, at this position in the bar" and it formats
 * and sends. WHEN that happens belongs to the caller, which owns the timer and
 * the offset.
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A preset defines WHAT is sent, not merely how it is spelled: these are two
 * different synchronisation methods, not two dialects of one. See settings.h
 * for the reasoning. */
typedef enum {
    OSC_OUT_GENERIC = 0,   /* one message per beat, plus tempo and bar     */
    OSC_OUT_RESOLUME,      /* normalised tempo, plus resync on the downbeat */
    OSC_OUT_CUSTOM,        /* a list of messages built by the user          */
} osc_out_preset_t;

/* ===========================================================================
 * THE CUSTOM PROFILE
 *
 * A list of messages. Each says WHEN it goes out, WHERE it goes, and WHAT it
 * carries — and the last two may contain PLACEHOLDERS, written as {name}, which
 * are replaced at send time.
 *
 * WHY PLACEHOLDERS AND NOT A FIXED FIELD. A metadatum is a string and can serve
 * in either position: /track/{artist} addresses, {title} as the value carries.
 * Different software wants one or the other, and a design that allowed only the
 * second would rule out half of them.
 *
 * They are CHOSEN rather than typed, from a mode of the on-screen keyboard, so
 * there is no syntax to remember and no misspelling that would fail silently —
 * an OSC packet with a wrong path leaves and nobody answers.
 *
 * AN UNKNOWN PLACEHOLDER BECOMES AN EMPTY STRING and does not block the send:
 * asking for {title} on every beat works, and in the first seconds it is
 * simply blank.
 * ==========================================================================*/
#define OSC_CUSTOM_MAX    8      /* messages: beyond this the list is unwieldy */
#define OSC_PATH_MAX     64      /* /composition/tempocontroller/tempo is 36  */
#define OSC_VALUE_MAX    40      /* a value with a placeholder or two          */

/* WHEN a message goes out. All of them derive from what the bridge already
 * knows: the beat engine gives the first three, the metadata table the fourth,
 * the transport the fifth. */
typedef enum {
    OSC_TRIG_BEAT = 0,     /* every beat                                     */
    OSC_TRIG_DOWNBEAT,     /* the first beat of the bar                      */
    OSC_TRIG_TEMPO,        /* the tempo changed                              */
    OSC_TRIG_TRACK,        /* a player loaded another track                  */
    OSC_TRIG_TRANSPORT,    /* play or stop                                   */
    OSC_TRIG_COUNT
} osc_trigger_t;

/* HOW the value travels. DEDUCED from the value itself rather than chosen: see
 * the rule beside fmt_deduce in osc_out.c. It stays an enum because the sending
 * code needs to name the cases. */
typedef enum {
    OSC_FMT_NONE = 0,      /* no argument: the path alone                    */
    OSC_FMT_INT,
    OSC_FMT_FLOAT,
    OSC_FMT_STRING,
    OSC_FMT_COUNT
} osc_fmt_t;

typedef struct {
    uint8_t trigger;                 /* osc_trigger_t                        */
    uint8_t reserved;                /* keeps the struct aligned; was fmt    */
    char    path[OSC_PATH_MAX];      /* empty = the slot is unused           */
    char    value[OSC_VALUE_MAX];
} osc_custom_msg_t;

/* Hands over the whole list. Safe to call again whenever the user edits it. */
void osc_out_set_custom(const osc_custom_msg_t *msgs, int n);

/* The names a placeholder may take, for the keyboard that offers them and for
 * whoever documents them. Returns NULL past the end. */
const char *osc_out_placeholder(int i);

/* Open the UDP socket. Call once at start-up; if it fails the output stays
 * mute and osc_out_send_beat() does nothing. */
esp_err_t osc_out_init(void);

/* Destination and preset. Safe to call again on every settings change: if the
 * address is not valid the output disables itself. */
void osc_out_configure(const char *host, uint16_t port,
                       osc_out_preset_t preset, bool enabled);

/* True when the output is configured and enabled. The UI uses this to decide
 * whether to offer the OSC offset among the adjustable ones. */
bool osc_out_active(void);

/* Fine-grained status for the indicator, which must tell apart two different
 * failures: no socket (a real error, nothing the user can do) versus an
 * invalid destination (a configuration mistake, fixable from the menu).
 * osc_out_active() conflates them, because all it needs to know is whether
 * anything can be sent. */
bool osc_out_socket_ok(void);
bool osc_out_dest_ok(void);

/* Release the UDP socket. Call when the output is disabled: the lwIP
 * descriptor pool is a scarce resource. */
void osc_out_close_socket(void);

/* A beat has just landed. bpm is the current tempo, bb the position in the bar
 * (1..4). Call it AT the right instant: this sends immediately, it does not
 * queue and does not delay. */
void osc_out_send_beat(double bpm, uint8_t bb);

/* --- events the custom profile can react to --------------------------------
 *
 * These are ignored by the two fixed profiles, which send on the beat alone.
 *
 * THE METADATA ARE PUSHED IN rather than fetched: osc_out would otherwise need
 * to know about the player table, the dbserver and the whole chain behind them.
 * It holds the last values it was given and substitutes them when a message
 * asks for one. */
void osc_out_note_track(uint8_t player, uint32_t id,
                        const char *title, const char *artist);

/* Play or stop on the active source. Only a CHANGE sends anything. */
void osc_out_note_transport(bool playing);

/* Diagnostics: packets sent, and send errors. */
void osc_out_stats(uint32_t *sent, uint32_t *err);

#ifdef __cplusplus
}
#endif
