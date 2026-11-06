/*
 * w5500_net.c — see w5500_net.h.
 *
 * The glue between the WIZnet ioLibrary and spi_master. The two key points:
 *  - cs_select():   take the bus (spi_device_acquire_bus), switch GPIO35 to
 *                   MISO (bsp_dc_as_miso), then drive CS low
 *  - cs_deselect(): CS high, release the bus
 * So every W5500 transaction is serialised against the display through the bus
 * lock, and the shared MISO/DC pin is always in the right direction.
 *
 * Requires the WIZnet ioLibrary sources — see fetch_iolibrary.sh.
 */
#include "w5500_net.h"
#include "bsp_cores3.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "dhcp_lite.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* --- WIZnet ioLibrary --- */
/* Name clash: the ioLibrary defines MR (the W5500 Mode register), while the
 * Xtensa header specreg.h, pulled in via spi_master, defines MR as 32. We do
 * not use Xtensa's MR, so it is undefined before including the ioLibrary. */
#undef MR
#include "wizchip_conf.h"
#include "socket.h"

static const char *TAG = "w5500";
static spi_device_handle_t s_dev;

/* Chip select at runtime, defaulting to the BSP value. The scanner changes it
 * to find which pin the module's JC jumper is on. */
static int s_cs_gpio = BSP_W5500_CS_GPIO;

/* True only once open_udp has successfully opened the socket, meaning the chip
 * is alive. It stops recv from touching a W5500 that is not answering, which
 * would otherwise hang. */
static bool s_socket_open = false;

void w5500_net_set_cs_gpio(int gpio)
{
    s_cs_gpio = gpio;
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cs);
    gpio_set_level(gpio, 1);   /* idle high, deselected */
}

/* Configure the chip: soft reset, buffers, IP and MAC. Used by both init and
 * reinit, the latter recovering after the W5500 has been re-powered. */
/* ============================================================================
 * AUTO-IP — choosing a link-local address that nobody else is using
 *
 * WHAT THE PLAYERS DO, and what we now do too: with no DHCP server answering,
 * a device picks an address in the link-local block, checks nobody has it, and
 * keeps it.
 *
 * THE RANGE IS 169.254.1.0 to 169.254.254.255. [E] RFC 3927 reserves the first
 * and last /24 of the block, so a conforming device will never pick them. The
 * old fixed 169.254.0.77 sat in the reserved range: outside the standard, and
 * for that reason safe from collisions — which is a poor way to be safe.
 *
 * THE ADDRESS IS DERIVED FROM THE MAC, not drawn afresh each boot. The same
 * device then asks for the same address every time, which keeps a network
 * stable across restarts and makes a capture readable a second time. A device
 * that has to move gets its number from the retry counter, so the sequence is
 * still deterministic.
 *
 * ============================================================================
 * HOW WE PROBE, AND WHY NOT THE WAY THE RFC SAYS
 *
 * RFC 3927 wants an ARP probe with the sender address set to zero, so as not
 * to pollute anybody's cache. Building one by hand means MACRAW mode, which on
 * the W5500 TAKES OVER SOCKET 0 EXCLUSIVELY — the beat socket. Not something to
 * switch on at start-up and hope to switch off cleanly.
 *
 * [M] BUT THE CHIP DOES IT ANYWAY: setting SIPR to 0.0.0.0 and sending a
 * unicast makes it emit exactly that probe, and sendto returns SOCKERR_TIMEOUT
 * when nobody answers. Measured at 406 ms on a live network. So the conforming
 * probe is what runs, and the temporary sender below is only a fallback that
 * has never been needed.
 *
 * [U] WHAT THIS DOES NOT DO: defend the address afterwards. RFC 3927 has a
 * device watch for ARP claiming its own address and step aside; hearing those
 * needs MACRAW again. In practice a late collision would show as traffic going
 * astray, and the remedy is a restart. Worth knowing before trusting it in a
 * crowded network.
 * ==========================================================================*/
/* THE PROBE SHARES THE DHCP SOCKET, and that is not a coincidence: both are
 * occasional users of the one socket not held by a permanent service.
 *
 * It opens and closes it around each probe. At start-up that is free — the DHCP
 * client has not started yet — but on a LINK-UP RECHECK the client is holding
 * it open, so the caller must let dhcp_lite know afterwards: dhcp_lite_on_link_up()
 * reopens it. The order in main is therefore FIXED — recheck first, dhcp
 * second — and swapping them would leave the DHCP client with a closed socket
 * and no sign of why. */
#define AUTOIP_PROBE_SN  7

/* --- SENDING IS SUSPENDED WHILE AN ADDRESS IS BEING ACQUIRED ---------------
 *
 * During a DHCP exchange SIPR is zero: anything sent then leaves with 0.0.0.0
 * as its source, and a player receiving our keep-alive from a nonexistent
 * address has every reason to discard it — or worse, to remember it.
 *
 * THE GUARD LIVES HERE, IN THE ONE PLACE EVERY SEND PASSES THROUGH, and not in
 * the fourteen callers: one of them would be forgotten sooner or later. The
 * quiet window is enforced caller by caller, and it shows.
 *
 * The DHCP socket is exempt for the obvious reason: it is the exchange. */
static TaskHandle_t      s_acq_task;   /* the acquisition task, NULL when none */
static w5500_net_done_fn s_done_cb;
static volatile bool s_acquiring;

/* --- A LINK THAT DROPPED WHILE WE WERE WORKING -----------------------------
 *
 * Flapping is not switch bounce to be filtered out — it means a faulty switch
 * or a bad cable, and there is no settling time that makes it go away. What
 * matters is that it must not leave us with a half-finished acquisition, nor
 * with several tasks racing.
 *
 * So the drop is REMEMBERED rather than waited out. The task reads the flag at
 * its slow points and gives up early; when it ends, a flag still set means the
 * run it just completed was made on a link that has since changed, and the
 * whole thing is worth doing again. The caller sees that through
 * w5500_net_address_stale() and starts one more task — never two at once,
 * because the previous one has already destroyed itself by then.
 *
 * [U] A FLAP SHORTER THAN THE CALLER'S POLL can pass unseen: main looks at the
 * link every CHECK_MS. Down-and-up inside that window leaves no trace, and the
 * acquisition would stand on a link that did wobble. Catching it would mean
 * polling the PHY far more often, for a fault that is already pathological. */
static volatile bool s_link_dirty;

void w5500_net_note_link_down(void) { s_link_dirty = true; }
/* Worth starting another run? Three things must hold, and the LINK BEING UP is
 * the one that was missing: without it a task that gave up because the cable
 * was gone set the flag on its way out, which made this true again, which
 * started another task that gave up for the same reason. [M] Seen in the
 * field as a run every 600 ms for as long as the cable stayed out, each one
 * reopening the sockets on its way past. */
bool w5500_net_address_stale(void)
{
    return s_link_dirty && !s_acq_task && w5500_net_link_up();
}

bool w5500_net_acquiring(void) { return s_acquiring; }

static inline bool send_blocked(uint8_t sn)
{
    return s_acquiring && sn != AUTOIP_PROBE_SN;
}

/* A pause between the link appearing and the first question asked over it.
 * Half a second, spent before sending is suspended — see the note in the
 * task. */
#define LINK_SETTLE_MS    500

#define AUTOIP_TRIES     8       /* candidates to try before giving up        */
#define AUTOIP_RCR       2       /* ARP retries while probing: 3 x 200 ms     */

/* The candidate for attempt n. The low two bytes come from the MAC, folded into
 * the usable range, and the attempt number moves us along. */
static void autoip_candidate(const uint8_t mac[6], int n, uint8_t ip[4])
{
    const uint16_t seme = (uint16_t)((mac[3] << 8) ^ (mac[4] << 4) ^ mac[5]);
    const uint16_t off  = (uint16_t)((seme + (uint16_t)n * 7919u) % 65024u);
    ip[0] = 169; ip[1] = 254;
    ip[2] = (uint8_t)(1 + off / 256);      /* 1..254 */
    ip[3] = (uint8_t)(off % 256);
}

/* Which sender address the probe went out with, once we know. It is decided on
 * the first attempt and holds for the rest of the run. */
static enum { PROBE_UNTESTED = 0, PROBE_ZERO, PROBE_SPARE } s_probe_via;

/* How long the last probe took. Kept because the duration is what tells a real
 * check from one that returned without going out, and every message reporting
 * an outcome should be able to show it. */
static int s_probe_ms;

/* True if somebody answers ARP for this address, that is: it is taken.
 *
 * THE SENDER ADDRESS IS THE INTERESTING PART. RFC 3927 wants the probe sent
 * with the sender set to 0.0.0.0, so that nobody caches a mapping for an
 * address we may yet abandon. Whether the W5500 will emit ARP at all with SIPR
 * zeroed is not documented either way, so the first probe TRIES IT and looks at
 * what comes back:
 *
 *   a timeout            the packet went out, nobody holds the address —
 *                        and the conforming probe works
 *   any other failure    the chip refused to send from a null address, so we
 *                        fall back on a temporary one
 *
 * The fallback is 169.254.255.254, in the range RFC 3927 keeps out of automatic
 * selection. That does NOT make it unused — our own previous fixed address sat
 * in that range, put there by hand — but it is held for a few milliseconds and
 * never receives anything, so the worst case is disturbing whoever owns it for
 * an instant. It cannot falsify the test, which watches the timeout on the
 * CANDIDATE address, not on this one. */
static bool autoip_taken(const uint8_t ip[4])
{
    const uint8_t sn = AUTOIP_PROBE_SN;
    static const uint8_t none_addr[4] = {0,0,0,0};
    static const uint8_t spare[4] = {169,254,255,254};

    for (int pass = 0; pass < 2; pass++) {
        /* The sender for this probe. It must NOT be the address under test — a
         * device does not ARP for itself — and it is set HERE so that the
         * retry below really goes out differently from the attempt that
         * failed. */
        const uint8_t *sender = (s_probe_via == PROBE_SPARE) ? spare : none_addr;
        wiz_NetInfo ni;
        wizchip_getnetinfo(&ni);
        memcpy(ni.ip, sender, 4);
        memcpy(ni.gw, sender, 4);
        wizchip_setnetinfo(&ni);

        if (socket(sn, Sn_MR_UDP, 5000, 0) != sn) return true;  /* cannot tell */

        uint8_t dst[4] = { ip[0], ip[1], ip[2], ip[3] };
        uint8_t nothing  = 0;
        /* The parentheses are REQUIRED: setSn_IR expands to (ir & 0x1F), and
         * & binds tighter than |, so an unparenthesised pair would clear only
         * one of the two flags. Every other call in this file passes a single
         * flag, which is why the trap had not been sprung before. */
        setSn_IR(sn, (Sn_IR_TIMEOUT | Sn_IR_SENDOK));
        const int64_t t_probe = esp_timer_get_time();
        const int32_t result   = sendto(sn, &nothing, 1, dst, 5000);
        const int     ms      = (int)((esp_timer_get_time() - t_probe) / 1000);
        s_probe_ms = ms;
        const bool timed_out  = (getSn_IR(sn) & Sn_IR_TIMEOUT) != 0
                           || result == SOCKERR_TIMEOUT;
        setSn_IR(sn, (Sn_IR_TIMEOUT | Sn_IR_SENDOK));
        close(sn);

        if (s_probe_via != PROBE_UNTESTED) return !timed_out;

        /* First probe of the run: decide which way works. SOCKERR_TIMEOUT is
         * the ANSWER, not a failure — it means the ARP went out and nobody
         * replied. Anything else negative means the chip would not send from a
         * null address, which is the case we are testing for. */
        if (result >= 0 || result == SOCKERR_TIMEOUT) {
            s_probe_via = PROBE_ZERO;
            /* THE DURATION IS THE THING TO READ. [M] Measured on hardware:
             * 406 ms with the cable connected and nobody holding the address —
             * two retransmission periods at RCR=2. A probe returning in single
             * digits did not go out at all, whatever it reported, which is the
             * difference between checking an address and only appearing to.
             *
             * [M] The W5500 DOES send with SIPR at zero: the conforming probe
             * works, and leaves no entry in anybody's ARP cache. */
            ESP_LOGI(TAG, "auto-ip: probing from 0.0.0.0 as RFC 3927 asks, first probe %d ms", ms);
            return !timed_out;
        }
        s_probe_via = PROBE_SPARE;
        ESP_LOGW(TAG, "auto-ip: the chip will not send from 0.0.0.0 (%d), "
                      "probing from 169.254.255.254 instead (%d ms)",
                      (int)result, ms);
        /* and round again, this time from the temporary address */
    }
    return true;                          /* neither way worked: assume taken */
}

static bool autoip_run(const uint8_t mac[6], uint8_t chosen[4])
{
    const uint8_t rcr = getRCR();
    setRCR(AUTOIP_RCR);                    /* a slow probe would delay boot */

    bool ok = false;
    for (int n = 0; n < AUTOIP_TRIES && !ok; n++) {
        autoip_candidate(mac, n, chosen);

        if (autoip_taken(chosen)) {
            ESP_LOGW(TAG, "auto-ip: %u.%u.%u.%u is taken, trying another",
                     chosen[0], chosen[1], chosen[2], chosen[3]);
            continue;
        }
        ok = true;
    }
    setRCR(rcr);
    return ok;
}

/* THE MAC MUST BE UNIQUE PER DEVICE, and it cannot be a constant.
 *
 * A fixed MAC would mean two bridges on one network sharing an Ethernet
 * address — worse than an IP clash, because the symptoms appear below the level
 * anyone thinks to look at: frames delivered to the wrong device, switch
 * forwarding tables thrashing, and no message anywhere saying why.
 *
 * The ESP32 carries a unique address in its eFuses, assigned at the factory,
 * and esp_read_mac() with ESP_MAC_ETH derives the Ethernet one from it.
 *
 * IT MATTERS BEYOND THE ETHERNET LAYER: the DJ Link keep-alive carries our MAC
 * at offset 0x26, and the players use it to tell devices apart. Two bridges
 * announcing the same one would be indistinguishable to them too.
 *
 * THE "LOCALLY ADMINISTERED" BIT IS NOT SET, deliberately: the eFuse address is
 * a real globally unique one assigned to Espressif, so claiming otherwise would
 * be a lie, and a needless one. */
static void mac_from_efuse(uint8_t mac[6])
{
    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK) return;
    /* Should the eFuse be unreadable — which would mean a deeper fault — we
     * fall back on the old constant rather than leaving the field at zero, and
     * say so: a shared MAC is worth a line in the log. */
    static const uint8_t fixed[6] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 };
    memcpy(mac, fixed, 6);
    ESP_LOGE(TAG, "eFuse MAC unreadable: falling back on the fixed one, "
                  "which two devices would share");
}

static esp_err_t w5500_configure(void)
{
    uint8_t bufsize[2][8] = { {2,2,2,2,2,2,2,2}, {2,2,2,2,2,2,2,2} };
    if (wizchip_init(bufsize[0], bufsize[1]) != 0) {
        ESP_LOGE(TAG, "wizchip_init failed");
        return ESP_FAIL;
    }
    wiz_NetInfo netinfo = {
        .ip  = W5500_IP,
        .sn  = W5500_SN,
        .gw  = W5500_GW,
        .dhcp = NETINFO_STATIC,
    };
    mac_from_efuse(netinfo.mac);
    wizchip_setnetinfo(&netinfo);

    /* PHY RESET, register PHYCFGR bit 7.
     *
     * The MR soft reset inside wizchip_init resets the TCP/IP registers but NOT
     * the PHY. [M] After a power glitch on the module — the DC jack pulled and
     * reinserted — the PHY can stay down even though the chip answers over SPI.
     * Forcing it to reset and renegotiate the link is the piece that was
     * missing, and the reason a soft reset alone did not bring DJ Link back.
     *
 * IT HAPPENS HERE, BEFORE ANYTHING WAITS FOR THE LINK: resetting the PHY after
 * an address has been obtained would knock down the very link that was waited
 * for. */
    uint8_t phy = getPHYCFGR();
    setPHYCFGR((uint8_t)(phy & ~0x80));   /* bit 7 = 0 -> PHY in reset */
    vTaskDelay(pdMS_TO_TICKS(2));
    setPHYCFGR((uint8_t)(phy | 0x80));    /* bit 7 = 1 -> release      */
    vTaskDelay(pdMS_TO_TICKS(50));        /* time for auto-negotiation */

    return ESP_OK;
}

/* --- GETTING AN ADDRESS, IN A TASK OF ITS OWN ------------------------------
 *
 * WHY A TASK AND NOT A STATE MACHINE. Waiting for the link and for a DHCP
 * server takes up to five seconds, and the ARP probe inside auto-ip cannot be
 * broken up at all: those 400 ms are the chip deciding nobody answered, and the
 * ioLibrary's sendto waits inside. In the main loop that would be a 400 ms stall
 * per candidate; in a task it stalls nothing but itself.
 *
 * IT DESTROYS ITSELF when done. Internal memory is the scarce resource here —
 * [M] the heap has been down to 13 KB with an Ableton peer connected — and a
 * sequence that runs for a few seconds has no business holding a stack all
 * evening. A link event creates it again.
 *
 * THE COST OF THE CHOICE, and it is the one thing a state machine would not
 * have: while the DHCP exchange runs, SIPR is zero. The main loop would happily
 * go on sending keep-alives and status from 0.0.0.0 — so sending is suspended
 * for everyone but the exchange itself, in w5500_net_sendto and its siblings
 * rather than in their callers. See the note beside send_blocked.
 *
 * WITH NO CABLE we skip both DHCP and auto-ip and keep the static address. It
 * is a guess, and marked as one: the recheck on the rising edge of the link
 * settles it, and that is where DHCP gets its second chance too. */
#define ACQ_TASK_STACK   3072
#define ACQ_TASK_PRIO    4      /* above main's 1, below anything time-critical */

/* Should this run stop? Either the link is down right now, or it went down at
 * some point since we started — the caller noted it — and whatever we obtain
 * from here on would be about a network that is no longer the one we started
 * on. */
static bool give_up(void)
{
    if (s_link_dirty) {
        ESP_LOGW(TAG, "the link dropped while we were getting an address: "
                      "stopping, and starting over once it is back");
        return true;
    }
    if (!w5500_net_link_up()) {
        s_link_dirty = true;          /* so the caller knows to ask again */
        ESP_LOGW(TAG, "link down while getting an address: stopping");
        return true;
    }
    return false;
}

static void acquire_task(void *arg)
{
    (void)arg;

    /* CLEARED AT THE START, not at the end. Anything the caller noticed before
     * we began is what this run is FOR; anything noticed from here on means
     * this run is already out of date, and the flag is left set so the caller
     * asks for another. */
    s_link_dirty = false;

    /* A MOMENT FOR THE LINK TO SETTLE before asking anything over it.
     *
     * THE LINK IS ALREADY UP — this task is started on its rising edge and
     * nowhere else — but the PHY raises it as soon as it sees a partner, which
     * is not the same as the far end being ready to carry traffic: a switch
     * just powered on takes longer to start forwarding. [M] That is how three
     * DISCOVERs went out over a cable that was electrically up and carried
     * nothing.
     *
     * Half a second costs nothing here: we are coming from a link that was
     * DOWN, so there was nothing useful going out anyway. */
    vTaskDelay(pdMS_TO_TICKS(LINK_SETTLE_MS));

    /* From here on nothing else may transmit: SIPR is about to go to zero and
     * would put 0.0.0.0 on the wire as our source. */
    s_acquiring = true;

    /* In the order the players use: DHCP first, link-local if nobody answers.
     * The link is checked between the two — the DHCP attempt is the long one,
     * and there is no sense probing for a link-local address on a cable that
     * went away while we asked. */
    bool changed = false;
    if (!give_up()) {
        if (dhcp_lite_acquire()) {
            changed = true;                 /* the lease carries a new address */
        } else if (!give_up()) {
            wiz_NetInfo netinfo;
            wizchip_getnetinfo(&netinfo);
            uint8_t ip[4];
            if (autoip_run(netinfo.mac, ip)) {
                memcpy(netinfo.ip, ip, 4);
                wizchip_setnetinfo(&netinfo);
                ESP_LOGI(TAG, "auto-ip: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                changed = true;
            } else {
                ESP_LOGE(TAG, "auto-ip: every candidate taken, keeping the static address");
            }
        }
    }

    /* Cleared on EVERY path out, including the abandoned one: leaving it set
     * would silence the bridge for good. */
    s_acquiring = false;

    /* THE CALLBACK ONLY IF SOMETHING CHANGED. Reopening sockets is not free —
     * it closes and relistens on three of them — and a run that gave up before
     * touching the address has nothing for the caller to react to. Telling it
     * anyway is how a cable left unplugged produced a reopen every 600 ms. */
    if (changed && s_done_cb) s_done_cb();

    s_acq_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t w5500_net_acquire_address(w5500_net_done_fn on_done)
{
    if (s_acq_task) return ESP_ERR_INVALID_STATE;   /* one at a time */
    s_done_cb = on_done;
    if (xTaskCreatePinnedToCore(acquire_task, "netaddr", ACQ_TASK_STACK, NULL,
                                ACQ_TASK_PRIO, &s_acq_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "acquisition task not created: keeping the static address");
        s_acq_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ===========================================================================
 * GROUPING BUS TRANSACTIONS
 *
 * The reason, measured: [M] sending a beat cost about 3100 us, of which only
 * ~260 were spent WAITING for the bus (paired measurement, probe on the beat
 * alone). The rest was the PER-TRANSACTION cost of the 24 elementary
 * transactions the ioLibrary splits a send into: about 129 us each.
 *
 * Those 129 us are not the transfer — one byte at 20 MHz takes 0.4 us. They are
 * semaphore, spi_device_acquire_bus/release_bus, and two direction changes of
 * GPIO35, paid for EVERY register. And acquire_bus is meant to be held around a
 * GROUP of transactions, not taken and released on every access.
 *
 * The group takes lock, bus and pin direction ONCE per call: inside it,
 * cs_select and cs_deselect shrink to moving CS alone.
 *
 * INTENDED CONSEQUENCE: whoever transmits holds the bus for the whole call —
 * about 0.5 ms for a beat — instead of releasing it 24 times. The display can
 * therefore wait longer, and the reported bus hold WILL rise. That is not a
 * regression: it is exactly what the quiet window exists for, and half a
 * millisecond of display waiting is worth three milliseconds of beat
 * punctuality.
 *
 * Set to 0, everything reverts to the previous behaviour line for line.
 * =========================================================================== */
#define W5500_BUS_GROUP 1

#if W5500_BUS_GROUP

static TaskHandle_t s_grp_task  = NULL;
static int          s_grp_depth = 0;

/* Filtering on the task handle is MANDATORY, not a refinement: with a global
 * flag another task would find "we are in a group", skip the lock, and trample
 * on the bus of whoever is using it. */
static inline bool in_group(void)
{
    return s_grp_task && s_grp_task == xTaskGetCurrentTaskHandle();
}

static void bus_group_begin(void)
{
    if (in_group()) { s_grp_depth++; return; }   /* nested: already ours */
    bsp_spi_lock();
    spi_device_acquire_bus(s_dev, portMAX_DELAY);
    bsp_dc_as_miso();
    /* s_grp_task is written AFTER taking the lock: before that, another task
     * would see us "in a group" while the bus is not really ours yet. */
    s_grp_task  = xTaskGetCurrentTaskHandle();
    s_grp_depth = 1;
}

static void bus_group_end(void)
{
    if (!in_group()) return;
    if (--s_grp_depth > 0) return;
    /* And cleared BEFORE releasing: after the unlock another task could come in
     * and read a stale handle. */
    s_grp_task = NULL;
    bsp_dc_as_output();
    spi_device_release_bus(s_dev);
    bsp_spi_unlock();
}

#else   /* grouping off: behaviour reverts exactly to the previous one */
static inline bool in_group(void)   { return false; }
static inline void bus_group_begin(void) { }
static inline void bus_group_end(void)   { }
#endif

/* ---------- SPI and CS callbacks for the ioLibrary ---------- */
static void cs_select(void)
{
    /* Outside a group: the usual path. The semaphore is shared with the
     * display, which must not flush while we read the W5500 — a flush is an
     * asynchronous DMA, and without this polling_transmit asserts.
     * Inside a group: lock, bus and pin direction are already ours, and only CS
     * remains to be moved. */
    if (!in_group()) {
        bsp_spi_lock();
        spi_device_acquire_bus(s_dev, portMAX_DELAY);
        /* GPIO35 -> MISO (input), needed to read from the W5500. On the CoreS3
         * that pin is also the display's DC, so cs_deselect puts it back. */
        bsp_dc_as_miso();
    }
    gpio_set_level(s_cs_gpio, 0);
}

static void cs_deselect(void)
{
    gpio_set_level(s_cs_gpio, 1);
    if (!in_group()) {
        /* GPIO35 -> output, drivable again as DC by the display. */
        bsp_dc_as_output();
        spi_device_release_bus(s_dev);
        bsp_spi_unlock();
    }
}

static uint8_t spi_readbyte(void)
{
    uint8_t tx = 0xFF, rx = 0x00;
    spi_transaction_t t = { .length = 8, .tx_buffer = &tx, .rx_buffer = &rx };
    spi_device_polling_transmit(s_dev, &t);
    return rx;
}

static void spi_writebyte(uint8_t b)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &b };
    spi_device_polling_transmit(s_dev, &t);
}

static void spi_readburst(uint8_t *buf, uint16_t len)
{
    spi_transaction_t t = { .length = 8 * len, .rxlength = 8 * len,
                            .rx_buffer = buf, .tx_buffer = NULL, .flags = 0 };
    /* With no tx_buffer the master sends 0x00, which is fine for a W5500 read. */
    spi_device_polling_transmit(s_dev, &t);
}

static void spi_writeburst(uint8_t *buf, uint16_t len)
{
    spi_transaction_t t = { .length = 8 * len, .tx_buffer = buf };
    spi_device_polling_transmit(s_dev, &t);
}

/* Critical section: on ESP-IDF briefly disabling the scheduler would do, but
 * here the bus lock is already guaranteed by acquire/release. A light no-op. */
static void cris_enter(void) { }
static void cris_exit(void)  { }

esp_err_t w5500_net_init(void)
{
    /* The W5500 CS is driven by hand in the callbacks, so the SPI device is
     * registered without an automatic CS. */
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << BSP_W5500_CS_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cs);
    gpio_set_level(BSP_W5500_CS_GPIO, 1);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = BSP_W5500_SPI_HZ,     /* 20 MHz or below on the CoreS3 */
        .mode = 0,
        .spics_io_num = -1,                     /* CS handled in the callbacks */
        .queue_size = 4,
        .command_bits = 0,
        .address_bits = 0,
    };
    esp_err_t err = spi_bus_add_device(BSP_SPI_HOST, &devcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    /* Register the ioLibrary callbacks. */
    reg_wizchip_cris_cbfunc(cris_enter, cris_exit);
    reg_wizchip_cs_cbfunc(cs_select, cs_deselect);
    reg_wizchip_spi_cbfunc(spi_readbyte, spi_writebyte);
    reg_wizchip_spiburst_cbfunc(spi_readburst, spi_writeburst);

    /* Socket buffers and IP/MAC, through the shared helper. */
    esp_err_t cerr = w5500_configure();
    if (cerr != ESP_OK) return cerr;

    ESP_LOGI(TAG, "W5500 init ok");
    return ESP_OK;
}

/* Automatic recovery, to be called when the W5500 comes back — for instance
 * after its DC jack was pulled and reinserted. The chip has already reset
 * itself when power returned; here it is reconfigured and the socket flagged
 * as needing to be reopened. Returns ESP_OK if the chip answers. */
esp_err_t w5500_net_reinit(void)
{
    s_socket_open = false;                 /* the socket went with the reset */
    if (getVERSIONR() != 0x04) return ESP_FAIL;   /* not ready yet: retry later */
    return w5500_configure();
}

int w5500_net_open_udp(uint8_t sn, uint16_t port)
{
    /* GUARD: if the chip does not answer (VERSIONR != 0x04), do NOT attempt the
     * socket — the ioLibrary's close() and socket() would enter an endless
     * polling loop and trip the watchdog. Better to fail cleanly and leave the
     * display and WiFi alive. */
    if (getVERSIONR() != 0x04) {
        ESP_LOGE(TAG, "W5500 not answering (VERSIONR!=0x04): socket %u NOT opened", sn);
        return -1;
    }
    int8_t s = socket(sn, Sn_MR_UDP, port, 0);
    if (s != sn) {
        ESP_LOGE(TAG, "socket(%u) -> %d", sn, s);
        return -1;
    }
    s_socket_open = true;
    return sn;
}

/* ===========================================================================
 * SPECIALISED UDP RECEIVE
 *
 * Symmetric to the send path, for the same measured reasons. [M] An ioLibrary
 * recvfrom costs about 19 SPI transactions, and with grouping active that is
 * where all the time goes: roughly 100 us each. The waste mirrors the send
 * path exactly —
 *
 *   getSn_RX_RSR      four reads, because of the WIZnet errata double read
 *   pointers          read and rewritten TWICE, once for the header and once
 *                     for the payload
 *   Sn_CR = RECV      issued TWICE, each with its own wait
 *
 * — and it compresses using the same two facts about the W5500: the receive
 * registers are CONTIGUOUS (Sn_RX_RSR 0x26, Sn_RX_RD 0x28, Sn_RX_WR 0x2A), and
 * the ring WRAP is handled by the chip, since wiz_recv_data does not check it.
 *
 * Six transactions: state and pointers, header, payload, advanced pointer,
 * RECV, command acknowledgement.
 *
 * THE MOST IMPORTANT DESIGN DECISION HERE:
 *
 *   Sn_RX_RSR is used ONLY AS A GATE — "is there anything?" — and NEVER as a
 *   measure of how much to read. The length comes from the datagram HEADER.
 *
 * That is the lesson of the fault that kept the bridge out of service for days:
 * sizing the read on RSR produces partial reads while the chip is still
 * writing, the stream loses alignment, and status packets disappear for seconds
 * from BOTH players. The defect was fixed in the wrapper; this path must not
 * reintroduce it by another route.
 *
 * If the payload does not fit the caller's buffer it is truncated, BUT the
 * pointer still advances by the FULL length: better to deliver a clipped packet
 * than to leave the stream misaligned.
 * =========================================================================== */
#define W5500_FAST_RECV 1

#if W5500_FAST_RECV

/* Register window, from Sn_CR (0x01) to Sn_RX_WR+1 (0x2B). Deliberately
 * SEPARATE from the send path's window, so the two stay independent and can be
 * disabled one at a time. */
#define FR_LEN      0x2B                  /* 43 bytes: 0x01..0x2B inclusive */
#define FR_OFF_SR   (0x03 - 0x01)
#define FR_OFF_RSR  (0x26 - 0x01)
#define FR_OFF_RD   (0x28 - 0x01)

static uint32_t s_fr_n = 0, s_fr_bad = 0, s_fr_trunc = 0;

static int32_t recv_fast(uint8_t sn, uint8_t *buf, size_t maxlen,
                         uint8_t from_ip[4], uint16_t *from_port)
{
    uint8_t regs[FR_LEN];
    bus_group_begin();

    /* (1) receive state and pointers, in a single read */
    WIZCHIP_READ_BUF(Sn_CR(sn), regs, FR_LEN);
    uint8_t  sr  = regs[FR_OFF_SR];
    uint16_t rsr = ((uint16_t)regs[FR_OFF_RSR] << 8) | regs[FR_OFF_RSR + 1];
    uint16_t rd  = ((uint16_t)regs[FR_OFF_RD]  << 8) | regs[FR_OFF_RD  + 1];

    if (sr != SOCK_UDP) { bus_group_end(); return -3; }
    if (rsr == 0)       { bus_group_end(); return 0; }   /* a gate, not a measure */

    const uint32_t rxblk = (uint32_t)(WIZCHIP_RXBUF_BLOCK(sn) << 3);

    /* (2) header: source IP (4), port (2), payload length (2) */
    uint8_t head[8];
    WIZCHIP_READ_BUF(((uint32_t)rd << 8) + rxblk, head, 8);
    uint16_t plen = ((uint16_t)head[6] << 8) | head[7];

    /* Sanity. With the ioLibrary this case could not arise; here it can, and it
     * must be VISIBLE rather than silent. The pointer is not advanced: better to
     * reread the same state next time round than to lose alignment over an
     * absurd length. */
    if (plen == 0 || plen > 1472) { s_fr_bad++; bus_group_end(); return -5; }

    uint16_t take = (plen > maxlen) ? (uint16_t)maxlen : plen;
    if (take != plen) s_fr_trunc++;

    /* (3) payload, right after the header in the ring */
    WIZCHIP_READ_BUF(((uint32_t)(uint16_t)(rd + 8) << 8) + rxblk, buf, take);

    /* (4) pointer advanced ONCE, and by the FULL length */
    uint16_t newrd = (uint16_t)(rd + 8 + plen);
    uint8_t  p[2]  = { (uint8_t)(newrd >> 8), (uint8_t)newrd };
    WIZCHIP_WRITE_BUF(Sn_RX_RD(sn), p, 2);

    /* (5) a single RECV command, not two */
    WIZCHIP_WRITE(Sn_CR(sn), Sn_CR_RECV);

    /* (6) the chip must accept the command */
    int guard = 0;
    while (WIZCHIP_READ(Sn_CR(sn))) { if (++guard > 64) break; }

    bus_group_end();

    if (from_ip)   memcpy(from_ip, head, 4);
    if (from_port) *from_port = ((uint16_t)head[4] << 8) | head[5];
    s_fr_n++;
    return (int32_t)take;
}

void w5500_net_fast_recv_stats(uint32_t *n, uint32_t *bad, uint32_t *trunc)
{
    if (n)     *n     = s_fr_n;
    if (bad)   *bad   = s_fr_bad;
    if (trunc) *trunc = s_fr_trunc;
    s_fr_n = s_fr_bad = s_fr_trunc = 0;
}
#else
void w5500_net_fast_recv_stats(uint32_t *n, uint32_t *bad, uint32_t *trunc)
{
    if (n) *n = 0; if (bad) *bad = 0; if (trunc) *trunc = 0;
}
#endif

int32_t w5500_net_recv(uint8_t sn, uint8_t *buf, size_t maxlen,
                       uint8_t from_ip[4], uint16_t *from_port)
{
    if (!s_socket_open) return 0;             /* chip not ready: no access */
    if (!buf || maxlen == 0) return 0;

    /* Sn_RX_RSR only answers whether there is anything: it is a gate, not a
     * measure to pass downstream. */
#if W5500_FAST_RECV
    /* The specialised path. Callers are unaffected: the switch lives here, and
     * is independent of the send and grouping switches. */
    return recv_fast(sn, buf, maxlen, from_ip, from_port);
#else
    bus_group_begin();
    if (getSn_RX_RSR(sn) == 0) { bus_group_end(); return 0; }

    /* recvfrom is given the BUFFER CAPACITY, not the number of pending bytes.
     *
     * In the ioLibrary that parameter decides how much of the datagram to take:
     *
     *     if (len < sock_remained_size[sn]) pack_len = len;   // PARTIAL READ
     *     else                              pack_len = sock_remained_size[sn];
     *
     * Passing Sn_RX_RSR — as the canonical WIZnet example does, and as we once
     * did — only requires the poll to land while the chip is still writing a
     * datagram: RSR reports less than the whole packet, the library still reads
     * the header (which declares the true length), finds len smaller, and takes
     * a PART of it, consuming bytes from the ring that were not yet written and
     * leaving sock_remained_size non-zero.
     *
     * From then on the stream is MISALIGNED: every subsequent read takes its
     * header from the middle of a payload and every parse fails, until the ring
     * resynchronises by itself — seconds, not milliseconds.
     *
     * [M] It is the explanation consistent with the episode observed twice in
     * the field: status packets gone from BOTH players at the same instant —
     * which points at the socket stream, not the players — with beat packets
     * regular on the other socket, the main loop alive, and transmission normal
     * throughout.
     *
     * With the buffer capacity, a partial read can only happen if a datagram
     * exceeds the caller's buffer: 512 bytes here against 284 for a status
     * packet, so never. */
    uint16_t fport = 0;
    int32_t n = recvfrom(sn, buf, (uint16_t)(maxlen > 0xFFFF ? 0xFFFF : maxlen),
                         from_ip, &fport);
    bus_group_end();
    if (from_port) *from_port = fport;
    return n;
#endif
}

bool w5500_net_link_up(void)
{
    uint8_t phy = 0;
    ctlwizchip(CW_GET_PHYLINK, &phy);
    return phy == PHY_LINK_ON;
}

uint8_t w5500_net_version(void)
{
    /* VERSIONR (0x0039) must read 0x04 on a live W5500 (datasheet 4.1). */
    return getVERSIONR();
}

void w5500_net_sock_state(uint8_t sn, uint8_t *sr, uint16_t *rx_pending, uint16_t *tx_free)
{
    if (!s_socket_open) {
        if (sr) *sr = 0xFF;                /* 0xFF = not queried */
        if (rx_pending) *rx_pending = 0;
        if (tx_free) *tx_free = 0;
        return;
    }
    bus_group_begin();
    if (sr)         *sr         = getSn_SR(sn);
    if (rx_pending) *rx_pending = getSn_RX_RSR(sn);
    if (tx_free)    *tx_free    = getSn_TX_FSR(sn);
    bus_group_end();
}

/* ===========================================================================
 * SPECIALISED UDP SEND
 *
 * Why it exists, in measured numbers. [M] A send through the ioLibrary's sendto
 * costs 23 SPI transactions, and with bus grouping active the remaining time is
 * all there: about 66 us per transaction (spi_device_polling_transmit plus the
 * two CS movements), some 1530 us of WORK in total. The 96-byte payload is ONE
 * of those 23; everything else is protocol, and much of it waste:
 *
 *   getSn_TX_FSR      four reads — it implements the WIZnet errata, reading
 *                     twice and repeating until two reads agree. On a socket
 *                     idle for 500 ms that is pure ceremony.
 *   setSn_DPORT       two writes — the macro issues two one-byte WIZCHIP_WRITEs
 *                     instead of one two-byte burst.
 *   getSn_IR          about six reads — waiting for SEND_OK, the confirmation
 *                     that transmission finished. For a fire-and-forget
 *                     broadcast that is unnecessary: the next send is 500 ms
 *                     away.
 *
 * This version uses SEVEN, exploiting two facts about the W5500:
 *
 *   - the useful registers are CONTIGUOUS. Sn_CR (0x01) through Sn_TX_WR (0x25)
 *     span 37 bytes, so a single burst read fetches CR, IR, SR, TX_FSR and
 *     TX_WR. Likewise Sn_DIPR (0x0C) and Sn_DPORT (0x10) are six adjacent
 *     bytes: one write.
 *   - the transmit ring WRAP is done by THE CHIP. There is no arithmetic to
 *     replicate: the ioLibrary's wiz_send_data is three lines, and none of them
 *     checks for wrap.
 *
 * COEXISTING WITH THE ioLibrary. For UDP the library keeps no per-socket state
 * — sock_is_sending concerns TCP only — so the only shared thing is the chip's
 * registers. Therefore:
 *   - the destination is rewritten on EVERY send rather than once at open:
 *     takeover replies also go out on this socket through the library's sendto,
 *     which changes it. One transaction, and one fragile assumption removed;
 *   - the previous round's SEND_OK flag is cleared here, or a library sendto
 *     would find it already high and return before actually sending. It is
 *     cleared only if genuinely set, which we have just read.
 *
 * Returns the bytes queued, or: -1 bad arguments or socket not ready, -3 the
 * socket is not in UDP state, -4 not enough transmit space.
 * =========================================================================== */
#define W5500_FAST_SEND 1

#if W5500_FAST_SEND

/* Register window read in one go, from Sn_CR (0x01) to Sn_TX_WR+1 (0x25).
 * Offsets are relative to the start of the window. */
/* Counters for the two failures this path CANNOT report immediately, because it
 * does not wait for the transmission confirmation:
 *
 *   timeout   ARP failed. It concerns UNICAST only (the status packets sent to
 *             the players): the chip could not find the destination MAC and
 *             dropped the packet. The library's sendto returned SOCKERR_TIMEOUT;
 *             here it is discovered on the NEXT round — but it IS discovered,
 *             because Sn_IR is read at the start of every send anyway. It costs
 *             nothing.
 *   nospace   not enough room in the transmit ring. It should never happen
 *             (2 KB against 284 bytes, and the sockets are idle between sends):
 *             if it moves, something is wrong. */
static uint32_t s_fs_sends = 0, s_fs_timeout = 0, s_fs_nospace = 0;

#define FS_LEN      0x25            /* 37 bytes: 0x01..0x25 inclusive */
#define FS_OFF_IR   (0x02 - 0x01)
#define FS_OFF_SR   (0x03 - 0x01)
#define FS_OFF_FSR  (0x20 - 0x01)
#define FS_OFF_WR   (0x24 - 0x01)

int32_t w5500_net_sendto_fast(uint8_t sn, const uint8_t *buf, size_t len,
                              const uint8_t ip[4], uint16_t port)
{
    if (send_blocked(sn)) return 0;   /* acquiring an address: see the note */
    if (!s_socket_open || !buf || !ip || len == 0 || len > 1472) return -1;

    uint8_t regs[FS_LEN];
    bus_group_begin();

    /* (1) a single read: CR, IR, SR, TX_FSR, TX_WR */
    WIZCHIP_READ_BUF(Sn_CR(sn), regs, FS_LEN);

    uint8_t  ir  = regs[FS_OFF_IR];
    uint8_t  sr  = regs[FS_OFF_SR];
    uint16_t fsr = ((uint16_t)regs[FS_OFF_FSR] << 8) | regs[FS_OFF_FSR + 1];
    uint16_t wr  = ((uint16_t)regs[FS_OFF_WR]  << 8) | regs[FS_OFF_WR  + 1];

    if (sr != SOCK_UDP)          { bus_group_end(); return -3; }
    if (fsr < (uint16_t)len)     { s_fs_nospace++; bus_group_end(); return -4; }

    /* (2) leftovers from the previous pass, only if there really are any.
     * The outcome of the PREVIOUS send is collected here too: we did not wait
     * for it, but the chip recorded it and we come through here anyway. */
    uint8_t stale = ir & (Sn_IR_SENDOK | Sn_IR_TIMEOUT);
    if (ir & Sn_IR_TIMEOUT) s_fs_timeout++;
    if (stale) WIZCHIP_WRITE(Sn_IR(sn), stale);

    /* (3) destination: DIPR (4 bytes) and DPORT (2) are adjacent -> one burst */
    uint8_t dst[6] = { ip[0], ip[1], ip[2], ip[3],
                       (uint8_t)(port >> 8), (uint8_t)port };
    WIZCHIP_WRITE_BUF(Sn_DIPR(sn), dst, 6);

    /* (4) payload into the transmit ring: the chip handles the wrap */
    WIZCHIP_WRITE_BUF(((uint32_t)wr << 8) + (WIZCHIP_TXBUF_BLOCK(sn) << 3),
                      (uint8_t *)buf, (uint16_t)len);

    /* (5) advance the write pointer */
    wr += (uint16_t)len;
    uint8_t p[2] = { (uint8_t)(wr >> 8), (uint8_t)wr };
    WIZCHIP_WRITE_BUF(Sn_TX_WR(sn), p, 2);

    /* (6) SEND */
    WIZCHIP_WRITE(Sn_CR(sn), Sn_CR_SEND);

    /* (7) the chip must ACCEPT the command, clearing Sn_CR. This is not the
     * wait for transmission to finish — that one we deliberately skip, and
     * skipping it is the whole point. */
    int guard = 0;
    while (WIZCHIP_READ(Sn_CR(sn))) { if (++guard > 64) break; }

    s_fs_sends++;
    bus_group_end();
    return (int32_t)len;
}

void w5500_net_fast_stats(uint32_t *sends, uint32_t *timeouts, uint32_t *nospace)
{
    if (sends)    *sends    = s_fs_sends;
    if (timeouts) *timeouts = s_fs_timeout;
    if (nospace)  *nospace  = s_fs_nospace;
    s_fs_sends = s_fs_timeout = s_fs_nospace = 0;
}

#else   /* disabled: falls back to the ioLibrary's sendto, exactly as before */
void w5500_net_fast_stats(uint32_t *sends, uint32_t *timeouts, uint32_t *nospace)
{
    if (sends) *sends = 0; if (timeouts) *timeouts = 0; if (nospace) *nospace = 0;
}

int32_t w5500_net_sendto_fast(uint8_t sn, const uint8_t *buf, size_t len,
                              const uint8_t ip[4], uint16_t port)
{
    return w5500_net_sendto(sn, buf, len, ip, port);
}
#endif

int32_t w5500_net_sendto(uint8_t sn, const uint8_t *buf, size_t len,
                         const uint8_t ip[4], uint16_t port)
{
    if (send_blocked(sn)) return 0;   /* acquiring an address: see the note */
    if (!s_socket_open) return -1;
    uint8_t addr[4] = { ip[0], ip[1], ip[2], ip[3] };
    bus_group_begin();
    int32_t r = sendto(sn, (uint8_t *)buf, (uint16_t)len, addr, port);
    bus_group_end();
    return r;
}

void w5500_net_get_mac(uint8_t out[6]) { getSHAR(out); }
void w5500_net_get_ip(uint8_t out[4])  { getSIPR(out); }

/* ===================== TCP (dbserver on port 12523) ========================
 *
 * NOTE: the SPI lock is NOT taken here. The cs_select/cs_deselect callbacks
 * registered with the ioLibrary already do it — lock, GPIO35 to MISO, release —
 * and taking it again here would deadlock. Same arrangement as the UDP path. */

int w5500_net_tcp_listen(uint8_t sn, uint16_t port)
{
    close(sn);
    int8_t r = socket(sn, Sn_MR_TCP, port, 0);
    if (r != (int8_t)sn) {
        ESP_LOGE(TAG, "tcp_listen: socket() sn=%u -> %d (expected %u)", sn, (int)r, sn);
        return -1;
    }
    int8_t l = listen(sn);
    if (l != SOCK_OK) {
        ESP_LOGE(TAG, "tcp_listen: listen() -> %d (SOCK_OK=%d)", (int)l, (int)SOCK_OK);
        return -2;
    }
    ESP_LOGI(TAG, "tcp_listen: sn=%u port=%u OK (SR=0x%02x)", sn, port, getSn_SR(sn));
    return 0;
}

/* Direct read from the chip's RX buffer.
 *
 * The ioLibrary's recv() is NOT used. A LIBRARY BUG (confirmed in
 * socket.c): on a NON-BLOCKING socket it ALWAYS returns SOCK_BUSY (= 0) *even*
 * before even looking for data --
 *
 *     #else
 *         if (sock_io_mode & (1 << sn)) return SOCK_BUSY;   <- was
 *         if (recvsize != 0) break;                          <- never reached
 *     #endif
 *
 * -- while in the IPv6 branch the two checks are in the right order. With
 * SF_IO_NONBLOCK nothing is ever read.
 *
 * The library is not patched (it is fetched by fetch_iolibrary.sh: a patch
 * would be lost at the first update, and the bug would go quiet again). It is
 * simpler not to call the faulty function at all: the check it is missing is
 * done by the caller (RX_RSR says how many bytes there really are), and here
 * we do what recv() would do after its wrong guard.
 * No waiting: register accesses only. */
static int32_t rx_drain(uint8_t sn, uint8_t *buf, uint16_t avail)
{
    wiz_recv_data(sn, buf, avail);
    setSn_CR(sn, Sn_CR_RECV);
    while (getSn_CR(sn)) { }        /* the command completes in microseconds */
    return (int32_t)avail;
}

int32_t w5500_net_tcp_poll(uint8_t sn, uint8_t *buf, size_t maxlen, uint8_t from_ip[4])
{
    /* One group for the whole function: up to four accesses plus the drain.
     * Mind the EXITS - there are three, and the group must be closed on every
     * one of them: leaving it open would block the bus for everybody, the
     * display included. */
    bus_group_begin();
    int32_t out = 0;
    uint8_t st = getSn_SR(sn);
    if (st == SOCK_ESTABLISHED) {
        if (from_ip) getSn_DIPR(sn, from_ip);
        uint16_t avail = getSn_RX_RSR(sn);
        if (avail != 0) {
            if (avail > maxlen) avail = (uint16_t)maxlen;
            out = rx_drain(sn, buf, avail);
        }
    } else if (st == SOCK_CLOSE_WAIT) {
        disconnect(sn);
        out = -2;                      /* the peer closed */
    }
    bus_group_end();
    return out;
}

int32_t w5500_net_tcp_send(uint8_t sn, const uint8_t *buf, size_t len)
{
    if (send_blocked(sn)) return 0;   /* acquiring an address: see the note */
    uint8_t st = getSn_SR(sn);
    if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) return -1;
    if (len == 0) return 0;

    /* The ioLibrary's send() returns SOCK_BUSY (which is 0!) if the W5500 has
     * not yet finished the previous send (SENDOK not raised). It is NOT an
     * error: it means "retry". Ignoring it made back-to-back messages vanish
     * (confirmed with Wireshark: a 0-byte segment, sequence number stuck).
     *
     * We do NOT wait here: stopping the loop even for a few milliseconds
     * would skew the beats. We simply return 0 = "it did not go out", and it is
     * up to the CALLER to retry on the next pass (the state machine does: the
     * send stays pending until the chip really takes it). */
    bus_group_begin();
    int32_t r = send(sn, (uint8_t *)buf, (uint16_t)len);
    bus_group_end();
    if (r == SOCK_BUSY) return 0;      /* busy: retry, not an error */
    return r;
}

void w5500_net_tcp_close(uint8_t sn, uint16_t port)
{
    close(sn);
    w5500_net_tcp_listen(sn, port);    /* back to listening */
}

/* ---------- TCP client (we query a player's dbserver) ---------- */

int w5500_net_tcp_connect(uint8_t sn, const uint8_t ip[4], uint16_t port)
{
    close(sn);
    /* arbitrary but stable local source port */
    if (socket(sn, Sn_MR_TCP, (uint16_t)(40000 + sn), 0) != (int8_t)sn) {
        ESP_LOGE(TAG, "tcp_connect: socket() failed");
        return -1;
    }
    uint8_t addr[4] = { ip[0], ip[1], ip[2], ip[3] };
    int8_t r = connect(sn, addr, port);          /* blocks until the result */
    if (r != SOCK_OK) {
        ESP_LOGE(TAG, "tcp_connect: connect() -> %d", (int)r);
        close(sn);
        return -2;
    }
    return 0;
}

int32_t w5500_net_tcp_recv(uint8_t sn, uint8_t *buf, size_t maxlen)
{
    uint8_t st = getSn_SR(sn);
    /* CLOSE_WAIT means the peer has closed ITS side, but the data it sent us is
     * STILL in the receive buffer and must be read. [M] Pioneer players close
     * immediately after answering — the port-mapper on :12523 sends its two
     * bytes and closes — so treating CLOSE_WAIT as an error lost exactly the
     * reply we were waiting for. */
    if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) return -1;
    uint16_t avail = getSn_RX_RSR(sn);
    if (avail == 0) return (st == SOCK_CLOSE_WAIT) ? -2 : 0;  /* closed AND empty */
    if (avail > maxlen) avail = (uint16_t)maxlen;

    return rx_drain(sn, buf, avail);
}

void w5500_net_tcp_disconnect(uint8_t sn)
{
    disconnect(sn);
    close(sn);
}

/* --- NON-BLOCKING TCP connection ------------------------------------------
 * The ioLibrary's connect() waits for the outcome: unacceptable in the loop,
 * where beats fall every ~470 ms. Here the socket is opened NON-blocking and
 * the caller polls the state when it suits them. */
int w5500_net_tcp_connect_start(uint8_t sn, const uint8_t ip[4], uint16_t port)
{
    close(sn);
    if (socket(sn, Sn_MR_TCP, (uint16_t)(40000 + sn), SF_IO_NONBLOCK) != (int8_t)sn) {
        ESP_LOGE(TAG, "tcp_connect_start: socket() failed");
        return -1;
    }
    uint8_t addr[4] = { ip[0], ip[1], ip[2], ip[3] };
    int8_t r = connect(sn, addr, port);      /* non-blocking: returns SOCK_BUSY */
    if (r != SOCK_OK && r != SOCK_BUSY) {
        close(sn);
        return -2;
    }
    return 0;
}

int w5500_net_tcp_connect_poll(uint8_t sn)
{
    uint8_t st = getSn_SR(sn);
    if (st == SOCK_ESTABLISHED) return 1;
    if (st == SOCK_INIT || st == SOCK_SYNSENT) return 0;   /* in progress */
    return -1;                                             /* closed/failed */
}

void w5500_net_tcp_diag(uint8_t sn, uint8_t *sr, uint8_t *ir,
                        uint16_t *tx_free, uint16_t *tx_max,
                        uint16_t *rx_avail, uint16_t *rx_max)
{
    if (sr)       *sr       = getSn_SR(sn);
    if (ir)       *ir       = getSn_IR(sn);        /* bit2=RECV bit4=SENDOK */
    if (tx_free)  *tx_free  = getSn_TX_FSR(sn);
    if (tx_max)   *tx_max   = getSn_TxMAX(sn);
    if (rx_avail) *rx_avail = getSn_RX_RSR(sn);    /* bytes received, pending */
    if (rx_max)   *rx_max   = getSn_RxMAX(sn);     /* RX buffer size          */
}

/* =========================================================================
 * INTERRUPTS
 *
 * The W5500 raises INTn when a socket ENABLED in SIMR sees an event enabled in
 * its own Sn_IMR. MIND THIS: INTn stays ASSERTED, held low, until Sn_IR is
 * cleared by writing a 1 over the bit. It is not a pulse — without the clear
 * you get a single edge and then nothing more.
 *
 * Here only the requested socket is enabled, and only on the RECV event, so the
 * pin fires solely for the packets we care about and the other sockets stay on
 * polling without interfering.
 * ========================================================================= */
void w5500_net_int_enable_recv(uint8_t sn)
{
    setSn_IMR(sn, Sn_IR_RECV);        /* this socket: "data received" only     */
    setSIMR((uint8_t)(1u << sn));     /* only this socket may raise INTn       */
    setSn_IR(sn, Sn_IR_RECV);         /* start clean: clear anything pending    */
}

void w5500_net_int_disable(void)
{
    setSIMR(0x00);                    /* no socket raises INTn */
}

uint8_t w5500_net_int_ack(uint8_t sn)
{
    uint8_t ir = getSn_IR(sn);
    if (ir & Sn_IR_RECV) setSn_IR(sn, Sn_IR_RECV);   /* writing 1 clears it */
    return ir;
}

uint8_t w5500_net_int_pending(void)
{
    return getSIR();                  /* bitmap of sockets with a live interrupt */
}
