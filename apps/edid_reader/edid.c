#include "edid.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_CHARS 25

typedef struct {
    char* buf;
    size_t size, len;
} Out;

static void out_line(Out* o, const char* fmt, ...) {
    if(o->len + 2 >= o->size) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(o->buf + o->len, o->size - o->len - 1, fmt, args);
    va_end(args);
    if(n < 0) return;
    if((size_t)n > o->size - o->len - 2) n = (int)(o->size - o->len - 2);
    o->len += (size_t)n;
    o->buf[o->len++] = '\n';
    o->buf[o->len] = 0;
}

// Appends space-separated words, wrapping at the screen width. `prefix` starts the first line.
typedef struct {
    Out* o;
    char line[LINE_MAX_CHARS + 1];
    size_t len;
} Wrap;

static void wrap_start(Wrap* w, Out* o, const char* prefix) {
    w->o = o;
    w->len = (size_t)snprintf(w->line, sizeof(w->line), "%s", prefix);
}

static void wrap_word(Wrap* w, const char* word) {
    size_t wl = strlen(word);
    if(w->len && w->len + 1 + wl > LINE_MAX_CHARS) {
        out_line(w->o, "%s", w->line);
        w->len = (size_t)snprintf(w->line, sizeof(w->line), " ");
    }
    w->len += (size_t)snprintf(
        w->line + w->len, sizeof(w->line) - w->len, "%s%s", (w->len && w->line[w->len - 1] != ' ') ? " " : "", word);
    if(w->len > LINE_MAX_CHARS) w->len = LINE_MAX_CHARS;
}

static void wrap_end(Wrap* w) {
    if(w->len) out_line(w->o, "%s", w->line);
    w->len = 0;
}

bool edid_block_checksum_ok(const uint8_t* b) {
    uint8_t sum = 0;
    for(int i = 0; i < EDID_BLOCK_SIZE; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

// Common CTA-861 video identification codes
static const char* vic_name(uint8_t vic) {
    switch(vic) {
    case 1: return "480p60(VGA)";
    case 2: case 3: return "480p60";
    case 4: return "720p60";
    case 5: return "1080i60";
    case 6: case 7: return "480i60";
    case 16: return "1080p60";
    case 17: case 18: return "576p50";
    case 19: return "720p50";
    case 20: return "1080i50";
    case 21: case 22: return "576i50";
    case 31: return "1080p50";
    case 32: return "1080p24";
    case 33: return "1080p25";
    case 34: return "1080p30";
    case 60: return "720p24";
    case 61: return "720p25";
    case 62: return "720p30";
    case 63: return "1080p120";
    case 64: return "1080p100";
    case 93: return "2160p24";
    case 94: return "2160p25";
    case 95: return "2160p30";
    case 96: return "2160p50";
    case 97: return "2160p60";
    case 98: return "4096x2160p24";
    case 99: return "4096x2160p25";
    case 100: return "4096x2160p30";
    case 101: return "4096x2160p50";
    case 102: return "4096x2160p60";
    case 117: return "2160p100";
    case 118: return "2160p120";
    default: return NULL;
    }
}

// 18-byte detailed timing descriptor -> resolution and refresh rate
static bool dtd_parse(const uint8_t* d, uint16_t* w, uint16_t* h, uint16_t* hz, uint32_t* clk_khz, bool* interlaced) {
    uint32_t clk = (uint32_t)(d[0] | (d[1] << 8)) * 10; // kHz
    if(clk == 0) return false;
    uint32_t hact = d[2] | ((uint32_t)(d[4] & 0xF0) << 4), hbl = d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
    uint32_t vact = d[5] | ((uint32_t)(d[7] & 0xF0) << 4), vbl = d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
    uint32_t total = (hact + hbl) * (vact + vbl);
    if(total == 0) return false;
    *interlaced = (d[17] & 0x80) != 0;
    *w = (uint16_t)hact;
    *h = (uint16_t)(*interlaced ? vact * 2 : vact);
    *hz = (uint16_t)((clk * 1000 + total / 2) / total);
    *clk_khz = clk;
    return true;
}

static void descriptor_text(const uint8_t* d, char* dst) {
    int n = 0;
    for(int i = 5; i < 18; i++) {
        if(d[i] == 0x0A) break;
        dst[n++] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '?';
    }
    while(n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

static void decode_base(const uint8_t* b, Out* o, EdidInfo* info) {
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    info->header_ok = memcmp(b, header, 8) == 0;
    if(!info->header_ok) {
        out_line(o, "Not an EDID: bad header");
        return;
    }
    uint16_t m = (uint16_t)((b[8] << 8) | b[9]);
    info->mfr[0] = (char)('@' + ((m >> 10) & 31));
    info->mfr[1] = (char)('@' + ((m >> 5) & 31));
    info->mfr[2] = (char)('@' + (m & 31));
    info->mfr[3] = 0;
    info->product = (uint16_t)(b[10] | (b[11] << 8));
    info->extensions = b[126];

    char name[14] = "", serial[14] = "";
    unsigned range_vmin = 0, range_vmax = 0;
    for(int i = 0; i < 4; i++) {
        const uint8_t* d = b + 54 + 18 * i;
        if(d[0] || d[1]) continue; // a timing, not a text descriptor
        if(d[3] == 0xFC) descriptor_text(d, name);
        if(d[3] == 0xFF) descriptor_text(d, serial);
        if(d[3] == 0xFD) {
            range_vmin = d[5];
            range_vmax = d[6];
        }
    }
    if(name[0]) out_line(o, "Name: %s", name);
    out_line(o, "Mfr: %s  Prod: %04X", info->mfr, info->product);
    if(serial[0]) out_line(o, "S/N: %s", serial);
    if(b[16] == 0xFF)
        out_line(o, "Model year: %u", 1990 + b[17]);
    else if(b[16] >= 1 && b[16] <= 54)
        out_line(o, "Made: week %u of %u", b[16], 1990 + b[17]);
    else
        out_line(o, "Made: %u", 1990 + b[17]);
    out_line(o, "EDID %u.%u  %s", b[18], b[19], (b[20] & 0x80) ? "digital" : "analogue");
    if(b[21] && b[22]) out_line(o, "Size: %ux%u cm", b[21], b[22]);

    uint32_t clk = 0;
    bool il = false;
    // The preferred timing is normally the first descriptor; accept a later one if not
    bool have_dtd = false;
    for(int i = 0; i < 4 && !have_dtd; i++)
        have_dtd = dtd_parse(b + 54 + 18 * i, &info->native_w, &info->native_h, &info->native_hz, &clk, &il);
    if(have_dtd) {
        out_line(o, "Native: %ux%u%s %uHz", info->native_w, info->native_h, il ? "i" : "", info->native_hz);
        out_line(o, "Pixel clock: %lu.%02lu MHz", (unsigned long)(clk / 1000), (unsigned long)(clk % 1000 / 10));
    } else {
        out_line(o, "Native: not stated");
    }
    if(range_vmax) out_line(o, "V range: %u-%u Hz", range_vmin, range_vmax);
}

static void decode_cta(const uint8_t* b, Out* o, EdidInfo* info) {
    info->has_cta = true;
    uint8_t rev = b[1], dtd_off = b[2];
    out_line(o, "-- CTA-861 block --");
    if(rev >= 2) {
        if(b[3] & 0x40) info->audio = true;
        Wrap w;
        wrap_start(&w, o, "Basic:");
        if(b[3] & 0x40) wrap_word(&w, "audio");
        if(b[3] & 0x20) wrap_word(&w, "YCbCr444");
        if(b[3] & 0x10) wrap_word(&w, "YCbCr422");
        if(!(b[3] & 0x70)) wrap_word(&w, "RGB only");
        wrap_end(&w);
    }
    if(rev < 3 || dtd_off < 4 || dtd_off > 127) return;

    bool y420_only[256] = {false};
    for(int i = 4; i < dtd_off;) {
        uint8_t tag = b[i] >> 5, len = b[i] & 0x1F;
        const uint8_t* p = b + i + 1;
        if(i + 1 + len > dtd_off) break;
        if(tag == 2) { // video data block
            Wrap w;
            wrap_start(&w, o, "Modes:");
            for(int k = 0; k < len; k++) {
                uint8_t vic = p[k];
                if(vic >= 129 && vic <= 192) vic &= 0x7F; // native flag
                char tmp[16];
                const char* n = vic_name(vic);
                if(!n) {
                    snprintf(tmp, sizeof(tmp), "VIC%u", vic);
                    n = tmp;
                }
                wrap_word(&w, n);
                if(vic >= 93 && vic <= 95) info->vic_4k30 = true;
                if(vic == 96 || vic == 97 || vic == 101 || vic == 102) info->vic_4k60 = true;
            }
            wrap_end(&w);
        } else if(tag == 1) { // audio data block
            static const char* const fmt[] = {"?", "LPCM", "AC-3", "MPEG1", "MP3", "MPEG2", "AAC", "DTS",
                                              "ATRAC", "DSD", "E-AC-3", "DTS-HD", "TrueHD", "DST", "WMA", "ext"};
            info->audio = true;
            for(int k = 0; k + 3 <= len; k += 3) {
                uint8_t code = (p[k] >> 3) & 0x0F, ch = (uint8_t)((p[k] & 7) + 1), rates = p[k + 1];
                unsigned top = rates & 0x40 ? 192 : rates & 0x20 ? 176 : rates & 0x10 ? 96 :
                               rates & 0x08 ? 88 : rates & 0x04 ? 48 : rates & 0x02 ? 44 : 32;
                out_line(o, "Audio: %s %uch %uk", fmt[code], ch, top);
            }
        } else if(tag == 3 && len >= 5 && p[0] == 0x03 && p[1] == 0x0C && p[2] == 0x00) { // HDMI VSDB
            info->hdmi = true;
            info->has_phys_addr = true;
            info->phys_addr[0] = p[3];
            info->phys_addr[1] = p[4];
            out_line(o, "HDMI: yes");
            out_line(o, "CEC addr: %u.%u.%u.%u", p[3] >> 4, p[3] & 15, p[4] >> 4, p[4] & 15);
            if(len >= 6 && (p[5] & 0x70)) {
                Wrap w;
                wrap_start(&w, o, "Deep colour:");
                if(p[5] & 0x10) wrap_word(&w, "30");
                if(p[5] & 0x20) wrap_word(&w, "36");
                if(p[5] & 0x40) wrap_word(&w, "48");
                wrap_word(&w, "bit");
                wrap_end(&w);
            }
            if(len >= 7 && p[6]) {
                uint16_t mhz = (uint16_t)(p[6] * 5);
                if(mhz > info->max_tmds_mhz) info->max_tmds_mhz = mhz;
                out_line(o, "Max TMDS: %u MHz", mhz);
            }
            // HDMI 1.4 4K modes: skip latency fields, then HDMI_VIC list
            if(len >= 8) {
                int idx = 8;
                if(p[7] & 0x80) idx += 2;
                if(p[7] & 0x40) idx += 2;
                if((p[7] & 0x20) && idx + 1 < len) {
                    int vic_len = p[idx + 1] >> 5;
                    if(vic_len && idx + 2 + vic_len <= len) {
                        info->vic_4k30 = true;
                        out_line(o, "HDMI 1.4 4K: %d mode%s", vic_len, vic_len == 1 ? "" : "s");
                    }
                }
            }
        } else if(tag == 3 && len >= 7 && p[0] == 0xD8 && p[1] == 0x5D && p[2] == 0xC4) { // HDMI Forum VSDB
            uint16_t mhz = (uint16_t)(p[4] * 5);
            if(mhz > info->max_tmds_mhz) info->max_tmds_mhz = mhz;
            out_line(o, "HDMI 2.x: yes");
            if(mhz) out_line(o, "Max TMDS: %u MHz", mhz);
            info->scdc = (p[5] & 0x80) != 0;
            out_line(o, "SCDC: %s", info->scdc ? "yes" : "no");
            if(len >= 7 && (p[6] >> 4)) {
                static const uint8_t frl_gbps[] = {0, 9, 18, 24, 32, 40, 48};
                uint8_t rate = p[6] >> 4;
                info->max_frl_gbps = rate < 7 ? frl_gbps[rate] : 48;
                out_line(o, "HDMI 2.1 FRL: %u Gbps", info->max_frl_gbps);
            }
        } else if(tag == 7 && len >= 1) { // extended tags
            uint8_t ext = p[0];
            if(ext == 6 && len >= 2) { // HDR static metadata
                Wrap w;
                wrap_start(&w, o, "HDR:");
                if(p[1] & 0x04) {
                    wrap_word(&w, "HDR10");
                    info->hdr10 = true;
                }
                if(p[1] & 0x08) {
                    wrap_word(&w, "HLG");
                    info->hlg = true;
                }
                if(!(p[1] & 0x0C)) wrap_word(&w, "SDR only");
                wrap_end(&w);
            } else if(ext == 14) { // modes available only as YCbCr 4:2:0
                for(int k = 1; k < len; k++) y420_only[p[k]] = true;
            } else if(ext == 1 && len >= 4 && p[1] == 0x46 && p[2] == 0xD0 && p[3] == 0x00) {
                out_line(o, "Dolby Vision: yes");
            }
        }
        i += 1 + len;
    }
    if(!info->vic_4k60 && (y420_only[96] || y420_only[97] || y420_only[101] || y420_only[102])) {
        info->only_420_4k60 = true;
    }
}

void edid_decode(const uint8_t* data, size_t blocks, char* out, size_t out_size, EdidInfo* info) {
    Out o = {.buf = out, .size = out_size, .len = 0};
    memset(info, 0, sizeof(*info));
    if(out_size) out[0] = 0;
    if(blocks == 0) return;

    info->checksums_ok = true;
    for(size_t i = 0; i < blocks; i++)
        if(!edid_block_checksum_ok(data + i * EDID_BLOCK_SIZE)) info->checksums_ok = false;

    decode_base(data, &o, info);
    if(!info->header_ok) return;
    out_line(&o, "Ext blocks: %u (read %u)", info->extensions, (unsigned)(blocks - 1));
    out_line(&o, "Checksums: %s", info->checksums_ok ? "OK" : "BAD");

    for(size_t i = 1; i < blocks; i++) {
        const uint8_t* b = data + i * EDID_BLOCK_SIZE;
        if(b[0] == 0x02)
            decode_cta(b, &o, info);
        else
            out_line(&o, "-- Block %u: tag %02X --", (unsigned)i, b[0]);
    }

    out_line(&o, "-- Summary --");
    if(!info->has_cta) {
        out_line(&o, "No CTA block: DVI or");
        out_line(&o, " PC-only display");
    } else {
        out_line(&o, "4K 30Hz: %s", info->vic_4k30 || info->vic_4k60 ? "yes" : "no");
        out_line(&o, "4K 60Hz: %s", info->vic_4k60 ? "yes" : info->only_420_4k60 ? "4:2:0 only" : "no");
        out_line(&o, "HDR10: %s  Audio: %s", info->hdr10 ? "yes" : "no", info->audio ? "yes" : "no");
    }
}

bool hdcp_bksv_valid(const uint8_t* bksv) {
    int ones = 0;
    for(int i = 0; i < 5; i++)
        for(int b = 0; b < 8; b++) ones += (bksv[i] >> b) & 1;
    return ones == 20;
}

void edid_format_live(const HdcpProbe* hdcp, const ScdcProbe* scdc, char* out, size_t out_size) {
    size_t used = strlen(out);
    Out o = {.buf = out, .size = out_size, .len = used};
    out_line(&o, "-- Live check --");
    if(!hdcp->port_ack) {
        out_line(&o, "HDCP: no reply");
        out_line(&o, " (none, or display is");
        out_line(&o, " off or on another input)");
    } else {
        if(hdcp_bksv_valid(hdcp->bksv)) {
            out_line(&o, "HDCP 1.4: yes");
            out_line(&o, "HDCP repeater: %s", (hdcp->bcaps & 0x40) ? "yes" : "no");
        } else {
            out_line(&o, "HDCP 1.4: key not ready");
            out_line(&o, " (display in standby?)");
        }
        out_line(&o, "HDCP 2.2: %s", (hdcp->v2_read && (hdcp->v2 & 0x04)) ? "yes" : "no");
    }
    if(!scdc->ack) {
        out_line(&o, "SCDC: no reply");
    } else {
        out_line(&o, "SCDC: version %u", scdc->sink_version);
        if(scdc->device_id[0]) out_line(&o, "Chip: %s", scdc->device_id);
        if(scdc->oui[0] || scdc->oui[1] || scdc->oui[2])
            out_line(&o, "Chip OUI: %02X-%02X-%02X", scdc->oui[2], scdc->oui[1], scdc->oui[0]);
    }
}
