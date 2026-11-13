# VGM Test Patterns

Flipper Zero app that controls the test pattern generator in the
[combined module firmware](../../firmware/).

## Controls

| Button | Action |
|---|---|
| Up / Down | Previous / next pattern |
| Left / Right | Input number on the label (1–99) |
| OK | Show or hide the label |
| Hold OK | Next resolution (the module restarts) |
| Hold Down | Auto-cycle patterns every 5 seconds |
| Back | Exit; the module returns to mirroring after about 2 seconds |

The label reads `INPUT n`, which makes it easy to see at the display which source a
matrix or switcher has routed. The bottom of the screen shows whether the module is
answering.

## Build and install

```bash
ufbt
```

Copy `dist/vgm_tpg.fap` to `SD Card/apps/GPIO/`, or run `ufbt launch` with the Flipper
connected over USB.

## How it works

The app disables the Flipper's expansion-module service, takes the USART (pins 13/14) at
9600 baud and sends the current state as an `S,...` line on every change and every
500 ms. On exit it releases the port and re-enables the service.
