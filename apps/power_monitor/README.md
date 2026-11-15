# Power Monitor

Flipper Zero app that measures DC voltage, current and power with an INA219 sensor board,
charts the current and logs to the SD card. Use it to see what a device really draws, or
to catch a power supply that sags when the load comes on.

**Limits: 26 V and 3.2 A, low-voltage DC only.** Never connect it to mains or to
70 V / 100 V speaker lines.

## Wiring

Sensor board to Flipper:

| Sensor pin | Flipper pin |
|---|---|
| VCC | 9 (3V3) |
| GND | 11 (GND) |
| SCL | 16 (C0) |
| SDA | 15 (C1) |

Circuit being measured (the board goes in the positive wire):

| From | To |
|---|---|
| Supply positive | Vin+ |
| Vin- | Device positive |
| Supply negative | Device negative, **and** the sensor board's GND |

The shared ground is what lets the board measure voltage. Without it the current reading
still works but the voltage does not. The voltage shown is on the device side of the board.

The app assumes the board's standard 0.1 ohm shunt resistor (marked R100). It uses the
same four Flipper pins as the Lux Meter; both boards can share them, since they answer at
different addresses.

## Screen

- **Top:** voltage, current and power now.
- **Chart:** current over the last minute, scaled to its own peak.
- **Below:** lowest and highest voltage, peak current, charge used (mAh), energy used
  (Wh) and run time.

| Button | Action |
|---|---|
| OK | Reset the figures and chart |
| Hold OK | Start or stop logging to the SD card |
| Back | Exit |

While logging, the app writes one line a second to
`SD Card/apps_data/power_monitor/power_<date>_<time>.csv` with seconds, volts, amps and
watts, ready to open in a spreadsheet. "OVER RANGE" means the reading exceeded what the
sensor can measure.

Current flowing backwards (Vin- to Vin+) shows with a minus sign.

## Build

```bash
ufbt
```

## Status

Compiles against Flipper firmware 1.5.1; the charge and energy arithmetic is checked. Not
yet run on hardware.
