# Getting started

[← documentation index](README.md)

For whoever has a device in hand: what to connect it to, and how to get the
first bridge running. Compiling and flashing the firmware is a different job
with a different audience, and it has its own page —
[**Building**](building.md).

## What you need

**The device: three M5Stack modules, stacked.** From the top down:

| | what it brings |
|---|---|
| **CoreS3-SE** | the ESP32-S3: touch display, PSRAM, I²S audio, USB-C, and the WiFi used for Ableton Link |
| **LAN module** (W5500) | wired Ethernet for the DJ Link side — RJ45, and a 9–24 V DC input that also powers the stack |
| **LED base** | the battery, and the ten status LEDs in two strips down the sides of the unit |

The LAN module is not an optional extra: the players speak DJ Link over the
wire and nothing else will reach them.

The LED base is, strictly, optional — the firmware runs without it — but it is
what lets you read the device's state from across a room with the screen
unreadable, which is the state a bridge on a shelf is usually in. See
[what the LEDs say](dashboard.md#what-the-leds-say).

**Also optional:** an **M5Stack Unit MIDI (U187)** on Port C, for MIDI DIN out
and the audible click on the SAM2695. USB-MIDI, network MIDI, CV and OSC all
work without it.

**The two networks.** This is the part people get wrong, so it comes first.

DJ Link and Ableton Link normally do not live on the same network, and
AGGEGGINO does not try to pretend otherwise. It sits on **both**:

- **DJ Link over the wire.** The players expect wired Ethernet, and that is what
  the W5500 is for. No switch is needed for two players and the bridge if they
  are already on a switch; if they are not, you need one.
- **Ableton Link over WiFi.** Either AGGEGGINO makes its own access point and
  your laptop or Push joins it, or AGGEGGINO joins a network you already have.

A Push 3 standalone is WiFi-only. XDJ-700s speak DJ Link on the wire. In a
mixed setup, the two protocols are genuinely on separate networks and the
bridge has to be on both at once. That is the whole reason this thing exists.

## Getting the firmware onto it

Covered in [**Building**](building.md) — the toolchain, the build, the
`DJB_DEVELOPER` switch and the `sdkconfig` trap that goes with it, and where
the serial console comes out.

Everything below assumes a flashed device.

## First run, step by step

### 1. Power on and look at the LAN badge

The status strip at the bottom of the dashboard always carries a **LAN** badge.
Its colour is the first thing to check:

| LAN badge | meaning |
|---|---|
| red | the W5500 is not answering |
| grey | the chip is fine, the cable is unplugged |
| green | the chip answered and the cable is in |

Those three are the whole vocabulary, and the badge deliberately knows nothing
beyond them: not whether an address was obtained, and not whether any player
is out there. Green is the physical chain working, no more than that.

For the address, `Settings > LAN > Status` gives it and how it was obtained.
There may not be a DHCP server on a DJ booth network, and there usually is
not: after three seconds without an answer the device assigns itself a
`169.254.x.x` address, which is exactly what the players do, and everything
works. That is not a failure, and the log says so plainly:

```
I (7257) dhcp: no DHCP server answered in 3000 ms (sent 3, heard 0): auto-ip from here, and no further asking
I (7668) w5500: auto-ip: 169.254.179.175
```

### 2. Set up the WiFi side

`Settings > WiFi > Mode` switches between **SoftAP** and **Client**.

**SoftAP** is the quicker route and needs nothing at the other end. AGGEGGINO
becomes the access point; your laptop or Push joins it.

- default network name: `aggeggino`
- default password: `m5stack1`
- the device is at `172.16.42.1`, and hands out `.100` to `.150`

Both the name and the password can be changed under `Settings > WiFi`, and
should be if the device ever leaves your own bench.

**Client** joins an existing network. `Settings > WiFi > Choose network` scans
and lists what it finds; pick one and type the key.

One thing to check on a network you did not set up: **client isolation must be
off**. Ableton Link's measurement phase is unicast between peers. With client
isolation on, the multicast discovery gets through and the peers see each other,
but they never merge — so Link appears to work and the tempo never agrees.

### 3. Check that the players are there

With the cable in and the players on, `Settings > LAN > Status` shows the DJ
Link ID the device took. The dashboard shows the players' beat as four boxes
lighting in turn, and the **MASTER** badge shows which player is the tempo
master.

At this point nothing is bridged yet. The dashboard shows **two** BPM figures,
grey, side by side: `DJ LINK` on the left, `ABLETON` on the right. They are
independent, and that is the point — you can see both before deciding to join
them.

### 4. Turn on a bridge

Two toggles on the dashboard, one at each side, and they are mutually exclusive:

- **left toggle (cyan, arrow down)** — DJ Link → Ableton. The players lead;
  the Link session follows them.
- **right toggle (arrow up)** — Ableton → DJ Link. Ableton leads; the players
  follow, driven through their pitch. **This one** requires AGGEGGINO to become
  the DJ Link tempo master, and that has to be negotiated with the players.

Tap one. The two grey BPMs are replaced by a single large number: the tempo the
two sides now share.

Going left to right — following the players — nothing else happens: a Pioneer
player stays master and the badge keeps its number.

Going the other way, taking the master role is a negotiation rather than a
declaration, and the **MASTER** badge shows how it is going:

| badge | meaning |
|---|---|
| blinking amber | negotiating; the role is not ours yet |
| steady green | master acquired |
| blinking red | the negotiation failed, and the bridge has stopped |
| steady amber | a Pioneer player is master |

### 5. Pick up the tempo somewhere else

Nothing more is needed for the bridge itself. Everything under
`Settings > Clock > Sink` is about getting that tempo out as something else —
MIDI clock, CV pulses, OSC — and has its own page:
[**Clock outputs**](outputs.md).

## What to do if it does not work

[**When something is wrong**](troubleshooting.md) is organised by symptom.
Two things worth knowing before you start looking anywhere else:

**`E-8309 Link Access Error` on the players' displays** is not a fault in your
setup. It has a specific cause and this firmware already answers it; if you see
it, the explanation is in the troubleshooting page.

**Ableton Link not merging** is almost always client isolation on the access
point, or a firewall on the computer silently dropping inbound UDP. On macOS,
Little Snitch blocks inbound UDP to unsigned applications **without showing any
warning at all** — and traffic from a terminal still gets through, so a
`tcpdump` sees everything while the application sees nothing.
