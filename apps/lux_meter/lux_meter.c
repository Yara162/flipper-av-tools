// Lux Meter: light meter and nine-point projector brightness test using a BH1750 sensor
// board (GY-302).
//
// Wiring (sensor board -> Flipper GPIO header):
//   VCC -> pin 9  (3V3)
//   GND -> pin 11 (GND)
//   SCL -> pin 16 (C0)
//   SDA -> pin 15 (C1)
//   ADDR: leave unconnected
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <datetime/datetime.h>

#define I2C (&furi_hal_i2c_handle_external)
#define I2C_TIMEOUT 50
#define SAMPLE_MS 150
#define SMOOTH 4

// BH1750 commands
#define BH_POWER_ON 0x01
#define BH_CONT_HIRES 0x10 // 1 lux steps, up to about 54,000 lux
#define BH_CONT_HIRES2 0x11 // 0.5 lux steps, up to about 27,000 lux

typedef enum { ScreenWiring, ScreenLive, ScreenSetup, ScreenGrid, ScreenResult } Screen;

typedef struct {
    const char* name;
    uint16_t w, h;
} Aspect;

static const Aspect aspects[] = {{"16:9", 160, 90}, {"16:10", 160, 100}, {"4:3", 40, 30}, {"2.35:1", 235, 100}};
#define N_ASPECTS COUNT_OF(aspects)

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    Screen screen;
    uint8_t address; // 8-bit I2C address of the sensor, 0 if not found
    uint8_t mode;
    bool sensor_ok, hold;
    uint32_t lux10; // latest reading in tenths of a lux
    uint32_t recent[SMOOTH];
    uint8_t recent_count, recent_pos;
    uint32_t min10, max10;
    uint64_t sum10;
    uint32_t samples;
    // nine-point test
    uint16_t diagonal_in;
    uint8_t aspect;
    uint32_t point[9];
    uint8_t point_count;
    uint32_t avg10, lumens, area_cm2;
    int16_t low_pct, high_pct, ratio_pct;
    bool saved;
    char file_name[40];
} App;

// --- sensor ---

static bool bh_command(App* app, uint8_t command) {
    furi_hal_i2c_acquire(I2C);
    bool ok = furi_hal_i2c_tx(I2C, app->address, &command, 1, I2C_TIMEOUT);
    furi_hal_i2c_release(I2C);
    return ok;
}

static bool sensor_find(App* app) {
    static const uint8_t candidates[] = {0x23 << 1, 0x5C << 1};
    for(size_t i = 0; i < COUNT_OF(candidates); i++) {
        furi_hal_i2c_acquire(I2C);
        bool present = furi_hal_i2c_is_device_ready(I2C, candidates[i], I2C_TIMEOUT);
        furi_hal_i2c_release(I2C);
        if(present) {
            app->address = candidates[i];
            app->mode = BH_CONT_HIRES2;
            return bh_command(app, BH_POWER_ON) && bh_command(app, app->mode);
        }
    }
    app->address = 0;
    return false;
}

// Reads one sample into tenths of a lux; switches range when the reading is near either end
static bool sensor_read(App* app, uint32_t* lux10) {
    uint8_t data[2];
    furi_hal_i2c_acquire(I2C);
    bool ok = furi_hal_i2c_rx(I2C, app->address, data, 2, I2C_TIMEOUT);
    furi_hal_i2c_release(I2C);
    if(!ok) return false;
    uint32_t raw = ((uint32_t)data[0] << 8) | data[1];
    // lux = raw / 1.2, halved again in the finer mode
    *lux10 = app->mode == BH_CONT_HIRES2 ? raw * 100 / 24 : raw * 100 / 12;
    if(app->mode == BH_CONT_HIRES2 && raw > 60000) {
        app->mode = BH_CONT_HIRES;
        bh_command(app, app->mode);
    } else if(app->mode == BH_CONT_HIRES && raw < 20000) {
        app->mode = BH_CONT_HIRES2;
        bh_command(app, app->mode);
    }
    return true;
}

static void stats_reset(App* app) {
    app->samples = 0;
    app->sum10 = 0;
    app->min10 = app->max10 = 0;
}

static uint32_t smoothed(const App* app) {
    if(app->recent_count == 0) return app->lux10;
    uint32_t sum = 0;
    for(uint8_t i = 0; i < app->recent_count; i++) sum += app->recent[i];
    return sum / app->recent_count;
}

// --- nine-point test ---

// Image area in square centimetres from the diagonal in inches and the aspect ratio
static uint32_t image_area_cm2(uint16_t diagonal_in, const Aspect* a) {
    // width = d * w / sqrt(w^2 + h^2); keep it in integers by working in tenths of a millimetre
    uint64_t d = (uint64_t)diagonal_in * 254; // 0.1 mm
    uint64_t hyp2 = (uint64_t)a->w * a->w + (uint64_t)a->h * a->h;
    // area = d^2 * w * h / hyp2, in (0.1 mm)^2; 1 cm^2 = 10^4 of those
    return (uint32_t)(d * d * a->w * a->h / hyp2 / 10000);
}

static void compute_result(App* app) {
    uint64_t sum = 0;
    uint32_t lo = app->point[0], hi = app->point[0];
    for(int i = 0; i < 9; i++) {
        sum += app->point[i];
        if(app->point[i] < lo) lo = app->point[i];
        if(app->point[i] > hi) hi = app->point[i];
    }
    app->avg10 = (uint32_t)(sum / 9);
    app->area_cm2 = image_area_cm2(app->diagonal_in, &aspects[app->aspect]);
    // lumens = average lux x area in square metres
    app->lumens = (uint32_t)((uint64_t)app->avg10 * app->area_cm2 / 100000);
    if(app->avg10) {
        app->low_pct = (int16_t)(((int64_t)lo - app->avg10) * 100 / app->avg10);
        app->high_pct = (int16_t)(((int64_t)hi - app->avg10) * 100 / app->avg10);
    } else {
        app->low_pct = app->high_pct = 0;
    }
    app->ratio_pct = hi ? (int16_t)((uint64_t)lo * 100 / hi) : 0;
}

static void save_result(App* app) {
    DateTime t;
    furi_hal_rtc_get_datetime(&t);
    snprintf(
        app->file_name, sizeof(app->file_name), "lux_%04u%02u%02u_%02u%02u%02u.txt", t.year, t.month, t.day, t.hour,
        t.minute, t.second);
    char* text = malloc(1024);
    int n = snprintf(
        text, 1024,
        "Lux Meter: nine-point brightness test\nDate: %04u-%02u-%02u %02u:%02u:%02u\n\n"
        "Image: %u inch diagonal, %s (%lu.%02lu square metres)\n\nReadings in lux, as seen facing the screen:\n",
        t.year, t.month, t.day, t.hour, t.minute, t.second, app->diagonal_in, aspects[app->aspect].name,
        (unsigned long)(app->area_cm2 / 10000), (unsigned long)(app->area_cm2 % 10000 / 100));
    for(int row = 0; row < 3; row++) {
        for(int col = 0; col < 3; col++)
            n += snprintf(
                text + n, 1024 - (size_t)n, "%8lu.%lu", (unsigned long)(app->point[row * 3 + col] / 10),
                (unsigned long)(app->point[row * 3 + col] % 10));
        n += snprintf(text + n, 1024 - (size_t)n, "\n");
    }
    n += snprintf(
        text + n, 1024 - (size_t)n,
        "\nAverage: %lu.%lu lux\nBrightness: about %lu lumens (average x image area)\n"
        "Dimmest point: %d%% from average\nBrightest point: %+d%% from average\n"
        "Dimmest as a share of brightest: %d%%\n\nThe sensor is not calibrated; treat lumens as an estimate.\n",
        (unsigned long)(app->avg10 / 10), (unsigned long)(app->avg10 % 10), (unsigned long)app->lumens, app->low_pct,
        app->high_pct, app->ratio_pct);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_printf("%s/%s", STORAGE_APP_DATA_PATH_PREFIX, app->file_name);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    app->saved = storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
                 storage_file_write(file, text, (size_t)n) == (size_t)n;
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    free(text);
}

// --- drawing ---

static void format_lux(uint32_t lux10, char* out, size_t size) {
    if(lux10 < 10000)
        snprintf(out, size, "%lu.%lu", (unsigned long)(lux10 / 10), (unsigned long)(lux10 % 10));
    else
        snprintf(out, size, "%lu", (unsigned long)(lux10 / 10));
}

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    char buf[48], num[16];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(app->screen == ScreenWiring) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Lux Meter");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "BH1750 > Flipper");
        canvas_draw_line(canvas, 0, 12, 127, 12);
        canvas_draw_str(canvas, 2, 22, "VCC  >  pin 9 (3V3)");
        canvas_draw_str(canvas, 2, 31, "GND  >  pin 11 (GND)");
        canvas_draw_str(canvas, 2, 40, "SCL   >  pin 16 (C0)");
        canvas_draw_str(canvas, 2, 49, "SDA  >  pin 15 (C1)");
        canvas_draw_line(canvas, 0, 52, 127, 52);
        canvas_draw_str(canvas, 2, 62, "Looking for the sensor...");
    } else if(app->screen == ScreenLive) {
        format_lux(app->lux10, num, sizeof(num));
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, 2, 18, num);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, app->hold ? "HOLD" : "lux");
        canvas_set_font(canvas, FontSecondary);
        // one foot-candle is 10.764 lux
        uint32_t fc10 = (uint32_t)((uint64_t)app->lux10 * 1000 / 10764);
        snprintf(buf, sizeof(buf), "%lu.%lu fc", (unsigned long)(fc10 / 10), (unsigned long)(fc10 % 10));
        canvas_draw_str_aligned(canvas, 126, 19, AlignRight, AlignBottom, buf);
        canvas_draw_line(canvas, 0, 22, 127, 22);
        if(!app->sensor_ok) {
            canvas_draw_str(canvas, 2, 33, "Sensor not answering: check wires");
        } else if(app->samples) {
            snprintf(
                buf, sizeof(buf), "min %lu  max %lu", (unsigned long)(app->min10 / 10), (unsigned long)(app->max10 / 10));
            canvas_draw_str(canvas, 2, 32, buf);
            snprintf(buf, sizeof(buf), "average %lu lux", (unsigned long)(app->sum10 / app->samples / 10));
            canvas_draw_str(canvas, 2, 41, buf);
        }
        canvas_draw_str(canvas, 2, 54, "OK reset   hold OK freeze");
        canvas_draw_str(canvas, 2, 63, "Right: nine-point test");
    } else if(app->screen == ScreenSetup) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Nine-point test");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_line(canvas, 0, 12, 127, 12);
        snprintf(buf, sizeof(buf), "Image diagonal: %u in  (U/D)", app->diagonal_in);
        canvas_draw_str(canvas, 2, 24, buf);
        snprintf(buf, sizeof(buf), "Shape: %s  (L/R)", aspects[app->aspect].name);
        canvas_draw_str(canvas, 2, 34, buf);
        uint32_t area = image_area_cm2(app->diagonal_in, &aspects[app->aspect]);
        snprintf(
            buf, sizeof(buf), "Area: %lu.%02lu sq m", (unsigned long)(area / 10000), (unsigned long)(area % 10000 / 100));
        canvas_draw_str(canvas, 2, 44, buf);
        canvas_draw_str(canvas, 2, 54, "Show a full white image.");
        canvas_draw_str(canvas, 2, 63, "OK: start   Back: cancel");
    } else if(app->screen == ScreenGrid) {
        // 3 x 3 grid on the left, live reading on the right
        for(int i = 0; i < 9; i++) {
            int32_t x = 2 + (i % 3) * 22, y = 2 + (i / 3) * 16;
            if(i == app->point_count)
                canvas_draw_box(canvas, x, y, 21, 15);
            else
                canvas_draw_frame(canvas, x, y, 21, 15);
            if(i < app->point_count) {
                snprintf(buf, sizeof(buf), "%lu", (unsigned long)(app->point[i] / 10));
                canvas_draw_str_aligned(canvas, x + 10, y + 11, AlignCenter, AlignBottom, buf);
            }
        }
        format_lux(smoothed(app), num, sizeof(num));
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 72, 12, num);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 72, 21, "lux");
        snprintf(buf, sizeof(buf), "Point %u of 9", app->point_count + 1);
        canvas_draw_str(canvas, 72, 33, buf);
        canvas_draw_str(canvas, 72, 43, "OK: take");
        canvas_draw_str(canvas, 72, 52, "Left: redo");
        canvas_draw_str(canvas, 2, 63, "Sensor at the filled square");
    } else {
        format_lux(app->avg10, num, sizeof(num));
        snprintf(buf, sizeof(buf), "Average %s lux", num);
        canvas_draw_str(canvas, 2, 9, buf);
        canvas_set_font(canvas, FontPrimary);
        snprintf(buf, sizeof(buf), "~%lu lumens", (unsigned long)app->lumens);
        canvas_draw_str(canvas, 2, 22, buf);
        canvas_set_font(canvas, FontSecondary);
        snprintf(buf, sizeof(buf), "Dimmest %d%%  brightest %+d%%", app->low_pct, app->high_pct);
        canvas_draw_str(canvas, 2, 33, buf);
        snprintf(buf, sizeof(buf), "Dimmest is %d%% of brightest", app->ratio_pct);
        canvas_draw_str(canvas, 2, 42, buf);
        canvas_draw_str(canvas, 2, 53, app->saved ? app->file_name : "Could not save the report");
        canvas_draw_str(canvas, 2, 63, "OK: again   Back: meter");
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t lux_meter_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->diagonal_in = 100;
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_cb, app);
    view_port_input_callback_set(view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool running = true, do_save = false;
    while(running) {
        InputEvent event;
        FuriStatus status = furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(SAMPLE_MS));
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(status != FuriStatusOk) {
            // time for a sample
            if(!app->address) {
                if(sensor_find(app) && app->screen == ScreenWiring) app->screen = ScreenLive;
            } else {
                uint32_t lux10;
                app->sensor_ok = sensor_read(app, &lux10);
                if(!app->sensor_ok) {
                    app->address = 0; // look for it again
                } else if(!app->hold) {
                    app->lux10 = lux10;
                    app->recent[app->recent_pos] = lux10;
                    app->recent_pos = (uint8_t)((app->recent_pos + 1) % SMOOTH);
                    if(app->recent_count < SMOOTH) app->recent_count++;
                    if(!app->samples || lux10 < app->min10) app->min10 = lux10;
                    if(!app->samples || lux10 > app->max10) app->max10 = lux10;
                    app->sum10 += lux10;
                    app->samples++;
                }
            }
        } else {
            bool is_short = event.type == InputTypeShort, is_long = event.type == InputTypeLong;
            bool step = is_short || event.type == InputTypeRepeat;
            if(event.key == InputKeyBack && is_short) {
                if(app->screen == ScreenWiring || app->screen == ScreenLive)
                    running = false;
                else
                    app->screen = ScreenLive;
            } else if(app->screen == ScreenLive) {
                if(event.key == InputKeyOk && is_short) stats_reset(app);
                if(event.key == InputKeyOk && is_long) app->hold = !app->hold;
                if(event.key == InputKeyRight && is_short) {
                    app->hold = false;
                    app->screen = ScreenSetup;
                }
            } else if(app->screen == ScreenSetup) {
                if(event.key == InputKeyUp && step && app->diagonal_in < 600) app->diagonal_in += 5;
                if(event.key == InputKeyDown && step && app->diagonal_in > 20) app->diagonal_in -= 5;
                if(event.key == InputKeyRight && is_short) app->aspect = (uint8_t)((app->aspect + 1) % N_ASPECTS);
                if(event.key == InputKeyLeft && is_short)
                    app->aspect = (uint8_t)((app->aspect + N_ASPECTS - 1) % N_ASPECTS);
                if(event.key == InputKeyOk && is_short) {
                    app->point_count = 0;
                    app->screen = ScreenGrid;
                }
            } else if(app->screen == ScreenGrid) {
                if(event.key == InputKeyOk && is_short && app->sensor_ok) {
                    app->point[app->point_count++] = smoothed(app);
                    if(app->point_count == 9) {
                        compute_result(app);
                        do_save = true;
                    }
                }
                if(event.key == InputKeyLeft && is_short && app->point_count > 0) app->point_count--;
            } else if(app->screen == ScreenResult) {
                if(event.key == InputKeyOk && is_short) app->screen = ScreenSetup;
            }
        }
        furi_mutex_release(app->mutex);

        if(do_save) {
            do_save = false;
            save_result(app);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->screen = ScreenResult;
            furi_mutex_release(app->mutex);
        }
        view_port_update(view_port);
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
