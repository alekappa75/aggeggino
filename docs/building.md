# Building AGGEGGINO

[← documentation index](README.md)

For whoever compiles the firmware. If you only want to *use* a device somebody
already flashed, you want [**Getting started**](getting-started.md) instead.

---

## 1. The toolchain

**ESP-IDF 5.5.4.** The version matters more than usual here: the project pins
managed components with caret ranges, and the build has been exercised on this
one.

Espressif's own instructions are the ones to follow, because they change and
this page would go stale:
[ESP-IDF Get Started](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/index.html).
In outline:

```
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Then, **in every new shell**:

```
. $HOME/esp/esp-idf/export.sh
```

Forgetting that is the commonest first failure: `idf.py` is not found, or a
different IDF on the machine answers instead. `idf.py --version` tells you
which one you have.

---

## 2. Clone and build

```
git clone https://github.com/alekappa75/aggeggino.git
cd aggeggino
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` is `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` or `/dev/ttyUSB*` on
Linux, `COM*` on Windows.

### What comes down by itself

Managed components arrive from the component manager on the first
`set-target` or `build`, driven by the `idf_component.yml` manifests:

| component | manifest | what it brings |
|---|---|---|
| `ui` | LVGL 9, `esp_lvgl_port` | the graphics |
| `display` | `esp_lcd_ili9341` | the panel driver |
| `linkbridge` | `docwilco/esp_abl_link` | Ableton Link, and ASIO with it |
| `midi_usb` | `esp_tinyusb` | USB-MIDI |
| `midi_network` | `espressif/mdns` | the service announcement |
| `leds` | `espressif/led_strip` | the Bottom3 LEDs |

They land in `managed_components/`, which is not in the repository and does not
belong there.

### What is already in the repository

**WIZnet's ioLibrary**, under `components/w5500_net/ioLibrary/`. It is
committed rather than fetched, and
[`VENDORED.md`](../components/w5500_net/VENDORED.md) gives the reason: this
component deliberately works around a defect in the library's own `recv()`, and
a shallow clone of upstream's default branch would move that premise with
nothing to show for it.

`fetch_iolibrary.sh` (and its PowerShell twin) are kept for *updating* it, not
as a build step. You do not need to run them.

---

## 3. `DJB_DEVELOPER`, and the trap that goes with it

One switch at the top of `CMakeLists.txt`, line 24.

| | |
|---|---|
| **0** | the production build. What the repository ships. |
| **1** | the development build. |

At 1 you get the Developer page in the settings, screenshots to the SD card,
and the CPU / SPI-bus / beat-jitter / stack metrics — together with the
FreeRTOS instrumentation that feeds them.

It moves three sdkconfig options with it, which is the whole reason it is one
switch and not several:

| option | dev | prod |
|---|---|---|
| `CONFIG_LOG_DEFAULT_LEVEL` | `INFO` | `WARN` |
| `CONFIG_FREERTOS_USE_TRACE_FACILITY` | `y` | `n` |
| `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` | `y` | `n` |

Separate switches would have let you turn the metrics off while the
instrumentation that feeds them stayed on — paying at every context switch to
collect numbers nobody reads, which is the worst of both.

### ⚠ After changing it, `sdkconfig` must be DELETED

**`sdkconfig` is generated once, from the `.defaults` files, and then kept.**
The defaults are read when it does not exist. Change the switch and rebuild
without removing it, and the build quietly keeps the old configuration: the
Developer menu does not appear, or does not go away, and the switch looks
inert.

```
rm sdkconfig
idf.py reconfigure
idf.py build
```

**`idf.py fullclean` does not do this.** It empties the build directory;
Espressif's documentation says in as many words that the project configuration
is not deleted. It is a reasonable thing to assume and it is wrong, which is
why it is written here.

### Before you delete it: what you are about to lose

Every change ever made with `menuconfig` lives in `sdkconfig` and reaches no
file in the repository. Deleting it throws all of them away without a word.

`tools/sdkconfig_diff.py` exists for exactly this moment:

```
python3 tools/sdkconfig_diff.py --save      # before deleting
rm sdkconfig && idf.py reconfigure
python3 tools/sdkconfig_diff.py --compare   # after
```

It separates the differences `DJB_DEVELOPER` is *supposed* to cause from
settings that would have been silently lost.

**If you want a setting to survive**, it belongs in `sdkconfig.defaults` (or in
`.dev` / `.prod` when it should follow the switch), not in `menuconfig`.

### Measuring timing? Measure on a production build

The instrumentation costs work at **every context switch**. Phase figures taken
with `DJB_DEVELOPER 1` are not the figures of the firmware people run. This is
a device whose point is sub-millisecond accuracy; measuring the handicapped
build and believing the number is the mistake worth naming.

---

## 4. Reading the logs, and how Port B's two pins are allocated

**The serial console is on Port B, not on USB-C.**

Once USB-C goes to TinyUSB for class-compliant MIDI, the USB-Serial-JTAG
console cannot coexist with it — the peripheral is claimed. It is therefore
switched off explicitly (`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`), because
leaving it on would give a conflict at start-up instead of a clear build error.

The console is UART0 at 115200, moved off the default pins (GPIO 43/44, which
sit on the M-Bus under the stack) onto Port B. **It is output-only, on purpose**:
nothing is ever typed into this console, so the receive pin is not spent on it.

| Port B | GPIO | what it is |
|---|---|---|
| pin 2 | 9 | console **TX** — the log comes out here |
| pin 1 | 8 | **CV pulse output**, taken from the console's nominal RX |

The console's RX is configured in `sdkconfig.defaults` because the setting
comes in pairs, but `cv_pin_init()` in `main.c` reconfigures the pin as a GPIO
output and keeps it. Its comment says so in one line: *"Takes the pin from the
console and makes it an output."* Two functions out of one Grove connector,
deliberately.

> **⚠ So, when wiring a serial adapter: GND and the device's TX only.**
>
> ```
> adapter GND  ──  Port B GND
> adapter RX   ──  Port B pin 2   (GPIO 9)
> adapter TX   ──  NOT CONNECTED  (pin 1 is an output)
> ```
>
> Wiring the adapter's TX to pin 1 out of habit would put two outputs on one
> wire. Nothing in the firmware is confused by it — the pin is an output by
> design — but the cable would be a mistake, and with CV enabled the two would
> push against each other on every beat.

**UART0 and not UART1**, incidentally, because UART1 belongs to MIDI DIN on
Port C.

> **One inference to be aware of.** That G9 carries the TX, rather than G8, is
> *deduced* from how the M5Stack library addresses Port C on the base Core — RX
> on the lower GPIO, TX on the higher — and is not a figure read from a
> datasheet. The note is in `sdkconfig.defaults` beside the setting. **If
> nothing arrives on Port B, the first thing to try is swapping the two
> numbers** — and if they do turn out to be swapped, the CV output and the log
> swap pins with them.

---

## 5. The project, in outline

```
CMakeLists.txt            DJB_DEVELOPER and the version
sdkconfig.defaults        the substantive configuration, commented
sdkconfig.defaults.dev    the three options that follow the switch
sdkconfig.defaults.prod
main/main.c               the network state machine, drift compensation,
                          the beat engine, the quiet window on the SPI bus
components/               twenty-three of them
tools/sdkconfig_diff.py
docs/                     this manual and the protocol notes
```

The components worth knowing first:

| | |
|---|---|
| `djlink` | parsing **and construction** of keep-alive, beat and status packets |
| `linkbridge` | a thin wrapper over the official `abl_link` |
| `dbserver` | the TCP metadata protocol |
| `w5500_net` | the Ethernet chip, its sockets, auto-ip and DHCP |
| `ui` / `settings_ui` | the dashboard and the menus |
| `display` | the panel, the quiet window, the text masks |
| `bsp_cores3` | power rails, I²C, touch, the shared SPI bus |

**The one thing to understand before changing anything that touches the bus:**
the W5500, the display and the SD card share one SPI bus, and the MISO pin is
also the display's DC. Beat packets are the players' phase reference, and
anything that holds the bus delays them. There is a *quiet window* around each
beat that every consumer of the bus has to honour, the network drains included.
[§4.3 of the protocol notes](protocol-notes.md#43-spi-bus-contention--the-main-problem)
has the measurements.

**The other rule for new code:** a setting that can change in quick succession
must not write to NVS on every change. A flash write stalls the instruction
cache on **both cores** for as long as it takes, which truncates whatever the
audio task is playing. The phase offset is therefore written 1.5 s after the
last adjustment, and the custom OSC profile on leaving its menu; anything new
that cycles needs the same treatment.

---

## 6. When the build fails

**`idf.py: command not found`** — `export.sh` was not sourced in this shell.

**A managed component will not download** — the component manager needs network
access. Behind a proxy, ESP-IDF honours the usual environment variables.

**`WIZnet ioLibrary not found`** — the `ioLibrary/` directory is missing, which
should not happen from a clone. `components/w5500_net/fetch_iolibrary.sh`
fetches it again.

**The switch appears to do nothing** — `sdkconfig` was not deleted. §3 above.

**Something built last week and does not now** — a `reconfigure` recomputes the
component dependency graph, and a header that had been arriving transitively
can stop. That is not a regression, it is the graph being recomputed correctly:
the component is missing a `REQUIRES`. `esp_timer` is the usual culprit.

---

## 7. Where the serial output comes from

With `DJB_DEVELOPER 1`, a heartbeat every 30 seconds plus per-subsystem
metrics. With it at 0, the log level is compiled down to warnings, so those
lines are not merely hidden — they are **not in the binary**. 132 `ESP_LOGI`
calls not generated, and every line on the UART at 115200 blocks its caller for
about 9 ms.

[**When something is wrong**](troubleshooting.md#reading-the-logs) explains how
to read the heartbeat.
