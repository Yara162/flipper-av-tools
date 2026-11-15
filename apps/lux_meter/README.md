# Lux Meter

Flipper Zero app that turns a BH1750 light sensor board (sold as GY-302) into a light
meter, with a nine-point test for projector brightness and uniformity.

## Wiring

| Sensor pin | Flipper pin |
|---|---|
| VCC | 9 (3V3) |
| GND | 11 (GND) |
| SCL | 16 (C0) |
| SDA | 15 (C1) |
| ADDR | leave unconnected |

The app shows this table until it finds the sensor, then switches to the meter by itself.

## Meter

The live reading is shown in lux and foot-candles, with minimum, maximum and average
since the last reset. The sensor switches range by itself: steps of 0.5 lux up to about
27,000 lux, and 1 lux up to about 54,000.

| Button | Action |
|---|---|
| OK | Reset minimum, maximum and average |
| Hold OK | Freeze or release the reading |
| Right | Start the nine-point test |
| Back | Exit |

## Nine-point test

This is the standard way to estimate a projector's light output: measure the light
falling on the screen at the centre of each cell of a 3 x 3 grid, average the nine
readings, and multiply by the image area.

1. Show a full white image and let the projector warm up.
2. Set the image diagonal (Up/Down, in inches) and shape (Left/Right), then press OK.
3. Hold the sensor flat against the screen, facing the projector, at the square the app
   highlights, and press OK. Left steps back to redo a point.
4. After the ninth point the app shows the average, the estimated lumens, how far the
   dimmest and brightest points are from the average, and the dimmest point as a share of
   the brightest.

Each test is saved to `SD Card/apps_data/lux_meter/lux_<date>_<time>.txt` with the grid
of readings.

The same test works on a flat panel or video wall for uniformity; ignore the lumens
figure there.

## Accuracy

These sensor boards are not individually calibrated and can be 20 percent off a
reference meter, so treat lumens as an estimate. Readings from the same sensor compare
well with each other, which is what uniformity and before-and-after checks need. Keep
your own shadow and room lighting off the sensor.

## Build

```bash
ufbt
```

## Status

Working on real hardware: the live meter and the nine-point test, including the saved
report.
