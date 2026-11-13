# Flipper IR Converter

A single web page that converts a Flipper Zero `.ir` remote file into formats control
systems accept:

- **Pronto hex**, for building Crestron IR drivers
- **Global Caché `sendir`** strings, for an iTach or Flex
- **A Q-SYS Lua table** of those strings

Open `ir-converter.html` in any browser. Everything runs locally; the file is not uploaded.

## Supported signals

Parsed buttons in every protocol the Flipper firmware can save (NEC, NECext, NEC42,
NEC42ext, Samsung32, RC5, RC5X, RC6, SIRC, SIRC15, SIRC20, Kaseikyo, RCA, Pioneer) and
raw captures. Parsed buttons are rebuilt from the timings and bit layouts in the Flipper
firmware's infrared library.

- NEC-family codes carry the standard repeat frame; Sony codes repeat three times and
  Pioneer twice.
- RC5 and RC6 codes are generated with the toggle bit fixed at 0. Some devices ignore a
  second identical press.

## Develop

```bash
node src/test.js
```

```bash
node build.js
```

`src/irconv.js` holds the conversion logic, `src/page.template.html` the page, and
`build.js` inlines one into the other.

## Not yet verified

The output passes tests against the well-known LG, Samsung and Sony power codes but has
not been fired at a real device. The notes on the page about where Crestron and Extron
accept each format were written from memory.
