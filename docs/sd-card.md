# The SD card

[← documentation index](README.md)

A microSD slot, used for two things: a record of what was played, and — in
development builds — screenshots.

Everything here is optional. With no card in the slot the **SD** badge is simply
absent from the dashboard and nothing else changes.

---

## Why the card writes when it does

Worth knowing before anything else, because it explains a behaviour that would
otherwise look like a bug.

**The card shares the SPI bus with the display and the W5500.** No amount of
task priority fixes that: a low-priority task holding the bus blocks the beat
exactly as a high-priority one would. Worse, the duration is not ours to
choose — a card doing wear levelling holds the bus for as long as it takes, and
that can reach hundreds of milliseconds against a beat jitter budget under
1.3 ms.

So the lever is **when**, not with what priority:

- never while the DJ Link status cadence is fast, which is to say never while
  the tempo is moving
- as master, only just after a beat has gone out, which leaves a whole beat
  period before the next is due
- otherwise there is no constraint

A playlist line is the least urgent thing this device does. The track has
already been playing for a minute; writing it down a few seconds later changes
nothing. **So a line can appear in the file noticeably later than the moment it
describes, and that is deliberate.** The timestamp in the line is the moment the
track qualified, not the moment it was written.

---

## Playlists

`Settings > SD card > Store playlist`

### What counts as played

A track counts once it has been in **continuous playback** past the threshold
set in `Settings > SD card > Track after`: 15, 30, 60, 90, 120 or 180 seconds.

The same number does a second job: **a silence longer than the threshold closes
the file**, and the next track opens a new one. One number, two meanings, and
deliberately so — the length of pause that means "the set is over" is the same
judgement as the length of play that means "this was really played".

The criterion reads the players' status rather than asking who is master, so it
works in both bridge directions. It is the same idea Beat Link Trigger uses.

### The bookkeeping is per player

Each player has its own stopwatch and remembers the last track it wrote. That
gives the behaviour you would want:

| what you do | what you get |
|---|---|
| pause and resume the same track on the same player | **one** line |
| play A, then B, then A again | **two** lines — B displaced the memory |
| the same track on two players | **two** lines, and the device column says which |

### The files

`PLST0001.CSV`, `PLST0002.CSV`, and so on. On mounting a card the firmware looks
at what is already there and carries on from the highest number, so an existing
card is never overwritten.

A header row, then one line per track:

```
time,player,track_id,title,artist,bpm
00:01:32,1,129,"Bass Drums","Ramadanman",120.00
00:04:48,2,8,"Cyclo","Endlec",120.00
```

| column | |
|---|---|
| `time` | `hh:mm:ss` since power-on, not clock time — there is no battery-backed clock |
| `player` | the DJ Link device number, 1–4 |
| `track_id` | the rekordbox track id |
| `title`, `artist` | quoted. **May be empty** |
| `bpm` | the tempo at the moment the track qualified |

**On the empty title and artist.** They come from the player over the `dbserver`
protocol, a separate TCP conversation, and that query can fail for reasons
nothing to do with the card — most often because it needs to present a device
number that is both 1–4 *and* currently announced, so with all four slots taken
by real players the metadata is unavailable. The line is still written, with the
track id, which is enough to identify the track in rekordbox afterwards. A line
with an id and no title is the system working as designed, not a fault.

Tracks rekordbox has not analysed are a different case: the beat grid is not
trustworthy and the firmware knows it, so the beat-in-bar is ignored for them.

---

## Formatting the card

`Settings > SD card > Format card`. Two touches.

The entry is **disabled** — greyed, no response, no message — in two cases: when
a bridge is running in either direction, and when there is no card in the slot.
No explanation is offered in either, because an entry you cannot touch and a
message telling you so are two ways of saying one thing.

While it runs, the second line of the entry says so. **It takes a while** —
around two minutes is normal, and it depends on the card's size and speed, which
is why no progress figure is promised. The outcome, success or failure, stays on
that second line **until you leave the menu**: an operation that took two
minutes should not have its result vanish on a four-second timer while you are
looking away.

If the format fails, the card is marked unusable in software and the **SD** badge
goes red. Re-inserting it starts over.

The card menu notices insertion and removal as they happen, so you do not have
to leave and come back for the entry to become available.

---

## Screenshots

Development builds only — `DJB_DEVELOPER`. In production neither the Developer
menu nor this code exists.

`Settings > System > Developer > Save screenshots`, then a **long press on the
circle** under the glass, from any screen. A beep confirms the write.

Files are `SCRN0001.BMP`, `SCRN0002.BMP`, and so on, numbered the same way as
the playlists.

> **This borrows the screen lock's gesture, by design.** The long press on the
> circle is also the lock: same zone, same 1.5 s, and the screenshot is
> recognised first. With screenshots on, the long press photographs instead of
> locking — and if the screen is already locked, the circle will not unlock it.
> The two are never wanted in the same hour: this is a desk feature and the
> lock is a booth feature. See [the dashboard](dashboard.md#the-screen-lock).

### The format, and why

320×240, 24-bit BMP, uncompressed. A 54-byte header and the pixels.

**BMP and not PNG** because PNG means zlib and a hundred milliseconds of
squeezing to save space on a card that has plenty. Converting on a computer
costs nothing.

**24-bit and not the 16 that would copy the framebuffer verbatim**, which is
worth recording because it cost an evening. A 16-bit BMP needs bit masks to say
which bits carry which colour; those masks live in a header that exists in three
incompatible lengths, and readers disagree about all of it. The first attempt
here wrote the masks of one variant with the length of another, and macOS
Preview refused the file outright. A 24-bit BMP has none of that — plain
`BITMAPINFOHEADER`, no masks — and everything opens it. The cost is three shifts
per pixel and 230 KB instead of 150.

Three things happen on the way out, and each was a bug before it was a line of
code: the bytes are swapped, because the panel is big-endian and a file is not;
the colour is expanded from 565 to 888 **repeating each field's high bits into
the gap**, or `0x1F` becomes `0xF8` rather than `0xFF`, white comes out slightly
grey and the whole picture reads dark; and the height is written negative, which
is how BMP says "rows top to bottom" and saves reversing 240 of them.

The picture is captured from the display driver's flush, after the byte swap —
not from LVGL's buffers, which it hands straight to the panel and never touches
again. Reading those gave a black image.
