# Combined Video Game Module firmware

The official Video Game Module firmware with a test pattern generator added. The module
mirrors the Flipper's screen exactly as stock, and switches to test patterns while a
controlling Flipper app is open.

Files here:

- `vgm-combined.patch`: changes to the official firmware (video, serial task, startup, LED)
- `patterns/tpg.c`, `patterns/tpg.h`: the pattern generator
- `build.sh`: fetches the official source, applies both and builds

## Build

```bash
./build.sh
```

The script clones `flipperdevices/video-game-module` at commit `b57986de`, applies the
patch and builds with Flipper's own toolchain, which it downloads on first run (about
320 MB). Output: `work/build/app/firmware.uf2`. It needs `git`, `cmake`, `make`, `patch`
and `curl` on Linux or macOS.

Use Flipper's toolchain. A build made with a distribution `arm-none-eabi-gcc` produced a
noticeably different binary and was never confirmed working.

## Flash

Detach the module, hold BOOT, plug in USB-C and copy the `.uf2` to the drive that
appears. The official firmware goes back on the same way.

## Video modes

| Mode | Output | Rendered at | Notes |
|---|---|---|---|
| 0 | 640x480 60 Hz | 320x240 | Stock mode; the only one used for mirroring |
| 1 | 720x480 60 Hz | 360x240 | CEA 480p timing |
| 2 | 1280x720 30 Hz | 640x360 | Experimental: 372 MHz overclock, non-standard timing |

A mode change restarts the module (about a second of black). The wanted state travels
across the restart in the watchdog scratch registers, so a power cycle always returns to
mode 0 and mirroring.

## Patterns

| No. | Pattern | No. | Pattern |
|---|---|---|---|
| 0 | Colour bars (SMPTE style) | 7 | Grey 50% |
| 1 | Bars 100% | 8 | Grey and RGB ramps |
| 2 | White | 9 | Crosshatch |
| 3 | Red | 10 | Border with 90% safe box |
| 4 | Green | 11 | Checkerboard |
| 5 | Blue | 12 | Moving bar with timecode |
| 6 | Black | 13 | Colour noise (Pixel Fixer only) |

An optional overlay shows a label of up to 12 characters and the current mode.

## Control protocol

The module and Flipper share one UART (module GP0/GP1, Flipper pins 13/14). The official
expansion protocol idles at 9600 baud and then negotiates a faster rate. Pattern commands
are plain text lines sent at 9600 baud while the link is idle:

| Line | Meaning |
|---|---|
| `S,<mode>,<pattern>,<overlay 0\|1>,<label>\n` | Show pattern 0–12 |
| `P,<mode>,<chunk>,<hold>\n` | Colour noise: block size 2–128 half-resolution pixels, each image held 1–120 frames |
| `K,<mode>,<pattern>\n` | Reply from the module |

The Flipper app must disable the expansion service and take the USART before sending.
While lines keep arriving (the apps resend every 500 ms) the module stays in pattern
mode. Two seconds after they stop it returns to the expansion handshake and mirroring,
restarting first if it was not in mode 0. Pattern 13 is accepted only through the `P`
command.

## LED states

| LED | Meaning |
|---|---|
| Blinking blue | Looking for the Flipper |
| Green | Mirroring |
| Cyan | Test patterns |

## Implementation notes

- **Rendering:** each pattern is a few precomputed row templates. The video interrupt
  hands the DVI library a pointer to the right template and only composes a fresh line
  where the overlay, timecode or noise needs it.
- **Vertical position:** the DVI library keeps two lines in flight, so the line queued
  during display row `n` is drawn for row `n + 2`. The row is derived from the library's
  line counter, which keeps the picture aligned after a late line.
- **Link speed:** this build uses the 0.1.0 release settings (921600 baud, FIFO off). The
  development branch's 1843200 baud completed the handshake and then dropped the session
  on the first large message when tested with Flipper firmware 1.5.1.
- **Idle receiver:** the serial task only rewrites the baud rate when returning from the
  fast rate. Rewriting it on every idle loop reset the receiver and cut incoming commands
  in half.
- **Flash clock:** mode 2 halves the flash clock before overclocking; the other modes
  leave it untouched.
