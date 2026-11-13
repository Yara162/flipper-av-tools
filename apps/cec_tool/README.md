# CEC Tool

Flipper Zero app that watches HDMI-CEC traffic and sends common CEC commands, using an
HDMI breakout board. CEC is the control channel HDMI devices use to switch each other on,
change inputs and pass volume keys.

## Wiring

| HDMI pin | Signal | Flipper pin |
|---|---|---|
| 13 | CEC | 4 (A4) |
| 17 | Ground | 18 (GND) |

CEC is a single 3.3 V wire, so it connects directly with no resistors. The board can sit
in line between a source and a display to watch them talk, or be connected to a display
alone to control it. The wiring does not clash with the EDID Reader's, so both can stay
connected.

## Monitor

The app opens listening. Each message is one line:

```
4>0 Image View On
4>F Active Source 1.0.0.0
0>4 Power Standby
```

The two digits are the sender and destination addresses (`0` is the TV, `4` the first
playback device, `5` the audio system, `F` everyone). Up and Down move through the last
48 messages. The bottom of the screen shows the selected message's raw bytes, the device
names and whether it was acknowledged. An `x` at the right marks a message that was not
acknowledged or was cut short.

## Sending

Press OK for the command list, OK again to send, Back to return to the monitor.

| Command | What is sent |
|---|---|
| Scan bus | Polls addresses 0–14 and lists the devices that answer |
| TV: power on / standby | Image View On / Standby to the TV |
| Everything: standby | Broadcast Standby |
| TV: go to HDMI n | Broadcast Active Source for input n (Left/Right picks 1–4) |
| TV: volume up / down / mute | Key press and release to the TV |
| Audio: volume up / down | Key press and release to the audio system |
| Ask TV power state / name | Requests; the reply appears in the monitor |
| Ask active source | Broadcast request; the active device replies |

The Flipper sends from logical address `E` ("free use"). It does not acknowledge messages
addressed to it, so a device replying to a request may repeat its reply a few times.

## Build

```bash
ufbt
```

`cec.c` (bit-level decoder and message naming) has no Flipper dependencies and is tested
on a PC.

## Status

The decoder and message naming pass host tests, including timing jitter, messages cut
short and random input. Receiving and sending on a real bus have not yet been run on
hardware; CEC bit timing is the part most likely to need tuning.
