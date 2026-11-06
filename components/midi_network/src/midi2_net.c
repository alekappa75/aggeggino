/*
 * midi2_net.c - Network MIDI 2.0 (UDP) Host transport implementation.
 *
 * One FreeRTOS task owns the UDP socket. The application enqueues UMP for
 * transmission; the task drains the queue (woken immediately via a loopback
 * "self-pipe" socket so TX latency is not gated on the poll interval), services
 * received datagrams, and runs periodic session maintenance (ping, timeout,
 * repeated Bye/Reset). No lock is ever held across socket I/O.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "psa/crypto.h"   /* SHA-256 via PSA (portable across mbedtls 3.x/4.x) */

#include "midi2_net.h"
#include "midi2_net_proto.h"

/* From midi2_net_mdns.c */
esp_err_t midi2_net_mdns_start(const char *hostname, uint16_t port,
                               const char *endpoint_name, const char *product_id);
void midi2_net_mdns_stop(void);

static const char *TAG = "midi2_net";

/* ---- Tunables (Kconfig-overridable) ------------------------------------- */
#ifdef CONFIG_MIDI2_NET_RETX_RING
#define RETX_RING            CONFIG_MIDI2_NET_RETX_RING
#else
#define RETX_RING            16   /* stored sent UMP Data commands per session */
#endif

#ifdef CONFIG_MIDI2_NET_RETX_MAX_WORDS
#define RETX_MAX_WORDS       CONFIG_MIDI2_NET_RETX_MAX_WORDS
#else
#define RETX_MAX_WORDS       16   /* max words retained per command (FEC/retransmit) */
#endif

#ifdef CONFIG_MIDI2_NET_TX_QUEUE_LEN
#define TX_QUEUE_LEN         CONFIG_MIDI2_NET_TX_QUEUE_LEN
#else
#define TX_QUEUE_LEN         16
#endif

#define POLL_TIMEOUT_MS      200   /* select() timeout => maintenance granularity */
#define REPEAT_INTERVAL_MS   500   /* Bye / Session Reset repeat cadence */
#define MAX_REPEATS          6     /* give up repeating after this many */
#define RETX_REQUEST_DEBOUNCE_MS 10
#define GAP_RESYNC_MS        500   /* abandon an unfilled RX gap after this long */

#define DEFAULT_PING_INTERVAL_MS   2000
#define DEFAULT_SESSION_TIMEOUT_MS 10000
#define RX_BUF_SIZE          (MIDI2_NET_MAX_UDP_PAYLOAD + 16)

/* ---- Session state ------------------------------------------------------ */
typedef enum {
    SESS_FREE = 0,
    SESS_PENDING_INVITATION, /* (client) sent Invitation, awaiting a reply */
    SESS_AUTH_REQUIRED,   /* (host) sent Invitation Reply: Auth Required, awaiting digest */
    SESS_ESTABLISHED,
    SESS_PENDING_RESET,   /* sent Session Reset, awaiting Reply */
    SESS_PENDING_BYE,     /* sent Bye, awaiting Bye Reply */
} sess_state_t;

typedef struct {
    bool     valid;
    uint16_t seq;
    uint8_t  num_words;
    uint32_t words[RETX_MAX_WORDS];
} retx_slot_t;

typedef struct {
    sess_state_t state;
    bool     is_client;     /* we initiated this session (Client role) */
    struct sockaddr_storage addr;
    socklen_t addrlen;
    char     addr_str[48];
    uint16_t port;
    char     name[MIDI2_NET_MAX_ENDPOINT_NAME + 1];
    char     product_id[MIDI2_NET_MAX_PRODUCT_ID + 1];

    uint16_t tx_seq;        /* next outbound UMP Data sequence number */
    bool     rx_init;       /* have we delivered any inbound UMP Data */
    uint16_t rx_seq;        /* last delivered (in-order) inbound sequence number */
    int64_t  gap_since_ms;  /* when the current RX gap began (0 = none) */
    int64_t  retx_req_ms;   /* last Retransmit Request time (debounce) */

    uint8_t  nonce[MIDI2_NET_NONCE_LEN];
    uint8_t  auth_fail;

    int64_t  last_rx_ms;
    int64_t  last_ping_ms;
    uint32_t ping_id;
    bool     ping_pending;

    uint8_t  repeat_cmd;    /* 0 = none, else MIDI2_CMD_BYE / SESSION_RESET */
    uint8_t  repeat_reason;
    uint8_t  repeat_count;
    int64_t  repeat_due_ms;

    retx_slot_t retx[RETX_RING];
    uint8_t     retx_head;
} session_t;

struct midi2_net_host_s {
    int sock;
    int wake_sock;
    struct sockaddr_in wake_addr;
    uint16_t port;

    TaskHandle_t  task;
    QueueHandle_t tx_queue;
    SemaphoreHandle_t done;     /* task signals completion on stop */
    volatile bool running;

    char endpoint_name[MIDI2_NET_MAX_ENDPOINT_NAME + 1];
    char product_id[MIDI2_NET_MAX_PRODUCT_ID + 1];
    char auth_secret[64];
    bool enable_mdns;
    uint8_t  max_clients;
    uint8_t  fec_repeats;
    bool     request_retransmit;
    bool     require_auth;
    bool     act_as_host;       /* accept inbound Invitations + advertise mDNS */
    bool     respond_stream;
    char     fb_name[64];
    uint8_t  fb_first_group;
    uint8_t  fb_num_groups;
    uint32_t ping_interval_ms;
    uint32_t session_timeout_ms;

    midi2_net_recv_cb_t  recv_cb;
    midi2_net_event_cb_t event_cb;
    void *user_ctx;

    session_t *sessions;
};

/* Control-message ops carried over the same queue as UMP TX. */
#define TX_OP_UMP        0
#define TX_OP_CONNECT    1  /* start an outbound (Client) session to target */
#define TX_OP_DISCONNECT 2  /* Bye an existing session to target */

typedef struct {
    uint8_t  op;
    bool broadcast;
    struct sockaddr_storage target;
    socklen_t targetlen;
    uint8_t  num_words;
    uint32_t words[MIDI2_NET_MAX_UMP_DATA_WORDS];
} tx_msg_t;

/* ---- Small UDP packet builder ------------------------------------------- */
typedef struct {
    uint8_t buf[MIDI2_NET_MAX_UDP_PAYLOAD];
    size_t  len;
} pkt_t;

static void pkt_init(pkt_t *p)
{
    midi2_wr_u32(p->buf, MIDI2_NET_SIGNATURE);
    p->len = 4;
}

/* Append a command with a raw byte payload (payload_bytes must be 4-aligned). */
static bool pkt_add(pkt_t *p, uint8_t code, uint8_t csd1, uint8_t csd2,
                    const uint8_t *payload, size_t payload_bytes)
{
    size_t words = payload_bytes / 4;
    if (words > 255 || (payload_bytes & 3) != 0) {
        return false;
    }
    if (p->len + 4 + payload_bytes > sizeof(p->buf)) {
        return false;
    }
    p->buf[p->len++] = code;
    p->buf[p->len++] = (uint8_t)words;
    p->buf[p->len++] = csd1;
    p->buf[p->len++] = csd2;
    if (payload_bytes) {
        memcpy(p->buf + p->len, payload, payload_bytes);
        p->len += payload_bytes;
    }
    return true;
}

/* Append a UMP Data command (code 0xFF) with words in network byte order. */
static bool pkt_add_ump(pkt_t *p, uint16_t seq, const uint32_t *words, uint8_t num_words)
{
    size_t payload_bytes = (size_t)num_words * 4;
    if (p->len + 4 + payload_bytes > sizeof(p->buf)) {
        return false;
    }
    p->buf[p->len++] = MIDI2_CMD_UMP_DATA;
    p->buf[p->len++] = num_words;
    p->buf[p->len++] = (uint8_t)(seq >> 8);
    p->buf[p->len++] = (uint8_t)seq;
    for (uint8_t i = 0; i < num_words; i++) {
        midi2_wr_u32(p->buf + p->len, words[i]);
        p->len += 4;
    }
    return true;
}

/* ---- Helpers ------------------------------------------------------------ */
static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool addr_equal(const struct sockaddr_storage *a, socklen_t alen,
                       const struct sockaddr_storage *b, socklen_t blen)
{
    if (alen != blen || a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a;
        const struct sockaddr_in *y = (const struct sockaddr_in *)b;
        return x->sin_port == y->sin_port &&
               x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
#if LWIP_IPV6
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b;
        return x->sin6_port == y->sin6_port &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
    }
#endif
    return false;
}

static void addr_describe(const struct sockaddr_storage *a, char *out, size_t outsz,
                          uint16_t *port_out)
{
    out[0] = '\0';
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a;
        inet_ntop(AF_INET, &x->sin_addr, out, outsz);
        if (port_out) *port_out = ntohs(x->sin_port);
    }
#if LWIP_IPV6
    else if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;
        inet_ntop(AF_INET6, &x->sin6_addr, out, outsz);
        if (port_out) *port_out = ntohs(x->sin6_port);
    }
#endif
}

/* Parse a printable IP (v4 or v6) + port into a sockaddr_storage. Scope id is
 * not recovered for link-local v6, but find_session() matches on address+port
 * (ignoring scope) and replies use the session's stored address. */
static bool parse_addr(const char *ip, uint16_t port,
                       struct sockaddr_storage *out, socklen_t *len)
{
#if LWIP_IPV6
    /* The engine socket is dual-stack IPv6, so outbound targets must be IPv6
     * sockaddrs. Accept an IPv6 literal directly; map an IPv4 literal to an
     * IPv4-mapped IPv6 address (::ffff:a.b.c.d) so it goes out over IPv4. */
    struct sockaddr_in6 in6 = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
    if (inet_pton(AF_INET6, ip, &in6.sin6_addr) == 1) {
        memcpy(out, &in6, sizeof(in6));
        *len = sizeof(in6);
        return true;
    }
    struct in_addr v4;
    if (inet_pton(AF_INET, ip, &v4) == 1) {
        uint8_t *a = (uint8_t *)&in6.sin6_addr; /* 16 contiguous bytes */
        memset(a, 0, 16);
        a[10] = 0xff;
        a[11] = 0xff;
        memcpy(a + 12, &v4.s_addr, 4);
        memcpy(out, &in6, sizeof(in6));
        *len = sizeof(in6);
        return true;
    }
    return false;
#else
    struct sockaddr_in in4 = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &in4.sin_addr) == 1) {
        memcpy(out, &in4, sizeof(in4));
        *len = sizeof(in4);
        return true;
    }
    return false;
#endif
}

static void peer_from_session(const session_t *s, midi2_net_peer_t *peer)
{
    copy_str(peer->addr, sizeof(peer->addr), s->addr_str);
    peer->port = s->port;
    copy_str(peer->name, sizeof(peer->name), s->name);
    copy_str(peer->product_id, sizeof(peer->product_id), s->product_id);
}

static session_t *find_session(struct midi2_net_host_s *h,
                               const struct sockaddr_storage *addr, socklen_t alen)
{
    for (uint8_t i = 0; i < h->max_clients; i++) {
        if (h->sessions[i].state != SESS_FREE &&
            addr_equal(&h->sessions[i].addr, h->sessions[i].addrlen, addr, alen)) {
            return &h->sessions[i];
        }
    }
    return NULL;
}

static session_t *alloc_session(struct midi2_net_host_s *h,
                                const struct sockaddr_storage *addr, socklen_t alen)
{
    for (uint8_t i = 0; i < h->max_clients; i++) {
        if (h->sessions[i].state == SESS_FREE) {
            session_t *s = &h->sessions[i];
            memset(s, 0, sizeof(*s));
            memcpy(&s->addr, addr, alen);
            s->addrlen = alen;
            addr_describe(addr, s->addr_str, sizeof(s->addr_str), &s->port);
            s->last_rx_ms = now_ms();
            return s;
        }
    }
    return NULL;
}

static void emit_event(struct midi2_net_host_s *h, session_t *s, midi2_net_event_t ev)
{
    if (h->event_cb) {
        midi2_net_peer_t peer;
        peer_from_session(s, &peer);
        h->event_cb(h, &peer, ev, h->user_ctx);
    }
}

/* ---- Raw send ----------------------------------------------------------- */
static void send_pkt(struct midi2_net_host_s *h, session_t *s, const pkt_t *p)
{
    sendto(h->sock, p->buf, p->len, 0,
           (const struct sockaddr *)&s->addr, s->addrlen);
}

static void send_pkt_to(struct midi2_net_host_s *h, const struct sockaddr_storage *addr,
                        socklen_t alen, const pkt_t *p)
{
    sendto(h->sock, p->buf, p->len, 0, (const struct sockaddr *)addr, alen);
}

/* ---- Build identity payload: name (padded) + product id (padded) -------- */
/* Returns number of name words via *name_words. Writes to out, returns total bytes. */
static size_t build_identity(const struct midi2_net_host_s *h, uint8_t *out,
                             uint8_t *name_words_out)
{
    size_t nlen = strlen(h->endpoint_name);
    size_t plen = strlen(h->product_id);
    size_t nwords = (nlen + 3) / 4;
    size_t pwords = (plen + 3) / 4;
    size_t off = 0;
    memset(out, 0, nwords * 4 + pwords * 4);
    memcpy(out, h->endpoint_name, nlen);
    off = nwords * 4;
    memcpy(out + off, h->product_id, plen);
    off += pwords * 4;
    *name_words_out = (uint8_t)nwords;
    return off;
}

/* ---- Specific senders --------------------------------------------------- */
static void send_invitation_accept(struct midi2_net_host_s *h, session_t *s)
{
    uint8_t payload[(MIDI2_NET_MAX_ENDPOINT_NAME + 3) / 4 * 4 +
                    (MIDI2_NET_MAX_PRODUCT_ID + 3) / 4 * 4];
    uint8_t name_words = 0;
    size_t bytes = build_identity(h, payload, &name_words);
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_INVITATION_REPLY_ACCEPT, name_words, 0, payload, bytes);
    send_pkt(h, s, &p);
}

static void send_auth_required(struct midi2_net_host_s *h, session_t *s, uint8_t auth_state)
{
    uint8_t payload[MIDI2_NET_NONCE_LEN +
                    (MIDI2_NET_MAX_ENDPOINT_NAME + 3) / 4 * 4 +
                    (MIDI2_NET_MAX_PRODUCT_ID + 3) / 4 * 4];
    memcpy(payload, s->nonce, MIDI2_NET_NONCE_LEN);
    uint8_t name_words = 0;
    size_t idbytes = build_identity(h, payload + MIDI2_NET_NONCE_LEN, &name_words);
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_INVITATION_REPLY_AUTH, name_words, auth_state,
            payload, MIDI2_NET_NONCE_LEN + idbytes);
    send_pkt(h, s, &p);
}

/* Client -> Host: Invitation (6.4). Advertises our identity + capabilities. */
static void send_invitation(struct midi2_net_host_s *h, session_t *s)
{
    uint8_t payload[(MIDI2_NET_MAX_ENDPOINT_NAME + 3) / 4 * 4 +
                    (MIDI2_NET_MAX_PRODUCT_ID + 3) / 4 * 4];
    uint8_t name_words = 0;
    size_t bytes = build_identity(h, payload, &name_words);
    /* D0: we support Invitation with Authentication (shared secret). */
    uint8_t caps = (strlen(h->auth_secret) > 0) ? MIDI2_CAP_AUTH : 0;
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_INVITATION, name_words, caps, payload, bytes);
    send_pkt(h, s, &p);
}

/* Client -> Host: Invitation with Authentication (6.9). digest over s->nonce. */
static void send_invitation_auth(struct midi2_net_host_s *h, session_t *s)
{
    uint8_t input[MIDI2_NET_NONCE_LEN + sizeof(h->auth_secret)];
    size_t slen = strlen(h->auth_secret);
    memcpy(input, s->nonce, MIDI2_NET_NONCE_LEN);
    memcpy(input + MIDI2_NET_NONCE_LEN, h->auth_secret, slen);
    uint8_t digest[MIDI2_NET_DIGEST_LEN];
    size_t dl = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, input, MIDI2_NET_NONCE_LEN + slen,
                         digest, sizeof(digest), &dl) != PSA_SUCCESS || dl != MIDI2_NET_DIGEST_LEN) {
        return;
    }
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_INVITATION_AUTH, 0, 0, digest, MIDI2_NET_DIGEST_LEN);
    send_pkt(h, s, &p);
}

/* Copy peer endpoint name (csd1 words) + product id from a command payload,
 * starting `offset` bytes in (16 for Auth Required replies, after the nonce). */
static void parse_peer_identity(session_t *s, const midi2_cmd_view_t *cmd, size_t offset)
{
    size_t total = (size_t)cmd->payload_words * 4;
    size_t nbytes = (size_t)cmd->csd1 * 4;
    if (offset + nbytes > total) {
        return;
    }
    const uint8_t *base = cmd->payload + offset;
    size_t ncopy = nbytes < MIDI2_NET_MAX_ENDPOINT_NAME ? nbytes : MIDI2_NET_MAX_ENDPOINT_NAME;
    memcpy(s->name, base, ncopy);
    s->name[ncopy] = '\0';
    size_t pbytes = total - offset - nbytes;
    size_t pcopy = pbytes < MIDI2_NET_MAX_PRODUCT_ID ? pbytes : MIDI2_NET_MAX_PRODUCT_ID;
    memcpy(s->product_id, base + nbytes, pcopy);
    s->product_id[pcopy] = '\0';
}

static void send_ping_reply(struct midi2_net_host_s *h, const struct sockaddr_storage *addr,
                            socklen_t alen, uint32_t ping_id)
{
    uint8_t pl[4];
    midi2_wr_u32(pl, ping_id);
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_PING_REPLY, 0, 0, pl, 4);
    send_pkt_to(h, addr, alen, &p);
}

static void send_ping(struct midi2_net_host_s *h, session_t *s)
{
    s->ping_id = esp_random();
    s->ping_pending = true;
    s->last_ping_ms = now_ms();
    uint8_t pl[4];
    midi2_wr_u32(pl, s->ping_id);
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_PING, 0, 0, pl, 4);
    send_pkt(h, s, &p);
}

static void send_bye_reply(struct midi2_net_host_s *h, const struct sockaddr_storage *addr,
                           socklen_t alen)
{
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_BYE_REPLY, 0, 0, NULL, 0);
    send_pkt_to(h, addr, alen, &p);
}

static void send_bye(struct midi2_net_host_s *h, session_t *s, uint8_t reason)
{
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_BYE, reason, 0, NULL, 0);
    send_pkt(h, s, &p);
}

static void send_nak(struct midi2_net_host_s *h, const struct sockaddr_storage *addr,
                     socklen_t alen, uint8_t reason, const uint8_t *naked_header)
{
    uint8_t pl[4] = {0};
    if (naked_header) memcpy(pl, naked_header, 4);
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_NAK, reason, 0, pl, 4);
    send_pkt_to(h, addr, alen, &p);
}

static void send_session_reset_reply(struct midi2_net_host_s *h, session_t *s)
{
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_SESSION_RESET_REPLY, 0, 0, NULL, 0);
    send_pkt(h, s, &p);
}

static void send_retransmit_error(struct midi2_net_host_s *h, session_t *s,
                                  uint8_t reason, uint16_t seq)
{
    uint8_t pl[4];
    midi2_wr_u16(pl, seq);
    pl[2] = 0; pl[3] = 0;
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_RETRANSMIT_ERROR, reason, 0, pl, 4);
    send_pkt(h, s, &p);
}

static void send_retransmit_request(struct midi2_net_host_s *h, session_t *s,
                                    uint16_t from_seq, uint16_t count)
{
    uint8_t pl[4];
    midi2_wr_u16(pl, count);
    pl[2] = 0; pl[3] = 0;
    pkt_t p; pkt_init(&p);
    pkt_add(&p, MIDI2_CMD_RETRANSMIT_REQUEST,
            (uint8_t)(from_seq >> 8), (uint8_t)from_seq, pl, 4);
    send_pkt(h, s, &p);
}

/* ---- Retransmit ring ---------------------------------------------------- */
static void retx_store(session_t *s, uint16_t seq, const uint32_t *words, uint8_t num_words)
{
    if (num_words > RETX_MAX_WORDS) {
        /* Too large to retain; mark the slot invalid so a later request for this
         * seq yields a Retransmit Error rather than stale data. */
        retx_slot_t *slot = &s->retx[s->retx_head];
        slot->valid = false;
        slot->seq = seq;
        s->retx_head = (uint8_t)((s->retx_head + 1) % RETX_RING);
        return;
    }
    retx_slot_t *slot = &s->retx[s->retx_head];
    slot->valid = true;
    slot->seq = seq;
    slot->num_words = num_words;
    memcpy(slot->words, words, (size_t)num_words * 4);
    s->retx_head = (uint8_t)((s->retx_head + 1) % RETX_RING);
}

static retx_slot_t *retx_find(session_t *s, uint16_t seq)
{
    for (uint8_t i = 0; i < RETX_RING; i++) {
        if (s->retx[i].seq == seq && s->retx[i].valid) {
            return &s->retx[i];
        }
    }
    return NULL;
}

/* ---- Send UMP (called from the transport task) -------------------------- */
static void tx_to_session(struct midi2_net_host_s *h, session_t *s,
                          const uint32_t *words, uint8_t num_words)
{
    if (s->state != SESS_ESTABLISHED) {
        return;
    }
    pkt_t p; pkt_init(&p);
    /* Forward Error Correction: prepend up to fec_repeats previously sent
     * commands, oldest first (7.2.2). Walk back from the most recent. */
    for (int rep = h->fec_repeats; rep >= 1; rep--) {
        /* slot index of the rep-th most recent stored command */
        int idx = ((int)s->retx_head - rep + 2 * RETX_RING) % RETX_RING;
        retx_slot_t *slot = &s->retx[idx];
        if (slot->valid) {
            pkt_add_ump(&p, slot->seq, slot->words, slot->num_words);
        }
    }
    uint16_t seq = s->tx_seq;
    pkt_add_ump(&p, seq, words, num_words);
    retx_store(s, seq, words, num_words);
    s->tx_seq++;
    send_pkt(h, s, &p);
}

/* ---- Session teardown --------------------------------------------------- */
static void close_session(struct midi2_net_host_s *h, session_t *s, bool notify)
{
    if (s->state == SESS_FREE) return;
    bool was_active = (s->state == SESS_ESTABLISHED || s->state == SESS_PENDING_RESET ||
                       s->state == SESS_PENDING_BYE);
    if (notify && was_active) {
        emit_event(h, s, MIDI2_NET_EVENT_SESSION_CLOSED);
    }
    s->state = SESS_FREE; /* word-sized write; the transport task is the sole writer */
}

static void begin_bye(struct midi2_net_host_s *h, session_t *s, uint8_t reason, bool notify)
{
    send_bye(h, s, reason);
    if (notify && s->state == SESS_ESTABLISHED) {
        emit_event(h, s, MIDI2_NET_EVENT_SESSION_CLOSED);
    }
    s->state = SESS_PENDING_BYE;
    s->repeat_cmd = MIDI2_CMD_BYE;
    s->repeat_reason = reason;
    s->repeat_count = 0;
    s->repeat_due_ms = now_ms() + REPEAT_INTERVAL_MS;
}

/* ---- Client: initiate an outbound session ------------------------------- */
static void start_client_session(struct midi2_net_host_s *h,
                                 const struct sockaddr_storage *addr, socklen_t alen)
{
    session_t *s = find_session(h, addr, alen);
    if (s) {
        /* Already connecting/connected to this peer: nudge the Invitation. */
        if (s->is_client && s->state == SESS_PENDING_INVITATION) {
            send_invitation(h, s);
        }
        return;
    }
    s = alloc_session(h, addr, alen);
    if (!s) {
        ESP_LOGW(TAG, "connect: no free session slot");
        return;
    }
    s->is_client = true;
    s->state = SESS_PENDING_INVITATION;
    send_invitation(h, s);
    s->repeat_cmd = MIDI2_CMD_INVITATION;
    s->repeat_count = 0;
    s->repeat_due_ms = now_ms() + REPEAT_INTERVAL_MS;
    ESP_LOGI(TAG, "connecting to %s:%u", s->addr_str, s->port);
}

/* ---- Inbound UMP Data handling ------------------------------------------ */
static void deliver_ump(struct midi2_net_host_s *h, session_t *s,
                        const uint32_t *words, uint8_t num_words)
{
    if (num_words == 0 || !h->recv_cb) {
        return; /* zero-length keepalive, or nobody listening */
    }
    midi2_net_peer_t peer;
    peer_from_session(s, &peer);
    h->recv_cb(h, &peer, words, num_words, h->user_ctx);
}

/* Respond to an incoming UMP Stream message (Endpoint / Function Block
 * discovery, Stream Config Request). Builds the requested notifications and
 * sends them back over the session. msg points at one 4-word stream message. */
static void handle_stream_message(struct midi2_net_host_s *h, session_t *s,
                                  const uint32_t *msg, size_t n)
{
    if (n < 4) {
        return;
    }
    uint32_t w[28]; /* up to 7 text packets (98-byte name) */
    switch (midi2_ump_stream_status(msg[0])) {
    case MIDI2_UMP_STREAM_ENDPOINT_DISCOVERY: {
        uint8_t filter = (uint8_t)(msg[1] & 0xFF);
        if (filter & MIDI2_UMP_DISC_ENDPOINT_INFO) {
            midi2_ump_stream_endpoint_info(w, 1, true, true, true);
            tx_to_session(h, s, w, 4);
        }
        if (filter & MIDI2_UMP_DISC_ENDPOINT_NAME) {
            size_t nw = midi2_ump_stream_text(w, 28, MIDI2_UMP_STREAM_ENDPOINT_NAME, -1, h->endpoint_name);
            if (nw) tx_to_session(h, s, w, (uint8_t)nw);
        }
        if (filter & MIDI2_UMP_DISC_PRODUCT_ID) {
            size_t nw = midi2_ump_stream_text(w, 28, MIDI2_UMP_STREAM_PRODUCT_INSTANCE_ID, -1, h->product_id);
            if (nw) tx_to_session(h, s, w, (uint8_t)nw);
        }
        if (filter & MIDI2_UMP_DISC_STREAM_CONFIG) {
            midi2_ump_stream_config_notification(w, MIDI2_UMP_PROTOCOL_MIDI2);
            tx_to_session(h, s, w, 4);
        }
        break;
    }
    case MIDI2_UMP_STREAM_CONFIG_REQUEST:
        midi2_ump_stream_config_notification(w, MIDI2_UMP_PROTOCOL_MIDI2);
        tx_to_session(h, s, w, 4);
        break;
    case MIDI2_UMP_STREAM_FB_DISCOVERY: {
        uint8_t fb = (uint8_t)((msg[0] >> 8) & 0xFF);
        uint8_t filter = (uint8_t)(msg[0] & 0xFF);
        if (fb == 0xFF || fb == 0) { /* we declare exactly one Function Block (0) */
            if (filter & MIDI2_UMP_DISC_FB_INFO) {
                midi2_ump_stream_fb_info(w, 0, MIDI2_UMP_FB_DIR_BIDIRECTIONAL,
                                         h->fb_first_group, h->fb_num_groups,
                                         MIDI2_UMP_FB_UI_BOTH, 0);
                tx_to_session(h, s, w, 4);
            }
            if (filter & MIDI2_UMP_DISC_FB_NAME) {
                size_t nw = midi2_ump_stream_text(w, 28, MIDI2_UMP_STREAM_FB_NAME, 0, h->fb_name);
                if (nw) tx_to_session(h, s, w, (uint8_t)nw);
            }
        }
        break;
    }
    default:
        break; /* peer notifications / unsupported: ignore */
    }
}

/* Auto-respond to UMP Stream discovery, then hand the messages to the app. */
static void process_delivered(struct midi2_net_host_s *h, session_t *s,
                              const uint32_t *words, uint8_t num_words)
{
    if (h->respond_stream) {
        size_t i = 0;
        while (i < num_words) {
            size_t n = midi2_ump_message_num_words(words[i]);
            if (i + n > num_words) break;
            if (midi2_ump_message_type(words[i]) == MIDI2_UMP_MT_STREAM) {
                handle_stream_message(h, s, &words[i], n);
            }
            i += n;
        }
    }
    deliver_ump(h, s, words, num_words);
}

static void handle_ump_data(struct midi2_net_host_s *h, session_t *s,
                            uint16_t seq, const uint8_t *payload, uint8_t num_words)
{
    /* Decode payload words (network byte order) into host words. */
    uint32_t words[MIDI2_NET_MAX_UMP_DATA_WORDS];
    for (uint8_t i = 0; i < num_words; i++) {
        words[i] = midi2_rd_u32(payload + (size_t)i * 4);
    }

    if (!s->rx_init) {
        s->rx_init = true;
        s->rx_seq = seq;
        process_delivered(h, s, words, num_words);
        return;
    }

    int16_t d = (int16_t)(seq - s->rx_seq);
    if (d <= 0) {
        /* Duplicate / FEC repeat / old: already processed (5.x, 7.2). Skip. */
        return;
    }
    if (d == 1) {
        s->rx_seq = seq;
        s->gap_since_ms = 0;
        process_delivered(h, s, words, num_words);
        return;
    }

    /* Forward gap: packets missing between rx_seq+1 and seq. */
    if (h->request_retransmit) {
        int64_t t = now_ms();
        if (t - s->retx_req_ms >= RETX_REQUEST_DEBOUNCE_MS) {
            send_retransmit_request(h, s, (uint16_t)(s->rx_seq + 1), (uint16_t)(d - 1));
            s->retx_req_ms = t;
        }
        if (s->gap_since_ms == 0) {
            s->gap_since_ms = t;
        }
        /* Hold this ahead-of-order packet; the sender resends the gap in order.
         * A stuck gap is abandoned in maintenance (GAP_RESYNC_MS). */
        return;
    }

    /* No retransmit: accept the loss, deliver, and resync forward. */
    ESP_LOGW(TAG, "%s: dropped %d UMP packet(s) (seq gap)", s->addr_str, d - 1);
    s->rx_seq = seq;
    s->gap_since_ms = 0;
    process_delivered(h, s, words, num_words);
}

/* ---- Command dispatch --------------------------------------------------- */
static void handle_command(struct midi2_net_host_s *h, session_t **psess,
                           const struct sockaddr_storage *addr, socklen_t alen,
                           const midi2_cmd_view_t *cmd)
{
    session_t *s = *psess;
    uint8_t naked_hdr[4] = { cmd->code, cmd->payload_words, cmd->csd1, cmd->csd2 };

    if (s) {
        s->last_rx_ms = now_ms();
    }

    switch (cmd->code) {
    case MIDI2_CMD_PING: {
        uint32_t ping_id = (cmd->payload_words >= 1) ? midi2_rd_u32(cmd->payload) : 0;
        send_ping_reply(h, addr, alen, ping_id);
        break;
    }
    case MIDI2_CMD_PING_REPLY: {
        if (s && s->ping_pending && cmd->payload_words >= 1 &&
            midi2_rd_u32(cmd->payload) == s->ping_id) {
            s->ping_pending = false;
        } else if (s) {
            send_nak(h, addr, alen, MIDI2_NAK_BAD_PING_REPLY, naked_hdr);
        }
        break;
    }
    case MIDI2_CMD_BYE: {
        send_bye_reply(h, addr, alen);
        if (s) {
            if (s->is_client && s->state == SESS_PENDING_INVITATION) {
                emit_event(h, s, MIDI2_NET_EVENT_INVITATION_DECLINED);
            }
            close_session(h, s, true);
            *psess = NULL;
        }
        break;
    }
    case MIDI2_CMD_BYE_REPLY: {
        if (s && s->state == SESS_PENDING_BYE) {
            close_session(h, s, false);
            *psess = NULL;
        }
        break;
    }
    case MIDI2_CMD_NAK: {
        if (cmd->payload_words >= 1) {
            ESP_LOGW(TAG, "%s: received NAK reason 0x%02x for cmd 0x%02x",
                     s ? s->addr_str : "?", cmd->csd1, cmd->payload[0]);
        }
        break;
    }
    case MIDI2_CMD_INVITATION: {
        if (!h->act_as_host) {
            /* Client-only endpoint: an Invitation is not expected (6.4). */
            send_nak(h, addr, alen, MIDI2_NAK_NOT_EXPECTED, naked_hdr);
            break;
        }
        if (cmd->payload_words < 2) {
            send_nak(h, addr, alen, MIDI2_NAK_MALFORMED, naked_hdr);
            break;
        }
        if (!s) {
            s = alloc_session(h, addr, alen);
            if (!s) {
                /* No capacity: reject (6.4 / Bye reason 0x40). */
                pkt_t p; pkt_init(&p);
                pkt_add(&p, MIDI2_CMD_BYE, MIDI2_BYE_TOO_MANY_SESSIONS, 0, NULL, 0);
                send_pkt_to(h, addr, alen, &p);
                break;
            }
            *psess = s;
        }
        /* Parse Client identity: name (csd1 words), product id (rest). */
        uint8_t name_words = cmd->csd1;
        uint8_t cap = cmd->csd2;
        if (name_words > cmd->payload_words) {
            send_nak(h, addr, alen, MIDI2_NAK_MALFORMED, naked_hdr);
            break;
        }
        size_t nbytes = (size_t)name_words * 4;
        size_t pbytes = (size_t)(cmd->payload_words - name_words) * 4;
        size_t ncopy = nbytes < MIDI2_NET_MAX_ENDPOINT_NAME ? nbytes : MIDI2_NET_MAX_ENDPOINT_NAME;
        memcpy(s->name, cmd->payload, ncopy);
        s->name[ncopy] = '\0';
        size_t pcopy = pbytes < MIDI2_NET_MAX_PRODUCT_ID ? pbytes : MIDI2_NET_MAX_PRODUCT_ID;
        memcpy(s->product_id, cmd->payload + nbytes, pcopy);
        s->product_id[pcopy] = '\0';

        if (s->state == SESS_ESTABLISHED) {
            /* Already in a session with this Client: re-accept (6.5). */
            send_invitation_accept(h, s);
            break;
        }
        if (h->require_auth) {
            if (!(cap & MIDI2_CAP_AUTH)) {
                begin_bye(h, s, MIDI2_BYE_NO_MATCHING_AUTH, false);
                emit_event(h, s, MIDI2_NET_EVENT_INVITATION_DECLINED);
                break;
            }
            esp_fill_random(s->nonce, MIDI2_NET_NONCE_LEN);
            s->state = SESS_AUTH_REQUIRED;
            send_auth_required(h, s, MIDI2_AUTH_STATE_FIRST);
        } else {
            s->state = SESS_ESTABLISHED;
            send_invitation_accept(h, s);
            emit_event(h, s, MIDI2_NET_EVENT_SESSION_ESTABLISHED);
            ESP_LOGI(TAG, "session established with %s:%u (%s)",
                     s->addr_str, s->port, s->name);
        }
        break;
    }
    case MIDI2_CMD_INVITATION_AUTH: {
        if (!s || s->state != SESS_AUTH_REQUIRED) {
            send_nak(h, addr, alen, MIDI2_NAK_NOT_EXPECTED, naked_hdr);
            break;
        }
        if (cmd->payload_words < (MIDI2_NET_DIGEST_LEN / 4)) {
            send_nak(h, addr, alen, MIDI2_NAK_MALFORMED, naked_hdr);
            break;
        }
        /* digest = SHA-256(CryptoNonce || shared secret) (6.9). */
        uint8_t input[MIDI2_NET_NONCE_LEN + sizeof(h->auth_secret)];
        size_t slen = strlen(h->auth_secret);
        memcpy(input, s->nonce, MIDI2_NET_NONCE_LEN);
        memcpy(input + MIDI2_NET_NONCE_LEN, h->auth_secret, slen);
        uint8_t digest[MIDI2_NET_DIGEST_LEN];
        size_t digest_len = 0;
        psa_status_t ps = psa_hash_compute(PSA_ALG_SHA_256, input,
                                           MIDI2_NET_NONCE_LEN + slen,
                                           digest, sizeof(digest), &digest_len);
        if (ps == PSA_SUCCESS && digest_len == MIDI2_NET_DIGEST_LEN &&
            memcmp(digest, cmd->payload, MIDI2_NET_DIGEST_LEN) == 0) {
            s->state = SESS_ESTABLISHED;
            send_invitation_accept(h, s);
            emit_event(h, s, MIDI2_NET_EVENT_SESSION_ESTABLISHED);
            ESP_LOGI(TAG, "session established (authenticated) with %s:%u",
                     s->addr_str, s->port);
        } else if (++s->auth_fail >= 3) {
            begin_bye(h, s, MIDI2_BYE_AUTH_FAILED, false);
            emit_event(h, s, MIDI2_NET_EVENT_INVITATION_DECLINED);
        } else {
            send_auth_required(h, s, MIDI2_AUTH_STATE_DIGEST_WRONG);
        }
        break;
    }
    case MIDI2_CMD_UMP_DATA: {
        if (!s || s->state != SESS_ESTABLISHED) {
            /* 7.1: not in an Established Session => Bye (0x05). */
            pkt_t p; pkt_init(&p);
            pkt_add(&p, MIDI2_CMD_BYE, MIDI2_BYE_SESSION_NOT_ESTAB, 0, NULL, 0);
            send_pkt_to(h, addr, alen, &p);
            break;
        }
        uint16_t seq = ((uint16_t)cmd->csd1 << 8) | cmd->csd2;
        handle_ump_data(h, s, seq, cmd->payload, cmd->payload_words);
        break;
    }
    case MIDI2_CMD_RETRANSMIT_REQUEST: {
        if (!s || s->state != SESS_ESTABLISHED) {
            pkt_t p; pkt_init(&p);
            pkt_add(&p, MIDI2_CMD_BYE, MIDI2_BYE_SESSION_NOT_ESTAB, 0, NULL, 0);
            send_pkt_to(h, addr, alen, &p);
            break;
        }
        uint16_t from = ((uint16_t)cmd->csd1 << 8) | cmd->csd2;
        uint16_t count = (cmd->payload_words >= 1) ? midi2_rd_u16(cmd->payload) : 0;
        /* count 0 => all stored from `from`. Bound by ring size. */
        uint16_t limit = count ? count : RETX_RING;
        for (uint16_t i = 0; i < limit; i++) {
            uint16_t want = (uint16_t)(from + i);
            retx_slot_t *slot = retx_find(s, want);
            if (!slot) {
                send_retransmit_error(h, s, MIDI2_RETX_ERR_NOT_BUFFERED, want);
                break;
            }
            pkt_t p; pkt_init(&p);
            pkt_add_ump(&p, slot->seq, slot->words, slot->num_words);
            send_pkt(h, s, &p);
        }
        break;
    }
    case MIDI2_CMD_RETRANSMIT_ERROR: {
        if (s) {
            ESP_LOGW(TAG, "%s: peer cannot retransmit (reason 0x%02x)",
                     s->addr_str, cmd->csd1);
        }
        break;
    }
    case MIDI2_CMD_SESSION_RESET: {
        if (!s) {
            pkt_t p; pkt_init(&p);
            pkt_add(&p, MIDI2_CMD_BYE, MIDI2_BYE_SESSION_NOT_ESTAB, 0, NULL, 0);
            send_pkt_to(h, addr, alen, &p);
            break;
        }
        /* Reset our state and acknowledge (6.11). */
        s->tx_seq = 0;
        s->rx_init = false;
        s->gap_since_ms = 0;
        for (uint8_t i = 0; i < RETX_RING; i++) s->retx[i].valid = false;
        s->retx_head = 0;
        if (s->state != SESS_FREE) s->state = SESS_ESTABLISHED;
        send_session_reset_reply(h, s);
        break;
    }
    case MIDI2_CMD_SESSION_RESET_REPLY: {
        if (s && s->state == SESS_PENDING_RESET) {
            s->tx_seq = 0;
            s->rx_init = false;
            for (uint8_t i = 0; i < RETX_RING; i++) s->retx[i].valid = false;
            s->retx_head = 0;
            s->state = SESS_ESTABLISHED;
            s->repeat_cmd = 0;
        }
        break;
    }
    case MIDI2_CMD_INVITATION_REPLY_ACCEPT:
        if (s && s->is_client &&
            (s->state == SESS_PENDING_INVITATION || s->state == SESS_ESTABLISHED)) {
            parse_peer_identity(s, cmd, 0);
            if (s->state != SESS_ESTABLISHED) {
                s->state = SESS_ESTABLISHED;
                s->repeat_cmd = 0;
                emit_event(h, s, MIDI2_NET_EVENT_SESSION_ESTABLISHED);
                ESP_LOGI(TAG, "connected to %s:%u (%s)", s->addr_str, s->port, s->name);
            }
        } else {
            send_nak(h, addr, alen, MIDI2_NAK_NOT_EXPECTED, naked_hdr);
        }
        break;
    case MIDI2_CMD_INVITATION_REPLY_PEND:
        if (s && s->is_client && s->state == SESS_PENDING_INVITATION) {
            /* Host needs time (e.g. user prompt); keep waiting, slow the repeat. */
            s->repeat_count = 0;
            s->repeat_due_ms = now_ms() + REPEAT_INTERVAL_MS * 4;
        } else {
            send_nak(h, addr, alen, MIDI2_NAK_NOT_EXPECTED, naked_hdr);
        }
        break;
    case MIDI2_CMD_INVITATION_REPLY_AUTH:
        if (s && s->is_client && s->state == SESS_PENDING_INVITATION) {
            if (cmd->payload_words * 4 < MIDI2_NET_NONCE_LEN || strlen(h->auth_secret) == 0) {
                send_bye(h, s, MIDI2_BYE_INVITATION_CANCELED);
                emit_event(h, s, MIDI2_NET_EVENT_INVITATION_DECLINED);
                close_session(h, s, false);
                *psess = NULL;
                break;
            }
            memcpy(s->nonce, cmd->payload, MIDI2_NET_NONCE_LEN); /* CryptoNonce */
            send_invitation_auth(h, s);
            s->repeat_cmd = MIDI2_CMD_INVITATION_AUTH;
            s->repeat_count = 0;
            s->repeat_due_ms = now_ms() + REPEAT_INTERVAL_MS;
        } else {
            send_nak(h, addr, alen, MIDI2_NAK_NOT_EXPECTED, naked_hdr);
        }
        break;
    case MIDI2_CMD_INVITATION_REPLY_UAUTH:
        /* User authentication as a Client is not implemented: cancel. */
        if (s && s->is_client && s->state == SESS_PENDING_INVITATION) {
            send_bye(h, s, MIDI2_BYE_INVITATION_CANCELED);
            emit_event(h, s, MIDI2_NET_EVENT_INVITATION_DECLINED);
            close_session(h, s, false);
            *psess = NULL;
        } else {
            send_nak(h, addr, alen, MIDI2_NAK_NOT_EXPECTED, naked_hdr);
        }
        break;
    case MIDI2_CMD_INVITATION_USER_AUTH:
        send_nak(h, addr, alen, MIDI2_NAK_NOT_SUPPORTED, naked_hdr);
        break;
    default:
        send_nak(h, addr, alen, MIDI2_NAK_NOT_SUPPORTED, naked_hdr);
        break;
    }
}

static void process_datagram(struct midi2_net_host_s *h, const uint8_t *buf, size_t len,
                             const struct sockaddr_storage *addr, socklen_t alen)
{
    if (len < 4 || midi2_rd_u32(buf) != MIDI2_NET_SIGNATURE) {
        return; /* 5.2: bad signature => ignore the whole packet */
    }
    session_t *s = find_session(h, addr, alen);
    size_t off = 4;
    while (off + 4 <= len) {
        midi2_cmd_view_t cmd;
        size_t consumed = midi2_proto_parse_command(buf + off, len - off, &cmd);
        if (consumed == 0) {
            break; /* truncated/malformed trailing data */
        }
        handle_command(h, &s, addr, alen, &cmd);
        off += consumed;
    }
}

/* ---- Periodic maintenance ----------------------------------------------- */
static void maintenance(struct midi2_net_host_s *h)
{
    int64_t t = now_ms();
    for (uint8_t i = 0; i < h->max_clients; i++) {
        session_t *s = &h->sessions[i];
        if (s->state == SESS_FREE) continue;

        /* Repeated Invitation (client) / Bye (6.2). begin_bye() already emitted
         * any SESSION_CLOSED event, so finalize quietly when repeats run out. */
        if (s->repeat_cmd && t >= s->repeat_due_ms) {
            if (s->repeat_count >= MAX_REPEATS) {
                if (s->repeat_cmd == MIDI2_CMD_INVITATION ||
                    s->repeat_cmd == MIDI2_CMD_INVITATION_AUTH) {
                    /* Invitation never answered: cancel and report the failure. */
                    send_bye(h, s, MIDI2_BYE_INVITATION_CANCELED);
                    emit_event(h, s, MIDI2_NET_EVENT_INVITATION_DECLINED);
                }
                close_session(h, s, false);
                continue;
            }
            switch (s->repeat_cmd) {
            case MIDI2_CMD_BYE:             send_bye(h, s, s->repeat_reason); break;
            case MIDI2_CMD_INVITATION:      send_invitation(h, s);           break;
            case MIDI2_CMD_INVITATION_AUTH: send_invitation_auth(h, s);      break;
            default: break;
            }
            s->repeat_count++;
            s->repeat_due_ms = t + REPEAT_INTERVAL_MS;
        }

        if (s->state != SESS_ESTABLISHED) continue;

        /* Abandon a stuck RX gap (we asked for retransmit but it never filled). */
        if (s->gap_since_ms && (t - s->gap_since_ms) > GAP_RESYNC_MS) {
            ESP_LOGW(TAG, "%s: RX gap unrecovered, resyncing", s->addr_str);
            s->gap_since_ms = 0;
            s->rx_init = false; /* next packet re-seeds rx_seq */
        }

        /* Session timeout => terminate with Bye(timeout) (6.13/6.16). */
        if ((t - s->last_rx_ms) > (int64_t)h->session_timeout_ms) {
            ESP_LOGW(TAG, "%s: session timed out", s->addr_str);
            begin_bye(h, s, MIDI2_BYE_TIMEOUT, true);
            continue;
        }

        /* Keepalive ping when idle. */
        if (!s->ping_pending &&
            (t - s->last_rx_ms) > (int64_t)h->ping_interval_ms &&
            (t - s->last_ping_ms) > (int64_t)h->ping_interval_ms) {
            send_ping(h, s);
        }
    }
}

/* ---- TX queue drain ----------------------------------------------------- */
static void drain_tx(struct midi2_net_host_s *h)
{
    tx_msg_t msg;
    while (xQueueReceive(h->tx_queue, &msg, 0) == pdTRUE) {
        if (msg.op == TX_OP_CONNECT) {
            start_client_session(h, &msg.target, msg.targetlen);
            continue;
        }
        if (msg.op == TX_OP_DISCONNECT) {
            session_t *s = find_session(h, &msg.target, msg.targetlen);
            if (s && s->state != SESS_PENDING_BYE) {
                begin_bye(h, s, MIDI2_BYE_USER_TERMINATED, s->state == SESS_ESTABLISHED);
            }
            continue;
        }
        if (msg.broadcast) {
            for (uint8_t i = 0; i < h->max_clients; i++) {
                if (h->sessions[i].state == SESS_ESTABLISHED) {
                    tx_to_session(h, &h->sessions[i], msg.words, msg.num_words);
                }
            }
        } else {
            session_t *s = find_session(h, &msg.target, msg.targetlen);
            if (s) {
                tx_to_session(h, s, msg.words, msg.num_words);
            }
        }
    }
}

/* ---- Transport task ----------------------------------------------------- */
static void transport_task(void *arg)
{
    struct midi2_net_host_s *h = arg;
    uint8_t *rxbuf = malloc(RX_BUF_SIZE);
    if (!rxbuf) {
        ESP_LOGE(TAG, "no memory for rx buffer");
        xSemaphoreGive(h->done);
        vTaskDelete(NULL);
        return;
    }

    int maxfd = (h->sock > h->wake_sock ? h->sock : h->wake_sock) + 1;

    while (h->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(h->sock, &rfds);
        FD_SET(h->wake_sock, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = POLL_TIMEOUT_MS * 1000 };

        int r = select(maxfd, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select() failed: %d", errno);
            break;
        }

        if (r > 0 && FD_ISSET(h->wake_sock, &rfds)) {
            uint8_t drainb[16];
            struct sockaddr_storage from; socklen_t fl = sizeof(from);
            recvfrom(h->wake_sock, drainb, sizeof(drainb), 0,
                     (struct sockaddr *)&from, &fl);
        }

        if (r > 0 && FD_ISSET(h->sock, &rfds)) {
            for (;;) {
                struct sockaddr_storage src; socklen_t sl = sizeof(src);
                int n = recvfrom(h->sock, rxbuf, RX_BUF_SIZE, MSG_DONTWAIT,
                                 (struct sockaddr *)&src, &sl);
                if (n <= 0) break;
                process_datagram(h, rxbuf, (size_t)n, &src, sl);
            }
        }

        drain_tx(h);
        maintenance(h);
    }

    /* Shutdown: say goodbye to every active session (best effort). */
    for (uint8_t i = 0; i < h->max_clients; i++) {
        if (h->sessions[i].state == SESS_ESTABLISHED ||
            h->sessions[i].state == SESS_AUTH_REQUIRED ||
            h->sessions[i].state == SESS_PENDING_INVITATION ||
            h->sessions[i].state == SESS_PENDING_RESET) {
            send_bye(h, &h->sessions[i], MIDI2_BYE_POWER_DOWN);
        }
    }

    free(rxbuf);
    xSemaphoreGive(h->done);
    vTaskDelete(NULL);
}

/* ---- Wake helper -------------------------------------------------------- */
static void wake_task(struct midi2_net_host_s *h)
{
    /* lwIP sockets are safe for concurrent senders; no lock needed. */
    uint8_t b = 0;
    sendto(h->wake_sock, &b, 1, 0,
           (const struct sockaddr *)&h->wake_addr, sizeof(h->wake_addr));
}

/* ---- Public API --------------------------------------------------------- */
void midi2_net_host_config_default(midi2_net_host_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->udp_port           = MIDI2_NET_DEFAULT_PORT;
    cfg->enable_mdns        = true;
    cfg->max_clients        = 4;
    cfg->fec_repeats        = 2;
    cfg->request_retransmit = false;
    cfg->require_auth       = false;
    cfg->act_as_host        = true;
    cfg->respond_to_stream_discovery = true;
    cfg->function_block_first_group  = 0;
    cfg->function_block_num_groups   = 1;
    cfg->ping_interval_ms   = DEFAULT_PING_INTERVAL_MS;
    cfg->session_timeout_ms = DEFAULT_SESSION_TIMEOUT_MS;
    cfg->task_priority      = 5;
    cfg->task_stack_size    = 4096;
    cfg->task_core_id       = -1;
}

esp_err_t midi2_net_host_start(const midi2_net_host_config_t *cfg,
                               midi2_net_host_handle_t *out)
{
    if (!cfg || !out || !cfg->endpoint_name || !cfg->product_instance_id) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t name_len = strlen(cfg->endpoint_name);
    size_t pid_len  = strlen(cfg->product_instance_id);
    if (name_len == 0 || name_len > MIDI2_NET_MAX_ENDPOINT_NAME ||
        pid_len == 0  || pid_len  > MIDI2_NET_MAX_PRODUCT_ID ||
        cfg->fec_repeats > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    struct midi2_net_host_s *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;

    h->sock = -1;
    h->wake_sock = -1;
    h->max_clients = cfg->max_clients ? cfg->max_clients : 1;
    h->fec_repeats = cfg->fec_repeats;
    h->request_retransmit = cfg->request_retransmit;
    h->require_auth = cfg->require_auth;
    h->act_as_host = cfg->act_as_host;
    h->respond_stream = cfg->respond_to_stream_discovery;
    h->fb_first_group = cfg->function_block_first_group;
    h->fb_num_groups = cfg->function_block_num_groups ? cfg->function_block_num_groups : 1;
    copy_str(h->fb_name, sizeof(h->fb_name),
             cfg->function_block_name ? cfg->function_block_name : "MIDI Port");
    h->enable_mdns = cfg->enable_mdns && cfg->act_as_host; /* only Hosts advertise */
    h->ping_interval_ms = cfg->ping_interval_ms ? cfg->ping_interval_ms : DEFAULT_PING_INTERVAL_MS;
    h->session_timeout_ms = cfg->session_timeout_ms ? cfg->session_timeout_ms : DEFAULT_SESSION_TIMEOUT_MS;
    h->recv_cb = cfg->recv_cb;
    h->event_cb = cfg->event_cb;
    h->user_ctx = cfg->user_ctx;
    copy_str(h->endpoint_name, sizeof(h->endpoint_name), cfg->endpoint_name);
    copy_str(h->product_id, sizeof(h->product_id), cfg->product_instance_id);
    copy_str(h->auth_secret, sizeof(h->auth_secret), cfg->auth_secret);

    /* PSA is needed for auth on either side (Host verify, Client digest). */
    if ((h->require_auth || strlen(h->auth_secret) > 0) &&
        psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init failed; authentication will not work");
    }

    esp_err_t err = ESP_FAIL;

    h->sessions = calloc(h->max_clients, sizeof(session_t));
    h->tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_msg_t));
    h->done = xSemaphoreCreateBinary();
    if (!h->sessions || !h->tx_queue || !h->done) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* Main UDP socket. Prefer a dual-stack IPv6 socket so the Host is reachable
     * over both IPv4 and IPv6 -- macOS connects via the link-local IPv6 address
     * from the mDNS AAAA record. Fall back to IPv4 if lwIP IPv6 is disabled. */
    uint16_t want_port = cfg->udp_port ? cfg->udp_port : MIDI2_NET_DEFAULT_PORT;
    int yes = 1;
#if LWIP_IPV6
    h->sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (h->sock < 0) {
        /* DIAGNOSTIC (ours, not upstream's): without this, ESP_FAIL leaves
         * from five different points and you cannot tell which. ENFILE/EMFILE
         * = lwIP netconn pool exhausted; ENOBUFS/ENOMEM = memory. Different
         * causes with different remedies. */
        ESP_LOGE(TAG, "socket(AF_INET6) failed: errno=%d", errno);
        err = ESP_FAIL; goto fail;
    }
    setsockopt(h->sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int v6only = 0; /* also accept IPv4 (as IPv4-mapped addresses) */
    setsockopt(h->sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(want_port) };
    /* sa.sin6_addr left zero == in6addr_any */
#else
    h->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h->sock < 0) {
        ESP_LOGE(TAG, "socket(AF_INET) failed: errno=%d", errno);
        err = ESP_FAIL; goto fail;
    }
    setsockopt(h->sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(want_port),
    };
#endif
    if (bind(h->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "bind to port %u failed: %d", want_port, errno);
        err = ESP_FAIL;
        goto fail;
    }
    struct sockaddr_storage bound; socklen_t bl = sizeof(bound);
    getsockname(h->sock, (struct sockaddr *)&bound, &bl);
    h->port = (bound.ss_family == AF_INET6)
            ? ntohs(((struct sockaddr_in6 *)&bound)->sin6_port)
            : ntohs(((struct sockaddr_in *)&bound)->sin_port);

    /* Loopback wake socket (self-pipe to interrupt select on TX/stop). */
    h->wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h->wake_sock < 0) {
        ESP_LOGE(TAG, "wake socket() failed: errno=%d (service sock=%d)",
                 errno, h->sock);
        err = ESP_FAIL; goto fail;
    }
    struct sockaddr_in wa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    if (bind(h->wake_sock, (struct sockaddr *)&wa, sizeof(wa)) < 0) {
        ESP_LOGE(TAG, "wake bind failed: errno=%d", errno);
        err = ESP_FAIL; goto fail;
    }
    socklen_t wl = sizeof(h->wake_addr);
    getsockname(h->wake_sock, (struct sockaddr *)&h->wake_addr, &wl);

    /* Advertise via mDNS. */
    if (h->enable_mdns) {
        esp_err_t merr = midi2_net_mdns_start(cfg->mdns_hostname, h->port,
                                              h->endpoint_name, h->product_id);
        if (merr != ESP_OK) {
            ESP_LOGW(TAG, "mDNS advertise failed (%s); continuing without discovery",
                     esp_err_to_name(merr));
            h->enable_mdns = false;
        }
    }

    h->running = true;
    BaseType_t core = (cfg->task_core_id >= 0) ? cfg->task_core_id : tskNO_AFFINITY;
    BaseType_t ok = xTaskCreatePinnedToCore(
        transport_task, "midi2_net",
        cfg->task_stack_size ? cfg->task_stack_size : 4096,
        h, cfg->task_priority ? cfg->task_priority : 5, &h->task, core);
    if (ok != pdPASS) {
        h->running = false;
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "Network MIDI 2.0 Host '%s' on UDP port %u (mDNS %s)",
             h->endpoint_name, h->port, h->enable_mdns ? "on" : "off");
    ESP_LOGI(TAG, "aperti: servizio=%d wake=%d", h->sock, h->wake_sock);
    *out = h;
    return ESP_OK;

fail:
    if (h->enable_mdns) midi2_net_mdns_stop();
    if (h->sock >= 0) lwip_close(h->sock);
    if (h->wake_sock >= 0) lwip_close(h->wake_sock);
    if (h->tx_queue) vQueueDelete(h->tx_queue);
    if (h->done) vSemaphoreDelete(h->done);
    free(h->sessions);
    free(h);
    return err;
}

esp_err_t midi2_net_host_stop(midi2_net_host_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    h->running = false;
    wake_task(h);
    xSemaphoreTake(h->done, portMAX_DELAY);

    if (h->enable_mdns) midi2_net_mdns_stop();
    /* lwip_close() AND NOT close(): this is the fix for the fault that
     * exhausted the descriptor pool in three on/off cycles.
     *
     * close() is the VFS function. lwIP descriptors start at an offset
     * (LWIP_SOCKET_OFFSET) and are not in the VFS table, so the VFS does not
     * recognise them and returns -1 WITHOUT closing anything: the socket stays
     * dangling forever. It showed up in the logs as "rc=255" on every close,
     * with the descriptors growing 58,59 -> 60,61 -> 62,63 and never being
     * reused, up to errno=23 (ENFILE, pool exhausted).
     *
     * lwip_close() goes straight to the stack and does not pass through the VFS.
     *
     * NOTE: the close() calls in w5500_net.c must NOT be touched - those are
     * the ioLibrary's close(uint8_t sn), which closes a HARDWARE socket of the
     * W5500. Identical name, completely different function. */
    int rc1 = (h->sock >= 0) ? lwip_close(h->sock) : 0;
    int rc2 = (h->wake_sock >= 0) ? lwip_close(h->wake_sock) : 0;
    ESP_LOGI(TAG, "closed: service=%d (rc=%d) wake=%d (rc=%d)",
             h->sock, rc1, h->wake_sock, rc2);
    vQueueDelete(h->tx_queue);
    vSemaphoreDelete(h->done);
    free(h->sessions);
    free(h);
    return ESP_OK;
}

static esp_err_t enqueue_tx(struct midi2_net_host_s *h, const tx_msg_t *msg)
{
    if (xQueueSend(h->tx_queue, msg, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM; /* queue full: drop (UDP semantics) */
    }
    wake_task(h);
    return ESP_OK;
}

esp_err_t midi2_net_host_send_ump(midi2_net_host_handle_t h,
                                  const uint32_t *words, size_t num_words)
{
    if (!h || !words || num_words == 0 || num_words > MIDI2_NET_MAX_UMP_DATA_WORDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (midi2_net_host_session_count(h) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_msg_t msg = { .broadcast = true, .num_words = (uint8_t)num_words };
    memcpy(msg.words, words, num_words * 4);
    return enqueue_tx(h, &msg);
}

esp_err_t midi2_net_host_send_ump_to(midi2_net_host_handle_t h,
                                     const midi2_net_peer_t *peer,
                                     const uint32_t *words, size_t num_words)
{
    if (!h || !peer || !words || num_words == 0 || num_words > MIDI2_NET_MAX_UMP_DATA_WORDS) {
        return ESP_ERR_INVALID_ARG;
    }
    struct sockaddr_storage dst;
    socklen_t dstlen;
    if (!parse_addr(peer->addr, peer->port, &dst, &dstlen)) {
        return ESP_ERR_INVALID_ARG;
    }
    tx_msg_t msg = { .broadcast = false, .num_words = (uint8_t)num_words };
    memcpy(&msg.target, &dst, dstlen);
    msg.targetlen = dstlen;
    memcpy(msg.words, words, num_words * 4);
    return enqueue_tx(h, &msg);
}

esp_err_t midi2_net_host_connect(midi2_net_host_handle_t h, const char *host_ip, uint16_t port)
{
    if (!h || !host_ip || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    tx_msg_t msg = { .op = TX_OP_CONNECT };
    if (!parse_addr(host_ip, port, &msg.target, &msg.targetlen)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_tx(h, &msg);
}

esp_err_t midi2_net_host_disconnect(midi2_net_host_handle_t h, const midi2_net_peer_t *peer)
{
    if (!h || !peer) {
        return ESP_ERR_INVALID_ARG;
    }
    tx_msg_t msg = { .op = TX_OP_DISCONNECT };
    if (!parse_addr(peer->addr, peer->port, &msg.target, &msg.targetlen)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_tx(h, &msg);
}

size_t midi2_net_host_session_count(midi2_net_host_handle_t h)
{
    if (!h) return 0;
    size_t n = 0;
    for (uint8_t i = 0; i < h->max_clients; i++) {
        if (h->sessions[i].state == SESS_ESTABLISHED) n++;
    }
    return n;
}

uint16_t midi2_net_host_port(midi2_net_host_handle_t h)
{
    return h ? h->port : 0;
}
