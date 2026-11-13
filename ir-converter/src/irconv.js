// Flipper .ir -> Pronto hex / Global Cache sendir.
// Protocol timings and bit layouts follow flipperzero-firmware lib/infrared/encoder_decoder.
(function (root) {
  'use strict';

  function parseIr(text) {
    var signals = [], cur = null;
    String(text).split(/\r?\n/).forEach(function (line) {
      line = line.trim();
      if (!line || line[0] === '#') return;
      var i = line.indexOf(':');
      if (i < 0) return;
      var key = line.slice(0, i).trim().toLowerCase(), val = line.slice(i + 1).trim();
      if (key === 'name') { cur = { name: val }; signals.push(cur); return; }
      if (!cur) return;
      if (key === 'type' || key === 'protocol') cur[key] = val;
      else if (key === 'address' || key === 'command') {
        var bytes = val.split(/\s+/).map(function (b) { return parseInt(b, 16); });
        var v = 0;
        for (var k = bytes.length - 1; k >= 0; k--) v = v * 256 + (bytes[k] || 0);
        cur[key] = v; cur[key + 'Hex'] = val;
      } else if (key === 'frequency') cur.frequency = parseInt(val, 10);
      else if (key === 'data') cur.data = val.split(/\s+/).map(Number).filter(function (n) { return n > 0; });
    });
    return signals;
  }

  // value -> n bits, least significant first (the order Flipper transmits)
  function lsb(value, n) {
    var out = [];
    for (var i = 0; i < n; i++) out.push(Math.floor(value / Math.pow(2, i)) % 2);
    return out;
  }
  function msb(value, n) { return lsb(value, n).reverse(); }
  function inv(bits) { return bits.map(function (b) { return b ? 0 : 1; }); }
  function sum(a) { return a.reduce(function (s, x) { return s + x; }, 0); }

  // Pulse-distance frame: preamble, (mark, space) per bit, stop mark
  function pdm(t, bits) {
    var out = [t.pm, t.ps];
    bits.forEach(function (b) { out.push(t.mark, b ? t.one : t.zero); });
    out.push(t.mark);
    return out;
  }
  function withGap(frame, gap) { return frame.concat([Math.max(Math.round(gap), 5000)]); }

  // Manchester half-bits -> merged mark/space list starting with a mark
  function manchester(halves, gapPeriod) {
    var merged = [];
    halves.forEach(function (h) {
      var last = merged[merged.length - 1];
      if (last && last[0] === h[0]) last[1] += h[1]; else merged.push([h[0], h[1]]);
    });
    if (merged.length && !merged[0][0]) merged.shift();
    if (merged.length && !merged[merged.length - 1][0]) merged.pop();
    var out = merged.map(function (m) { return m[1]; });
    return withGap(out, gapPeriod - sum(out));
  }

  var NEC = { pm: 9000, ps: 4500, mark: 560, one: 1690, zero: 560 };
  var NEC_DITTO = [9000, 2250, 560, 110000 - 9000 - 2250 - 560];

  function necFamily(bits) {
    var f = pdm(NEC, bits);
    return { freq: 38000, once: withGap(f, 110000 - sum(f)), repeat: NEC_DITTO.slice(), repeatCount: 1 };
  }
  function repeatOnly(freq, frame, gap, count, warn) {
    return { freq: freq, once: [], repeat: withGap(frame, gap), repeatCount: count || 1, warn: warn };
  }

  var TOGGLE_WARN = 'Toggle bit is fixed at 0. Some devices ignore a second identical press; if that happens, capture the button twice as raw and alternate.';

  var ENCODERS = {
    NEC: function (a, c) { return necFamily([].concat(lsb(a, 8), inv(lsb(a, 8)), lsb(c, 8), inv(lsb(c, 8)))); },
    NECext: function (a, c) { return necFamily([].concat(lsb(a, 16), lsb(c, 16))); },
    NEC42: function (a, c) { return necFamily([].concat(lsb(a, 13), inv(lsb(a, 13)), lsb(c, 8), inv(lsb(c, 8)))); },
    NEC42ext: function (a, c) { return necFamily([].concat(lsb(a, 26), lsb(c, 16))); },
    Samsung32: function (a, c) {
      var f = pdm({ pm: 4500, ps: 4500, mark: 550, one: 1650, zero: 550 },
        [].concat(lsb(a, 8), lsb(a, 8), lsb(c, 8), inv(lsb(c, 8))));
      return repeatOnly(38000, f, 46000);
    },
    RCA: function (a, c) {
      var f = pdm({ pm: 4000, ps: 4000, mark: 500, one: 2000, zero: 1000 },
        [].concat(lsb(a, 4), lsb(c, 8), inv(lsb(a, 4)), inv(lsb(c, 8))));
      return repeatOnly(38000, f, 8000);
    },
    Pioneer: function (a, c) {
      // 32 data bits; the 33rd (always 0) acts as the stop mark
      var f = pdm({ pm: 8500, ps: 4225, mark: 500, one: 1500, zero: 500 },
        [].concat(lsb(a, 8), inv(lsb(a, 8)), lsb(c, 8), inv(lsb(c, 8))));
      return repeatOnly(40000, f, 26500, 2);
    },
    Kaseikyo: function (a, c) {
      var id = Math.floor(a / 0x1000000) & 3, vendor = Math.floor(a / 256) & 0xffff;
      var g1 = (a >> 4) & 0xf, g2 = a & 0xf, d = [];
      d[0] = vendor & 0xff; d[1] = vendor >> 8;
      var vp = d[0] ^ d[1]; vp = (vp & 0xf) ^ (vp >> 4);
      d[2] = (vp & 0xf) | (g1 << 4);
      d[3] = (g2 & 0xf) | ((c & 0xf) << 4);
      d[4] = ((id << 6) | ((c >> 4) & 0x3f)) & 0xff;
      d[5] = d[2] ^ d[3] ^ d[4];
      var bits = [];
      d.forEach(function (b) { bits = bits.concat(lsb(b, 8)); });
      var U = 432, f = pdm({ pm: 8 * U, ps: 4 * U, mark: U, one: 3 * U, zero: U }, bits);
      return repeatOnly(38000, f, 130000 - sum(f));
    },
    RC5: function (a, c) { return rc5(a, c, 1); },
    RC5X: function (a, c) { return rc5(a, c, 0); },
    RC6: function (a, c) {
      var bits = [].concat([1, 0, 0, 0, 0], msb(a, 8), msb(c, 8)), T = 444;
      var halves = [[1, 2666], [0, 889]];
      bits.forEach(function (b, i) {
        var d = i === 4 ? 2 * T : T;
        halves.push([b ? 1 : 0, d], [b ? 0 : 1, d]);
      });
      var r = repeatOnly(36000, [], 0, 1, TOGGLE_WARN);
      r.repeat = manchester(halves, 106000);
      return r;
    }
  };
  function sirc(addrBits) {
    return function (a, c) {
      var out = [2400, 600];
      [].concat(lsb(c, 7), lsb(a, addrBits)).forEach(function (b) { out.push(b ? 1200 : 600, 600); });
      out.pop(); // no space after the last mark
      return repeatOnly(40000, out, 45000 - sum(out), 3);
    };
  }
  ENCODERS.SIRC = sirc(5); ENCODERS.SIRC15 = sirc(8); ENCODERS.SIRC20 = sirc(13);

  function rc5(a, c, secondStart) {
    // start, start (or inverted command bit 6 for RC5X), toggle, 5 address, 6 command
    var bits = [].concat([1, secondStart, 0], msb(a, 5), msb(c, 6)), T = 888, halves = [];
    bits.forEach(function (b) { halves.push([b ? 0 : 1, T], [b ? 1 : 0, T]); });
    var r = repeatOnly(36000, [], 0, 1, TOGGLE_WARN);
    r.repeat = manchester(halves, 113778);
    return r;
  }

  function encode(sig) {
    if (sig.type === 'raw') {
      if (!sig.data || !sig.data.length) return { error: 'Raw signal has no timing data.' };
      var d = sig.data.slice();
      if (d.length % 2) d.push(100000);
      return { freq: sig.frequency || 38000, once: d, repeat: [], repeatCount: 1 };
    }
    var enc = ENCODERS[sig.protocol];
    if (!enc) return { error: 'Protocol "' + sig.protocol + '" is not supported here. Re-capture the button as raw on the Flipper.' };
    if (sig.address === undefined || sig.command === undefined) return { error: 'Missing address or command.' };
    return enc(sig.address, sig.command);
  }

  function units(us, freq) { return Math.max(1, Math.round(us * freq / 1e6)); }
  function hex4(n) { return ('0000' + Math.min(n, 0xffff).toString(16).toUpperCase()).slice(-4); }

  function toPronto(e) {
    var words = ['0000', hex4(Math.round(1e6 / (e.freq * 0.241246))), hex4(e.once.length / 2), hex4(e.repeat.length / 2)];
    e.once.concat(e.repeat).forEach(function (us) { words.push(hex4(units(us, e.freq))); });
    return words.join(' ');
  }

  function toSendir(e, connector, id) {
    var all = e.once.concat(e.repeat).map(function (us) { return units(us, e.freq); });
    var offset = e.repeat.length && e.once.length ? e.once.length + 1 : 1;
    return ['sendir', connector || '1:1', id || 1, e.freq, e.repeatCount || 1, offset].concat(all).join(',');
  }

  function luaName(s) { return String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"'); }
  function toQsysLua(rows) {
    var lines = ['-- Send each string followed by "\\r" to the Global Cache on TCP port 4998', 'IrCodes = {'];
    rows.forEach(function (r) { lines.push('  ["' + luaName(r.name) + '"] = "' + r.sendir + '",'); });
    lines.push('}');
    return lines.join('\n');
  }

  var api = { parseIr: parseIr, encode: encode, toPronto: toPronto, toSendir: toSendir, toQsysLua: toQsysLua, protocols: Object.keys(ENCODERS) };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.IrConv = api;
})(typeof window !== 'undefined' ? window : this);
