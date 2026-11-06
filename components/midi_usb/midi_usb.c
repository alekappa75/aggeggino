#include "midi_usb.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
/* The TINYUSB_DEFAULT_CONFIG macro is NOT in tinyusb.h: it lives in a separate
 * header that must be included explicitly, and it only exists from
 * esp_tinyusb 2.x. __has_include detects it rather than assuming it: if it is
 * missing, the structure is filled by hand further down and the component
 * still compiles. */
#if defined(__has_include)
#  if __has_include("tinyusb_default_config.h")
#    include "tinyusb_default_config.h"
#    define MIDI_USB_HAS_DEFAULT_CFG 1
#  endif
#endif
#include "tusb.h"

static const char *TAG = "midi_usb";

/* --- Descriptor strings ----------------------------------------------------
 *
 * THE ORDER IS IMPOSED BY THE PROTOCOL, not by us:
 *   [0] language code (0x0409 = US English), NOT a string
 *   [1] iManufacturer
 *   [2] iProduct        <- this is the one the host lists
 *   [3] iSerialNumber
 *   [4] MIDI interface name
 *
 * Indices 1..4 are referenced by the i* fields of the device and configuration
 * descriptors: changing them here without changing them there yields a device
 * showing the wrong string, or none at all.
 *
 * The buffers are STATIC because TinyUSB keeps the pointers for the lifetime of
 * the device — its own header says so: "All non-NULL pointers must remain valid
 * for the lifetime of the TinyUSB device stack". Passing temporaries would be a
 * defect that surfaces much later, once the memory is reused. */
static char s_str_manuf[MIDI_USB_NAME_MAX + 1];
static char s_str_prod [MIDI_USB_NAME_MAX + 1];
static char s_str_seri [MIDI_USB_NAME_MAX + 1];
static char s_str_port [MIDI_USB_NAME_MAX + 1];

static const char *s_strings[5] = {
    (const char[]){ 0x09, 0x04 },   /* language code, two raw bytes */
    s_str_manuf,
    s_str_prod,
    s_str_seri,
    s_str_port,
};

/* --- Device descriptor -----------------------------------------------------
 *
 * VID/PID, and this deserves saying plainly. 0x303A is Espressif's Vendor ID,
 * and 0x4000+ is the range Espressif assigns to devices built on its own
 * chips. Using it is standard practice for ESP32 projects and works with any
 * host, but it is NOT an identifier of ours: if this project were ever
 * distributed, the correct route is an assigned PID — Espressif grants them
 * free on request — or a Vendor ID of our own.
 *
 * bcdUSB 0x0200 with bDeviceClass 0x00: the class is declared at INTERFACE
 * level rather than device level, which is how audio and MIDI do it. */
static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* --- Configuration descriptor ----------------------------------------------
 *
 * One configuration, one MIDI interface, two endpoints (in and out).
 * TUD_MIDI_DESCRIPTOR does all the work: MIDI 1.0 over USB is formally a
 * subclass of the Audio Class, and writing that descriptor by hand would be
 * long and easy to get wrong. */
#define MIDI_USB_CFG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)
#define MIDI_USB_EP_OUT         0x01
#define MIDI_USB_EP_IN          0x81

static const uint8_t s_desc_cfg[] = {
    /* config number, interface count, string index, total length,
     * attributes, power in mA */
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, MIDI_USB_CFG_TOTAL_LEN, 0x00, 100),
    /* interface number, string index, EP OUT, EP IN, EP size */
    TUD_MIDI_DESCRIPTOR(0, 4, MIDI_USB_EP_OUT, MIDI_USB_EP_IN, 64),
};

static bool s_running = false;

static void copy_name(char *dst, const char *src, const char *fallback)
{
    const char *v = (src && src[0]) ? src : fallback;
    strncpy(dst, v, MIDI_USB_NAME_MAX);
    dst[MIDI_USB_NAME_MAX] = '\0';
}

esp_err_t midi_usb_start(const midi_usb_cfg_t *cfg)
{
    if (s_running) return ESP_OK;

    const char *prod = (cfg && cfg->product && cfg->product[0])
                       ? cfg->product : "aggeggino";

    copy_name(s_str_manuf, cfg ? cfg->manufacturer : NULL, "aggeggino");
    copy_name(s_str_prod,  prod,                           "aggeggino");

    if (cfg && cfg->serial && cfg->serial[0]) {
        copy_name(s_str_seri, cfg->serial, "");
    } else {
        /* Derived from the MAC, so two units on the same machine stay
         * distinguishable — that is what the host uses to remember the
         * choices made for this particular device. */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_str_seri, sizeof(s_str_seri), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (cfg && cfg->port_name && cfg->port_name[0]) {
        copy_name(s_str_port, cfg->port_name, "");
    } else {
        snprintf(s_str_port, sizeof(s_str_port), "%.24s clock", prod);
    }

#ifdef MIDI_USB_HAS_DEFAULT_CFG
    tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG();
#else
    /* esp_tinyusb 1.x has no default macro. Zeroing and filling only the
     * fields we need works on both versions: those absent in 1.x — task, port,
     * phy — are simply never touched. */
    tinyusb_config_t tcfg = { 0 };
#endif
    tcfg.descriptor.device            = &s_desc_device;
    tcfg.descriptor.string            = s_strings;
    tcfg.descriptor.string_count      = sizeof(s_strings) / sizeof(s_strings[0]);
    tcfg.descriptor.full_speed_config = s_desc_cfg;

    /* The component already defaults to CPU1 on multicore builds, but it is
     * written out anyway: a default can change, and core 0 is the real-time
     * island of the wired side — the USB state machine must not set foot
     * there. */
    tcfg.task.xCoreID  = (cfg && cfg->task_core  >= 0) ? cfg->task_core  : 1;
    if (cfg && cfg->task_prio  > 0) tcfg.task.priority = (uint8_t)cfg->task_prio;
    if (cfg && cfg->task_stack > 0) tcfg.task.size     = (size_t)cfg->task_stack;
    /* If the macro was absent the fields are zero and need sensible values:
     * a stack size of 0 would create no task at all. */
    if (tcfg.task.size     == 0) tcfg.task.size     = 4096;
    if (tcfg.task.priority == 0) tcfg.task.priority = 5;

    esp_err_t e = tinyusb_driver_install(&tcfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(e));
        return e;
    }
    s_running = true;
    ESP_LOGW(TAG, "USB MIDI device started: '%s' (serial %s), "
                  "port '%s', core %d prio %d stack %d",
             s_str_prod, s_str_seri, s_str_port,
             tcfg.task.xCoreID, (int)tcfg.task.priority, (int)tcfg.task.size);
    return ESP_OK;
}

void midi_usb_stop(void)
{
    if (!s_running) return;
    tinyusb_driver_uninstall();
    s_running = false;
    ESP_LOGI(TAG, "USB MIDI device stopped");
}

bool midi_usb_running(void) { return s_running; }

bool midi_usb_mounted(void)
{
    return s_running && tud_midi_mounted();
}

bool midi_usb_send_rt(uint8_t status)
{
    if (!midi_usb_mounted()) return false;
    /* A System Real Time message is a single byte and may appear anywhere in
     * the stream: tud_midi_stream_write packs it into the 4-byte USB-MIDI
     * packet with the right Code Index Number by itself. */
    return tud_midi_stream_write(0, &status, 1) == 1;
}
