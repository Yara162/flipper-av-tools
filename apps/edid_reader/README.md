# EDID Reader

Flipper Zero app that reads a display's EDID through an HDMI breakout board and decodes
it on screen: what the display says it is and which formats it claims to accept.

## Wiring

Connect **only the display** to the breakout board (no source on the other end), then
four jumper wires:

| HDMI pin | Signal | Flipper pin |
|---|---|---|
| 15 | DDC clock (SCL) | 16 (C0) |
| 16 | DDC data (SDA) | 15 (C1) |
| 17 | DDC ground | 18 (GND) |
| 18 | +5 V | 1 (5V) |

The +5 V wire powers the display's EDID memory, as a source would. The app switches the
Flipper's 5 V output on for the read and off again afterwards. No resistors are needed:
the app drives the bus slowly using the Flipper's internal pull-ups.

Do not connect a source while the Flipper is wired in. Two devices would then drive the
same bus, and the source pulls the lines to 5 V.

## Use

Open Apps → GPIO → EDID Reader and press OK. Up and Down scroll the report, OK reads
again, Back returns.

The report covers:

- name, manufacturer ID, product code, serial, manufacture date, EDID version
- native resolution, refresh rate and pixel clock
- every video mode in the CTA-861 block, by name (for example `2160p60`)
- audio formats with channel count and top sample rate
- HDMI details: CEC physical address, deep colour, maximum TMDS rate, HDMI 2.x and SCDC
- HDR10, HLG and Dolby Vision support
- a summary: 4K at 30 Hz, 4K at 60 Hz (or 4:2:0 only), HDR, audio
- block checksums

Each successful read is saved to `SD Card/apps_data/edid_reader/` as `<ID>.bin` (the raw
EDID, loadable into other tools and EDID emulators) and `<ID>.txt` (the report).

## Build

```bash
ufbt
```

`edid.c` has no Flipper dependencies and can be compiled and tested on a PC.

## Status

The decoder agrees with `edid-decode` on manufacturer, product, native timing, maximum
TMDS rate, CEC address, checksums and 4K60 support for 398 of 398 real-world EDIDs
sampled from the linuxhw/EDID collection. The pin driver and the app have not yet run on
hardware.

Known limits: reads up to four blocks; DisplayID blocks are listed but not decoded; the
bus runs at 3.3 V logic, which a few displays may not accept.
