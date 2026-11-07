# AGGEGGINO — documentation

The firmware's own documentation, versioned with the code it describes. A manual
for a menu-driven device goes stale the moment the menu moves, so it lives in
the repository rather than in a wiki: `git checkout` of a tag gives you the
firmware and the manual that matches it.

## The manual

| | |
|---|---|
| [**Getting started**](getting-started.md) | What you need, how to wire it, and the first bridge from scratch |
| [**The dashboard**](dashboard.md) | Reading the main screen, the three buttons under the glass, the screen lock |
| [**Settings**](settings.md) | The complete menu tree, entry by entry |
| [**Clock outputs**](outputs.md) | MIDI (USB, DIN, network), CV pulses, OSC — including the custom OSC profile |
| [**The SD card**](sd-card.md) | Playlists written as CSV, formatting, screenshots |
| [**When something is wrong**](troubleshooting.md) | Symptoms, and what each one actually means |

## The protocol notes

Not a manual: a record of what was measured on real hardware while building
this, written so that the measurements can be separated from the guesses.

| | |
|---|---|
| [**Protocol notes**](protocol-notes.md) | Pioneer Pro DJ Link and Ableton Link, in English |
| [**Note sui protocolli**](protocol-notes.it.md) | The Italian original, the language they were measured in |

Every non-obvious claim in them carries a mark: **[M]** measured on real
hardware, **[D]** deduced from measurements with the reasoning spelled out,
**[I]** an unverified hypothesis, **[E]** from an external source. §5 lists
what is still unknown.

## A note on what this documentation does not do

It describes the firmware in this repository, on the hardware it was built for.
It does not teach DJ Link or Ableton Link — the protocol notes do some of that —
and it does not promise that what works here works on players we have never
seen. Where behaviour was measured on two XDJ-700s and nothing else, it says so.
