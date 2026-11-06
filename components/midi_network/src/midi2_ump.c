/*
 * midi2_ump.c - UMP message helpers implementation.
 * SPDX-License-Identifier: MIT
 */
#include "midi2_ump.h"
#include <string.h>

size_t midi2_ump_message_num_words(uint32_t word0)
{
    switch (midi2_ump_message_type(word0)) {
    case MIDI2_UMP_MT_UTILITY:
    case MIDI2_UMP_MT_SYSTEM_RT:
    case MIDI2_UMP_MT_MIDI1_CV:
        return 1;
    case MIDI2_UMP_MT_DATA64:
    case MIDI2_UMP_MT_MIDI2_CV:
        return 2;
    case MIDI2_UMP_MT_DATA128:
    case MIDI2_UMP_MT_FLEX_DATA:
    case MIDI2_UMP_MT_STREAM:
        return 4;
    default:
        /*
         * Unknown/reserved message types: the UMP spec assigns sizes by the
         * high bits of the message type. Types 0x6-0x7 are 1 word, 0x8-0xA are
         * 2 words, 0xB-0xC are 3 words, 0xD-0xF are 4 words. Fall back to that
         * table so a stream walker always advances correctly.
         */
        {
            uint8_t mt = midi2_ump_message_type(word0);
            if (mt <= 0x07) return 1;
            if (mt <= 0x0A) return 2;
            if (mt <= 0x0C) return 3;
            return 4;
        }
    }
}

/* word0 for a channel-voice message: mt=0x4, group, status, channel, b1, b2. */
static inline uint32_t cv_word0(uint8_t group, uint8_t status, uint8_t channel,
                                uint8_t b1, uint8_t b2)
{
    return ((uint32_t)MIDI2_UMP_MT_MIDI2_CV << 28)
         | ((uint32_t)(group   & 0x0F) << 24)
         | ((uint32_t)(status  & 0x0F) << 20)
         | ((uint32_t)(channel & 0x0F) << 16)
         | ((uint32_t)b1 << 8)
         | (uint32_t)b2;
}

size_t midi2_ump_note_on(uint32_t out[2], uint8_t group, uint8_t channel,
                         uint8_t note, uint16_t velocity,
                         uint8_t attr_type, uint16_t attr_data)
{
    out[0] = cv_word0(group, MIDI2_UMP_STATUS_NOTE_ON, channel,
                      note & 0x7F, attr_type);
    out[1] = ((uint32_t)velocity << 16) | attr_data;
    return 2;
}

size_t midi2_ump_note_off(uint32_t out[2], uint8_t group, uint8_t channel,
                          uint8_t note, uint16_t velocity,
                          uint8_t attr_type, uint16_t attr_data)
{
    out[0] = cv_word0(group, MIDI2_UMP_STATUS_NOTE_OFF, channel,
                      note & 0x7F, attr_type);
    out[1] = ((uint32_t)velocity << 16) | attr_data;
    return 2;
}

size_t midi2_ump_control_change(uint32_t out[2], uint8_t group, uint8_t channel,
                                uint8_t controller, uint32_t value)
{
    out[0] = cv_word0(group, MIDI2_UMP_STATUS_CC, channel, controller & 0x7F, 0);
    out[1] = value;
    return 2;
}

size_t midi2_ump_channel_pressure(uint32_t out[2], uint8_t group, uint8_t channel,
                                  uint32_t value)
{
    out[0] = cv_word0(group, MIDI2_UMP_STATUS_CHANNEL_PRESSURE, channel, 0, 0);
    out[1] = value;
    return 2;
}

size_t midi2_ump_program_change(uint32_t out[2], uint8_t group, uint8_t channel,
                                uint8_t program, bool bank_valid,
                                uint8_t bank_msb, uint8_t bank_lsb)
{
    /* Option flags live in byte2 of word0; bit0 = Bank Valid. */
    out[0] = cv_word0(group, MIDI2_UMP_STATUS_PROGRAM_CHANGE, channel, 0,
                      bank_valid ? 0x01 : 0x00);
    out[1] = ((uint32_t)(program & 0x7F) << 24)
           | ((uint32_t)(bank_msb & 0x7F) << 8)
           | (uint32_t)(bank_lsb & 0x7F);
    return 2;
}

size_t midi2_ump_pitch_bend(uint32_t out[2], uint8_t group, uint8_t channel,
                            uint32_t value)
{
    out[0] = cv_word0(group, MIDI2_UMP_STATUS_PITCH_BEND, channel, 0, 0);
    out[1] = value;
    return 2;
}

size_t midi2_ump_system_rt(uint32_t out[1], uint8_t group, uint8_t status)
{
    out[0] = ((uint32_t)MIDI2_UMP_MT_SYSTEM_RT << 28)
           | ((uint32_t)(group & 0x0F) << 24)
           | ((uint32_t)status << 16);
    return 1;
}

/* ---- UMP Stream message builders ---------------------------------------- */
/* UMP version this implementation conforms to (M2-104-UM v1.1). */
#define STREAM_UMP_VER_MAJOR 0x01
#define STREAM_UMP_VER_MINOR 0x01

static inline uint32_t stream_word0(uint8_t format, uint16_t status, uint16_t data16)
{
    return ((uint32_t)MIDI2_UMP_MT_STREAM << 28)
         | ((uint32_t)(format & 0x3) << 26)
         | ((uint32_t)(status & 0x3FF) << 16)
         | data16;
}

size_t midi2_ump_stream_endpoint_info(uint32_t out[4], uint8_t num_function_blocks,
                                      bool static_fb, bool midi2, bool midi1)
{
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_ENDPOINT_INFO,
                          ((uint16_t)STREAM_UMP_VER_MAJOR << 8) | STREAM_UMP_VER_MINOR);
    out[1] = ((uint32_t)(static_fb ? 1u : 0u) << 31)
           | ((uint32_t)(num_function_blocks & 0x7F) << 24)
           | ((uint32_t)(midi2 ? 1u : 0u) << 9)
           | ((uint32_t)(midi1 ? 1u : 0u) << 8);
    out[2] = 0;
    out[3] = 0;
    return 4;
}

size_t midi2_ump_stream_config_notification(uint32_t out[4], uint8_t protocol)
{
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_CONFIG_NOTIFICATION, (uint16_t)protocol << 8);
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    return 4;
}

size_t midi2_ump_stream_fb_info(uint32_t out[4], uint8_t fb, uint8_t direction,
                                uint8_t first_group, uint8_t num_groups,
                                uint8_t ui_hint, uint8_t midi1)
{
    uint16_t data = (uint16_t)(1u << 15)               /* active */
                  | ((uint16_t)(fb & 0x7F) << 8)
                  | ((uint16_t)(ui_hint & 0x3) << 4)
                  | ((uint16_t)(midi1 & 0x3) << 2)
                  | (uint16_t)(direction & 0x3);
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_FB_INFO, data);
    out[1] = ((uint32_t)first_group << 24)
           | ((uint32_t)num_groups << 16);  /* midi-ci version 0, max sysex8 0 */
    out[2] = 0;
    out[3] = 0;
    return 4;
}

size_t midi2_ump_stream_text(uint32_t *out, size_t max_words, uint8_t status,
                             int fb, const char *text)
{
    bool has_fb = (fb >= 0);
    size_t per_packet = has_fb ? 13 : 14;     /* payload bytes per UMP */
    size_t tlen = text ? strlen(text) : 0;
    size_t npackets = (tlen + per_packet - 1) / per_packet;
    if (npackets == 0) npackets = 1;          /* always emit at least one */
    if (npackets * 4 > max_words) {
        return 0;
    }

    size_t pos = 0;
    for (size_t p = 0; p < npackets; p++) {
        uint8_t format = (npackets == 1) ? 0x0
                       : (p == 0) ? 0x1
                       : (p == npackets - 1) ? 0x3 : 0x2;
        /* Lay the packet out as 16 big-endian bytes, then pack into 4 words. */
        uint8_t b[16];
        memset(b, 0, sizeof(b));
        b[0] = (uint8_t)((MIDI2_UMP_MT_STREAM << 4) | (format << 2) | ((status >> 8) & 0x3));
        b[1] = (uint8_t)(status & 0xFF);
        size_t payload_off;
        if (has_fb) {
            b[2] = (uint8_t)fb;
            payload_off = 3;
        } else {
            payload_off = 2;
        }
        for (size_t k = payload_off; k < 16 && pos < tlen; k++) {
            b[k] = (uint8_t)text[pos++];
        }
        for (int w = 0; w < 4; w++) {
            out[p * 4 + w] = ((uint32_t)b[w * 4] << 24) | ((uint32_t)b[w * 4 + 1] << 16)
                           | ((uint32_t)b[w * 4 + 2] << 8) | (uint32_t)b[w * 4 + 3];
        }
    }
    return npackets * 4;
}

bool midi2_ump_decode_cv(const uint32_t *words, size_t num_words, midi2_ump_cv_t *out)
{
    if (!words || !out || num_words < 2) {
        return false;
    }
    if (midi2_ump_message_type(words[0]) != MIDI2_UMP_MT_MIDI2_CV) {
        return false;
    }
    out->message_type = MIDI2_UMP_MT_MIDI2_CV;
    out->group   = (uint8_t)((words[0] >> 24) & 0x0F);
    out->status  = (uint8_t)((words[0] >> 20) & 0x0F);
    out->channel = (uint8_t)((words[0] >> 16) & 0x0F);
    out->byte1   = (uint8_t)((words[0] >> 8) & 0xFF);
    out->byte2   = (uint8_t)(words[0] & 0xFF);
    out->value   = words[1];
    return true;
}
