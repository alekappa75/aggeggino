/*
 * midi2_net_proto.c - command packet framing helpers.
 * SPDX-License-Identifier: MIT
 */
#include "midi2_net_proto.h"
#include <string.h>

size_t midi2_proto_parse_command(const uint8_t *buf, size_t len, midi2_cmd_view_t *out)
{
    if (len < 4) {
        return 0; /* not even a header */
    }
    uint8_t payload_words = buf[1];
    size_t total = 4u + (size_t)payload_words * 4u;
    if (len < total) {
        return 0; /* declared payload runs past the buffer */
    }
    out->code          = buf[0];
    out->payload_words = payload_words;
    out->csd1          = buf[2];
    out->csd2          = buf[3];
    out->payload       = (payload_words > 0) ? (buf + 4) : NULL;
    return total;
}

bool midi2_proto_put_header(uint8_t *buf, size_t cap, size_t *off,
                            uint8_t code, uint8_t payload_words,
                            uint8_t csd1, uint8_t csd2)
{
    if (*off + 4u > cap) {
        return false;
    }
    buf[*off + 0] = code;
    buf[*off + 1] = payload_words;
    buf[*off + 2] = csd1;
    buf[*off + 3] = csd2;
    *off += 4u;
    return true;
}
