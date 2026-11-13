# HDMI Link Monitor

Flipper Zero app that watches two HDMI housekeeping lines with a breakout board in line
between a source and a display:

- **+5 V** (pin 18), supplied by the source
- **Hot-plug detect, HPD** (pin 19), raised by the display to say "I am here, read my EDID"

A source that stops supplying 5 V, or a display (or extender, or switcher) that drops
hot-plug, causes blanking, flicker and repeated handshakes. The app logs every drop with
its time and length, and can force a re-plug without touching the cable.

## Wiring

| HDMI pin | Signal | Flipper pin |
|---|---|---|
| 18 | +5 V | 2 (A7) |
| 19 | Hot-plug detect | 3 (A6) |
| 17 | Ground | 18 (GND) |

Both lines carry 5 V. The app keeps the two pins as plain inputs with no pull resistors,
the mode in which Flipper's documentation says its pins tolerate 5 V. If you have them,
a 10 kΩ resistor in series with each signal wire adds a margin of safety and does not
affect the readings.

Do not use this wiring together with the EDID Reader's: that one connects the Flipper's
own 5 V output to pin 18, which must not meet a source's supply.

Readings only mean something with both a source and a display connected. With an end
missing the lines float and the app reports chattering.

## Use

Press OK on the wiring screen to start.

- The top row shows the current state of both lines and how many times each has dropped.
- The log lists drops, newest first, with the time since monitoring started and how long
  each lasted. Up and Down scroll it.
- Each drop gives a short vibration and a red blink.
- **Hold OK** pulls hot-plug low for 150 ms, 500 ms or 2 s (Left/Right selects), which
  makes the source re-read the EDID and renegotiate, as if the cable had been re-plugged.
  The pulse is logged as "re-plug sent" and not counted as a drop.

## Limits

- The lines are sampled about 1,000 times a second, so drops shorter than a millisecond
  can be missed and lengths are accurate to about a millisecond.
- Monitoring pauses for the length of a re-plug pulse.
- The log holds the last 32 events and is not saved to the SD card.

## Build

```bash
ufbt
```

## Status

Compiles against Flipper firmware 1.5.1. Not yet run on hardware.
