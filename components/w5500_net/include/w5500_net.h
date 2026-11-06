/*
 * w5500_net.h — W5500 (LAN Module 13.2) through the WIZnet ioLibrary, using
 * the chip's HARDWARE TCP/IP stack rather than lwIP.
 *
 * That is an architectural choice, not an accident. Keeping the W5500 out of
 * lwIP removes the esp_eth + esp_lcd race entirely, and leaves lwIP with the
 * WiFi interface alone — which matters because Ableton Link needs an
 * unambiguous multicast binding, and it gets one when only one interface
 * exists.
 *
 * The W5500 is a device on the SAME spi_master bus as the display: every
 * access takes the bus lock and switches GPIO35 back to MISO (bsp_dc_as_miso).
 *
 * NOTE: the WIZnet ioLibrary sources are NOT included in this repository. Run
 *       components/w5500_net/fetch_iolibrary.sh
 * to fetch them into components/w5500_net/ioLibrary/ — see CMakeLists.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- network configuration -------------------------------------------------
 *
 * Pioneer gear commonly sits on link-local 169.254.x, which is why the defaults
 * below do too.
 *
 * THE MAC IS NOT HERE, and deliberately: it is derived at start-up from the
 * ESP32's eFuse, so that two bridges on one network cannot share an Ethernet
 * address. See mac_from_efuse() in w5500_net.c. */
#define W5500_IP   {169, 254, 0, 77}
#define W5500_SN   {255, 255, 0, 0}
#define W5500_GW   {169, 254, 0, 1}

/* Add the W5500 as an SPI device on the shared bus, register the ioLibrary
 * callbacks, then reset and configure the chip. */
esp_err_t w5500_net_init(void);

/* Called when the acquisition has finished, from the acquisition task. Whoever
 * receives it should reopen its sockets: they were bound while the chip held a
 * different address. */
typedef void (*w5500_net_done_fn)(void);

/* Starts getting an address: waits for the link, tries DHCP, falls back on
 * link-local. RETURNS AT ONCE — the work happens in a task that destroys itself
 * when done, and calls on_done from there.
 *
 * It is slow, up to five seconds, and none of it is work anybody is waiting on:
 * doing it inline held the dashboard back by exactly that much. WHILE IT RUNS
 * NOTHING ELSE MAY TRANSMIT — the DHCP exchange needs SIPR at zero — and that
 * is enforced inside the send path, not by the callers.
 *
 * ESP_ERR_INVALID_STATE if one is already running. */
esp_err_t w5500_net_acquire_address(w5500_net_done_fn on_done);

/* True while an address is being acquired: sends are suspended. */
bool w5500_net_acquiring(void);

/* --- a link that flapped ---------------------------------------------------
 *
 * Tell us when the link goes DOWN. Flapping is not bounce to be filtered out —
 * it means a faulty switch or cable, and no settling time makes it go away —
 * but it must not leave a half-finished acquisition standing.
 *
 * So the drop is remembered: a run in progress gives up early, and a run that
 * had already finished is marked out of date. w5500_net_address_stale() then
 * says an acquisition is worth starting again, and is only ever true when no
 * task is running — so acting on it can never leave two of them racing. */
void w5500_net_note_link_down(void);
bool w5500_net_address_stale(void);

/* Automatic recovery: reconfigure the chip after it has "come back", for
 * instance when the DC jack was pulled and reinserted. The socket must then be
 * reopened with w5500_net_open_udp(). */
esp_err_t w5500_net_reinit(void);

/* MAC and IP actually in use, needed for the Virtual CDJ keep-alive. */
void w5500_net_get_mac(uint8_t out[6]);

/* The address the chip currently answers to. It changes: auto-ip picks it,
 * the link-up recheck may move it, and DHCP may replace it altogether — so
 * anything that displays or logs it should ask rather than remember. */
void w5500_net_get_ip(uint8_t out[4]);

/* --- UDP -------------------------------------------------------------------- */

/* Open a UDP socket (hardware socket sn) listening on 'port'. */
int w5500_net_open_udp(uint8_t sn, uint16_t port);

/* Send a UDP datagram, broadcast included, from socket sn to ip:port.
 * Returns bytes sent, or negative on error. */
int32_t w5500_net_sendto(uint8_t sn, const uint8_t *buf, size_t len,
                         const uint8_t ip[4], uint16_t port);

/* SPECIALISED UDP send: same signature as w5500_net_sendto, but talks to the
 * W5500 registers directly instead of going through the ioLibrary. Seven SPI
 * transactions instead of twenty-three.
 *
 * Two behavioural differences worth knowing:
 *   - it does NOT wait for the send confirmation (SEND_OK). It returns as soon
 *     as the chip has accepted the SEND command. For a periodic packet that is
 *     exactly what one wants; for a send that must be retried on failure, it
 *     is not.
 *   - it does not handle ARP: it is meant for BROADCAST. Unicast works, but an
 *     ARP timeout goes undetected and shows up only as a packet that never
 *     arrived.
 *
 * It coexists with the library's sendto on the same socket: it rewrites the
 * destination on every send and clears leftover flags.
 *
 * See the extended note in w5500_net.c for the reasoning and the numbers. */
int32_t w5500_net_sendto_fast(uint8_t sn, const uint8_t *buf, size_t len,
                              const uint8_t ip[4], uint16_t port);

/* Receive a datagram, non-blocking. Returns bytes received, 0 for none,
 * negative on error. */
int32_t w5500_net_recv(uint8_t sn, uint8_t *buf, size_t maxlen,
                       uint8_t from_ip[4], uint16_t *from_port);

/* Outcomes of the fast send path, reset on every read.
 *
 * These are the two ways that path can fail without being able to tell the
 * caller, because it does not wait for confirmation: they are collected on the
 * next pass by reading Sn_IR.
 *   timeouts  ARP failed, unicast only: the packet did NOT go out
 *   nospace   transmit ring full: should never happen */
void w5500_net_fast_stats(uint32_t *sends, uint32_t *timeouts, uint32_t *nospace);

/* Outcomes of the specialised RECEIVE path, reset on every read.
 *   n      packets delivered
 *   bad    headers with an absurd length (0, or above 1472). These must stay
 *          at ZERO: if they move, the path is reading the ring at the wrong
 *          offset
 *   trunc  payloads larger than the caller's buffer. With our 512 bytes
 *          against a 284-byte status packet this should never occur */
void w5500_net_fast_recv_stats(uint32_t *n, uint32_t *bad, uint32_t *trunc);

/* Socket state AS THE CHIP SEES IT. This exists to diagnose receive blackouts:
 * from the application, a socket that delivers nothing and a socket that is
 * closed look identical, whereas here they are distinguishable.
 *   sr          Sn_SR: 0x22 = SOCK_UDP (healthy), 0x00 = SOCK_CLOSED
 *   rx_pending  Sn_RX_RSR: bytes waiting to be read
 *   tx_free     Sn_TX_FSR: free space for transmission
 * Any pointer may be NULL. */
void w5500_net_sock_state(uint8_t sn, uint8_t *sr, uint16_t *rx_pending, uint16_t *tx_free);

/* --- TCP server (the dbserver: a player contacts us on port 12523) ---------
 *
 * Non-blocking model, to be driven periodically from the loop.
 *   tcp_listen  open or reopen the listening socket on 'port'
 *   tcp_poll    if a connection is established and has data, copy it into buf.
 *               Returns bytes read (>0), 0 for nothing, negative on error.
 *               from_ip receives the connected peer's address.
 *   tcp_send    send on the established connection
 *   tcp_close   close and go back to listening */
int     w5500_net_tcp_listen(uint8_t sn, uint16_t port);
int32_t w5500_net_tcp_poll(uint8_t sn, uint8_t *buf, size_t maxlen, uint8_t from_ip[4]);
int32_t w5500_net_tcp_send(uint8_t sn, const uint8_t *buf, size_t len);
void    w5500_net_tcp_close(uint8_t sn, uint16_t port);

/* --- TCP client (querying a real player's dbserver) ------------------------ */

/* Blocking connect: returns 0 once established. */
int     w5500_net_tcp_connect(uint8_t sn, const uint8_t ip[4], uint16_t port);

/* NON-BLOCKING connect, for state machines.
 *   start  open the socket and begin connecting; 0 means started
 *   poll   1 = established, 0 = still in progress, negative = failed or closed */
int     w5500_net_tcp_connect_start(uint8_t sn, const uint8_t ip[4], uint16_t port);
int     w5500_net_tcp_connect_poll(uint8_t sn);

int32_t w5500_net_tcp_recv(uint8_t sn, uint8_t *buf, size_t maxlen);
void    w5500_net_tcp_disconnect(uint8_t sn);

/* Diagnostics: socket state and room in the transmit buffer. Answers the
 * question of why a send() is not sending anything. */
void    w5500_net_tcp_diag(uint8_t sn, uint8_t *sr, uint8_t *ir,
                           uint16_t *tx_free, uint16_t *tx_max,
                           uint16_t *rx_avail, uint16_t *rx_max);

/* --- diagnostics ------------------------------------------------------------ */

/* Set the GPIO used as the W5500 chip select at runtime. Used by the scanner
 * that hunts for the right pin on the JC jumper. */
void w5500_net_set_cs_gpio(int gpio);

/* Read the VERSIONR register (0x0039). [M] It must read 0x04 when the W5500 is
 * alive and SPI and CS are correct — the simplest possible proof of life. */
uint8_t w5500_net_version(void);

/* PHY link up or down. It is the RISING EDGE of this that starts an address
 * acquisition — see w5500_net_acquire_address — and the falling edge that
 * invalidates one in progress. */
bool w5500_net_link_up(void);

/* --- interrupts -------------------------------------------------------------
 *
 * INTn is ACTIVE LOW and stays asserted until Sn_IR is cleared: after every
 * trigger the handler MUST call w5500_net_int_ack(), or no further edges
 * arrive. The acknowledgement goes over SPI, so it must happen in a task and
 * never in the ISR. */
void    w5500_net_int_enable_recv(uint8_t sn);  /* INTn for socket sn only, RECV event */
void    w5500_net_int_disable(void);            /* no socket raises INTn               */
uint8_t w5500_net_int_ack(uint8_t sn);          /* clear RECV, return the Sn_IR read   */
uint8_t w5500_net_int_pending(void);            /* SIR: bitmap of sockets with an interrupt */

#ifdef __cplusplus
}
#endif
