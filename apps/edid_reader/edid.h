// EDID decoder: turns raw EDID blocks into a short text report (lines of at most 25 characters,
// sized for the Flipper's screen). No hardware dependencies, so it can be tested on a PC.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EDID_BLOCK_SIZE 128
#define EDID_MAX_BLOCKS 4

typedef struct {
    char mfr[4]; // three-letter manufacturer ID
    uint16_t product;
    bool header_ok;
    bool checksums_ok;
    uint8_t extensions; // extension count declared by block 0
    // preferred (native) timing, 0 if absent
    uint16_t native_w, native_h, native_hz;
    // capability summary
    bool has_cta, hdmi, audio, hdr10, hlg;
    bool vic_4k30, vic_4k60, only_420_4k60;
    uint16_t max_tmds_mhz; // 0 if not stated
    uint8_t max_frl_gbps; // HDMI 2.1 fixed rate link, 0 if none
    bool scdc; // display declares the HDMI 2.0 status registers
    uint8_t phys_addr[2]; // HDMI/CEC physical address
    bool has_phys_addr;
} EdidInfo;

bool edid_block_checksum_ok(const uint8_t* block);

// Decode `blocks` 128-byte blocks. Writes a NUL-terminated report into `out` and fills `info`.
void edid_decode(const uint8_t* data, size_t blocks, char* out, size_t out_size, EdidInfo* info);

// --- Live checks: registers read from the display itself, beyond the EDID ---

typedef struct {
    bool port_ack; // something answered at the HDCP address
    uint8_t bksv[5]; // HDCP 1.4 receiver key selection vector
    uint8_t bcaps;
    bool v2_read; // the HDCP2Version register could be read
    uint8_t v2;
} HdcpProbe;

typedef struct {
    bool ack; // something answered at the SCDC address
    uint8_t sink_version;
    uint8_t oui[3];
    char device_id[9];
} ScdcProbe;

// A valid HDCP 1.4 key selection vector has exactly 20 one bits
bool hdcp_bksv_valid(const uint8_t* bksv);

// Appends a "Live check" section to a report produced by edid_decode
void edid_format_live(const HdcpProbe* hdcp, const ScdcProbe* scdc, char* out, size_t out_size);
