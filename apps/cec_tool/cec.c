#include "cec.h"
#include <stdio.h>
#include <string.h>

void cec_decoder_reset(CecDecoder* d) {
    memset(d, 0, sizeof(*d));
}

bool cec_decoder_flush(CecDecoder* d, CecFrame* out) {
    bool pending = d->in_frame && d->frame.len > 0;
    if(pending) {
        *out = d->frame;
        out->complete = false;
    }
    cec_decoder_reset(d);
    return pending;
}

bool cec_decoder_low_pulse(CecDecoder* d, uint32_t low_us, CecFrame* out) {
    // Start bit: 3.7 ms low. Anything already collected is a message that was cut short.
    if(low_us >= 3400 && low_us <= 4000) {
        bool had = cec_decoder_flush(d, out);
        d->in_frame = true;
        return had;
    }
    if(!d->in_frame) return false;

    bool one = low_us >= 350 && low_us <= 850;
    bool zero = low_us >= 1250 && low_us <= 1750;
    if(!one && !zero) return cec_decoder_flush(d, out); // not a valid bit: abandon the frame

    if(d->bit < 8) {
        d->byte = (uint8_t)((d->byte << 1) | (one ? 1 : 0));
        d->bit++;
    } else if(d->bit == 8) {
        d->eom = one;
        d->bit++;
    } else {
        CecFrame* f = &d->frame;
        if(f->len < CEC_MAX_BYTES) {
            f->data[f->len] = d->byte;
            if(zero) f->ack_mask |= (uint16_t)(1u << f->len);
            f->len++;
        }
        d->bit = 0;
        d->byte = 0;
        if(d->eom || f->len >= CEC_MAX_BYTES) {
            *out = *f;
            out->complete = d->eom;
            cec_decoder_reset(d);
            return true;
        }
    }
    return false;
}

bool cec_frame_acked(const CecFrame* f) {
    if(f->len == 0) return false;
    uint16_t all = (uint16_t)((1u << f->len) - 1);
    bool broadcast = (f->data[0] & 0x0F) == CEC_ADDR_BROADCAST;
    return broadcast ? (f->ack_mask & all) == 0 : (f->ack_mask & all) == all;
}

const char* cec_address_name(uint8_t addr) {
    static const char* const names[16] = {"TV", "Recorder 1", "Recorder 2", "Tuner 1", "Playback 1", "Audio system",
                                          "Tuner 2", "Tuner 3", "Playback 2", "Recorder 3", "Tuner 4", "Playback 3",
                                          "Reserved", "Reserved", "Free use", "Broadcast"};
    return names[addr & 15];
}

typedef struct {
    uint8_t op;
    const char* name;
} OpName;

static const OpName op_names[] = {
    {0x00, "Refused"}, {0x04, "Image View On"}, {0x0D, "Text View On"}, {0x1A, "Give Deck Status"},
    {0x1B, "Deck Status"}, {0x32, "Set Menu Lang"}, {0x36, "Standby"}, {0x44, "Key"}, {0x45, "Key Release"},
    {0x46, "Give OSD Name"}, {0x47, "OSD Name"}, {0x64, "Set OSD String"}, {0x70, "Sys Audio Req"},
    {0x71, "Give Audio Status"}, {0x72, "Set Sys Audio"}, {0x7A, "Audio Status"}, {0x7D, "Give Sys Audio"},
    {0x7E, "Sys Audio"}, {0x80, "Routing"}, {0x81, "Routing Info"}, {0x82, "Active Source"},
    {0x83, "Give Phys Addr"}, {0x84, "Phys Addr"}, {0x85, "Req Active Source"}, {0x86, "Set Stream Path"},
    {0x87, "Vendor ID"}, {0x89, "Vendor Cmd"}, {0x8C, "Give Vendor ID"}, {0x8D, "Menu Request"},
    {0x8E, "Menu Status"}, {0x8F, "Give Power Status"}, {0x90, "Power"}, {0x91, "Get Menu Lang"},
    {0x9D, "Inactive Source"}, {0x9E, "CEC Version"}, {0x9F, "Get CEC Version"}, {0xA0, "Vendor Cmd+ID"},
    {0xA5, "Give Features"}, {0xA6, "Features"}, {0xC0, "Initiate ARC"}, {0xC1, "ARC Initiated"},
    {0xC2, "ARC Terminated"}, {0xC3, "Request ARC On"}, {0xC4, "Request ARC Off"}, {0xC5, "Terminate ARC"},
    {0xFF, "Abort"},
};

static const char* op_name(uint8_t op) {
    for(size_t i = 0; i < sizeof(op_names) / sizeof(op_names[0]); i++)
        if(op_names[i].op == op) return op_names[i].name;
    return NULL;
}

static const char* key_name(uint8_t key) {
    switch(key) {
    case 0x00: return "Select";
    case 0x01: return "Up";
    case 0x02: return "Down";
    case 0x03: return "Left";
    case 0x04: return "Right";
    case 0x09: return "Root Menu";
    case 0x0D: return "Exit";
    case 0x40: return "Power";
    case 0x41: return "Vol Up";
    case 0x42: return "Vol Down";
    case 0x43: return "Mute";
    case 0x44: return "Play";
    case 0x45: return "Stop";
    case 0x46: return "Pause";
    case 0x69: return "Input Select";
    case 0x6B: return "Power Toggle";
    case 0x6C: return "Power Off";
    case 0x6D: return "Power On";
    default: return NULL;
    }
}

static size_t append(char* out, size_t size, size_t len, const char* fmt, unsigned a, unsigned b, unsigned c, unsigned d) {
    if(len >= size) return len;
    int n = snprintf(out + len, size - len, fmt, a, b, c, d);
    return n > 0 ? len + (size_t)n : len;
}

void cec_hex(const CecFrame* f, char* out, size_t size) {
    size_t len = 0;
    if(size) out[0] = 0;
    for(uint8_t i = 0; i < f->len && len + 3 < size; i++)
        len += (size_t)snprintf(out + len, size - len, i ? " %02X" : "%02X", f->data[i]);
}

void cec_describe(const CecFrame* f, char* out, size_t size) {
    if(size == 0) return;
    out[0] = 0;
    if(f->len == 0) return;
    const uint8_t* p = f->data + 2; // operands
    int n = f->len - 2;
    size_t len = (size_t)snprintf(out, size, "%X>%X ", f->data[0] >> 4, f->data[0] & 15);
    if(len >= size) return;
    if(f->len == 1) {
        snprintf(out + len, size - len, "Poll");
        return;
    }
    uint8_t op = f->data[1];
    const char* name = op_name(op);
    if(name)
        len += (size_t)snprintf(out + len, size - len, "%s", name);
    else
        len += (size_t)snprintf(out + len, size - len, "Op %02X", op);
    if(len >= size || n <= 0) return;

    switch(op) {
    case 0x82: case 0x84: case 0x86: case 0x9D: case 0x70: case 0x81: // physical address
        if(n >= 2) append(out, size, len, " %u.%u.%u.%u", p[0] >> 4, p[0] & 15, p[1] >> 4, p[1] & 15);
        break;
    case 0x80: // routing change: new address
        if(n >= 4) append(out, size, len, " >%u.%u.%u.%u", p[2] >> 4, p[2] & 15, p[3] >> 4, p[3] & 15);
        break;
    case 0x90: {
        static const char* const st[] = {" On", " Standby", " To On", " To Standby"};
        if(p[0] < 4) snprintf(out + len, size - len, "%s", st[p[0]]);
        break;
    }
    case 0x44: {
        const char* k = key_name(p[0]);
        if(k)
            snprintf(out + len, size - len, " %s", k);
        else
            append(out, size, len, " %02X", p[0], 0, 0, 0);
        break;
    }
    case 0x47: case 0x64: { // text
        int skip = op == 0x64 ? 1 : 0;
        if(len + 1 < size) out[len++] = ' ';
        for(int i = skip; i < n && len + 1 < size; i++) out[len++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
        out[len] = 0;
        break;
    }
    case 0x87:
        if(n >= 3) append(out, size, len, " %02X%02X%02X", p[0], p[1], p[2], 0);
        break;
    case 0x9E: {
        const char* v = p[0] == 4 ? " 1.3a" : p[0] == 5 ? " 1.4" : p[0] == 6 ? " 2.0" : " ?";
        snprintf(out + len, size - len, "%s", v);
        break;
    }
    case 0x00:
        if(n >= 2) append(out, size, len, " op %02X r%u", p[0], p[1], 0, 0);
        break;
    case 0x7A:
        append(out, size, len, (p[0] & 0x80) ? " mute %u%%" : " %u%%", p[0] & 0x7F, 0, 0, 0);
        break;
    case 0x72: case 0x7E:
        snprintf(out + len, size - len, "%s", p[0] ? " On" : " Off");
        break;
    default:
        for(int i = 0; i < n && len + 3 < size; i++) len = append(out, size, len, " %02X", p[i], 0, 0, 0);
        break;
    }
}
