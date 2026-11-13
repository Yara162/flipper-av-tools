// HDMI-CEC protocol helpers: bit-level frame decoder and message description.
// No hardware dependencies, so it can be tested on a PC.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CEC_MAX_BYTES 16

// Nominal bit timing in microseconds
#define CEC_START_LOW_US 3700
#define CEC_START_HIGH_US 800
#define CEC_BIT_US 2400
#define CEC_ONE_LOW_US 600
#define CEC_ZERO_LOW_US 1500
#define CEC_SAMPLE_US 1050

#define CEC_ADDR_TV 0
#define CEC_ADDR_BROADCAST 15

typedef struct {
    uint8_t data[CEC_MAX_BYTES]; // data[0] is the header: initiator << 4 | destination
    uint8_t len;
    uint16_t ack_mask; // bit i set: the line was pulled low in the ACK slot of byte i
    bool complete; // ended with an end-of-message bit
    bool tx; // sent by us
} CecFrame;

typedef struct {
    CecFrame frame;
    bool in_frame;
    uint8_t bit; // 0-7 data, 8 end of message, 9 acknowledge
    uint8_t byte;
    bool eom;
} CecDecoder;

void cec_decoder_reset(CecDecoder* d);

// Feed the length of one low pulse on the line. Returns true when `out` holds a frame.
bool cec_decoder_low_pulse(CecDecoder* d, uint32_t low_us, CecFrame* out);

// Emit whatever was received so far (a message cut short). Returns false if nothing pending.
bool cec_decoder_flush(CecDecoder* d, CecFrame* out);

// For a directed message, acknowledged means the destination pulled the ACK slot low.
// For a broadcast, a low ACK slot means some device rejected it.
bool cec_frame_acked(const CecFrame* f);

// One-line summary such as "4>0 Image View On" (at most `size - 1` characters)
void cec_describe(const CecFrame* f, char* out, size_t size);

// Raw bytes as hex, such as "40 04"
void cec_hex(const CecFrame* f, char* out, size_t size);

const char* cec_address_name(uint8_t addr);
