/*
 * midi2_ump_messages.c - full UMP message builders, packetizers and decoder.
 *
 * Covers every UMP message type per MIDI Association M2-104-UM v1.1.2:
 * Utility, System Common, MIDI 1.0 / MIDI 2.0 Channel Voice, SysEx7, SysEx8,
 * Flex Data, and UMP Stream requests, plus a unified decoder/iterator. Bit
 * layouts taken from Appendix F/G and sections 7.1-7.9.
 *
 * SPDX-License-Identifier: MIT
 */
#include "midi2_ump.h"
#include <string.h>

/* Pack 4 big-endian bytes from b[off] into a 32-bit UMP word. */
static inline uint32_t pack4(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

/* ---- Utility (mt 0x0) --------------------------------------------------- */
size_t midi2_ump_utility(uint32_t out[1], uint8_t status, uint32_t data20)
{
    out[0] = ((uint32_t)MIDI2_UMP_MT_UTILITY << 28)
           | ((uint32_t)(status & 0x0F) << 20)
           | (data20 & 0x000FFFFF);
    return 1;
}
size_t midi2_ump_noop(uint32_t out[1]) { return midi2_ump_utility(out, MIDI2_UMP_UTILITY_NOOP, 0); }
size_t midi2_ump_jr_clock(uint32_t out[1], uint16_t t) { return midi2_ump_utility(out, MIDI2_UMP_UTILITY_JR_CLOCK, t); }
size_t midi2_ump_jr_timestamp(uint32_t out[1], uint16_t t) { return midi2_ump_utility(out, MIDI2_UMP_UTILITY_JR_TIMESTAMP, t); }
size_t midi2_ump_dctpq(uint32_t out[1], uint16_t t) { return midi2_ump_utility(out, MIDI2_UMP_UTILITY_DCTPQ, t); }
size_t midi2_ump_delta_clockstamp(uint32_t out[1], uint32_t t) { return midi2_ump_utility(out, MIDI2_UMP_UTILITY_DELTA_CLOCKSTAMP, t); }

/* ---- System Common / Real Time (mt 0x1) -------------------------------- */
size_t midi2_ump_system_common(uint32_t out[1], uint8_t group, uint8_t status,
                               uint8_t data1, uint8_t data2)
{
    out[0] = ((uint32_t)MIDI2_UMP_MT_SYSTEM_RT << 28)
           | ((uint32_t)(group & 0x0F) << 24)
           | ((uint32_t)status << 16)
           | ((uint32_t)(data1 & 0x7F) << 8)
           | (data2 & 0x7F);
    return 1;
}
size_t midi2_ump_mtc(uint32_t out[1], uint8_t group, uint8_t time_code)
{ return midi2_ump_system_common(out, group, MIDI2_UMP_SYS_MTC, time_code, 0); }
size_t midi2_ump_song_position(uint32_t out[1], uint8_t group, uint16_t position14)
{ return midi2_ump_system_common(out, group, MIDI2_UMP_SYS_SONG_POSITION, position14 & 0x7F, (position14 >> 7) & 0x7F); }
size_t midi2_ump_song_select(uint32_t out[1], uint8_t group, uint8_t song)
{ return midi2_ump_system_common(out, group, MIDI2_UMP_SYS_SONG_SELECT, song, 0); }
size_t midi2_ump_tune_request(uint32_t out[1], uint8_t group)
{ return midi2_ump_system_common(out, group, MIDI2_UMP_SYS_TUNE_REQUEST, 0, 0); }

/* ---- MIDI 1.0 Channel Voice (mt 0x2) ----------------------------------- */
size_t midi2_ump_midi1_cv(uint32_t out[1], uint8_t group, uint8_t status,
                          uint8_t channel, uint8_t data1, uint8_t data2)
{
    out[0] = ((uint32_t)MIDI2_UMP_MT_MIDI1_CV << 28)
           | ((uint32_t)(group & 0x0F) << 24)
           | ((uint32_t)(status & 0x0F) << 20)
           | ((uint32_t)(channel & 0x0F) << 16)
           | ((uint32_t)(data1 & 0x7F) << 8)
           | (data2 & 0x7F);
    return 1;
}
size_t midi2_ump_midi1_note_on(uint32_t out[1], uint8_t g, uint8_t c, uint8_t n, uint8_t v)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_NOTE_ON, c, n, v); }
size_t midi2_ump_midi1_note_off(uint32_t out[1], uint8_t g, uint8_t c, uint8_t n, uint8_t v)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_NOTE_OFF, c, n, v); }
size_t midi2_ump_midi1_poly_pressure(uint32_t out[1], uint8_t g, uint8_t c, uint8_t n, uint8_t p)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_POLY_PRESSURE, c, n, p); }
size_t midi2_ump_midi1_control_change(uint32_t out[1], uint8_t g, uint8_t c, uint8_t cc, uint8_t v)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_CC, c, cc, v); }
size_t midi2_ump_midi1_program_change(uint32_t out[1], uint8_t g, uint8_t c, uint8_t p)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_PROGRAM_CHANGE, c, p, 0); }
size_t midi2_ump_midi1_channel_pressure(uint32_t out[1], uint8_t g, uint8_t c, uint8_t p)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_CHANNEL_PRESSURE, c, p, 0); }
size_t midi2_ump_midi1_pitch_bend(uint32_t out[1], uint8_t g, uint8_t c, uint16_t b)
{ return midi2_ump_midi1_cv(out, g, MIDI2_UMP_STATUS_PITCH_BEND, c, b & 0x7F, (b >> 7) & 0x7F); }

/* ---- MIDI 2.0 Channel Voice - additional types (mt 0x4) ---------------- */
static inline uint32_t cv2_word0(uint8_t group, uint8_t status, uint8_t channel,
                                 uint8_t b3, uint8_t b4)
{
    return ((uint32_t)MIDI2_UMP_MT_MIDI2_CV << 28)
         | ((uint32_t)(group & 0x0F) << 24)
         | ((uint32_t)(status & 0x0F) << 20)
         | ((uint32_t)(channel & 0x0F) << 16)
         | ((uint32_t)b3 << 8) | b4;
}
size_t midi2_ump_midi2_cv(uint32_t out[2], uint8_t group, uint8_t status, uint8_t channel,
                          uint8_t byte3, uint8_t byte4, uint32_t data)
{
    out[0] = cv2_word0(group, status, channel, byte3, byte4);
    out[1] = data;
    return 2;
}
size_t midi2_ump_poly_pressure(uint32_t out[2], uint8_t g, uint8_t c, uint8_t note, uint32_t p)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_POLY_PRESSURE, c, note & 0x7F, 0, p); }
size_t midi2_ump_registered_pernote(uint32_t out[2], uint8_t g, uint8_t c, uint8_t note, uint8_t idx, uint32_t d)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_REG_PERNOTE, c, note & 0x7F, idx, d); }
size_t midi2_ump_assignable_pernote(uint32_t out[2], uint8_t g, uint8_t c, uint8_t note, uint8_t idx, uint32_t d)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_ASN_PERNOTE, c, note & 0x7F, idx, d); }
size_t midi2_ump_rpn(uint32_t out[2], uint8_t g, uint8_t c, uint8_t bank, uint8_t idx, uint32_t d)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_RPN, c, bank & 0x7F, idx & 0x7F, d); }
size_t midi2_ump_nrpn(uint32_t out[2], uint8_t g, uint8_t c, uint8_t bank, uint8_t idx, uint32_t d)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_NRPN, c, bank & 0x7F, idx & 0x7F, d); }
size_t midi2_ump_relative_rpn(uint32_t out[2], uint8_t g, uint8_t c, uint8_t bank, uint8_t idx, uint32_t d)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_REL_RPN, c, bank & 0x7F, idx & 0x7F, d); }
size_t midi2_ump_relative_nrpn(uint32_t out[2], uint8_t g, uint8_t c, uint8_t bank, uint8_t idx, uint32_t d)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_REL_NRPN, c, bank & 0x7F, idx & 0x7F, d); }
size_t midi2_ump_pernote_pitch_bend(uint32_t out[2], uint8_t g, uint8_t c, uint8_t note, uint32_t v)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_PERNOTE_PITCH, c, note & 0x7F, 0, v); }
size_t midi2_ump_pernote_management(uint32_t out[2], uint8_t g, uint8_t c, uint8_t note, uint8_t flags)
{ return midi2_ump_midi2_cv(out, g, MIDI2_UMP_STATUS_PERNOTE_MGMT, c, note & 0x7F, flags & 0x03, 0); }

/* ---- SysEx7 (mt 0x3) --------------------------------------------------- */
size_t midi2_ump_sysex7(uint32_t *out, size_t max_words, uint8_t group,
                        const uint8_t *data, size_t len)
{
    size_t npackets = len ? (len + 5) / 6 : 1; /* up to 6 data bytes per packet */
    if (npackets * 2 > max_words) {
        return 0;
    }
    size_t pos = 0;
    for (size_t p = 0; p < npackets; p++) {
        uint8_t status = (npackets == 1) ? MIDI2_UMP_SYSEX_COMPLETE
                       : (p == 0) ? MIDI2_UMP_SYSEX_START
                       : (p == npackets - 1) ? MIDI2_UMP_SYSEX_END : MIDI2_UMP_SYSEX_CONTINUE;
        size_t n = len - pos; if (n > 6) n = 6;
        uint8_t b[8] = {0};
        b[0] = (uint8_t)((MIDI2_UMP_MT_DATA64 << 4) | (group & 0x0F));
        b[1] = (uint8_t)((status << 4) | (n & 0x0F));
        for (size_t i = 0; i < n; i++) b[2 + i] = data[pos + i] & 0x7F;
        out[p * 2 + 0] = pack4(&b[0]);
        out[p * 2 + 1] = pack4(&b[4]);
        pos += n;
    }
    return npackets * 2;
}

/* ---- SysEx8 (mt 0x5) --------------------------------------------------- */
size_t midi2_ump_sysex8(uint32_t *out, size_t max_words, uint8_t group,
                        uint8_t stream_id, const uint8_t *data, size_t len)
{
    size_t npackets = len ? (len + 12) / 13 : 1; /* up to 13 data bytes per packet */
    if (npackets * 4 > max_words) {
        return 0;
    }
    size_t pos = 0;
    for (size_t p = 0; p < npackets; p++) {
        uint8_t status = (npackets == 1) ? MIDI2_UMP_SYSEX_COMPLETE
                       : (p == 0) ? MIDI2_UMP_SYSEX_START
                       : (p == npackets - 1) ? MIDI2_UMP_SYSEX_END : MIDI2_UMP_SYSEX_CONTINUE;
        size_t n = len - pos; if (n > 13) n = 13;
        uint8_t b[16] = {0};
        b[0] = (uint8_t)((MIDI2_UMP_MT_DATA128 << 4) | (group & 0x0F));
        b[1] = (uint8_t)((status << 4) | ((n + 1) & 0x0F)); /* #bytes counts the stream id */
        b[2] = stream_id;
        for (size_t i = 0; i < n; i++) b[3 + i] = data[pos + i];
        out[p * 4 + 0] = pack4(&b[0]);
        out[p * 4 + 1] = pack4(&b[4]);
        out[p * 4 + 2] = pack4(&b[8]);
        out[p * 4 + 3] = pack4(&b[12]);
        pos += n;
    }
    return npackets * 4;
}

/* ---- Flex Data (mt 0xD) ------------------------------------------------ */
size_t midi2_ump_flex(uint32_t out[4], uint8_t group, uint8_t form, uint8_t addr,
                      uint8_t channel, uint8_t status_bank, uint8_t status,
                      uint32_t d1, uint32_t d2, uint32_t d3)
{
    out[0] = ((uint32_t)MIDI2_UMP_MT_FLEX_DATA << 28)
           | ((uint32_t)(group & 0x0F) << 24)
           | ((uint32_t)(form & 0x03) << 22)
           | ((uint32_t)(addr & 0x03) << 20)
           | ((uint32_t)(channel & 0x0F) << 16)
           | ((uint32_t)status_bank << 8)
           | status;
    out[1] = d1;
    out[2] = d2;
    out[3] = d3;
    return 4;
}
size_t midi2_ump_flex_set_tempo(uint32_t out[4], uint8_t group, uint32_t ten_ns_per_quarter)
{
    return midi2_ump_flex(out, group, MIDI2_UMP_FLEX_FORM_COMPLETE, MIDI2_UMP_FLEX_ADDR_GROUP,
                          0, MIDI2_UMP_FLEX_BANK_SETUP, 0x00, ten_ns_per_quarter, 0, 0);
}
size_t midi2_ump_flex_set_time_signature(uint32_t out[4], uint8_t group, uint8_t numerator,
                                         uint8_t denominator, uint8_t num_32nd_notes)
{
    uint32_t d1 = ((uint32_t)numerator << 24) | ((uint32_t)denominator << 16)
                | ((uint32_t)num_32nd_notes << 8);
    return midi2_ump_flex(out, group, MIDI2_UMP_FLEX_FORM_COMPLETE, MIDI2_UMP_FLEX_ADDR_GROUP,
                          0, MIDI2_UMP_FLEX_BANK_SETUP, 0x01, d1, 0, 0);
}
size_t midi2_ump_flex_set_key_signature(uint32_t out[4], uint8_t group, uint8_t addr,
                                        uint8_t channel, int8_t sharps_flats, uint8_t tonic_note)
{
    uint32_t d1 = ((uint32_t)(sharps_flats & 0x0F) << 28) | ((uint32_t)(tonic_note & 0x0F) << 24);
    return midi2_ump_flex(out, group, MIDI2_UMP_FLEX_FORM_COMPLETE, addr, channel,
                          MIDI2_UMP_FLEX_BANK_SETUP, 0x05, d1, 0, 0);
}
size_t midi2_ump_flex_text(uint32_t *out, size_t max_words, uint8_t group, uint8_t addr,
                           uint8_t channel, uint8_t status_bank, uint8_t status, const char *text)
{
    size_t tlen = text ? strlen(text) : 0;
    size_t npackets = tlen ? (tlen + 11) / 12 : 1; /* 12 text bytes per packet */
    if (npackets * 4 > max_words) {
        return 0;
    }
    size_t pos = 0;
    for (size_t p = 0; p < npackets; p++) {
        uint8_t form = (npackets == 1) ? MIDI2_UMP_FLEX_FORM_COMPLETE
                     : (p == 0) ? MIDI2_UMP_FLEX_FORM_START
                     : (p == npackets - 1) ? MIDI2_UMP_FLEX_FORM_END : MIDI2_UMP_FLEX_FORM_CONTINUE;
        uint8_t d[12] = {0};
        size_t n = tlen - pos; if (n > 12) n = 12;
        memcpy(d, text + pos, n);
        midi2_ump_flex(&out[p * 4], group, form, addr, channel, status_bank, status,
                       pack4(&d[0]), pack4(&d[4]), pack4(&d[8]));
        pos += n;
    }
    return npackets * 4;
}

/* ---- UMP Stream requests / extras (mt 0xF) ----------------------------- */
static inline uint32_t stream_word0(uint8_t format, uint16_t status, uint16_t data16)
{
    return ((uint32_t)MIDI2_UMP_MT_STREAM << 28)
         | ((uint32_t)(format & 0x3) << 26)
         | ((uint32_t)(status & 0x3FF) << 16)
         | data16;
}
size_t midi2_ump_stream_endpoint_discovery(uint32_t out[4], uint8_t filter)
{
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_ENDPOINT_DISCOVERY, 0x0101); /* UMP v1.1 */
    out[1] = filter;
    out[2] = 0;
    out[3] = 0;
    return 4;
}
size_t midi2_ump_stream_fb_discovery(uint32_t out[4], uint8_t fb, uint8_t filter)
{
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_FB_DISCOVERY, ((uint16_t)fb << 8) | filter);
    out[1] = 0; out[2] = 0; out[3] = 0;
    return 4;
}
size_t midi2_ump_stream_config_request(uint32_t out[4], uint8_t protocol)
{
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_CONFIG_REQUEST, (uint16_t)protocol << 8);
    out[1] = 0; out[2] = 0; out[3] = 0;
    return 4;
}
size_t midi2_ump_stream_device_identity(uint32_t out[4], const uint8_t manufacturer[3],
                                        uint16_t family, uint16_t model,
                                        const uint8_t sw_revision[4])
{
    out[0] = stream_word0(0, MIDI2_UMP_STREAM_DEVICE_IDENTITY, 0);
    out[1] = ((uint32_t)(manufacturer[0] & 0x7F) << 16)
           | ((uint32_t)(manufacturer[1] & 0x7F) << 8)
           | (manufacturer[2] & 0x7F);
    out[2] = ((uint32_t)(family & 0x7F) << 24) | ((uint32_t)((family >> 7) & 0x7F) << 16)
           | ((uint32_t)(model & 0x7F) << 8) | ((model >> 7) & 0x7F);
    out[3] = ((uint32_t)(sw_revision[0] & 0x7F) << 24) | ((uint32_t)(sw_revision[1] & 0x7F) << 16)
           | ((uint32_t)(sw_revision[2] & 0x7F) << 8) | (sw_revision[3] & 0x7F);
    return 4;
}
size_t midi2_ump_stream_start_clip(uint32_t out[4])
{
    out[0] = stream_word0(0, 0x20, 0); out[1] = 0; out[2] = 0; out[3] = 0;
    return 4;
}
size_t midi2_ump_stream_end_clip(uint32_t out[4])
{
    out[0] = stream_word0(0, 0x21, 0); out[1] = 0; out[2] = 0; out[3] = 0;
    return 4;
}

/* ---- Unified decoder / iterator ---------------------------------------- */
size_t midi2_ump_parse(const uint32_t *words, size_t num_words, midi2_ump_view_t *out)
{
    if (!words || !out || num_words == 0) {
        return 0;
    }
    uint32_t w0 = words[0];
    size_t n = midi2_ump_message_num_words(w0);
    if (n > num_words) {
        n = num_words; /* truncated tail: report what is present */
    }
    memset(out, 0, sizeof(*out));
    out->words = words;
    out->num_words = n;
    out->message_type = midi2_ump_message_type(w0);
    out->group = midi2_ump_group(w0);

    switch (out->message_type) {
    case MIDI2_UMP_MT_UTILITY:
        out->group = 0;
        out->status = (uint8_t)((w0 >> 20) & 0x0F);
        out->data = w0 & 0x000FFFFF;
        break;
    case MIDI2_UMP_MT_SYSTEM_RT:
        out->status = (uint8_t)((w0 >> 16) & 0xFF);
        out->byte3 = (uint8_t)((w0 >> 8) & 0x7F);
        out->byte4 = (uint8_t)(w0 & 0x7F);
        break;
    case MIDI2_UMP_MT_MIDI1_CV:
        out->status = (uint8_t)((w0 >> 20) & 0x0F);
        out->channel = (uint8_t)((w0 >> 16) & 0x0F);
        out->byte3 = (uint8_t)((w0 >> 8) & 0x7F);
        out->byte4 = (uint8_t)(w0 & 0x7F);
        break;
    case MIDI2_UMP_MT_MIDI2_CV:
        out->status = (uint8_t)((w0 >> 20) & 0x0F);
        out->channel = (uint8_t)((w0 >> 16) & 0x0F);
        out->byte3 = (uint8_t)((w0 >> 8) & 0xFF);
        out->byte4 = (uint8_t)(w0 & 0xFF);
        out->data = (n >= 2) ? words[1] : 0;
        break;
    case MIDI2_UMP_MT_DATA64:  /* SysEx7 */
    case MIDI2_UMP_MT_DATA128: /* SysEx8 / Mixed Data Set */
        out->status = (uint8_t)((w0 >> 20) & 0x0F);
        out->byte3 = (uint8_t)((w0 >> 16) & 0x0F); /* number of valid bytes */
        break;
    case MIDI2_UMP_MT_FLEX_DATA:
        out->channel = (uint8_t)((w0 >> 16) & 0x0F);
        out->byte3 = (uint8_t)((w0 >> 8) & 0xFF); /* status bank */
        out->status = (uint8_t)(w0 & 0xFF);
        out->byte4 = (uint8_t)((w0 >> 20) & 0x03); /* addr */
        out->data = (n >= 2) ? words[1] : 0;
        break;
    case MIDI2_UMP_MT_STREAM:
        out->group = 0;
        out->status = (uint8_t)((w0 >> 16) & 0x3FF);
        break;
    default:
        break;
    }
    return n;
}

bool midi2_ump_iter_next(midi2_ump_iter_t *it, midi2_ump_view_t *out)
{
    if (!it || it->pos >= it->num_words) {
        return false;
    }
    size_t n = midi2_ump_parse(&it->words[it->pos], it->num_words - it->pos, out);
    if (n == 0) {
        it->pos = it->num_words;
        return false;
    }
    it->pos += n;
    return true;
}
