#pragma once
/*
 * dbserver.h — minimal remotedb (data) service.
 *
 * Answers players asking for metadata about the track we claim to be playing.
 * Without these replies the CDJs eventually report "Link Access Error": on a
 * DJ Link network silence is not neutral, and an unanswered query is retried
 * forever.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t txid;      /* transaction id: must be echoed back unchanged */
    uint16_t type;      /* type of the message received                  */
} db_request_t;

/* True if b is a valid remotedb message (correct magic). */
bool db_parse_message(const uint8_t *b, int32_t n, db_request_t *out);

/* Success reply (type 0x4000). count is the number of available items, or
 * 0xffffffff when the requested item does not exist. */
size_t db_msg_success(uint8_t *out, uint32_t txid, uint32_t req_type, uint32_t count);

/* Type 0x4003 reply: a SINGLE argument, the type of the request.
 * [M] Captured from a real XDJ-700 queried with our own 0x3d03. */
size_t db_msg_ack(uint8_t *out, uint32_t txid, uint32_t req_type);

/* Menu framing: header -> items... -> footer */
size_t db_msg_menu_header(uint8_t *out, uint32_t txid);
size_t db_msg_menu_footer(uint8_t *out, uint32_t txid);

/* One menu item. */
#define DB_ITEM_TITLE   0x0004
#define DB_ITEM_ARTIST  0x0007
#define DB_ITEM_TEMPO   0x000d
size_t db_msg_menu_item(uint8_t *out, uint32_t txid, const char *label1,
                        uint32_t num2, uint16_t item_type);

/* Request types we handle. */
#define DB_REQ_SETUP        0x0000   /* query context setup */
#define DB_REQ_METADATA     0x2002   /* track metadata      */
#define DB_REQ_RENDER_MENU  0x3000   /* send me the items   */

/* --- CLIENT side: we query a player's dbserver -----------------------------
 * Used to learn which track the tempo master is playing. */

/* Context setup: we introduce ourselves with our device number.
 *
 * The TxID is FIXED at 0xFFFFFFFE. It is a reserved value marking the opening
 * of the dialogue [M, verified byte by byte against requests from real
 * players]; with any other TxID the player closes the connection without
 * answering. */
#define DB_TXID_SETUP  0xFFFFFFFEu
size_t db_req_setup(uint8_t *out, uint8_t my_device);

/* Metadata request (0x2002) for one track:
 *   dmst         = (our device, 1, media slot, track type)
 *   rekordbox_id = id of the track to describe */
size_t db_req_metadata(uint8_t *out, uint32_t txid, uint8_t my_device,
                       uint8_t slot, uint8_t track_type, uint32_t rekordbox_id);

/* Menu render request (0x3000): "send me the N items". */
size_t db_req_render(uint8_t *out, uint32_t txid, uint8_t my_device,
                     uint8_t slot, uint8_t track_type, uint32_t count);

/* Pull the text of the item of the given type out of the reply stream.
 * Returns true when found. */
bool db_extract_item(const uint8_t *buf, int32_t len, uint16_t item_type,
                     char *out, size_t out_sz);
