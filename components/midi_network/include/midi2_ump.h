/*
 * midi2_ump.h - Universal MIDI Packet (UMP) message helpers
 *
 * Small, self-contained builders and parsers for the UMP message format used
 * by MIDI 2.0. This layer knows nothing about the network transport; it only
 * assembles/inspects 32-bit UMP words. The transport (midi2_net) carries these
 * words verbatim.
 *
 * UMP reference: MIDI Association "Universal MIDI Packet (UMP) Format and
 * MIDI 2.0 Protocol" (M2-104-UM). Words are host-native uint32_t here; the
 * transport is responsible for placing them on the wire in network byte order.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UMP Message Type (top nibble of the first word). Determines packet size. */
typedef enum {
    MIDI2_UMP_MT_UTILITY        = 0x0, /* 1 word  */
    MIDI2_UMP_MT_SYSTEM_RT      = 0x1, /* 1 word  - system real time / common */
    MIDI2_UMP_MT_MIDI1_CV       = 0x2, /* 1 word  - MIDI 1.0 channel voice */
    MIDI2_UMP_MT_DATA64         = 0x3, /* 2 words - SysEx7 */
    MIDI2_UMP_MT_MIDI2_CV       = 0x4, /* 2 words - MIDI 2.0 channel voice */
    MIDI2_UMP_MT_DATA128        = 0x5, /* 4 words - SysEx8 / mixed data */
    MIDI2_UMP_MT_FLEX_DATA      = 0xD, /* 4 words */
    MIDI2_UMP_MT_STREAM         = 0xF, /* 4 words - UMP Stream (endpoint disc) */
} midi2_ump_mt_t;

/* MIDI 2.0 channel-voice status nibbles (bits 23..20 of word0). */
#define MIDI2_UMP_STATUS_NOTE_OFF          0x8
#define MIDI2_UMP_STATUS_NOTE_ON           0x9
#define MIDI2_UMP_STATUS_POLY_PRESSURE     0xA
#define MIDI2_UMP_STATUS_CC                0xB
#define MIDI2_UMP_STATUS_PROGRAM_CHANGE    0xC
#define MIDI2_UMP_STATUS_CHANNEL_PRESSURE  0xD
#define MIDI2_UMP_STATUS_PITCH_BEND        0xE

/* System real-time status bytes (carried whole in MIDI2_UMP_MT_SYSTEM_RT). */
#define MIDI2_UMP_SYS_TIMING_CLOCK   0xF8
#define MIDI2_UMP_SYS_START          0xFA
#define MIDI2_UMP_SYS_CONTINUE       0xFB
#define MIDI2_UMP_SYS_STOP           0xFC
#define MIDI2_UMP_SYS_ACTIVE_SENSING 0xFE
#define MIDI2_UMP_SYS_RESET          0xFF

/* Note Attribute types (MIDI 2.0). 0 = none. */
#define MIDI2_UMP_ATTR_NONE          0x00
#define MIDI2_UMP_ATTR_PITCH_7_9     0x03

/* Largest single UMP message is 128 bits = 4 words. */
#define MIDI2_UMP_MAX_MESSAGE_WORDS  4

/* ---- Inspection ---------------------------------------------------------- */

/* Message type (top nibble) of a first UMP word. */
static inline uint8_t midi2_ump_message_type(uint32_t word0)
{
    return (uint8_t)((word0 >> 28) & 0x0F);
}

/* Group (second nibble, 0..15) of a first UMP word. */
static inline uint8_t midi2_ump_group(uint32_t word0)
{
    return (uint8_t)((word0 >> 24) & 0x0F);
}

/*
 * Number of 32-bit words occupied by the UMP message that starts with word0,
 * derived from its message type. Returns 1, 2, or 4. Never returns 0.
 * This is the canonical way to walk a UMP stream of concatenated messages.
 */
size_t midi2_ump_message_num_words(uint32_t word0);

/* ---- MIDI 2.0 channel-voice builders (message type 0x4, 2 words) -------- */
/*
 * group:   0..15 (wire value; the user-facing "Group N" label is group+1)
 * channel: 0..15
 * Each builder writes exactly 2 words into out[] and returns 2.
 */
size_t midi2_ump_note_on(uint32_t out[2], uint8_t group, uint8_t channel,
                         uint8_t note, uint16_t velocity,
                         uint8_t attr_type, uint16_t attr_data);

size_t midi2_ump_note_off(uint32_t out[2], uint8_t group, uint8_t channel,
                          uint8_t note, uint16_t velocity,
                          uint8_t attr_type, uint16_t attr_data);

/* Control Change with full 32-bit value. */
size_t midi2_ump_control_change(uint32_t out[2], uint8_t group, uint8_t channel,
                                uint8_t controller, uint32_t value);

/* Channel Pressure (aftertouch) with full 32-bit value. */
size_t midi2_ump_channel_pressure(uint32_t out[2], uint8_t group, uint8_t channel,
                                  uint32_t value);

/* Program Change. If bank_valid, the 14-bit bank (bank_msb/lsb) is included. */
size_t midi2_ump_program_change(uint32_t out[2], uint8_t group, uint8_t channel,
                                uint8_t program, bool bank_valid,
                                uint8_t bank_msb, uint8_t bank_lsb);

/* Pitch Bend with full 32-bit value (0x80000000 = center). */
size_t midi2_ump_pitch_bend(uint32_t out[2], uint8_t group, uint8_t channel,
                            uint32_t value);

/* ---- System real-time (message type 0x1, 1 word) ------------------------ */
size_t midi2_ump_system_rt(uint32_t out[1], uint8_t group, uint8_t status);

/* ======================================================================== *
 *  Full UMP message API (all message types, MIDI 2.0 spec M2-104-UM v1.1.2)
 * ======================================================================== */

/* ---- Utility messages (message type 0x0, 1 word, groupless) ------------- */
#define MIDI2_UMP_UTILITY_NOOP             0x0
#define MIDI2_UMP_UTILITY_JR_CLOCK         0x1
#define MIDI2_UMP_UTILITY_JR_TIMESTAMP     0x2
#define MIDI2_UMP_UTILITY_DCTPQ            0x3  /* Delta Clockstamp Ticks Per Quarter Note */
#define MIDI2_UMP_UTILITY_DELTA_CLOCKSTAMP 0x4

size_t midi2_ump_utility(uint32_t out[1], uint8_t status, uint32_t data20);
size_t midi2_ump_noop(uint32_t out[1]);
size_t midi2_ump_jr_clock(uint32_t out[1], uint16_t time);
size_t midi2_ump_jr_timestamp(uint32_t out[1], uint16_t time);
size_t midi2_ump_dctpq(uint32_t out[1], uint16_t ticks_per_quarter_note);
size_t midi2_ump_delta_clockstamp(uint32_t out[1], uint32_t ticks20);

/* ---- System Common (message type 0x1, 1 word) --------------------------- */
#define MIDI2_UMP_SYS_MTC            0xF1
#define MIDI2_UMP_SYS_SONG_POSITION  0xF2
#define MIDI2_UMP_SYS_SONG_SELECT    0xF3
#define MIDI2_UMP_SYS_TUNE_REQUEST   0xF6
size_t midi2_ump_system_common(uint32_t out[1], uint8_t group, uint8_t status,
                               uint8_t data1, uint8_t data2);
size_t midi2_ump_mtc(uint32_t out[1], uint8_t group, uint8_t time_code);
size_t midi2_ump_song_position(uint32_t out[1], uint8_t group, uint16_t position14);
size_t midi2_ump_song_select(uint32_t out[1], uint8_t group, uint8_t song);
size_t midi2_ump_tune_request(uint32_t out[1], uint8_t group);

/* ---- MIDI 1.0 Channel Voice (message type 0x2, 1 word) ------------------ */
size_t midi2_ump_midi1_cv(uint32_t out[1], uint8_t group, uint8_t status,
                          uint8_t channel, uint8_t data1, uint8_t data2);
size_t midi2_ump_midi1_note_on(uint32_t out[1], uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity);
size_t midi2_ump_midi1_note_off(uint32_t out[1], uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity);
size_t midi2_ump_midi1_poly_pressure(uint32_t out[1], uint8_t group, uint8_t channel, uint8_t note, uint8_t pressure);
size_t midi2_ump_midi1_control_change(uint32_t out[1], uint8_t group, uint8_t channel, uint8_t controller, uint8_t value);
size_t midi2_ump_midi1_program_change(uint32_t out[1], uint8_t group, uint8_t channel, uint8_t program);
size_t midi2_ump_midi1_channel_pressure(uint32_t out[1], uint8_t group, uint8_t channel, uint8_t pressure);
size_t midi2_ump_midi1_pitch_bend(uint32_t out[1], uint8_t group, uint8_t channel, uint16_t bend14);

/* ---- MIDI 2.0 Channel Voice - additional status types (2 words) --------- */
#define MIDI2_UMP_STATUS_REG_PERNOTE   0x0  /* Registered Per-Note Controller */
#define MIDI2_UMP_STATUS_ASN_PERNOTE   0x1  /* Assignable Per-Note Controller */
#define MIDI2_UMP_STATUS_RPN           0x2  /* Registered Controller (RPN) */
#define MIDI2_UMP_STATUS_NRPN          0x3  /* Assignable Controller (NRPN) */
#define MIDI2_UMP_STATUS_REL_RPN       0x4  /* Relative Registered Controller */
#define MIDI2_UMP_STATUS_REL_NRPN      0x5  /* Relative Assignable Controller */
#define MIDI2_UMP_STATUS_PERNOTE_PITCH 0x6  /* Per-Note Pitch Bend */
#define MIDI2_UMP_STATUS_PERNOTE_MGMT  0xF  /* Per-Note Management */
#define MIDI2_UMP_PERNOTE_RESET        0x01 /* Per-Note Management: reset controllers */
#define MIDI2_UMP_PERNOTE_DETACH       0x02 /* Per-Note Management: detach controllers */

size_t midi2_ump_midi2_cv(uint32_t out[2], uint8_t group, uint8_t status, uint8_t channel,
                          uint8_t byte3, uint8_t byte4, uint32_t data);
size_t midi2_ump_poly_pressure(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t note, uint32_t pressure);
size_t midi2_ump_registered_pernote(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t note, uint8_t index, uint32_t data);
size_t midi2_ump_assignable_pernote(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t note, uint8_t index, uint32_t data);
size_t midi2_ump_rpn(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t bank, uint8_t index, uint32_t data);
size_t midi2_ump_nrpn(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t bank, uint8_t index, uint32_t data);
size_t midi2_ump_relative_rpn(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t bank, uint8_t index, uint32_t data);
size_t midi2_ump_relative_nrpn(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t bank, uint8_t index, uint32_t data);
size_t midi2_ump_pernote_pitch_bend(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t note, uint32_t value);
size_t midi2_ump_pernote_management(uint32_t out[2], uint8_t group, uint8_t channel, uint8_t note, uint8_t flags);

/* ---- System Exclusive 7-bit (message type 0x3, 2 words per packet) ------ */
#define MIDI2_UMP_SYSEX_COMPLETE 0x0
#define MIDI2_UMP_SYSEX_START    0x1
#define MIDI2_UMP_SYSEX_CONTINUE 0x2
#define MIDI2_UMP_SYSEX_END      0x3
/* Packetize a 7-bit SysEx payload (data bytes only, no F0/F7) into one or more
 * 2-word UMP packets with correct complete/start/continue/end status. Returns
 * words written (multiple of 2), or 0 if out is too small. */
size_t midi2_ump_sysex7(uint32_t *out, size_t max_words, uint8_t group,
                        const uint8_t *data, size_t len);

/* ---- System Exclusive 8-bit (message type 0x5, 4 words per packet) ------ */
/* Packetize an 8-bit SysEx payload into 4-word UMP packets. stream_id groups
 * interleaved messages (0 if single-stream). Returns words written, or 0. */
size_t midi2_ump_sysex8(uint32_t *out, size_t max_words, uint8_t group,
                        uint8_t stream_id, const uint8_t *data, size_t len);

/* ---- Flex Data (message type 0xD, 4 words) ------------------------------ */
#define MIDI2_UMP_FLEX_FORM_COMPLETE 0x0
#define MIDI2_UMP_FLEX_FORM_START    0x1
#define MIDI2_UMP_FLEX_FORM_CONTINUE 0x2
#define MIDI2_UMP_FLEX_FORM_END      0x3
#define MIDI2_UMP_FLEX_ADDR_CHANNEL  0x0  /* addressed to a specific channel */
#define MIDI2_UMP_FLEX_ADDR_GROUP    0x1  /* addressed to the whole group */
#define MIDI2_UMP_FLEX_BANK_SETUP    0x00 /* tempo / time sig / metronome / key / chord */
#define MIDI2_UMP_FLEX_BANK_METADATA 0x01 /* metadata text */
#define MIDI2_UMP_FLEX_BANK_PERF     0x02 /* performance text / lyric */
size_t midi2_ump_flex(uint32_t out[4], uint8_t group, uint8_t form, uint8_t addr,
                      uint8_t channel, uint8_t status_bank, uint8_t status,
                      uint32_t d1, uint32_t d2, uint32_t d3);
size_t midi2_ump_flex_set_tempo(uint32_t out[4], uint8_t group, uint32_t ten_ns_per_quarter);
size_t midi2_ump_flex_set_time_signature(uint32_t out[4], uint8_t group, uint8_t numerator,
                                         uint8_t denominator, uint8_t num_32nd_notes);
size_t midi2_ump_flex_set_key_signature(uint32_t out[4], uint8_t group, uint8_t addr,
                                        uint8_t channel, int8_t sharps_flats, uint8_t tonic_note);
/* Packetize UTF-8 text (lyric/metadata) as Flex Data Text Common Format. */
size_t midi2_ump_flex_text(uint32_t *out, size_t max_words, uint8_t group, uint8_t addr,
                           uint8_t channel, uint8_t status_bank, uint8_t status, const char *text);

/* ---- UMP Stream messages (message type 0xF, 4 words each) ---------------- */
/* These declare/discover endpoint and function-block metadata. Used to make a
 * UMP endpoint enumerable by a host (e.g. macOS "MIDI UMP"). Spec: M2-104-UM. */

/* Stream message status codes (bits 25..16 of word0). */
#define MIDI2_UMP_STREAM_ENDPOINT_DISCOVERY  0x00
#define MIDI2_UMP_STREAM_ENDPOINT_INFO       0x01
#define MIDI2_UMP_STREAM_DEVICE_IDENTITY     0x02
#define MIDI2_UMP_STREAM_ENDPOINT_NAME       0x03
#define MIDI2_UMP_STREAM_PRODUCT_INSTANCE_ID 0x04
#define MIDI2_UMP_STREAM_CONFIG_REQUEST      0x05
#define MIDI2_UMP_STREAM_CONFIG_NOTIFICATION 0x06
#define MIDI2_UMP_STREAM_FB_DISCOVERY        0x10
#define MIDI2_UMP_STREAM_FB_INFO             0x11
#define MIDI2_UMP_STREAM_FB_NAME             0x12

/* Endpoint Discovery filter bits (word1[7:0]). */
#define MIDI2_UMP_DISC_ENDPOINT_INFO   0x01 /* 'e' */
#define MIDI2_UMP_DISC_DEVICE_IDENTITY 0x02 /* 'd' */
#define MIDI2_UMP_DISC_ENDPOINT_NAME   0x04 /* 'n' */
#define MIDI2_UMP_DISC_PRODUCT_ID      0x08 /* 'i' */
#define MIDI2_UMP_DISC_STREAM_CONFIG   0x10 /* 's' */
/* Function Block Discovery filter bits (word0[7:0]). */
#define MIDI2_UMP_DISC_FB_INFO         0x01 /* 'i' */
#define MIDI2_UMP_DISC_FB_NAME         0x02 /* 'n' */

/* Function Block direction / UI-hint values. */
#define MIDI2_UMP_FB_DIR_INPUT         0x01
#define MIDI2_UMP_FB_DIR_OUTPUT        0x02
#define MIDI2_UMP_FB_DIR_BIDIRECTIONAL 0x03
#define MIDI2_UMP_FB_UI_RECEIVER       0x01
#define MIDI2_UMP_FB_UI_SENDER         0x02
#define MIDI2_UMP_FB_UI_BOTH           0x03

/* Protocol values for Stream Configuration. */
#define MIDI2_UMP_PROTOCOL_MIDI1       0x01
#define MIDI2_UMP_PROTOCOL_MIDI2       0x02

static inline uint16_t midi2_ump_stream_status(uint32_t word0) { return (uint16_t)((word0 >> 16) & 0x3FF); }
static inline uint8_t  midi2_ump_stream_format(uint32_t word0) { return (uint8_t)((word0 >> 26) & 0x3); }

/* Endpoint Info Notification (status 0x01). Declares UMP version, number of
 * function blocks, static flag, and MIDI1/MIDI2 protocol capability. */
size_t midi2_ump_stream_endpoint_info(uint32_t out[4], uint8_t num_function_blocks,
                                      bool static_fb, bool midi2, bool midi1);

/* Stream Configuration Notification (status 0x06). protocol = MIDI2_UMP_PROTOCOL_*. */
size_t midi2_ump_stream_config_notification(uint32_t out[4], uint8_t protocol);

/* Function Block Info Notification (status 0x11). */
size_t midi2_ump_stream_fb_info(uint32_t out[4], uint8_t fb, uint8_t direction,
                                uint8_t first_group, uint8_t num_groups,
                                uint8_t ui_hint, uint8_t midi1);

/*
 * Text notification packetizer for Endpoint Name (0x03), Product Instance Id
 * (0x04) and Function Block Name (0x12). Emits one or more 4-word UMP Stream
 * messages with the correct Format (complete/start/continue/end) bits. For FB
 * Name pass fb = block number; for endpoint name / product id pass fb = -1.
 * Returns the number of 32-bit words written (a multiple of 4), or 0 if out is
 * too small.
 */
size_t midi2_ump_stream_text(uint32_t *out, size_t max_words, uint8_t status,
                             int fb, const char *text);

/* Stream request / extra builders (useful for the Client role and clip files). */
size_t midi2_ump_stream_endpoint_discovery(uint32_t out[4], uint8_t filter);
size_t midi2_ump_stream_fb_discovery(uint32_t out[4], uint8_t fb, uint8_t filter);
size_t midi2_ump_stream_config_request(uint32_t out[4], uint8_t protocol);
size_t midi2_ump_stream_device_identity(uint32_t out[4], const uint8_t manufacturer[3],
                                        uint16_t family, uint16_t model,
                                        const uint8_t sw_revision[4]);
size_t midi2_ump_stream_start_clip(uint32_t out[4]);
size_t midi2_ump_stream_end_clip(uint32_t out[4]);

/* ---- Unified decoder / stream iterator ---------------------------------- */
/* A best-effort decode of any UMP message into common fields. The raw words
 * remain available via .words / .num_words for type-specific parsing. */
typedef struct {
    uint8_t  message_type; /* MIDI2_UMP_MT_* */
    uint8_t  group;        /* 0..15 (0 for groupless utility / stream) */
    uint8_t  status;       /* CV status nibble / system status byte / stream or sysex or flex status */
    uint8_t  channel;      /* 0..15 for channel voice / flex */
    uint8_t  byte3;        /* note / controller / index / data1 / sysex byte-count / flex status-bank */
    uint8_t  byte4;        /* attribute type / index2 / data2 / option flags */
    uint32_t data;         /* primary 32-bit data (MIDI 2.0 word1, utility data, flex word2) */
    size_t   num_words;    /* size of this message (1, 2 or 4) */
    const uint32_t *words; /* pointer to this message's words */
} midi2_ump_view_t;

/* Decode the first UMP message in words[] into *out. Returns the number of
 * words it spans (1/2/4, clamped to num_words if truncated), or 0 if empty. */
size_t midi2_ump_parse(const uint32_t *words, size_t num_words, midi2_ump_view_t *out);

/* Iterate the complete UMP messages in a buffer. */
typedef struct { const uint32_t *words; size_t num_words; size_t pos; } midi2_ump_iter_t;
static inline void midi2_ump_iter_init(midi2_ump_iter_t *it, const uint32_t *words, size_t num_words)
{ it->words = words; it->num_words = num_words; it->pos = 0; }
bool midi2_ump_iter_next(midi2_ump_iter_t *it, midi2_ump_view_t *out);

/* ---- Decoded view of a MIDI 2.0 channel-voice message ------------------- */
typedef struct {
    uint8_t  message_type; /* MIDI2_UMP_MT_* */
    uint8_t  group;        /* 0..15 */
    uint8_t  status;       /* status nibble for channel voice, else 0 */
    uint8_t  channel;      /* 0..15 for channel voice */
    uint8_t  byte1;        /* note / controller / program */
    uint8_t  byte2;        /* attribute type, etc. */
    uint32_t value;        /* velocity / CC value / pressure / bend (type dependent) */
} midi2_ump_cv_t;

/*
 * Decode a MIDI 2.0 channel-voice message (message type 0x4) from up to two
 * words into *out. Returns true if word0 is a MIDI 2.0 channel-voice message
 * and num_words >= 2; false otherwise (out left untouched).
 */
bool midi2_ump_decode_cv(const uint32_t *words, size_t num_words, midi2_ump_cv_t *out);

#ifdef __cplusplus
}
#endif
