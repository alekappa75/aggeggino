/* dhcp_lite — a small DHCP client for the W5500, ahead of auto-ip.
 *
 * WHY NOT THE ONE IN THE ioLibrary. That code exists and works, but it speaks
 * to the chip through the plain socket API, knowing nothing of the quiet window
 * around the beat, of the priority ceiling, or of the seven-transaction send
 * path. Its socket would also sit outside our accounting, so a failure would
 * show up nowhere in the heartbeat. What DHCP we need is small — four messages
 * and a timer — while what it could disturb is not.
 *
 * WHY NOT lwIP EITHER. The WiFi side gets DHCP for free because it goes through
 * lwIP, and ESP-IDF has an Ethernet driver that would attach the W5500 to the
 * same stack. It would cost every millisecond of latency work done on this
 * path. A DHCP client is not worth rewriting the bridge for.
 *
 * ============================================================================
 * WE DO WHAT THE PLAYERS DO: ASK ONCE, THEN SETTLE
 *
 * [M] Observed on two XDJ-700s: a player tries DHCP AT POWER-ON ONLY. Failing
 * that it takes a link-local address and STAYS THERE — plugging the switch into
 * a router afterwards changes nothing, and it keeps its 169.254 address
 * indefinitely. Only a power cycle makes it ask again.
 *
 * The consequence is worth stating, because it is the network's behaviour and
 * not ours to fix: two players started at different times can end up on
 * different subnets, and then they cannot see each other at all. [M] Seen in
 * the field — one player restarted after the router appeared took a DHCP
 * address and vanished from the other's view.
 *
 * So we copy them. Asking once and settling puts us on the same subnet as the
 * gear started alongside us, which is what matters; a client that kept probing
 * would drift onto the OTHER subnet from the players and be the odd one out.
 *
 * ============================================================================
 * THE ORDER, AND THE TWO SECONDS THAT COST
 *
 * A DISCOVER must go out from 0.0.0.0 — [E] RFC 2131 makes it a MUST for a
 * client with no address yet, and a real server silently dropped ours when it
 * came from a link-local address: the packet left, nothing came back.
 *
 * On this chip the source of a UDP datagram is whatever SIPR holds, with no
 * per-packet override short of MACRAW — which would take socket 0, the beat's.
 * So the exchange happens BEFORE auto-ip, while we have no address to lose:
 *
 *   1. SIPR at zero, DISCOVER, listen for up to DHCP_WAIT_MS
 *   2. an OFFER -> REQUEST -> ACK, and we keep that address for the session
 *   3. silence -> auto-ip, and we ask no more until the link drops
 *
 * The cost is that on a network with no DHCP server the bridge appears about
 * two seconds later than it would otherwise. Nobody is waiting for us at that
 * point, and it buys behaving like everything else on the wire.
 *
 * THE LEASE IS RENEWED, at T1 and T2 as the standard says: those renewals go
 * out in unicast FROM our own address, so nothing goes dark mid-evening. A
 * short lease on a guest network would otherwise be handed to somebody else
 * halfway through an evening. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Runs the whole acquisition, and RETURNS ONLY WHEN IT IS DECIDED — either with
 * a lease or with none. Called before auto-ip, from w5500_net_init and again
 * when the link comes back.
 *
 * Returns true if an address was obtained; false means the caller should fall
 * back on auto-ip. Blocks for at most DHCP_WAIT_MS. */
bool dhcp_lite_acquire(void);

/* Called from the slow loop. Does nothing at all unless a lease is held and T1
 * has passed. Returns true when the address has CHANGED — a renewal that moved
 * us — and the caller must then reopen its sockets. */
bool dhcp_lite_tick(int64_t now_us);

/* For the heartbeat. */
typedef enum {
    DHCP_ST_OFF = 0,     /* no lease: we are on a link-local address        */
    DHCP_ST_BOUND,       /* lease held                                     */
    DHCP_ST_RENEWING,    /* T1 passed, REQUEST out, waiting for the ACK    */
} dhcp_lite_state_t;

dhcp_lite_state_t dhcp_lite_state(void);
uint32_t          dhcp_lite_lease_left_s(int64_t now_us);
