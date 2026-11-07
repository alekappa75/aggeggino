# AGGEGGINO

/addʒedˈdʒino/ · *n.* [It., dim. of **aggeggio**: gizmo]
A small device persuading two machines to share a downbeat.

<img src="docs/img/aggeggino.png" alt="AGGEGGINO: an M5Stack CoreS3 stacked on an Ethernet base, its screen showing one shared tempo of 135 BPM" width="620">

---

**Bidirectional bridge between Pioneer Pro DJ Link and Ableton Link, running on
an ESP32-S3 (M5Stack CoreS3-SE). Acts as a real tempo master on both sides, with
MIDI, USB-MIDI, CV and OSC outputs.**

---

## What it does

![How AGGEGGINO sits between the two networks](docs/img/networks.svg)

AGGEGGINO lets Pioneer CDJ/XDJ players and Ableton Link software share a single
timeline, in both directions. It joins the DJ Link network over wired Ethernet
and the Ableton Link session over WiFi — the two protocols normally live on
separate networks — and can either follow the current master or become one
itself.

Tempo and phase are also available as MIDI clock, USB-MIDI, analog CV pulses and
OSC, so the same timeline can drive hardware sequencers, visuals and lighting.

Most existing work in this area reads the DJ Link network and stops there.
Transmitting, and being *accepted* as tempo master by real players, is a
considerably stricter requirement: it forces you to measure things you can
otherwise get away with guessing.

<table>
<tr>
<td width="50%"><img src="docs/img/dashboard-idle.png" alt="The dashboard with no bridge running: two independent tempos"></td>
<td width="50%"><img src="docs/img/dashboard-bridging.png" alt="The dashboard with a bridge running: one shared tempo"></td>
</tr>
<tr>
<td><b>No bridge.</b> The players at 128.5, the Link session at 122.0. Two
clocks, not talking.</td>
<td><b>Bridging.</b> One tempo, one phase, and the outputs that carry it —
here MIDI, OSC and CV at once, with a −6 ms offset on the OSC.</td>
</tr>
</table>

Screenshots taken by the device itself: it writes its own screen to the SD
card, which is how the pictures in [the manual](docs/) are made.

## Status

Working and tested on hardware against 2× Pioneer XDJ-700, Ableton Live and
Push 3 standalone. This is a personal project, not a product — expect rough
edges, and read the protocol notes before relying on it in a live situation.

The firmware sources still use `djbridge` as an internal name throughout
components, log tags and build files. It is the same thing.

## Hardware

**Three M5Stack modules, stacked** — which is what the picture above shows, from
the top down:

| | what it brings |
|---|---|
| **CoreS3-SE** | the ESP32-S3 itself: touch display, PSRAM, I²S audio, USB-C |
| **LAN module** (W5500) | wired Ethernet for the DJ Link side, RJ45 and a 9–24 V DC input |
| **LED base** | the battery, and the ten status LEDs in two strips down the sides |

WiFi for the Ableton Link side is built into the Core; nothing is added for it.

The two protocols genuinely live on separate networks — the players on the
wire, Link over the air — so the wired module is not an optional extra, it is
half of what the bridge is.

**Optional:** an **M5Stack Unit MIDI (U187)** on Port C, for MIDI DIN out and
for the audible click on the SAM2695. Everything else — USB-MIDI, network MIDI,
CV, OSC — works without it.

Built with ESP-IDF 5.5.4 and LVGL 9. [**docs/building.md**](docs/building.md)
has the toolchain and the build.

## Features

**Pioneer Pro DJ Link**

- Virtual CDJ: keep-alive, presence, device number claim (1–4) with a staged
  negotiation, and master takeover through the status channel
- reads beat packets and player status; transmits both when acting as master
- adaptive transmission rate (~150 ms at rest, ~64 ms while tempo is moving),
  matching what real players do — a fixed rate adds tens of milliseconds of
  latency for every player following you
- track metadata (title, artist, tempo) over the `dbserver` protocol
- answers ONC-RPC portmapper queries, which is what stops players from
  displaying `E-8309 Link Access Error`

**Ableton Link**

- full peer participation through Ableton's official `abl_link` library
- can join a session without imposing its own tempo

**Clock outputs**

- MIDI clock (DIN and USB-MIDI)
- analog CV pulses, configurable division (1/2/4/24 PPQN), width and polarity
- OSC, with generic and Resolume presets

**Timing**

- online crystal-drift compensation: the ESP32 and the players do not share a
  clock, and a stored calibration constant is not enough — the crystal warms up
  and shifts by roughly 20 ppm in the first five minutes
- a quiet window around beat transmission keeps display and network traffic off
  the shared SPI bus, so the packet that carries phase is not delayed by a
  screen refresh

## Documentation

The manual lives in [`docs/`](docs/) and is versioned with the firmware it
describes — a `git checkout` of a tag gives you both, matching.

- [**Getting started**](docs/getting-started.md) — what you need, the two
  networks, and the first bridge from scratch
- [**The dashboard**](docs/dashboard.md) — the main screen, the three buttons
  under the glass, the screen lock
- [**Settings**](docs/settings.md) — the complete menu tree
- [**Clock outputs**](docs/outputs.md) — MIDI, CV, OSC, and the custom OSC
  profile
- [**The SD card**](docs/sd-card.md) — playlists as CSV, formatting, screenshots
- [**When something is wrong**](docs/troubleshooting.md) — by symptom

## Protocol documentation

The DJ Link implementation is a clean-room effort based on direct observation of
real hardware. The notes are in this repository, in
[English](docs/protocol-notes.md) and in the
[Italian](docs/protocol-notes.it.md) they were measured and written in.

Every non-obvious claim in them is marked with its evidence:
**[M]** measured on real hardware, **[D]** deduced from measurements with the
reasoning spelled out, **[I]** an unverified hypothesis, **[E]** from an external
source. There is also a section listing what is still unknown.

This is deliberate. An earlier revision stated some plausible guesses as
established fact, and that led to wrong design decisions. A declared uncertainty
is worth more than an invented certainty.

## Building

ESP-IDF 5.5.4.

```
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

[**docs/building.md**](docs/building.md) has the rest, and three things in it
are worth knowing before you start:

- **`DJB_DEVELOPER`** in the top-level `CMakeLists.txt` is **0** here, the
  production build. At 1 it adds the Developer page, screenshots to the card
  and the CPU / SPI / beat-jitter metrics — and the FreeRTOS instrumentation
  that feeds them, which costs work at every context switch. Timing measured
  with it on is not the timing of the firmware people run.
- **After changing that switch, `sdkconfig` has to be deleted.** It is
  generated once from the `.defaults` files and then kept, so otherwise the
  build quietly keeps the old configuration and the switch looks inert.
  `idf.py fullclean` does **not** do this — it empties the build directory and
  leaves the project configuration alone.
- **The serial console is on Port B, not USB-C** (USB-C belongs to TinyUSB for
  MIDI). Connect only GND and TX: the console's RX pin is also the CV output,
  so an adapter's TX wired to it puts two outputs on one line.

Managed dependencies (LVGL, `esp_abl_link`, the ILI9341 driver, TinyUSB, mDNS)
come down by themselves from the `idf_component.yml` manifests. WIZnet's
ioLibrary is committed rather than fetched, and
[`components/w5500_net/VENDORED.md`](components/w5500_net/VENDORED.md) explains
why: this code works around a defect in the library's own `recv()`, so the
revision matters.

## The name

*Aggeggio* is Italian for a contraption, a gizmo — a thing that clearly does
something, though not always something you can name. *Aggeggino* is its
diminutive, and carries the affection that comes with it.

## License

**GPL-3.0-or-later.** See [LICENSE](LICENSE).

This is not a free choice: AGGEGGINO links against Ableton Link, which is
[dual licensed](https://github.com/Ableton/link/blob/master/LICENSE.md) under
GPLv2-or-later and a separate proprietary license. Distributing a combined work
that includes it therefore requires a GPL-compatible license. GPLv3 is used
rather than GPLv2 because ESP-IDF is Apache-2.0, which is compatible with GPLv3
but not with GPLv2.

If you want to build something proprietary on top of this, the Ableton Link
license is the thing to sort out first — contact `link-devs@ableton.com`.

## Acknowledgements

- **Ableton** for Link and for publishing it as open source.
- **Deep Symmetry** (`beat-link`, `dysentery`) for years of public work on the
  DJ Link protocol. Their implementations were used for orientation only, never
  copied — and in at least one case this project deliberately diverges from
  their choices, for reasons documented in the protocol notes.

## Trademarks

Pioneer DJ, CDJ and XDJ are trademarks of AlphaTheta Corporation. Ableton and
Ableton Link are trademarks of Ableton AG. This project is not affiliated with,
endorsed by or supported by either company.
