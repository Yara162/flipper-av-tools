// Power Monitor: DC voltage, current and power meter with logging, using an INA219 sensor
// board with its standard 0.1 ohm shunt (marked R100).
//
// Wiring (sensor board -> Flipper GPIO header):
//   VCC -> pin 9  (3V3)
//   GND -> pin 11 (GND)
//   SCL -> pin 16 (C0)
//   SDA -> pin 15 (C1)
//
// Measured circuit: break the positive wire. Supply positive -> Vin+, Vin- -> device positive.
// The supply's negative must also connect to GND, or the voltage reading means nothing.
// Limits: 26 V and 3.2 A. Low-voltage DC only.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <datetime/datetime.h>

#define I2C (&furi_hal_i2c_handle_external)
#define I2C_TIMEOUT 50
#define SAMPLE_MS 100
#define HISTORY 120
#define CHART_EVERY 5 // one chart column per half second

#define INA_REG_CONFIG 0x00
#define INA_REG_SHUNT 0x01
#define INA_REG_BUS 0x02
// 32 V range, +/-320 mV shunt range, 8-sample averaging on both, continuous
#define INA_CONFIG 0x3DDF

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    uint8_t address; // 8-bit I2C address, 0 if not found
    bool sensor_ok, overflow;
    int32_t mv, ma10; // bus millivolts; current in tenths of a milliamp (signed)
    int32_t mv_min, mv_max, ma10_max;
    uint32_t samples, start_tick, last_tick;
    uint64_t nah, nwh; // accumulated charge and energy, in nano-amp-hours and nano-watt-hours
    int16_t history[HISTORY]; // current in mA
    size_t history_count, history_head;
    // logging
    bool logging;
    File* log_file;
    Storage* storage;
    uint32_t log_last_s, log_lines;
    char log_name[40];
} App;

static bool ina_write(App* app, uint8_t reg, uint16_t value) {
    uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    furi_hal_i2c_acquire(I2C);
    bool ok = furi_hal_i2c_tx(I2C, app->address, data, 3, I2C_TIMEOUT);
    furi_hal_i2c_release(I2C);
    return ok;
}

static bool ina_read(App* app, uint8_t reg, uint16_t* value) {
    uint8_t data[2];
    furi_hal_i2c_acquire(I2C);
    bool ok = furi_hal_i2c_trx(I2C, app->address, &reg, 1, data, 2, I2C_TIMEOUT);
    furi_hal_i2c_release(I2C);
    if(ok) *value = (uint16_t)((data[0] << 8) | data[1]);
    return ok;
}

// The board's address jumpers select 0x40, 0x41, 0x44 or 0x45
static bool sensor_find(App* app) {
    static const uint8_t candidates[] = {0x40 << 1, 0x41 << 1, 0x44 << 1, 0x45 << 1};
    for(size_t i = 0; i < COUNT_OF(candidates); i++) {
        furi_hal_i2c_acquire(I2C);
        bool present = furi_hal_i2c_is_device_ready(I2C, candidates[i], I2C_TIMEOUT);
        furi_hal_i2c_release(I2C);
        if(present) {
            app->address = candidates[i];
            return ina_write(app, INA_REG_CONFIG, INA_CONFIG);
        }
    }
    app->address = 0;
    return false;
}

static void stats_reset(App* app) {
    app->samples = 0;
    app->nah = app->nwh = 0;
    app->history_count = app->history_head = 0;
    app->start_tick = app->last_tick = furi_get_tick();
}

static bool sample(App* app) {
    uint16_t shunt, bus;
    if(!ina_read(app, INA_REG_SHUNT, &shunt) || !ina_read(app, INA_REG_BUS, &bus)) return false;
    app->overflow = bus & 1;
    app->mv = (int32_t)(bus >> 3) * 4; // 4 mV per step
    // 10 uV per step across 0.1 ohm is 0.1 mA per step
    app->ma10 = (int16_t)shunt;

    uint32_t now = furi_get_tick();
    uint32_t dt_ms = (now - app->last_tick) * 1000 / furi_kernel_get_tick_frequency();
    app->last_tick = now;
    int32_t ma10_abs = app->ma10 < 0 ? -app->ma10 : app->ma10;
    if(!app->samples) {
        app->mv_min = app->mv_max = app->mv;
        app->ma10_max = ma10_abs;
    } else {
        if(app->mv < app->mv_min) app->mv_min = app->mv;
        if(app->mv > app->mv_max) app->mv_max = app->mv;
        if(ma10_abs > app->ma10_max) app->ma10_max = ma10_abs;
        // 0.1 mA for 1 ms is 1/36 of a nano-amp-hour
        app->nah += (uint64_t)ma10_abs * dt_ms / 36;
        // 1 mV x 0.1 mA for 1 ms is 1/36000 of a nano-watt-hour
        app->nwh += (uint64_t)app->mv * (uint64_t)ma10_abs * dt_ms / 36000;
    }
    app->samples++;
    if(app->samples % CHART_EVERY == 0) {
        app->history[(app->history_head + app->history_count) % HISTORY] = (int16_t)(ma10_abs / 10);
        if(app->history_count < HISTORY)
            app->history_count++;
        else
            app->history_head = (app->history_head + 1) % HISTORY;
    }
    return true;
}

// --- logging to SD Card/apps_data/power_monitor/power_<date>_<time>.csv ---

static void log_stop(App* app) {
    if(!app->logging) return;
    storage_file_close(app->log_file);
    storage_file_free(app->log_file);
    furi_record_close(RECORD_STORAGE);
    app->logging = false;
}

static void log_start(App* app) {
    DateTime t;
    furi_hal_rtc_get_datetime(&t);
    snprintf(
        app->log_name, sizeof(app->log_name), "power_%04u%02u%02u_%02u%02u%02u.csv", t.year, t.month, t.day, t.hour,
        t.minute, t.second);
    app->storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_printf("%s/%s", STORAGE_APP_DATA_PATH_PREFIX, app->log_name);
    storage_common_resolve_path_and_ensure_app_directory(app->storage, path);
    app->log_file = storage_file_alloc(app->storage);
    app->logging = storage_file_open(app->log_file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    furi_string_free(path);
    if(app->logging) {
        static const char header[] = "seconds,volts,amps,watts\n";
        storage_file_write(app->log_file, header, sizeof(header) - 1);
        app->log_last_s = UINT32_MAX;
        app->log_lines = 0;
    } else {
        storage_file_free(app->log_file);
        furi_record_close(RECORD_STORAGE);
    }
}

// One line per second while logging
static void log_tick(App* app) {
    if(!app->logging) return;
    uint32_t s = (furi_get_tick() - app->start_tick) / furi_kernel_get_tick_frequency();
    if(s == app->log_last_s) return;
    app->log_last_s = s;
    int32_t ma10 = app->ma10 < 0 ? -app->ma10 : app->ma10;
    uint32_t mw = (uint32_t)((uint64_t)app->mv * (uint64_t)ma10 / 10000);
    char line[64];
    int n = snprintf(
        line, sizeof(line), "%lu,%ld.%03ld,%s%ld.%04ld,%lu.%03lu\n", (unsigned long)s, (long)(app->mv / 1000),
        (long)(app->mv % 1000), app->ma10 < 0 ? "-" : "", (long)(ma10 / 10000), (long)(ma10 % 10000),
        (unsigned long)(mw / 1000), (unsigned long)(mw % 1000));
    if(storage_file_write(app->log_file, line, (size_t)n) != (size_t)n)
        log_stop(app);
    else
        app->log_lines++;
}

// --- drawing ---

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    char buf[48];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(!app->address && !app->samples) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Power Monitor");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_line(canvas, 0, 12, 127, 12);
        canvas_draw_str(canvas, 2, 21, "VCC > pin 9    GND > pin 11");
        canvas_draw_str(canvas, 2, 30, "SCL > pin 16   SDA > pin 15");
        canvas_draw_str(canvas, 2, 40, "Supply + > Vin+");
        canvas_draw_str(canvas, 2, 49, "Vin- > device +   grounds joined");
        canvas_draw_line(canvas, 0, 52, 127, 52);
        canvas_draw_str(canvas, 2, 62, "Max 26 V, 3.2 A. DC only.");
        furi_mutex_release(app->mutex);
        return;
    }

    int32_t ma10 = app->ma10 < 0 ? -app->ma10 : app->ma10;
    uint32_t mw = (uint32_t)((uint64_t)app->mv * (uint64_t)ma10 / 10000);
    canvas_set_font(canvas, FontPrimary);
    snprintf(buf, sizeof(buf), "%ld.%02ld V", (long)(app->mv / 1000), (long)(app->mv % 1000 / 10));
    canvas_draw_str(canvas, 2, 10, buf);
    if(ma10 < 10000)
        snprintf(buf, sizeof(buf), "%s%ld.%ld mA", app->ma10 < 0 ? "-" : "", (long)(ma10 / 10), (long)(ma10 % 10));
    else
        snprintf(buf, sizeof(buf), "%s%ld.%03ld A", app->ma10 < 0 ? "-" : "", (long)(ma10 / 10000), (long)(ma10 % 10000 / 10));
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%lu.%02lu W", (unsigned long)(mw / 1000), (unsigned long)(mw % 1000 / 10));
    canvas_draw_str(canvas, 2, 20, buf);
    if(!app->sensor_ok)
        canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, "SENSOR LOST");
    else if(app->overflow)
        canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, "OVER RANGE");
    else if(app->logging) {
        snprintf(buf, sizeof(buf), "LOG %lu", (unsigned long)app->log_lines);
        canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, buf);
    }

    // current over the last minute, scaled to its own peak
    const int32_t top = 23, height = 12;
    int32_t peak = 1;
    for(size_t i = 0; i < app->history_count; i++) {
        int16_t v = app->history[(app->history_head + i) % HISTORY];
        if(v > peak) peak = v;
    }
    canvas_draw_frame(canvas, 0, top - 1, 128, (size_t)(height + 2));
    for(size_t i = 0; i < app->history_count; i++) {
        int32_t v = app->history[(app->history_head + i) % HISTORY];
        int32_t h = v * height / peak;
        int32_t x = 4 + (int32_t)(HISTORY - app->history_count + i);
        if(h > 0) canvas_draw_line(canvas, x, top + height - h, x, top + height - 1);
    }

    snprintf(
        buf, sizeof(buf), "V %ld.%02ld to %ld.%02ld", (long)(app->mv_min / 1000), (long)(app->mv_min % 1000 / 10),
        (long)(app->mv_max / 1000), (long)(app->mv_max % 1000 / 10));
    canvas_draw_str(canvas, 2, 45, buf);
    snprintf(buf, sizeof(buf), "pk %ld mA", (long)(app->ma10_max / 10));
    canvas_draw_str_aligned(canvas, 126, 45, AlignRight, AlignBottom, buf);
    uint32_t secs = (furi_get_tick() - app->start_tick) / furi_kernel_get_tick_frequency();
    snprintf(
        buf, sizeof(buf), "%lu mAh  %lu.%02lu Wh  %02lu:%02lu", (unsigned long)(app->nah / 1000000),
        (unsigned long)(app->nwh / 1000000000), (unsigned long)(app->nwh % 1000000000 / 10000000),
        (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    canvas_draw_str(canvas, 2, 54, buf);
    canvas_draw_str(canvas, 2, 63, app->logging ? "OK reset   hold OK: stop log" : "OK reset   hold OK: log to SD");
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t power_monitor_app(void* p) {
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
        FuriStatus status = furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(SAMPLE_MS));
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(status != FuriStatusOk) {
            if(!app->address) {
                if(sensor_find(app)) {
                    app->sensor_ok = true;
                    if(!app->samples) stats_reset(app);
                }
            } else {
                app->sensor_ok = sample(app);
                if(!app->sensor_ok)
                    app->address = 0; // look for it again
                else
                    log_tick(app);
            }
        } else if(event.key == InputKeyBack && event.type == InputTypeShort) {
            running = false;
        } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
            if(!app->logging) stats_reset(app);
        } else if(event.key == InputKeyOk && event.type == InputTypeLong && app->address) {
            if(app->logging) {
                log_stop(app);
            } else {
                stats_reset(app);
                log_start(app);
            }
        }
        furi_mutex_release(app->mutex);
        view_port_update(view_port);
    }

    log_stop(app);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
