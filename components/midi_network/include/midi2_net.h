/*
 * midi2_net.h - Network MIDI 2.0 (UDP) transport for ESP-IDF.
 *
 * Implements the MIDI Association "User Datagram Protocol for Universal MIDI
 * Packets" (M2-124-UM v1.0), Host (responder) role: the device advertises a
 * "_midi2._udp" service over mDNS, listens on a UDP port, accepts session
 * Invitations from Clients (e.g. macOS "MIDI UMP"), and exchanges UMP messages.
 *
 * The transport is target-agnostic: it uses only BSD sockets, esp_netif and
 * (optionally) the espressif/mdns component, so it runs on any ESP-IDF target
 * once the application has brought up a network interface (Wi-Fi or Ethernet).
 *
 * Threading model: a single internal task owns the UDP socket. All TX from the
 * application is marshalled to that task through a queue, so the socket is
 * never touched concurrently and no lock is ever held across socket I/O.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "midi2_ump.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default Host UDP port. 0 in the config means "let the OS choose". 5673 is
 * the example port used throughout the Network MIDI 2.0 specification. */
#define MIDI2_NET_DEFAULT_PORT  5673

/* Field length limits (UMP / Network MIDI 2.0 spec). */
#define MIDI2_NET_MAX_ENDPOINT_NAME   98  /* bytes, UTF-8 */
#define MIDI2_NET_MAX_PRODUCT_ID      42  /* bytes, ASCII 32-126 */

/* 7.1: a single UMP Data Command payload carries at most 64 words. */
#define MIDI2_NET_MAX_UMP_DATA_WORDS  64

typedef struct midi2_net_host_s *midi2_net_host_handle_t;

/* Opaque identity of a connected Client, stable for the life of its session. */
typedef struct {
    char     addr[48];  /* printable source IP (IPv4 or IPv6) */
    uint16_t port;      /* source UDP port */
    char     name[MIDI2_NET_MAX_ENDPOINT_NAME + 1];      /* Client UMP Endpoint Name */
    char     product_id[MIDI2_NET_MAX_PRODUCT_ID + 1];   /* Client Product Instance Id */
} midi2_net_peer_t;

/* Session lifecycle events reported to the application. */
typedef enum {
    MIDI2_NET_EVENT_SESSION_ESTABLISHED, /* Invitation accepted, UMP may flow */
    MIDI2_NET_EVENT_SESSION_CLOSED,      /* Bye exchanged or session timed out */
    MIDI2_NET_EVENT_INVITATION_DECLINED, /* an Invitation was rejected (e.g. auth) */
} midi2_net_event_t;

/*
 * Callback for received UMP. Delivered from the internal transport task, so do
 * not block here; copy what you need and return. `words` points to one or more
 * complete UMP messages (already de-duplicated and ordered); `num_words` is the
 * count of 32-bit words. Walk them with midi2_ump_message_num_words().
 */
typedef void (*midi2_net_recv_cb_t)(midi2_net_host_handle_t host,
                                    const midi2_net_peer_t *peer,
                                    const uint32_t *words, size_t num_words,
                                    void *user_ctx);

/* Callback for session lifecycle events. Also delivered from the transport task. */
typedef void (*midi2_net_event_cb_t)(midi2_net_host_handle_t host,
                                     const midi2_net_peer_t *peer,
                                     midi2_net_event_t event,
                                     void *user_ctx);

typedef struct {
    /* Identity advertised to Clients (both via mDNS TXT and Invitation Reply). */
    const char *endpoint_name;        /* UTF-8, <= 98 bytes. Required. */
    const char *product_instance_id;  /* ASCII 32-126, <= 42 bytes. Required. */

    /* Network. */
    uint16_t    udp_port;             /* 0 => MIDI2_NET_DEFAULT_PORT */
    bool        enable_mdns;          /* advertise _midi2._udp via mDNS */
    const char *mdns_hostname;        /* host part of "<name>.local"; NULL => derive
                                         from endpoint_name */

    /* Sessions. */
    uint8_t     max_clients;          /* concurrent sessions (>=1) */

    /* Data integrity. */
    uint8_t     fec_repeats;          /* 0..2 previous UMP Data commands per packet */
    bool        request_retransmit;   /* on a detected gap, send Retransmit Request */

    /* Optional shared-secret authentication (Invitation with Authentication).
     * When require_auth is true, an Invitation must be confirmed with a correct
     * SHA-256 digest of (CryptoNonce || auth_secret) before the session opens. */
    bool        require_auth;
    const char *auth_secret;  /* also used as the Client digest secret on connect */

    /* Role. Default true: act as a Host (advertise mDNS, accept inbound
     * Invitations). Set false for a Client-only endpoint: it won't advertise
     * and rejects inbound Invitations, but can still connect out via
     * midi2_net_host_connect(). The endpoint can be both at once. */
    bool        act_as_host;

    /* UMP Stream endpoint discovery. When true (the default), the Host answers
     * the in-band UMP Stream Endpoint/Function-Block Discovery that hosts such
     * as macOS "MIDI UMP" send after the session opens, declaring one
     * bidirectional Function Block. Without this the endpoint connects but
     * exposes no MIDI ports. */
    bool        respond_to_stream_discovery;
    const char *function_block_name;        /* NULL => "MIDI Port" (UTF-8, <= 63 B) */
    uint8_t     function_block_first_group; /* 0..15 (default 0) */
    uint8_t     function_block_num_groups;  /* 1..16 (default 1) */

    /* Keepalive / liveness. */
    uint32_t    ping_interval_ms;     /* 0 => default (2000). Pings idle peers. */
    uint32_t    session_timeout_ms;   /* 0 => default (10000). No RX => Bye(timeout). */

    /* Application callbacks. */
    midi2_net_recv_cb_t  recv_cb;
    midi2_net_event_cb_t event_cb;
    void                *user_ctx;

    /* Internal task. Zeros => sensible defaults (prio 5, 4096 stack, no affinity). */
    int         task_priority;
    int         task_stack_size;
    int         task_core_id;         /* -1 (or 0 default) => tskNO_AFFINITY */
} midi2_net_host_config_t;

/* Fill cfg with library defaults. Caller then sets at least endpoint_name and
 * product_instance_id. */
void midi2_net_host_config_default(midi2_net_host_config_t *cfg);

/*
 * Start a Network MIDI 2.0 Host. On success *out receives a handle and the
 * transport task is running. The network interface must already be up.
 */
esp_err_t midi2_net_host_start(const midi2_net_host_config_t *cfg,
                               midi2_net_host_handle_t *out);

/*
 * Stop the Host: sends Bye to every active session, removes the mDNS service,
 * stops the task and frees resources. Safe to call once per handle.
 */
esp_err_t midi2_net_host_stop(midi2_net_host_handle_t host);

/*
 * Send UMP to all established sessions. `words` is one or more complete UMP
 * messages (<= MIDI2_NET_MAX_UMP_DATA_WORDS). Non-blocking: the data is queued
 * for the transport task. Returns ESP_ERR_NO_MEM if the TX queue is full,
 * ESP_ERR_INVALID_STATE if there are no established sessions.
 */
esp_err_t midi2_net_host_send_ump(midi2_net_host_handle_t host,
                                  const uint32_t *words, size_t num_words);

/*
 * Convenience: send to a single peer (matched by addr+port). Same semantics as
 * midi2_net_host_send_ump otherwise.
 */
esp_err_t midi2_net_host_send_ump_to(midi2_net_host_handle_t host,
                                     const midi2_net_peer_t *peer,
                                     const uint32_t *words, size_t num_words);

/*
 * Client role: initiate an outbound session to a remote Host at host_ip:port
 * (IPv4 or IPv6 literal). Non-blocking; the Invitation handshake runs on the
 * transport task. Success/failure is reported via the event callback
 * (SESSION_ESTABLISHED or INVITATION_DECLINED). If auth_secret is set and the
 * remote Host requires authentication, the digest is computed automatically.
 *
 * The same UDP socket serves both roles, so (per the spec) a device cannot
 * connect out to a Host with which it already shares a session on this port.
 */
esp_err_t midi2_net_host_connect(midi2_net_host_handle_t host,
                                 const char *host_ip, uint16_t port);

/* Gracefully end the session with a peer (sends Bye). */
esp_err_t midi2_net_host_disconnect(midi2_net_host_handle_t host,
                                    const midi2_net_peer_t *peer);

/* Number of currently established sessions (both inbound and outbound). */
size_t midi2_net_host_session_count(midi2_net_host_handle_t host);

/* The UDP port the Host is actually listening on (useful when udp_port was 0). */
uint16_t midi2_net_host_port(midi2_net_host_handle_t host);

#ifdef __cplusplus
}
#endif
