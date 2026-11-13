// Test pattern generator: renders half-resolution RGB565 scanlines.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TPG_MAX_W 640
#define TPG_LABEL_MAX 12

enum {
    TpgBarsSmpte, TpgBars100, TpgWhite, TpgRed, TpgGreen, TpgBlue, TpgBlack, TpgGrey,
    TpgRamp, TpgCrosshatch, TpgBorder, TpgChecker, TpgMotion, TpgPixelFix, TpgPatternCount
};

typedef struct {
    uint8_t mode, pattern, overlay;
    char label[TPG_LABEL_MAX + 1];
    // TpgPixelFix only: block size in half-resolution pixels, and frames each image is held
    uint8_t chunk, hold;
} TpgState;

// Call once before video starts. w/h are the half-resolution frame size.
void tpg_init(int w, int h, int fps, const char* mode_name, const TpgState* initial);

// Task context. Prepares a new state; it takes effect at the next frame start.
// Returns false if the previous submission has not been picked up yet (retry shortly).
bool tpg_submit(const TpgState* state);

// Video interrupt context. Returns the pixels for line y; y == 0 starts a new frame.
// The returned buffer stays valid for at least the next three calls.
uint16_t* tpg_scanline(int y);

#ifdef __cplusplus
}
#endif
