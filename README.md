# AGGEGGINO

/addʒedˈdʒino/ · *n.* [It., dim. of **aggeggio**: gizmo]
A small device persuading two machines to share a downbeat.

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

## Status

Working and tested on hardware against 2× Pioneer XDJ-700, Ableton Live and
Push 3 standalone. This is a personal project, not a product — expect rough
edges, and read the protocol notes before relying on it in a live situation.

The firmware sources still use `djbridge` as an internal name throughout
components, log tags and build files. It is the same thing.

## Hardware

- **M5Stack CoreS3-SE** (ESP32-S3) — touch display, PSRAM, I²S audio
- **W5500 Ethernet module** for the DJ Link side (wired, as the players expect)
- built-in WiFi for the Ableton Link side
- CV pulse output on a GPIO, MIDI DIN and USB-MIDI

Built with ESP-IDF 5.5.4 and LVGL 9.

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

Built with ESP-IDF 5.5.4.

```
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Managed dependencies (LVGL, `esp_abl_link`, the ILI9341 driver, TinyUSB, mDNS)
are pulled in by the component manager from the `idf_component.yml` manifests.
WIZnet's ioLibrary is committed rather than fetched, and
[`components/w5500_net/VENDORED.md`](components/w5500_net/VENDORED.md) explains
why: this code works around a defect in the library's own `recv()`, so the
revision matters.

### The development build

`DJB_DEVELOPER` in the top-level `CMakeLists.txt` is **0** here, which is the
production build. At 1 it adds the Developer page in the settings, screenshots
to the SD card, and the CPU / SPI-bus / beat-jitter / stack metrics, together
with the FreeRTOS instrumentation that feeds them.

Two things worth knowing before switching it on. The instrumentation costs work
at **every context switch**, so phase figures measured with it are not the
figures of the firmware people run — measure on a production build. And
`sdkconfig` is generated once and then kept, so after changing the switch you
have to delete `sdkconfig` (or run `idf.py fullclean`) for the new defaults to
be read; otherwise the build silently keeps the old configuration and the switch
appears to do nothing.

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
