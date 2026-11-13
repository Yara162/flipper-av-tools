// EDID Reader: reads a display's EDID over the HDMI DDC lines and decodes it.
//
// Wiring (HDMI breakout -> Flipper GPIO header), with only the display connected:
//   HDMI 15 (SCL)  -> pin 16 (C0)
//   HDMI 16 (SDA)  -> pin 15 (C1)
//   HDMI 17 (GND)  -> pin 18 (GND)
//   HDMI 18 (+5V)  -> pin 1  (5V)   powers the display's EDID memory
//
// After the EDID it also asks the display's HDCP port and HDMI 2.0 status registers what
// they support.
//
// The I2C bus is bit-banged slowly on open-drain pins with the Flipper's internal pull-ups,
// so no external resistors are needed.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include "edid.h"

#define PIN_SCL (&gpio_ext_pc0)
#define PIN_SDA (&gpio_ext_pc1)
#define HALF_PERIOD_US 100 // about 5 kHz: slow enough for weak pull-ups and long cables
#define STRETCH_TIMEOUT_US 3000
#define POWER_SETTLE_MS 400
#define REPORT_SIZE 2560
#define MAX_LINES 150
#define VISIBLE_LINES 7

typedef enum { ScreenWiring, ScreenReading, ScreenReport } Screen;

typedef enum { ReadOk, ReadStuckLow, ReadNoAck, ReadBadData } ReadStatus;

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    Screen screen;
    uint8_t edid[EDID_BLOCK_SIZE * EDID_MAX_BLOCKS];
    char report[REPORT_SIZE];
    const char* lines[MAX_LINES];
    HdcpProbe hdcp;
    ScdcProbe scdc;
    size_t line_count, scroll;
} App;

// --- bit-banged I2C master ---

static void i2c_delay(void) {
    furi_delay_us(HALF_PERIOD_US);
}

static void sda_write(bool level) {
    furi_hal_gpio_write(PIN_SDA, level);
}

static void scl_low(void) {
    furi_hal_gpio_write(PIN_SCL, false);
}

// Release SCL and wait for it to actually rise (slow pull-up or clock stretching)
static void scl_high(void) {
    furi_hal_gpio_write(PIN_SCL, true);
    for(uint32_t waited = 0; !furi_hal_gpio_read(PIN_SCL) && waited < STRETCH_TIMEOUT_US; waited += 10)
        furi_delay_us(10);
}

static void i2c_start(void) {
    sda_write(true);
    i2c_delay();
    scl_high();
    i2c_delay();
    sda_write(false);
    i2c_delay();
    scl_low();
    i2c_delay();
}

static void i2c_stop(void) {
    sda_write(false);
    i2c_delay();
    scl_high();
    i2c_delay();
    sda_write(true);
    i2c_delay();
}

static bool i2c_write(uint8_t byte) {
    for(int i = 7; i >= 0; i--) {
        sda_write((byte >> i) & 1);
        i2c_delay();
        scl_high();
        i2c_delay();
        scl_low();
    }
    sda_write(true); // release for the acknowledge bit
    i2c_delay();
    scl_high();
    bool ack = !furi_hal_gpio_read(PIN_SDA);
    i2c_delay();
    scl_low();
    return ack;
}

static uint8_t i2c_read(bool ack) {
    uint8_t byte = 0;
    sda_write(true);
    for(int i = 0; i < 8; i++) {
        i2c_delay();
        scl_high();
        byte = (uint8_t)((byte << 1) | (furi_hal_gpio_read(PIN_SDA) ? 1 : 0));
        i2c_delay();
        scl_low();
    }
    sda_write(!ack);
    i2c_delay();
    scl_high();
    i2c_delay();
    scl_low();
    sda_write(true);
    return byte;
}

static void i2c_pins_init(void) {
    furi_hal_gpio_write(PIN_SCL, true);
    furi_hal_gpio_write(PIN_SDA, true);
    furi_hal_gpio_init(PIN_SCL, GpioModeOutputOpenDrain, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_init(PIN_SDA, GpioModeOutputOpenDrain, GpioPullUp, GpioSpeedLow);
}

static void i2c_pins_release(void) {
    furi_hal_gpio_init(PIN_SCL, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(PIN_SDA, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

// Read one 128-byte EDID block. Blocks 2 and up need the E-DDC segment pointer.
static bool edid_read_block(uint8_t index, uint8_t* dst) {
    uint8_t segment = index / 2, offset = (uint8_t)((index % 2) * EDID_BLOCK_SIZE);
    if(segment) {
        i2c_start();
        i2c_write(0x60); // segment pointer; displays without it simply do not acknowledge
        i2c_write(segment);
    }
    i2c_start();
    bool ok = i2c_write(0xA0) && i2c_write(offset);
    if(ok) {
        i2c_start();
        ok = i2c_write(0xA1);
    }
    if(ok) {
        for(int i = 0; i < EDID_BLOCK_SIZE; i++) dst[i] = i2c_read(i < EDID_BLOCK_SIZE - 1);
    }
    i2c_stop();
    return ok;
}

// Read `len` bytes starting at register `reg` of the device at 8-bit write address `addr`
static bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t* dst, size_t len) {
    i2c_start();
    bool ok = i2c_write(addr) && i2c_write(reg);
    if(ok) {
        i2c_start();
        ok = i2c_write(addr | 1);
    }
    if(ok) {
        for(size_t i = 0; i < len; i++) dst[i] = i2c_read(i < len - 1);
    }
    i2c_stop();
    return ok;
}

// HDCP receiver port (0x74) and HDMI 2.0 status and control registers (0xA8)
static void probe_live(HdcpProbe* hdcp, ScdcProbe* scdc) {
    memset(hdcp, 0, sizeof(*hdcp));
    memset(scdc, 0, sizeof(*scdc));
    hdcp->port_ack = i2c_read_reg(0x74, 0x00, hdcp->bksv, 5);
    if(hdcp->port_ack) {
        i2c_read_reg(0x74, 0x40, &hdcp->bcaps, 1);
        hdcp->v2_read = i2c_read_reg(0x74, 0x50, &hdcp->v2, 1);
    }
    scdc->ack = i2c_read_reg(0xA8, 0x01, &scdc->sink_version, 1);
    if(scdc->ack) {
        uint8_t id[11] = {0};
        if(i2c_read_reg(0xA8, 0xD0, id, sizeof(id))) {
            memcpy(scdc->oui, id, 3);
            for(int i = 0; i < 8; i++) scdc->device_id[i] = (id[3 + i] >= 32 && id[3 + i] < 127) ? (char)id[3 + i] : 0;
        }
    }
}

static ReadStatus edid_read_all(App* app, size_t* blocks) {
    *blocks = 0;
    // Supply +5V to HDMI pin 18 unless it is already on (for example from USB)
    bool otg_was_on = furi_hal_power_is_otg_enabled();
    if(!otg_was_on) furi_hal_power_enable_otg();
    i2c_pins_init();
    furi_delay_ms(POWER_SETTLE_MS);

    ReadStatus status = ReadOk;
    if(!furi_hal_gpio_read(PIN_SCL) || !furi_hal_gpio_read(PIN_SDA)) {
        status = ReadStuckLow;
    } else if(!edid_read_block(0, app->edid)) {
        // one retry: some displays need a moment after +5V appears
        furi_delay_ms(POWER_SETTLE_MS);
        if(!edid_read_block(0, app->edid)) status = ReadNoAck;
    }
    if(status == ReadOk) {
        *blocks = 1;
        size_t wanted = 1 + (size_t)app->edid[126];
        if(wanted > EDID_MAX_BLOCKS) wanted = EDID_MAX_BLOCKS;
        bool all_ff = true, all_00 = true;
        for(int i = 0; i < EDID_BLOCK_SIZE; i++) {
            if(app->edid[i] != 0xFF) all_ff = false;
            if(app->edid[i] != 0x00) all_00 = false;
        }
        if(all_ff || all_00) {
            status = ReadBadData;
        } else if(app->edid[0] == 0x00 && app->edid[1] == 0xFF) {
            for(uint8_t i = 1; i < wanted; i++) {
                if(!edid_read_block(i, app->edid + i * EDID_BLOCK_SIZE)) break;
                (*blocks)++;
            }
        }
    }

    if(status == ReadOk) probe_live(&app->hdcp, &app->scdc);

    i2c_pins_release();
    if(!otg_was_on) furi_hal_power_disable_otg();
    return status;
}

// --- report ---

static void split_lines(App* app) {
    app->line_count = 0;
    app->scroll = 0;
    char* p = app->report;
    while(*p && app->line_count < MAX_LINES) {
        app->lines[app->line_count++] = p;
        char* nl = strchr(p, '\n');
        if(!nl) break;
        *nl = 0;
        p = nl + 1;
    }
}

static bool save_file(Storage* storage, const char* name, const void* data, size_t size) {
    FuriString* path = furi_string_alloc_printf("%s/%s", STORAGE_APP_DATA_PATH_PREFIX, name);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
              storage_file_write(file, data, size) == size;
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    return ok;
}

static void do_read(App* app) {
    size_t blocks = 0;
    ReadStatus status = edid_read_all(app, &blocks);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(status == ReadStuckLow) {
        snprintf(
            app->report, REPORT_SIZE,
            "A DDC line is held low.\nCheck that SCL and SDA\nare not shorted to GND\nand that no source is\nconnected to the board.\n\nOK: try again");
    } else if(status == ReadNoAck) {
        snprintf(
            app->report, REPORT_SIZE,
            "No reply from display.\nCheck: wires on HDMI\npins 15, 16, 17, 18;\nSCL/SDA not swapped;\ndisplay plugged in and\npowered.\nOK: try again");
    } else if(status == ReadBadData) {
        snprintf(
            app->report, REPORT_SIZE,
            "Display answered but\nsent blank data.\nIts EDID memory may be\nempty or unpowered\n(check the +5V wire).\n\nOK: try again");
    } else {
        EdidInfo info;
        edid_decode(app->edid, blocks, app->report, REPORT_SIZE, &info);
        if(info.header_ok) {
            edid_format_live(&app->hdcp, &app->scdc, app->report, REPORT_SIZE);
            Storage* storage = furi_record_open(RECORD_STORAGE);
            char name[24];
            snprintf(name, sizeof(name), "%s%04X.bin", info.mfr, info.product);
            bool saved = save_file(storage, name, app->edid, blocks * EDID_BLOCK_SIZE);
            snprintf(name, sizeof(name), "%s%04X.txt", info.mfr, info.product);
            saved = save_file(storage, name, app->report, strlen(app->report)) && saved;
            furi_record_close(RECORD_STORAGE);
            size_t len = strlen(app->report);
            snprintf(
                app->report + len, REPORT_SIZE - len, "%s %s%04X\n", saved ? "Saved as" : "Save failed:", info.mfr,
                info.product);
        }
    }
    split_lines(app);
    app->screen = ScreenReport;
    furi_mutex_release(app->mutex);
}

// --- UI ---

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(app->screen == ScreenWiring) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "EDID Reader");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "HDMI > Flipper");
        canvas_draw_line(canvas, 0, 12, 127, 12);
        canvas_draw_str(canvas, 2, 22, "15 SCL   >  pin 16 (C0)");
        canvas_draw_str(canvas, 2, 31, "16 SDA   >  pin 15 (C1)");
        canvas_draw_str(canvas, 2, 40, "17 GND  >  pin 18 (GND)");
        canvas_draw_str(canvas, 2, 49, "18 +5V   >  pin 1 (5V)");
        canvas_draw_line(canvas, 0, 52, 127, 52);
        canvas_draw_str(canvas, 2, 62, "Display only.  OK: read");
    } else if(app->screen == ScreenReading) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Reading EDID...");
    } else {
        canvas_set_font(canvas, FontSecondary);
        for(size_t i = 0; i < VISIBLE_LINES && app->scroll + i < app->line_count; i++)
            canvas_draw_str(canvas, 1, (int32_t)(8 + 9 * i), app->lines[app->scroll + i]);
        if(app->line_count > VISIBLE_LINES) {
            // scroll bar
            size_t track = 62, bar = track * VISIBLE_LINES / app->line_count;
            size_t pos = (track - bar) * app->scroll / (app->line_count - VISIBLE_LINES);
            canvas_draw_line(canvas, 127, (int32_t)(1 + pos), 127, (int32_t)(1 + pos + bar));
        }
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t edid_reader_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_cb, app);
    view_port_input_callback_set(view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent event;
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        bool step = event.type == InputTypeShort || event.type == InputTypeRepeat;
        bool read_now = false;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(event.key == InputKeyBack && event.type == InputTypeShort) {
            if(app->screen == ScreenReport)
                app->screen = ScreenWiring;
            else
                running = false;
        } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
            app->screen = ScreenReading;
            read_now = true;
        } else if(app->screen == ScreenReport && step) {
            size_t max_scroll = app->line_count > VISIBLE_LINES ? app->line_count - VISIBLE_LINES : 0;
            if(event.key == InputKeyDown && app->scroll < max_scroll) app->scroll++;
            if(event.key == InputKeyUp && app->scroll > 0) app->scroll--;
        }
        furi_mutex_release(app->mutex);
        view_port_update(view_port);

        if(read_now) {
            furi_delay_ms(50); // let the "Reading" screen draw
            do_read(app);
            view_port_update(view_port);
        }
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
