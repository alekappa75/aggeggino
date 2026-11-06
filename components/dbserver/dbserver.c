/*
 * dbserver.c — minimal remotedb (data) service.
 *
 * WHY IT IS NEEDED: to impose tempo as master we must declare Mv=0x8000,
 * "rekordbox track loaded", or [M] the players will not follow our BPM. But a
 * player that declares a track must also be able to DESCRIBE it: without
 * answers to metadata requests the players report "E-8309 Link Access Error"
 * and their UI can lock up. Here we answer with a virtual track that identifies
 * the bridge.
 *
 * [E] Protocol, per dysentery: messages carrying magic 872349ae, a TxID, a
 * type, and typed arguments (0x11 = 4-byte number, 0x26 = UTF-16BE string,
 * 0x14 = blob).
 * Dialogue: setup (0x0000) -> metadata request (0x2002) -> render menu (0x3000)
 *           reply: header (0x4001) + item (0x4101)... + footer (0x4201)
 */
#include "dbserver.h"
#include <string.h>
#include <stdio.h>
/* ---------- building fields ---------- */

static size_t put_num(uint8_t *p, uint32_t v)      /* 4-byte number field */
{
    p[0] = 0x11;
    p[1] = (uint8_t)(v >> 24); p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 8);  p[4] = (uint8_t)v;
    return 5;
}

/* String field: 0x26 + length (4 bytes, in CHARACTERS including the NUL) +
 * the text as UTF-16BE. */
static size_t put_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    size_t chars = n + 1;                      /* the trailing NUL counts */
    p[0] = 0x26;
    p[1] = (uint8_t)(chars >> 24); p[2] = (uint8_t)(chars >> 16);
    p[3] = (uint8_t)(chars >> 8);  p[4] = (uint8_t)chars;
    size_t i = 5;
    for (size_t k = 0; k < n; k++) {           /* ASCII -> UTF-16 big endian */
        p[i++] = 0x00;
        p[i++] = (uint8_t)s[k];
    }
    p[i++] = 0x00; p[i++] = 0x00;              /* NUL terminator */
    return i;
}

/* Shared header: magic, TxID, type, argument count, tag blob. */
static size_t put_header(uint8_t *p, uint32_t txid, uint16_t type,
                         const uint8_t *tags, uint8_t n)
{
    size_t i = 0;
    p[i++] = 0x11; p[i++] = 0x87; p[i++] = 0x23; p[i++] = 0x49; p[i++] = 0xae;
    i += put_num(&p[i], txid);                 /* TxID, as a 4-byte number */
    p[i++] = 0x10;                             /* tag: 2-byte number */
    p[i++] = (uint8_t)(type >> 8); p[i++] = (uint8_t)type;
    p[i++] = 0x0f;                             /* tag: 1-byte number */
    p[i++] = n;                                /* argument count */
    p[i++] = 0x14;                             /* tag: blob */
    p[i++] = 0x00; p[i++] = 0x00; p[i++] = 0x00; p[i++] = 0x0c;   /* 12 byte */
    for (int k = 0; k < 12; k++) p[i++] = (k < n) ? tags[k] : 0x00;
    return i;
}

/* ---------- messages ---------- */

size_t db_msg_success(uint8_t *out, uint32_t txid, uint32_t req_type, uint32_t count)
{
    static const uint8_t tags[12] = { 0x06, 0x06 };
    size_t i = put_header(out, txid, 0x4000, tags, 2);
    i += put_num(&out[i], req_type);
    i += put_num(&out[i], count);
    return i;
}

/* [M] Reply 0x4003, byte-exact from a real XDJ-700:
 *   11 87 23 49 ae | 11 <txid> | 10 4003 | 0f 01 | 14 0000000c | 06 00*11 | 11 <type>
 * A single numeric argument: the type of the request. */
size_t db_msg_ack(uint8_t *out, uint32_t txid, uint32_t req_type)
{
    static const uint8_t tags[12] = { 0x06 };
    size_t i = put_header(out, txid, 0x4003, tags, 1);
    i += put_num(&out[i], req_type);
    return i;
}

size_t db_msg_menu_header(uint8_t *out, uint32_t txid)
{
    static const uint8_t tags[12] = { 0x06, 0x06 };
    size_t i = put_header(out, txid, 0x4001, tags, 2);
    i += put_num(&out[i], 1);
    i += put_num(&out[i], 0);
    return i;
}

size_t db_msg_menu_footer(uint8_t *out, uint32_t txid)
{
    static const uint8_t tags[12] = { 0x06, 0x06 };
    size_t i = put_header(out, txid, 0x4201, tags, 2);
    i += put_num(&out[i], 0);
    i += put_num(&out[i], 0);
    return i;
}

/* One menu item: 12 arguments, ten numbers and two strings.
 * label1 is the main text; item_type is 0x0004 title, 0x0007 artist, 0x000d
 * tempo; num2 is the main ID, or BPM x 100 for the tempo item. */
size_t db_msg_menu_item(uint8_t *out, uint32_t txid, const char *label1,
                        uint32_t num2, uint16_t item_type)
{
    /* Argument types: n n n S n S n n n n n n */
    static const uint8_t tags[12] = {
        0x06, 0x06, 0x06, 0x02, 0x06, 0x02, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06
    };
    size_t i = put_header(out, txid, 0x4101, tags, 12);
    i += put_num(&out[i], 0);                       /* 1: parent ID       */
    i += put_num(&out[i], num2);                    /* 2: main ID or BPM  */
    i += put_num(&out[i], (uint32_t)(strlen(label1) + 1) * 2);  /* 3: len L1 */
    i += put_str(&out[i], label1);                  /* 4: label 1         */
    i += put_num(&out[i], 2);                       /* 5: len L2 (empty)  */
    i += put_str(&out[i], "");                      /* 6: label 2         */
    i += put_num(&out[i], item_type);               /* 7: item type       */
    i += put_num(&out[i], 0);                       /* 8: flags           */
    i += put_num(&out[i], 0);                       /* 9: artwork ID      */
    i += put_num(&out[i], 0);                       /* 10: playlist pos.  */
    i += put_num(&out[i], 0);                       /* 11: ?              */
    i += put_num(&out[i], 0);                       /* 12: ?              */
    return i;
}

/* ---------- parsing ---------- */

bool db_parse_message(const uint8_t *b, int32_t n, db_request_t *out)
{
    if (n < 15 || b[0] != 0x11) return false;
    if (!(b[1] == 0x87 && b[2] == 0x23 && b[3] == 0x49 && b[4] == 0xae)) return false;
    if (b[5] != 0x11 || b[10] != 0x10) return false;
    out->txid = ((uint32_t)b[6] << 24) | ((uint32_t)b[7] << 16) |
                ((uint32_t)b[8] << 8)  | b[9];
    out->type = ((uint16_t)b[11] << 8) | b[12];
    return true;
}


/* ==== CLIENT side: we query a player's dbserver ========================== */

size_t db_req_setup(uint8_t *out, uint8_t my_device)
{
    static const uint8_t tags[12] = { 0x06 };
    /* [M] RESERVED TxID 0xFFFFFFFE. Real players always use it for setup, and
     * reject a setup carrying any other TxID by closing the connection. */
    size_t i = put_header(out, DB_TXID_SETUP, DB_REQ_SETUP, tags, 1);
    i += put_num(&out[i], my_device);
    return i;
}

/* dmst: device, menu (1), slot and track type, packed into one u32. */
static uint32_t dmst(uint8_t dev, uint8_t slot, uint8_t ttype)
{
    return ((uint32_t)dev << 24) | (1u << 16) | ((uint32_t)slot << 8) | ttype;
}

size_t db_req_metadata(uint8_t *out, uint32_t txid, uint8_t my_device,
                       uint8_t slot, uint8_t track_type, uint32_t rekordbox_id)
{
    static const uint8_t tags[12] = { 0x06, 0x06 };
    size_t i = put_header(out, txid, DB_REQ_METADATA, tags, 2);
    i += put_num(&out[i], dmst(my_device, slot, track_type));
    i += put_num(&out[i], rekordbox_id);
    return i;
}

size_t db_req_render(uint8_t *out, uint32_t txid, uint8_t my_device,
                     uint8_t slot, uint8_t track_type, uint32_t count)
{
    static const uint8_t tags[12] = { 0x06, 0x06, 0x06, 0x06, 0x06, 0x06 };
    size_t i = put_header(out, txid, DB_REQ_RENDER_MENU, tags, 6);
    i += put_num(&out[i], dmst(my_device, slot, track_type));
    i += put_num(&out[i], 0);          /* offset: from the first */
    i += put_num(&out[i], count);      /* how many we want       */
    i += put_num(&out[i], 0);
    i += put_num(&out[i], count);
    i += put_num(&out[i], 0);
    return i;
}

/* Walks the messages in the buffer and, for every menu item (0x4101), looks at the
 * type (argument 7) and, if it matches, copies out the label (argument 4, UTF-16BE). */
bool db_extract_item(const uint8_t *buf, int32_t len, uint16_t item_type,
                     char *out, size_t out_sz)
{
    int32_t i = 0;
    while (i + 15 < len) {
        /* Start of a message? magic 11 87 23 49 ae */
        if (!(buf[i] == 0x11 && buf[i+1] == 0x87 && buf[i+2] == 0x23 &&
              buf[i+3] == 0x49 && buf[i+4] == 0xae)) { i++; continue; }
        uint16_t type = ((uint16_t)buf[i+11] << 8) | buf[i+12];
        int32_t p = i + 15;                       /* past magic/txid/type/n  */
        if (buf[p] != 0x14) { i++; continue; }    /* the tag blob            */
        p += 5 + 12;                              /* skip blob tag + 12 tags */

        if (type != 0x4101) { i++; continue; }    /* menu items only         */

        /* Arguments: 1 num, 2 num, 3 num, 4 STRING, 5 num, 6 STRING, 7 num... */
        char label[128]; label[0] = 0;
        uint32_t found_type = 0;
        for (int arg = 1; arg <= 7 && p < len; arg++) {
            if (buf[p] == 0x11) {                 /* 4-byte number           */
                uint32_t v = ((uint32_t)buf[p+1]<<24)|((uint32_t)buf[p+2]<<16)|
                             ((uint32_t)buf[p+3]<<8) | buf[p+4];
                if (arg == 7) found_type = v;
                p += 5;
            } else if (buf[p] == 0x26) {          /* UTF-16BE string         */
                uint32_t chars = ((uint32_t)buf[p+1]<<24)|((uint32_t)buf[p+2]<<16)|
                                 ((uint32_t)buf[p+3]<<8) | buf[p+4];
                p += 5;
                if (arg == 4 && chars > 1) {      /* the main label          */
                    size_t k = 0;
                    for (uint32_t ch = 0; ch + 1 < chars && k + 1 < sizeof(label); ch++) {
                        uint8_t hi = buf[p + ch*2], lo = buf[p + ch*2 + 1];
                        if (hi == 0 && lo >= 32 && lo < 127) label[k++] = (char)lo;
                        else if (hi != 0 || lo != 0)         label[k++] = '?';
                    }
                    label[k] = 0;
                }
                p += chars * 2;
            } else break;
        }
        if (found_type == item_type && label[0]) {
            strncpy(out, label, out_sz - 1);
            out[out_sz - 1] = 0;
            return true;
        }
        i++;
    }
    return false;
}
