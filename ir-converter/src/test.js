const assert = require('assert');
const C = require('./irconv.js');

// Decode a pulse-distance frame back to an MSB-first hex string (the way codes are usually quoted)
function pdmHex(t, nbits) {
  let bits = '';
  for (let i = 0; i < nbits; i++) bits += t[2 + 2 * i + 1] > 1000 ? '1' : '0';
  return parseInt(bits, 2).toString(16).toUpperCase();
}
const sig = (protocol, address, command) => ({ type: 'parsed', protocol, address, command });

// LG TV power: NEC 04/08 is the well-known 20DF10EF
let e = C.encode(sig('NEC', 0x04, 0x08));
assert.strictEqual(pdmHex(e.once, 32), '20DF10EF');
assert.strictEqual(e.once.length, 68);
assert.strictEqual(e.once.reduce((a, b) => a + b), 110000);
let p = C.toPronto(e).split(' ');
assert.deepStrictEqual(p.slice(0, 4), ['0000', '006D', '0022', '0002']);
assert.strictEqual(p.length, 4 + 68 + 4);
let s = C.toSendir(e).split(',');
assert.deepStrictEqual(s.slice(0, 6), ['sendir', '1:1', '1', '38000', '1', '69']);
assert.deepStrictEqual(s.slice(6, 8), ['342', '171']);

// NECext carries the full 16-bit command, so 04/F708 gives the same frame
assert.strictEqual(pdmHex(C.encode(sig('NECext', 0x0004 | 0xFB00, 0xF708)).once, 32), '20DF10EF');

// Samsung TV power: Samsung32 07/02 is E0E040BF
e = C.encode(sig('Samsung32', 0x07, 0x02));
assert.strictEqual(pdmHex(e.repeat, 32), 'E0E040BF');
assert.deepStrictEqual(C.toPronto(e).split(' ').slice(0, 4), ['0000', '006D', '0000', '0022']);
assert.strictEqual(C.toSendir(e).split(',')[5], '1');

// Sony TV power: SIRC 01/15 is A90, sent 3 times at 40 kHz, 45 ms period
e = C.encode(sig('SIRC', 0x01, 0x15));
let bits = '';
for (let i = 0; i < 12; i++) bits += e.repeat[2 + 2 * i] > 900 ? '1' : '0';
assert.strictEqual(parseInt(bits, 2).toString(16).toUpperCase(), 'A90');
assert.strictEqual(e.repeat.reduce((a, b) => a + b), 45000);
assert.strictEqual(e.repeat.length, 26);
assert.deepStrictEqual(C.toSendir(e).split(',').slice(3, 6), ['40000', '3', '1']);
assert.strictEqual(C.toPronto(e).split(' ')[1], '0068');

// RC5 00/01: S1 S2 T=0 00000 000001. Marks/spaces: 888 units, first mark after dropped leading space
e = C.encode(sig('RC5', 0x00, 0x01));
assert.strictEqual(C.toPronto(e).split(' ')[1], '0073');
// bits 1 1 0 00000 00000 1 -> halves: sm sm ms ms*10 sm ; merged from first mark:
// m s m(2) s ... expected: [888, 888, 1776, 888, (888,888)x10..., ]
assert.deepStrictEqual(e.repeat.slice(0, 4), [888, 888, 1776, 888]);
assert.strictEqual(e.repeat.length % 2, 0);
assert.strictEqual(e.repeat.reduce((a, b) => a + b), 113778);
assert.ok(e.warn);

// RC6 00/0C: leader 2666/889, then start bit 1 = mark 444
e = C.encode(sig('RC6', 0x00, 0x0C));
assert.deepStrictEqual(e.repeat.slice(0, 3), [2666, 889, 444]);
assert.strictEqual(e.repeat.length % 2, 0);
// toggle bit 0 with doubled halves: preceded by mode 000 (space,mark each) -> ...,444m, then 888s, 888m
assert.ok(e.repeat.includes(888));

// Kaseikyo vendor 0x2002, genre 8/0, cmd 0x3D, packed as the firmware does: 02 20 80 D0 03 53
e = C.encode(sig('Kaseikyo', 0x00200280, 0x3D));
let bytes = [];
for (let b = 0; b < 6; b++) { let v = 0; for (let i = 0; i < 8; i++) if (e.repeat[2 + 2 * (b * 8 + i) + 1] > 800) v |= 1 << i; bytes.push(v); }
assert.deepStrictEqual(bytes, [0x02, 0x20, 0x80, 0xD0, 0x03, 0x53]);
assert.strictEqual(e.repeat.reduce((a, b) => a + b), 130000);

// Raw passthrough, odd length gets a trailing gap
e = C.encode({ type: 'raw', frequency: 38000, data: [504, 3432, 502] });
assert.strictEqual(e.once.length, 4);
assert.deepStrictEqual(C.toPronto(e).split(' ').slice(0, 4), ['0000', '006D', '0002', '0000']);

// Parser
const parsed = C.parseIr(`Filetype: IR signals file
Version: 1
#
name: Power
type: parsed
protocol: NECext
address: EE 87 00 00
command: 5D A0 00 00
#
name: Raw_1
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 504 3432 502 483
`);
assert.strictEqual(parsed.length, 2);
assert.strictEqual(parsed[0].address, 0x87EE);
assert.strictEqual(parsed[0].command, 0xA05D);
assert.deepStrictEqual(parsed[1].data, [504, 3432, 502, 483]);
assert.ok(C.encode({ type: 'parsed', protocol: 'Nope', address: 1, command: 1 }).error);
for (const name of C.protocols) { const r = C.encode(sig(name, 0x15, 0x2A)); assert.ok(!r.error && (r.once.length + r.repeat.length) % 2 === 0, name); }
console.log('all tests passed:', C.protocols.join(', '));
