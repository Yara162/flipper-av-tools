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
- HDMI 2.1 fixed rate link speed, when declared
- HDR10, HLG and Dolby Vision support
- a summary: 4K at 30 Hz, 4K at 60 Hz (or 4:2:0 only), HDR, audio
- block checksums

After the EDID, the app asks the display two more things directly (shown under "Live
check"):

- **HDCP:** whether the HDCP port answers, whether its HDCP 1.4 key is valid, whether it
  reports HDCP 2.2, and whether it is a repeater
- **HDMI 2.0 status registers (SCDC):** version and the receiver chip's ID string

Many displays only answer these while switched on and set to that input, so "no reply"
on a display in standby does not mean it lacks HDCP.

Each successful read is saved to `SD Card/apps_data/edid_reader/` as `<ID>.bin` (the raw
EDID, loadable into other tools and EDID emulators) and `<ID>.txt` (the report).

## Build

```bash
ufbt
```

`edid.c` has no Flipper dependencies and can be compiled and tested on a PC.

## Status

Reading and decoding EDIDs works on real displays. The decoder agrees with `edid-decode`
on manufacturer, product, native timing, maximum TMDS rate, CEC address, checksums and
4K60 support for 398 of 398 real-world EDIDs sampled from the linuxhw/EDID collection,
and on HDMI 2.1 link rate for all 1,165 samples checked. The HDCP and SCDC live check also
works on real hardware.

Known limits: reads up to four blocks; DisplayID blocks are listed but not decoded; the
bus runs at 3.3 V logic, which a few displays may not accept.
