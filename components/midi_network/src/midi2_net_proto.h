/*
 * midi2_net_proto.h - Network MIDI 2.0 (UDP) wire protocol constants & framing.
 *
 * Internal header. All values per MIDI Association document M2-124-UM v1.0
 * "User Datagram Protocol for Universal MIDI Packets" (published 2024-11-20).
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

/* 5.2 Signature: every UDP packet starts with "MIDI" (0x4D494449), once. */
#define MIDI2_NET_SIGNATURE  0x4D494449u
#define MIDI2_NET_SIG_B0     0x4D /* 'M' */
#define MIDI2_NET_SIG_B1     0x49 /* 'I' */
#define MIDI2_NET_SIG_B2     0x44 /* 'D' */
#define MIDI2_NET_SIG_B3     0x49 /* 'I' */

/* 5.1.1 Avoid IP fragmentation. */
#define MIDI2_NET_MAX_UDP_PAYLOAD  1400

/* mDNS / DNS-SD service type (4.x). */
#define MIDI2_NET_SERVICE_TYPE   "_midi2"
#define MIDI2_NET_SERVICE_PROTO  "_udp"

/* TXT record keys (Table 6). */
#define MIDI2_NET_TXT_KEY_NAME   "UMPEndpointName"
#define MIDI2_NET_TXT_KEY_PID    "ProductInstanceId"

/* Field length limits (MIDI2_NET_MAX_ENDPOINT_NAME / _PRODUCT_ID) and
 * MIDI2_NET_MAX_UMP_DATA_WORDS are part of the public API; see midi2_net.h.
 *
 * 6.4 CryptoNonce length / 6.9 SHA-256 digest length. */
#define MIDI2_NET_NONCE_LEN   16
#define MIDI2_NET_DIGEST_LEN  32

/* Command Codes - Table 8 "List of all Commands". */
typedef enum {
    MIDI2_CMD_INVITATION              = 0x01,
    MIDI2_CMD_INVITATION_AUTH         = 0x02,
    MIDI2_CMD_INVITATION_USER_AUTH    = 0x03,
    MIDI2_CMD_INVITATION_REPLY_ACCEPT = 0x10,
    MIDI2_CMD_INVITATION_REPLY_PEND   = 0x11,
    MIDI2_CMD_INVITATION_REPLY_AUTH   = 0x12,
    MIDI2_CMD_INVITATION_REPLY_UAUTH  = 0x13,
    MIDI2_CMD_PING                    = 0x20,
    MIDI2_CMD_PING_REPLY              = 0x21,
    MIDI2_CMD_RETRANSMIT_REQUEST      = 0x80,
    MIDI2_CMD_RETRANSMIT_ERROR        = 0x81,
    MIDI2_CMD_SESSION_RESET           = 0x82,
    MIDI2_CMD_SESSION_RESET_REPLY     = 0x83,
    MIDI2_CMD_NAK                     = 0x8F,
    MIDI2_CMD_BYE                     = 0xF0,
    MIDI2_CMD_BYE_REPLY               = 0xF1,
    MIDI2_CMD_UMP_DATA                = 0xFF,
} midi2_cmd_t;

/* Invitation Capabilities bitmap - Table 11. */
#define MIDI2_CAP_AUTH       0x01  /* D0: supports Invitation with Authentication */
#define MIDI2_CAP_USER_AUTH  0x02  /* D1: supports Invitation with User Auth */

/* Authentication State - Table 15 / 17. */
#define MIDI2_AUTH_STATE_FIRST          0x00
#define MIDI2_AUTH_STATE_DIGEST_WRONG   0x01
#define MIDI2_AUTH_STATE_USER_NOT_FOUND 0x02

/* NAK reasons - Table 25. */
#define MIDI2_NAK_OTHER           0x00
#define MIDI2_NAK_NOT_SUPPORTED   0x01
#define MIDI2_NAK_NOT_EXPECTED    0x02
#define MIDI2_NAK_MALFORMED       0x03
#define MIDI2_NAK_BAD_PING_REPLY  0x20

/* Bye reasons - Table 27. */
#define MIDI2_BYE_UNKNOWN              0x00
#define MIDI2_BYE_USER_TERMINATED      0x01
#define MIDI2_BYE_POWER_DOWN           0x02
#define MIDI2_BYE_TOO_MANY_MISSING     0x03
#define MIDI2_BYE_TIMEOUT              0x04
#define MIDI2_BYE_SESSION_NOT_ESTAB    0x05
#define MIDI2_BYE_NO_PENDING_SESSION   0x06
#define MIDI2_BYE_PROTOCOL_ERROR       0x07
#define MIDI2_BYE_TOO_MANY_SESSIONS    0x40 /* H->C */
#define MIDI2_BYE_AUTH_NO_PRIOR_INV    0x41 /* H->C */
#define MIDI2_BYE_USER_DECLINED        0x42 /* H->C */
#define MIDI2_BYE_AUTH_FAILED          0x43 /* H->C */
#define MIDI2_BYE_USERNAME_NOT_FOUND   0x44 /* H->C */
#define MIDI2_BYE_NO_MATCHING_AUTH     0x45 /* H->C */
#define MIDI2_BYE_INVITATION_CANCELED  0x80 /* C->H */

/* Retransmit Error reasons - Table 32. */
#define MIDI2_RETX_ERR_UNKNOWN     0x00
#define MIDI2_RETX_ERR_NOT_BUFFERED 0x01

/* ---- Big-endian byte helpers (wire is network byte order) --------------- */

static inline uint16_t midi2_rd_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t midi2_rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void midi2_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void midi2_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- Command packet header --------------------------------------------- */

typedef struct {
    uint8_t  code;          /* Command Code */
    uint8_t  payload_words; /* Command Payload Length (32-bit words) */
    uint8_t  csd1;          /* Command Specific Data byte 1 (high) */
    uint8_t  csd2;          /* Command Specific Data byte 2 (low)  */
    const uint8_t *payload; /* points into the source buffer, payload_words*4 bytes */
} midi2_cmd_view_t;

/*
 * Parse one command packet starting at buf (len bytes available). On success
 * fills *out and returns the total command size in bytes (4 + payload bytes).
 * Returns 0 if the buffer is too short for the declared payload.
 */
size_t midi2_proto_parse_command(const uint8_t *buf, size_t len, midi2_cmd_view_t *out);

/*
 * Append a command header word to buf at *off (advancing *off). cap is the
 * total buffer capacity. Returns false (and leaves *off unchanged past the
 * header) if it would overflow.
 */
bool midi2_proto_put_header(uint8_t *buf, size_t cap, size_t *off,
                            uint8_t code, uint8_t payload_words,
                            uint8_t csd1, uint8_t csd2);

#ifdef __cplusplus
}
#endif
