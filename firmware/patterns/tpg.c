#include "tpg.h"
#include <string.h>
#include "font_8x8.h"

#ifndef TPG_FUNC
#include <pico/platform.h>
#define TPG_FUNC(name) __not_in_flash_func(name)
#endif

#define FONT_N_CHARS 95
#define FONT_FIRST 32
#define N_TMPL 6
#define N_SCRATCH 4

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define WHITE RGB(255, 255, 255)
#define BLACK 0
#define GREY50 RGB(128, 128, 128)
#define GREEN RGB(0, 255, 0)
#define YELLOW RGB(255, 255, 0)

// Two template sets: one on screen, one being prepared
static uint16_t tmpl[2][N_TMPL][TPG_MAX_W];
static uint16_t scratch[N_SCRATCH][TPG_MAX_W];
static uint16_t motion[2][TPG_MAX_W];
static uint16_t noise[N_SCRATCH][TPG_MAX_W];
static int noise_idx;
static uint32_t noise_epoch;

static int W, H, FPS, cell;
static const char* mode_name;
static TpgState cur, pending, submitted;
static volatile bool pending_valid;
static int active_set, scratch_idx, motion_idx;
static uint32_t frame_ctr;
static char timecode[12];
static int box_x, box_y, box_w, box_h, big_scale, small_scale, big_x, big_y, small_x, small_y;

static void TPG_FUNC(fill)(uint16_t* buf, int x0, int x1, uint16_t c) {
    if(x0 < 0) x0 = 0;
    if(x1 > W) x1 = W;
    for(int x = x0; x < x1; x++) buf[x] = c;
}

static uint16_t grey(int level, int max) {
    int g6 = level * 63 / max;
    return (uint16_t)(((g6 >> 1) << 11) | (g6 << 5) | (g6 >> 1));
}

static void bars(uint16_t* t, const uint16_t* colours, int n) {
    for(int x = 0; x < W; x++) t[x] = colours[x * n / W];
}

static void build_templates(int pattern, uint16_t t[N_TMPL][TPG_MAX_W]) {
    static const uint16_t bars75[] = {RGB(191, 191, 191), RGB(191, 191, 0), RGB(0, 191, 191), RGB(0, 191, 0),
                                      RGB(191, 0, 191),   RGB(191, 0, 0),   RGB(0, 0, 191)};
    static const uint16_t bars75rev[] = {RGB(0, 0, 191),   BLACK, RGB(191, 0, 191), BLACK,
                                         RGB(0, 191, 191), BLACK, RGB(191, 191, 191)};
    static const uint16_t bars100[] = {WHITE, YELLOW, RGB(0, 255, 255), GREEN,
                                       RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), BLACK};
    static const uint16_t solids[] = {WHITE, RGB(255, 0, 0), GREEN, RGB(0, 0, 255), BLACK, GREY50};
    for(int i = 0; i < N_TMPL; i++) fill(t[i], 0, W, BLACK);

    switch(pattern) {
    case TpgBarsSmpte: {
        bars(t[0], bars75, 7);
        bars(t[1], bars75rev, 7);
        // bottom row: -I, white, +Q, black, then near-black steps for setting brightness
        int u = W * 5 / 28;
        fill(t[2], 0, u, RGB(0, 33, 76));
        fill(t[2], u, 2 * u, WHITE);
        fill(t[2], 2 * u, 3 * u, RGB(50, 0, 106));
        int p0 = W * 5 / 7, pw = W / 21;
        fill(t[2], p0 + pw, p0 + 2 * pw, grey(1, 31));
        fill(t[2], p0 + 2 * pw, p0 + 3 * pw, grey(2, 31));
        break;
    }
    case TpgBars100:
        bars(t[0], bars100, 8);
        break;
    case TpgWhite: case TpgRed: case TpgGreen: case TpgBlue: case TpgBlack: case TpgGrey:
        fill(t[0], 0, W, solids[pattern - TpgWhite]);
        break;
    case TpgRamp:
        for(int x = 0; x < W; x++) {
            int g6 = x * 63 / (W - 1);
            t[0][x] = grey(g6, 63);
            t[1][x] = grey(x * 11 / W, 10);
            t[2][x] = (uint16_t)((g6 >> 1) << 11);
            t[3][x] = (uint16_t)(g6 << 5);
            t[4][x] = (uint16_t)(g6 >> 1);
        }
        break;
    case TpgCrosshatch:
        for(int x = 0; x < W; x += cell) t[0][x] = WHITE;
        t[0][W - 1] = WHITE;
        fill(t[1], 0, W, WHITE);
        break;
    case TpgBorder: {
        int ix = W / 20;
        t[0][W / 2] = GREY50;
        t[0][ix] = t[0][W - 1 - ix] = GREEN;
        t[0][0] = t[0][W - 1] = WHITE;
        fill(t[1], 0, W, WHITE);
        memcpy(t[2], t[0], sizeof(t[0]));
        fill(t[2], ix, W - ix, GREEN);
        memcpy(t[3], t[0], sizeof(t[0]));
        fill(t[3], 1, W - 1, GREY50);
        t[3][ix] = t[3][W - 1 - ix] = GREEN;
        break;
    }
    case TpgChecker:
        for(int x = 0; x < W; x++) {
            bool on = (x / cell) & 1;
            t[0][x] = on ? WHITE : BLACK;
            t[1][x] = on ? BLACK : WHITE;
        }
        break;
    default:
        break;
    }
}

static int TPG_FUNC(row_template)(int y) {
    switch(cur.pattern) {
    case TpgBarsSmpte:
        return y < H * 2 / 3 ? 0 : (y < H * 3 / 4 ? 1 : 2);
    case TpgRamp:
        if(y < H / 4) return 0;
        if(y < H / 2) return 1;
        return 2 + (y - H / 2) * 3 / (H - H / 2);
    case TpgCrosshatch:
        return (y % cell == 0 || y == H - 1) ? 1 : 0;
    case TpgBorder: {
        int iy = H / 20;
        if(y == 0 || y == H - 1) return 1;
        if(y == iy || y == H - 1 - iy) return 2;
        if(y == H / 2) return 3;
        return 0;
    }
    case TpgChecker:
        return (y / cell) & 1;
    default:
        return 0;
    }
}

// One scanline of 8x8 text scaled by `scale`; font bit 0 is the leftmost pixel
static void TPG_FUNC(text_line)(uint16_t* buf, int y, const char* s, int x0, int y0, int scale, uint16_t fg) {
    int row = (y - y0) / scale;
    for(int x = x0; *s; s++, x += 8 * scale) {
        int c = *s;
        if(c < FONT_FIRST || c >= FONT_FIRST + FONT_N_CHARS) c = '?';
        uint8_t bits = (uint8_t)font_8x8[row * FONT_N_CHARS + c - FONT_FIRST];
        for(int b = 0; b < 8; b++)
            if(bits & (1u << b)) fill(buf, x + b * scale, x + (b + 1) * scale, fg);
    }
}

static void TPG_FUNC(layout_overlay)(void) {
    big_scale = W >= 640 ? 5 : 3;
    small_scale = W >= 640 ? 2 : 1;
    int big_w = (int)strlen(cur.label) * 8 * big_scale;
    int small_w = (int)strlen(mode_name) * 8 * small_scale;
    int pad = 4 * small_scale;
    box_w = (big_w > small_w ? big_w : small_w) + 2 * pad;
    box_h = 8 * big_scale + 8 * small_scale + 3 * pad;
    box_x = (W - box_w) / 2;
    box_y = (H - box_h) / 2;
    big_x = (W - big_w) / 2;
    big_y = box_y + pad;
    small_x = (W - small_w) / 2;
    small_y = big_y + 8 * big_scale + pad;
}

static void TPG_FUNC(two_digits)(char* dst, uint32_t v) {
    dst[0] = (char)('0' + (v / 10) % 10);
    dst[1] = (char)('0' + v % 10);
}

static void TPG_FUNC(start_frame)(void) {
    if(pending_valid) {
        cur = pending;
        active_set ^= 1;
        layout_overlay();
        pending_valid = false;
    }
    frame_ctr++;
    if(cur.pattern == TpgPixelFix) {
        uint32_t hold = cur.hold ? cur.hold : 1;
        if(frame_ctr % hold == 0) noise_epoch++;
    }
    if(cur.pattern == TpgMotion) {
        uint32_t f = frame_ctr % (uint32_t)FPS, s = frame_ctr / (uint32_t)FPS;
        two_digits(timecode, s / 60);
        timecode[2] = ':';
        two_digits(timecode + 3, s % 60);
        timecode[5] = ':';
        two_digits(timecode + 6, f);
        timecode[8] = 0;

        motion_idx ^= 1;
        int bw = W / 16, step = FPS == 30 ? 4 : 2;
        int x = (int)((frame_ctr * (uint32_t)step) % (uint32_t)(W + bw)) - bw;
        fill(motion[motion_idx], 0, W, BLACK);
        fill(motion[motion_idx], x, x + bw, WHITE);
    }
}

// Random blocks of the eight full-intensity colours. One row of blocks is generated
// once and reused for every scanline it covers.
static uint16_t* TPG_FUNC(noise_line)(int y) {
    static const uint16_t palette[8] = {BLACK, RGB(255, 0, 0), GREEN, RGB(0, 0, 255),
                                        YELLOW, RGB(0, 255, 255), RGB(255, 0, 255), WHITE};
    int chunk = cur.chunk >= 2 ? cur.chunk : 8;
    if(y % chunk != 0 && y != 0) return noise[noise_idx];

    noise_idx = (noise_idx + 1) % N_SCRATCH;
    uint16_t* p = noise[noise_idx];
    uint16_t* end = p + W;
    uint32_t r = (noise_epoch * 2654435761u) ^ ((uint32_t)(y / chunk) * 40503u + 0x9E3779B9u);
    int bits = 0;
    uint32_t pool = 0;
    while(p < end) {
        if(bits < 3) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            pool = r;
            bits = 30;
        }
        uint16_t c = palette[pool & 7];
        pool >>= 3;
        bits -= 3;
        uint16_t* stop = p + chunk;
        if(stop > end) stop = end;
        while(p < stop) *p++ = c;
    }
    return noise[noise_idx];
}

uint16_t* TPG_FUNC(tpg_scanline)(int y) {
    if(y == 0) start_frame();
    if(cur.pattern == TpgPixelFix) return noise_line(y);

    bool is_motion = cur.pattern == TpgMotion;
    uint16_t* base = is_motion ? motion[motion_idx] : tmpl[active_set][row_template(y)];
    int tc_scale = 2 * small_scale, tc_y = H - 24 * small_scale;
    bool in_box = cur.overlay && y >= box_y && y < box_y + box_h;
    bool in_tc = is_motion && y >= tc_y && y < tc_y + 8 * tc_scale;
    if(!in_box && !in_tc) return base;

    uint16_t* buf = scratch[scratch_idx];
    scratch_idx = (scratch_idx + 1) % N_SCRATCH;
    memcpy(buf, base, (size_t)W * sizeof(uint16_t));
    if(in_tc) text_line(buf, y, timecode, 8 * small_scale, tc_y, tc_scale, YELLOW);
    if(in_box) {
        fill(buf, box_x, box_x + box_w, BLACK);
        if(y >= big_y && y < big_y + 8 * big_scale) text_line(buf, y, cur.label, big_x, big_y, big_scale, WHITE);
        if(y >= small_y && y < small_y + 8 * small_scale)
            text_line(buf, y, mode_name, small_x, small_y, small_scale, YELLOW);
    }
    return buf;
}

bool tpg_submit(const TpgState* state) {
    if(pending_valid) return false;
    if(memcmp(state, &submitted, sizeof(TpgState)) == 0) return true;
    build_templates(state->pattern, tmpl[active_set ^ 1]);
    submitted = *state;
    pending = *state;
    pending_valid = true;
    return true;
}

void tpg_init(int w, int h, int fps, const char* name, const TpgState* initial) {
    W = w > TPG_MAX_W ? TPG_MAX_W : w;
    H = h;
    FPS = fps;
    mode_name = name;
    cell = W >= 640 ? 40 : 20;
    memset(&submitted, 0, sizeof(submitted));
    memset(&cur, 0, sizeof(cur));
    memcpy(&cur, initial, sizeof(TpgState));
    cur.label[TPG_LABEL_MAX] = 0;
    submitted = cur;
    active_set = 0;
    build_templates(cur.pattern, tmpl[0]);
    layout_overlay();
    strcpy(timecode, "00:00:00");
}
