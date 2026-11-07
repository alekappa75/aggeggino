# When something is wrong

[← documentation index](README.md)

Organised by what you see. Where a cause was measured rather than guessed, it
says so, and points at the protocol notes.

---

## The players show `E-8309 Link Access Error`

**This is not a fault in your setup, and this firmware already handles it.** If
you are seeing it, something is stopping the answer from getting through.

What happens: when a device declares in its status that it is playing from a USB
slot, the players want to read its tracks, and to do that they try to **mount
its filesystem over NFS**. They send an ONC-RPC `PORTMAP GETPORT` on UDP port
111 asking where the `MOUNT` service answers. If the device says nothing, they
retry **every second, forever**, and after a while put the error on screen.

The fix is not to implement NFS. It is to **say no**, in a formally correct way:
a valid RPC reply with port 0 means "that program is not registered here", and
that is enough. This firmware answers `GETPORT` and, as a courtesy, `NULL`.

So if the error persists, something is blocking UDP 111 — a firewall on a
machine in between, or a managed switch filtering it.

The general lesson, which holds beyond this one symptom: **on a DJ Link network
silence is not neutral.** The players read an absent answer as a fault and
insist. An explicit refusal closes the matter.
[§1.11 of the protocol notes](protocol-notes.md#111-the-e-8309-link-access-error-and-the-rpc-portmapper).

---

## Ableton Link sees the peers but the tempo never agrees

Almost always one of two things, and both are outside this device.

**Client isolation on the access point.** Link's discovery is multicast and gets
through; its measurement phase is **unicast between peers** and does not. The
peers find each other and never merge, which looks exactly like a working Link
that refuses to sync. Turn client isolation off.

**A firewall dropping inbound UDP.** On macOS, *Little Snitch* blocks inbound
UDP to unsigned applications **silently, with no prompt at all** — and it does
this to Ableton Live as much as anything else. The confusing part: traffic from
a terminal still gets through, so `tcpdump` sees every packet while the
application sees none. Either add an explicit inbound rule for the ports in use
(DJ Link 50000–50002, Link 20808 plus the measurement port) or sign the bundle.

---

## The MASTER badge blinks red and the bridge stops

The master negotiation failed. We asked for the role and did not get it inside
the timeout.

It is a negotiation conducted **through the status packets**, not a declaration:
we ask, the current master yields by naming its successor in its own status, and
only then do we take over. If the current master never yields, nothing happens.

Things to check:

- **Is another virtual player on the network?** beat-link, rekordbox in a mode
  that claims a number, or a second bridge. Two of us asking for the same thing
  is the common case.
- **Are all four slots taken?** Being accepted as tempo master requires a real
  number, 1–4. With four players connected there is none left. `Settings > LAN >
  Status` shows what we actually hold.
- **Is `DJ Link ID` set to `OBSERVER`?** Then we never ask at all, by design.
  `Settings > LAN > DJ Link ID`.

[§1.8 of the protocol notes](protocol-notes.md#18-master-election-device-number-takeover).

---

## A white LED blinking fast on the left-hand side

An identity conflict: every number was taken, including the virtual ones in the
7–15 range, so we are using one that somebody else is also announcing.

Something else on the network is claiming a virtual player number. Turn off the
other virtual player — beat-link, rekordbox, or another bridge — and it clears
by itself.

---

## No title or artist in the playlist, or in `{title}` over OSC

The track id is there and the names are empty. The line is still correct; what
failed is a separate query.

Metadata comes from the player over Pioneer's `dbserver` protocol, a TCP
conversation of its own. Two constraints, both measured, and neither documented
anywhere else as far as we know:

1. **The device number we present must be 1–4.** With a high number the player
   closes the connection without replying.
2. **It must be a number actually announced on the network**, not merely free.
   Posing as a free-but-unannounced number, the player accepts the setup and
   answers "0 items" — it does not know us and has no menu for us.

So with all four slots taken by real players, metadata may be unavailable while
everything else works normally. The firmware borrows the number of a player that
really is present when it can, and says in the log when it cannot:

```
[now] no usable player number (1-4) to present to device 2: no metadata for now.
      The bridge works all the same.
```

It retries every 30 seconds. Switching a player off frees a number and it
resumes on its own, without needing the track to change.

A **delay of a second or so** after a track change is normal: the query runs
only when the master device or the track id changes.

[§1.10 of the protocol notes](protocol-notes.md#110-metadata-for-the-playing-track--the-dbserver-protocol).

---

## The LAN badge is red

The W5500 is not answering. Its version register should read `0x04` on a live
chip.

Check the module is seated and powered. The firmware notices the chip
reappearing and reconfigures by itself, so you do not have to restart:

```
W5500 back: reconfiguring and reopening sockets
DJ Link restored
```

**A grey LAN badge is different**: the chip is fine and the cable is unplugged.

---

## No DHCP address

Not a fault, and on a DJ booth network it is the normal case. After three
seconds without an answer the device assigns itself a `169.254.x.x` address, as
RFC 3927 describes and as the players themselves do. Everything works.

```
dhcp: no DHCP server answered in 3000 ms (sent 3, heard 0): auto-ip from here
w5500: auto-ip: 169.254.179.175
```

`Settings > LAN > Status` shows which of the two happened.

---

## An AUTH badge in red on the dashboard

The WiFi network was found and refused our key. It is the password, not the
signal — the distinction matters because the two call for opposite responses:
one is a setting to fix, the other a place to move.

`Settings > WiFi > Choose network`, pick it again, retype the key.

---

## The card will not mount, or the SD badge is red

A red badge means a card is in the slot and unusable. `sdmmc_card_init failed`
in the log says the same.

Try another card first — it is the likeliest answer by a wide margin. If several
cards fail, `Settings > SD card > Format card` will rewrite the filesystem, and
that entry is disabled while a bridge is running.

Insertion and removal are noticed as they happen, so a card put in later does
not need a restart.

---

## The clock is going at double speed, or at a crawl

`Settings > Clock > Sink > CV > Division`. At `24 PPQN` into an input expecting
one pulse per quarter you get twenty-four times the clock; at `1 PPQN` into
something expecting twenty-four you get a twenty-fourth.

That is why Division is the first entry on that page: the rest are refinements,
and this one is the mistake with a loud result. `1 PPQN` is the eurorack
convention.

---

## The CV output has stopped altogether, mid-set

Check **Pulse width** against the tempo. A pulse wider than the beat period
would never return low: the pin stays high and the clock simply vanishes.

The firmware clamps the width to **45% of the period** precisely so this cannot
happen, so seeing it would mean something else is wrong — but the symptom is
worth knowing, because a clock that disappears without a trace is otherwise
unreadable.

---

## Resolume pins at maximum tempo

The tempo has to be **normalised to 0..1** over the 20–500 BPM range: Resolume's
tempo controller is an interface parameter, not a beat count. Sending `132.0`
pins it at the top.

The `RESOLUME` preset does this for you. If you are building a custom profile,
use `{bpm-rel}` and not `{bpm}`.

---

## An OSC message is going nowhere

OSC is fire-and-forget: a packet with a wrong address leaves perfectly happily
and nobody answers. There is no error to see, which is why the custom profile
makes you **choose** placeholders from the keyboard instead of typing them.

Things to check, in order:

- **Host and port.** `Settings > Clock > Sink > OSC`. The default host is
  `172.16.42.100`, which is the first address the SoftAP hands out — right if
  the receiver is the first thing to join, wrong otherwise.
- **Is the OSC badge green?** Green means packets are leaving. Grey means
  enabled with nothing to send, which means no bridge is running.
- **The path, character by character.** This is the one that costs an evening.

---

## A beep comes out broken while cycling an option quickly

Fixed, but worth recording in case it reappears in new code.

A write to flash **stalls the instruction cache on both cores** for as long as
it takes, which truncates whatever the audio task was playing. Writing on every
tap of a cycling option therefore broke the beep.

Settings that can change in quick succession no longer write immediately: the
custom OSC profile is saved on leaving its menu, and the phase offset 1.5 s
after you stop adjusting it. If you add a new setting that cycles, defer its
write the same way.

---

## The long press takes a picture instead of locking

Intended, and it only happens when you have asked for it: `Save screenshots`
has to be switched on, and that entry only exists in a development build. In a
development build with it off — which is how one is flashed — the long press
locks the screen as usual.

With it on, the screenshot borrows the lock's gesture: same zone (`x` 125–195,
`y ≥ 248`), same 1.5 s threshold, recognised first and consuming the gesture.

The reasoning is that the two are never wanted in the same hour. Screenshots
exist to produce the pictures in this manual — something done at a desk, with
the device in hand and a development build flashed. The lock exists for a
booth, where nobody is compiling anything. Giving the screenshot its own
gesture would have meant one more thing to learn, for a feature that is not in
the firmware people run.

So with `Settings > System > Developer > Save screenshots` on, a long press
photographs — and if the screen was already locked, the circle will not unlock
it. Turn screenshots off to get the lock back.

Production builds have neither the Developer menu nor the substitution: there
the long press is the lock, always.

---

## Something changed in the settings and nothing happened

If you changed `DJB_DEVELOPER` in `CMakeLists.txt` and the build behaves as
before: `sdkconfig` is generated **once** and then kept, so the new defaults are
never read and the switch appears inert.

**`idf.py fullclean` does not fix this.** It empties the build directory and
leaves the project configuration alone — the documentation says so in as many
words. The file itself has to go:

```
rm sdkconfig
idf.py reconfigure
```

`tools/sdkconfig_diff.py` compares a saved `sdkconfig` against the current one
and distinguishes differences that `DJB_DEVELOPER` is supposed to cause from
settings that would be silently lost.

---

## Reading the logs

Serial logs come out of **Port B** (the USB serial console is disabled, because
it would clash with the MIDI class exposed on USB-C through TinyUSB).

A heartbeat line every 30 seconds carries the overall state:

```
[hb] up=60s master=0(Mh=4) dj=130.00 link=120.00 sync=OFF off=+0us drift=wait
     peers=0 W5500=0x04 heap=30323
```

| field | |
|---|---|
| `master` | which device is master; `Mh` is the handoff field |
| `dj` / `link` | the two tempos |
| `drift` | `wait` while the compensator is still measuring |
| `peers` | Ableton Link peers |
| `W5500` | the version register; `0x04` is a live chip |
| `heap` | free internal heap, the scarce resource on this board |

In production builds the log level is compiled down to warnings, so the
heartbeat and the metrics are not merely hidden — they are not in the binary.
That is 132 `ESP_LOGI` calls removed, and every line on the UART at 115200
blocks its caller for about 9 ms.
