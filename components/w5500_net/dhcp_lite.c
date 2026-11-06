/* dhcp_lite — see dhcp_lite.h for why this exists, why it is not the
 * ioLibrary's client, and why it asks once and then settles. */
#include "dhcp_lite.h"
#include "w5500_net.h"
#include "bsp_cores3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* --- WIZnet ioLibrary ---
 *
 * SAME NAME CLASH AS IN w5500_net.c: the ioLibrary defines MR as the W5500's
 * Mode register, while Xtensa's specreg.h — arriving here through
 * bsp_cores3.h and FreeRTOS — defines it as 32. Whichever comes last wins,
 * and the warning is the compiler saying so.
 *
 * Xtensa's MR is not used in this file, so it is undefined first. Doing it
 * here as well rather than once somewhere shared: the ioLibrary is included
 * by the file that needs it, and the clash belongs to that inclusion. */
#undef MR
#include "wizchip_conf.h"
#include "socket.h"

static const char *TAG = "dhcp";

/* The socket. Assigned PERMANENTLY, like the other seven, so the allocation
 * stays readable — see the table in main.c. It costs nothing at rest: an open
 * socket the loop does not drain is never touched over SPI, and a held lease
 * has nothing to receive until T1, hours away. */
#define DHCP_SN          7
#define DHCP_PORT_CLI    68
#define DHCP_PORT_SRV    67

/* How long to wait for a server before giving up and taking a link-local
 * address, and how many DISCOVERs to spread across that window.
 *
 * THREE SECONDS, NOT TWO, and the reason is a case the players never meet: a
 * switch powering up alongside us. [M] Observed in the field — the PHY raises
 * the link as soon as it sees a partner, but a switch that has just been
 * switched on does not forward yet, and our DISCOVERs went out over a cable
 * that was electrically up and carried nothing. A CDJ is almost always switched
 * on after the rig; the bridge shares a power strip with it.
 *
 * Three tries a second apart covers an unmanaged switch comfortably. [U] One
 * running spanning tree can take tens of seconds to start forwarding, and
 * nothing short of asking for a minute would cover that — at which point
 * unplugging and replugging the cable is the honest remedy, and it restarts the
 * whole sequence. */
#define DHCP_WAIT_MS     3000
#define TRIES            3       /* DISCOVERs spread across that window      */
#define ANSWER_MS        2000    /* wait for an ACK when renewing            */

/* The fixed part of a BOOTP message; the options that follow are what makes it
 * DHCP. The header is BOOTP's, which is why the field names look older than the
 * protocol. */
#define BOOTP_LEN        240
#define OPT_MAX          128
#define PKT_MAX          (BOOTP_LEN + OPT_MAX)   /* 368: room for the padding */
#define PKT_MIN          300     /* [E] RFC 951: a BOOTP message is this long */

#define DHCPDISCOVER     1
#define DHCPOFFER        2
#define DHCPREQUEST      3
#define DHCPACK          5
#define DHCPNAK          6

static dhcp_lite_state_t s_st;
static uint32_t s_xid;
static uint8_t  s_srv[4];              /* the server that answered           */
static uint8_t  s_offered[4];
static int64_t  s_t1_us, s_t2_us, s_expiry_us, s_wait_until_us;
static uint32_t s_lease_s;
static uint32_t s_sent, s_heard;       /* messages out, packets in on :68    */

dhcp_lite_state_t dhcp_lite_state(void) { return s_st; }

uint32_t dhcp_lite_lease_left_s(int64_t now_us)
{
    if (s_st == DHCP_ST_OFF || s_expiry_us <= now_us) return 0;
    return (uint32_t)((s_expiry_us - now_us) / 1000000);
}

/* --- building a message ---------------------------------------------------
 *
 * Only the fields a server needs. Everything else stays zero, which is what
 * BOOTP prescribes for a client that knows nothing yet. */
static int build_msg(uint8_t *p, uint8_t kind, const uint8_t *wanted_ip,
                     bool from_our_address)
{
    memset(p, 0, PKT_MAX);
    uint8_t mac[6];
    w5500_net_get_mac(mac);

    p[0] = 1;                      /* op: BOOTREQUEST                        */
    p[1] = 1;                      /* htype: Ethernet                        */
    p[2] = 6;                      /* hlen                                   */
    p[4] = (uint8_t)(s_xid >> 24); p[5] = (uint8_t)(s_xid >> 16);
    p[6] = (uint8_t)(s_xid >>  8); p[7] = (uint8_t)(s_xid);

    /* The broadcast flag, high bit of the flags field at offset 10. Set while
     * we have no address, so the server does not try to reply unicast to one we
     * do not hold. A renewal clears it: there we do have an address. */
    if (!from_our_address) p[10] = 0x80;

    /* ciaddr, at offset 12, is filled ONLY when we already hold the address we
     * are talking about — a renewal. A DISCOVER leaves it zero: the client is
     * declaring it has none. */
    if (from_our_address) w5500_net_get_ip(&p[12]);

    memcpy(&p[28], mac, 6);        /* chaddr                                 */

    /* The magic cookie that turns a BOOTP message into a DHCP one. */
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;

    int n = BOOTP_LEN;
    p[n++] = 53; p[n++] = 1; p[n++] = kind;             /* message type       */

    if (wanted_ip && !from_our_address) {
        p[n++] = 50; p[n++] = 4;                        /* requested address  */
        memcpy(&p[n], wanted_ip, 4); n += 4;
    }
    if (kind == DHCPREQUEST && s_srv[0]) {
        p[n++] = 54; p[n++] = 4;                        /* server identifier  */
        memcpy(&p[n], s_srv, 4); n += 4;
    }
    p[n++] = 55; p[n++] = 3; p[n++] = 1; p[n++] = 3; p[n++] = 6;  /* wanted:
                                                 netmask, router, DNS        */
    p[n++] = 255;                                       /* end                */

    /* PADDED TO 300 BYTES, the size [E] RFC 951 fixes for a BOOTP message.
     * Most servers accept a shorter one; some drop it without a word, and the
     * client then sees silence from a server that is plainly working. */
    while (n < PKT_MIN) p[n++] = 0;
    return n;
}

/* Reads one option. Returns its length, or 0 if absent. */
static int get_option(const uint8_t *p, int len, uint8_t code, uint8_t *out, int max)
{
    int i = BOOTP_LEN;
    while (i < len && p[i] != 255) {
        if (p[i] == 0) { i++; continue; }               /* padding            */
        if (i + 1 >= len) break;
        const int l = p[i + 1];
        if (i + 2 + l > len) break;
        if (p[i] == code) {
            const int n = (l < max) ? l : max;
            memcpy(out, &p[i + 2], n);
            return n;
        }
        i += 2 + l;
    }
    return 0;
}

/* Empties whatever the chip has buffered on our port: DHCP traffic meant for
 * OTHER clients is broadcast to port 68 as well, and without this the first
 * read would return somebody else's packet instead of our answer. */
static void drain_stale(void)
{
    uint8_t junk[PKT_MAX], ip[4]; uint16_t port;
    while (w5500_net_recv(DHCP_SN, junk, sizeof(junk), ip, &port) > 0) { }
}

/* Sends one message. Returns false if it did not go out — either the beat is
 * about to, or the chip refused.
 *
 * THE QUIET WINDOW IS RESPECTED BY EACH SENDER, not by the bus layer: this
 * packet is 300 bytes on the ordinary path, which [M] can hold the SPI for up
 * to 2 ms, against a beat jitter budget under 1.3. */
static bool send_msg(uint8_t kind, const uint8_t *wanted_ip, bool from_our_address)
{
    if (bsp_spi_in_quiet_window()) return false;

    uint8_t pkt[PKT_MAX];
    const int n = build_msg(pkt, kind, wanted_ip, from_our_address);
    static const uint8_t bcast[4] = { 255, 255, 255, 255 };
    drain_stale();

    const int32_t sent = w5500_net_sendto(DHCP_SN, pkt, (size_t)n, bcast, DHCP_PORT_SRV);
    if (sent != n) {
        ESP_LOGW(TAG, "send failed: %d of %d bytes went out", (int)sent, n);
        return false;
    }
    s_sent++;
    return true;
}

static void new_transaction(void)
{
    /* The id must differ from the last, or a late answer to a previous exchange
     * would be taken for this one's. Time is enough: nothing here needs it to
     * be unguessable. */
    s_xid = (uint32_t)esp_timer_get_time() ^ 0x5A17C0DEu;
    s_srv[0] = 0;
}

/* Waits for one message of the given kind, up to ms. Returns its length, or 0,
 * leaving the reply in pkt. */
static int wait_for(uint8_t kind, uint8_t *pkt, int ms)
{
    const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        uint8_t from_ip[4]; uint16_t port;
        const int32_t n = w5500_net_recv(DHCP_SN, pkt, PKT_MAX, from_ip, &port);
        if (n > 0) s_heard++;
        if (n >= BOOTP_LEN) {
            const uint32_t xid = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16)
                               | ((uint32_t)pkt[6] <<  8) |  (uint32_t)pkt[7];
            uint8_t got = 0;
            if (xid == s_xid && get_option(pkt, n, 53, &got, 1) == 1 && got == kind) {
                if (!s_srv[0]) memcpy(s_srv, from_ip, 4);
                return (int)n;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return 0;
}

/* Takes the lease apart and adopts the address it carries. */
static void take_lease(const uint8_t *pkt, int n, int64_t now)
{
    uint8_t mask[4] = {0}, gw[4] = {0}, lease[4] = {0};
    const bool has_mask = get_option(pkt, n, 1, mask, 4) == 4;
    const bool has_gw   = get_option(pkt, n, 3, gw,   4) == 4;
    s_lease_s = (get_option(pkt, n, 51, lease, 4) == 4)
              ? (((uint32_t)lease[0] << 24) | ((uint32_t)lease[1] << 16)
              |  ((uint32_t)lease[2] <<  8) |  (uint32_t)lease[3])
              : 3600;                                   /* no option: an hour */

    /* T1 AND T2 ARE HALF AND SEVEN EIGHTHS of the lease, as the standard says.
     * They are what makes a short lease survivable: an hour's lease is renewed
     * after thirty minutes, so a device left running all night keeps its
     * address instead of having it handed to somebody else halfway through. */
    s_t1_us     = now + (int64_t)s_lease_s * 500000;
    s_t2_us     = now + (int64_t)s_lease_s * 875000;
    s_expiry_us = now + (int64_t)s_lease_s * 1000000;

    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    memcpy(ni.ip, &pkt[16], 4);                         /* yiaddr             */
    if (has_mask) memcpy(ni.sn, mask, 4);
    if (has_gw)   memcpy(ni.gw, gw, 4);
    wizchip_setnetinfo(&ni);
    s_st = DHCP_ST_BOUND;
}

bool dhcp_lite_acquire(void)
{
    if (w5500_net_open_udp(DHCP_SN, DHCP_PORT_CLI) != DHCP_SN) {
        ESP_LOGE(TAG, "socket %d not opened: going straight to auto-ip", DHCP_SN);
        s_st = DHCP_ST_OFF;
        return false;
    }

    /* NO ADDRESS AT ALL for the length of this, and that is the point: a
     * DISCOVER must come from 0.0.0.0, and on this chip that means SIPR.
     * Nobody is waiting for us here — the exchange runs before auto-ip at
     * start-up, and after a link drop when we were off the network anyway. */
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    memset(ni.ip, 0, 4);
    wizchip_setnetinfo(&ni);
    s_st = DHCP_ST_OFF;

    uint8_t pkt[PKT_MAX];
    bool got = false;

    for (int t = 0; t < TRIES && !got; t++) {
        new_transaction();
        if (!send_msg(DHCPDISCOVER, NULL, false)) {
            vTaskDelay(pdMS_TO_TICKS(50));              /* quiet window: retry */
            continue;
        }
        const int n = wait_for(DHCPOFFER, pkt, DHCP_WAIT_MS / TRIES);
        if (!n) continue;

        memcpy(s_offered, &pkt[16], 4);
        ESP_LOGI(TAG, "OFFER %u.%u.%u.%u from %u.%u.%u.%u, requesting it",
                 s_offered[0], s_offered[1], s_offered[2], s_offered[3],
                 s_srv[0], s_srv[1], s_srv[2], s_srv[3]);

        if (!send_msg(DHCPREQUEST, s_offered, false)) continue;
        const int a = wait_for(DHCPACK, pkt, ANSWER_MS);
        if (!a) {
            ESP_LOGW(TAG, "no ACK after the OFFER: trying again");
            continue;
        }
        take_lease(pkt, a, esp_timer_get_time());
        got = true;
    }

    if (got) {
        uint8_t ip[4]; w5500_net_get_ip(ip);
        ESP_LOGW(TAG, "address %u.%u.%u.%u from DHCP, lease %lus",
                 ip[0], ip[1], ip[2], ip[3], (unsigned long)s_lease_s);
        return true;
    }

    /* NO SERVER, AND WE WILL NOT ASK AGAIN until the link drops — which is what
     * the players do, and what keeps us on the same subnet as the gear started
     * alongside us. The caller now runs auto-ip. */
    ESP_LOGI(TAG, "no DHCP server answered in %d ms (sent %lu, heard %lu): "
                  "auto-ip from here, and no further asking",
             DHCP_WAIT_MS, (unsigned long)s_sent, (unsigned long)s_heard);
    return false;
}

bool dhcp_lite_tick(int64_t now)
{
    if (s_st == DHCP_ST_OFF) return false;              /* link-local: nothing */

    if (s_st == DHCP_ST_BOUND) {
        if (now < s_t1_us) return false;                /* one comparison      */

        /* THE RENEWAL DOES NOT GO DARK. [E] RFC 2131 has a client renewing a
         * lease speak from its own address with ciaddr set, so nothing here
         * takes us off the network mid-evening. */
        new_transaction();
        uint8_t ip[4]; w5500_net_get_ip(ip);
        if (!send_msg(DHCPREQUEST, ip, true)) return false;  /* deferred: time to spare */
        s_st = DHCP_ST_RENEWING;
        s_wait_until_us = now + (int64_t)ANSWER_MS * 1000;
        ESP_LOGI(TAG, "renewing the lease (%lus left)",
                 (unsigned long)dhcp_lite_lease_left_s(now));
        return false;
    }

    /* RENEWING: watching for the ACK. */
    uint8_t pkt[PKT_MAX], from_ip[4]; uint16_t port;
    const int32_t n = w5500_net_recv(DHCP_SN, pkt, sizeof(pkt), from_ip, &port);
    if (n > 0) s_heard++;

    if (n >= BOOTP_LEN) {
        const uint32_t xid = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16)
                           | ((uint32_t)pkt[6] <<  8) |  (uint32_t)pkt[7];
        uint8_t kind = 0;
        if (xid == s_xid && get_option(pkt, n, 53, &kind, 1) == 1) {
            if (kind == DHCPACK) {
                uint8_t before[4]; w5500_net_get_ip(before);
                take_lease(pkt, (int)n, now);
                uint8_t after[4];  w5500_net_get_ip(after);
                const bool moved = memcmp(before, after, 4) != 0;
                if (moved)
                    ESP_LOGW(TAG, "renewal moved us from %u.%u.%u.%u to %u.%u.%u.%u",
                             before[0], before[1], before[2], before[3],
                             after[0], after[1], after[2], after[3]);
                else
                    ESP_LOGI(TAG, "lease renewed, %lus", (unsigned long)s_lease_s);
                return moved;
            }
            if (kind == DHCPNAK) {
                /* The server refuses: the lease is gone. We do NOT start a
                 * fresh acquisition here — that would mean going dark
                 * mid-evening. A link event is what triggers a new attempt,
                 * exactly as at power-on. */
                ESP_LOGW(TAG, "renewal refused (NAK): the address is no longer ours, "
                              "and stays that way until the link drops");
                s_st = DHCP_ST_OFF;
                return false;
            }
        }
    }

    if (now < s_wait_until_us) return false;

    /* No answer. Before T2 there is still time, so we go back to BOUND and try
     * again shortly; past it the lease is as good as gone and only a link event
     * will get us another. */
    if (now < s_t2_us) {
        s_st = DHCP_ST_BOUND;
        s_t1_us = now + (int64_t)ANSWER_MS * 1000;
        return false;
    }
    ESP_LOGW(TAG, "no answer to the renewal past T2: keeping the address, but the "
                  "lease may already be somebody else's");
    s_st = DHCP_ST_BOUND;
    s_t1_us = s_expiry_us;                              /* one last try at expiry */
    return false;
}
