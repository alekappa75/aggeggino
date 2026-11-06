/* midi_usb — MIDI output over USB-C, as a class-compliant device (TinyUSB).
 *
 * The device is here; wiring it to the clock engine is a separate step and is
 * not done. That split was deliberate: the first thing worth knowing is
 * whether hosts recognise the device and what it costs in memory, and a sink
 * that does not transmit answers that question on its own. One that transmits
 * mixes two questions together.
 *
 * COST TO KNOW BEFORE ENABLING IT: bringing up TinyUSB takes away the
 * USB-Serial-JTAG console. The log remains available on Port B.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Maximum length of the names shown by the host, terminator excluded.
 *
 * USB strings are UTF-16 and the descriptor carries a one-byte length, so the
 * protocol limit is 126 characters. 32 is our own choice: roomy for any
 * sensible name, and convenient to keep in NVS. */
#define MIDI_USB_NAME_MAX   32

typedef struct {
    /* PRODUCT name — what the host shows in its list of MIDI devices.
     * NULL or empty selects "aggeggino". */
    const char *product;
    /* Manufacturer. NULL selects "aggeggino". */
    const char *manufacturer;
    /* Name of the MIDI port inside the device. NULL selects "<product> clock".
     * Some hosts display it, some ignore it. */
    const char *port_name;
    /* Serial number. NULL derives one from the MAC, so two units on the same
     * machine stay distinguishable. */
    const char *serial;

    int  task_core;        /* -1 = component default (CPU1) */
    int  task_prio;        /* 0  = component default (5)    */
    int  task_stack;       /* 0  = component default (4096) */
} midi_usb_cfg_t;

/* Start the USB device. The strings are COPIED, so the caller need not keep
 * them alive — USB descriptors must stay valid for the whole session, and this
 * component owns them. */
esp_err_t midi_usb_start(const midi_usb_cfg_t *cfg);

/* Stop the device and release the driver. */
void midi_usb_stop(void);

/* The device is started. This does not imply a host has enumerated it. */
bool midi_usb_running(void);

/* A host has enumerated and configured the MIDI interface: only from here on
 * does transmitting mean anything. */
bool midi_usb_mounted(void);

/* Send a System Real Time byte (0xF8 clock, 0xFA start, 0xFC stop).
 * Returns false when the device is not mounted. */
bool midi_usb_send_rt(uint8_t status);
