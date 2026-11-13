// HDMI Link Monitor: watches the source's +5 V and the display's hot-plug (HPD) line with
// the breakout board in line between a source and a display. Logs every drop with its
// length, and can pull hot-plug low to make the source renegotiate, as if re-plugged.
//
// Wiring (HDMI breakout -> Flipper GPIO header):
//   HDMI 18 (+5V)  -> pin 2  (A7)
//   HDMI 19 (HPD)  -> pin 3  (A6)
//   HDMI 17 (GND)  -> pin 18 (GND)
//
// Both lines are at 5 V. The pins stay plain inputs with no pull resistors, which is the
// mode in which the Flipper's pins tolerate 5 V. For the re-plug pulse the HPD pin is
// driven low and returned to input before the line rises again, so it is never an output
// while at 5 V.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define PIN_5V (&gpio_ext_pa7)
#define PIN_HPD (&gpio_ext_pa6)
#define LOG_SIZE 32
#define LOG_LINES 3
#define DRAW_PERIOD_MS 100

typedef enum { LineHpd, Line5v } Line;

typedef enum { EntryDrop, EntryReplug } EntryKind;

typedef struct {
    EntryKind kind;
    Line line;
    uint32_t at_ms; // since monitoring started
    uint32_t length_ms; // 0 while still low
} Entry;

typedef struct {
    bool level;
    uint32_t fell_at;
    uint32_t drops;
    int open_entry; // log index of a drop still in progress, or -1
} LineState;

static const uint16_t pulse_ms[] = {150, 500, 2000};
#define N_PULSES COUNT_OF(pulse_ms)

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    NotificationApp* notifications;
    bool started;
    uint32_t start_tick;
    LineState lines[2];
    Entry log[LOG_SIZE];
    size_t log_count, log_head, scroll; // scroll: entries back from the newest
    uint32_t last_alert_ms; // alerts are rate-limited
    uint32_t window_start_ms, window_drops; // drops in the current second
    bool unstable; // a line is chattering: almost always an unconnected wire
    uint8_t pulse_idx;
} App;

static uint32_t now_ms(const App* app) {
    return (furi_get_tick() - app->start_tick) * 1000 / furi_kernel_get_tick_frequency();
}

static Entry* log_at(App* app, size_t index) {
    return &app->log[(app->log_head + index) % LOG_SIZE];
}

static int log_add(App* app, EntryKind kind, Line line, uint32_t at, uint32_t length) {
    if(app->log_count == LOG_SIZE) {
        app->log_head = (app->log_head + 1) % LOG_SIZE;
        app->log_count--;
        for(int i = 0; i < 2; i++)
            if(app->lines[i].open_entry >= 0) app->lines[i].open_entry--;
    }
    Entry* e = log_at(app, app->log_count);
    e->kind = kind;
    e->line = line;
    e->at_ms = at;
    e->length_ms = length;
    return (int)app->log_count++;
}

static void pins_input(void) {
    furi_hal_gpio_init(PIN_5V, GpioModeInput, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(PIN_HPD, GpioModeInput, GpioPullNo, GpioSpeedLow);
}

static void sample_line(App* app, Line line, bool level, uint32_t now) {
    LineState* s = &app->lines[line];
    if(level == s->level) return;
    s->level = level;
    if(!level) {
        s->fell_at = now;
        s->drops++;
        s->open_entry = log_add(app, EntryDrop, line, now, 0);
        if(now - app->window_start_ms >= 1000) {
            app->unstable = app->window_drops > 10;
            app->window_start_ms = now;
            app->window_drops = 0;
        }
        if(++app->window_drops > 10) app->unstable = true;
        if(app->last_alert_ms == 0 || now - app->last_alert_ms >= 500) {
            app->last_alert_ms = now ? now : 1;
            notification_message(app->notifications, &sequence_single_vibro);
            notification_message(app->notifications, &sequence_blink_red_100);
        }
    } else {
        uint32_t length = now - s->fell_at;
        if(s->open_entry >= 0) log_at(app, (size_t)s->open_entry)->length_ms = length ? length : 1;
        s->open_entry = -1;
    }
}

static void monitor_start(App* app) {
    pins_input();
    app->start_tick = furi_get_tick();
    app->lines[Line5v] = (LineState){.level = furi_hal_gpio_read(PIN_5V), .open_entry = -1};
    app->lines[LineHpd] = (LineState){.level = furi_hal_gpio_read(PIN_HPD), .open_entry = -1};
    app->started = true;
}

// Pull hot-plug low for a moment so the source re-reads the EDID and renegotiates
static void replug(App* app) {
    uint16_t length = pulse_ms[app->pulse_idx];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    log_add(app, EntryReplug, LineHpd, now_ms(app), length);
    furi_mutex_release(app->mutex);

    furi_hal_gpio_write(PIN_HPD, false);
    furi_hal_gpio_init(PIN_HPD, GpioModeOutputOpenDrain, GpioPullNo, GpioSpeedLow);
    furi_delay_ms(length);
    furi_hal_gpio_init(PIN_HPD, GpioModeInput, GpioPullNo, GpioSpeedLow);
    furi_delay_ms(5); // let the line rise before sampling again
    notification_message(app->notifications, &sequence_blink_blue_100);
}

static void format_length(uint32_t ms, char* out, size_t size) {
    if(ms < 1000)
        snprintf(out, size, "%lu ms", (unsigned long)ms);
    else if(ms < 60000)
        snprintf(out, size, "%lu.%lu s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
    else
        snprintf(out, size, "%lu min", (unsigned long)(ms / 60000));
}

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    char buf[48], len[16];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(!app->started) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Link Monitor");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "HDMI > Flipper");
        canvas_draw_line(canvas, 0, 12, 127, 12);
        canvas_draw_str(canvas, 2, 22, "18 +5V   >  pin 2 (A7)");
        canvas_draw_str(canvas, 2, 31, "19 HPD   >  pin 3 (A6)");
        canvas_draw_str(canvas, 2, 40, "17 GND  >  pin 18 (GND)");
        canvas_draw_str(canvas, 2, 49, "In line: source and display");
        canvas_draw_line(canvas, 0, 52, 127, 52);
        canvas_draw_str(canvas, 2, 62, "OK: start");
        furi_mutex_release(app->mutex);
        return;
    }

    uint32_t now = now_ms(app);
    bool v5 = app->lines[Line5v].level, hpd = app->lines[LineHpd].level;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, v5 ? "+5V on" : "+5V OFF");
    canvas_draw_str(canvas, 66, 10, hpd ? "HPD high" : "HPD LOW");
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buf, sizeof(buf), "drops: %lu", (unsigned long)app->lines[Line5v].drops);
    canvas_draw_str(canvas, 2, 19, buf);
    snprintf(buf, sizeof(buf), "drops: %lu", (unsigned long)app->lines[LineHpd].drops);
    canvas_draw_str(canvas, 66, 19, buf);
    canvas_draw_line(canvas, 0, 21, 127, 21);

    if(app->log_count == 0) {
        snprintf(
            buf, sizeof(buf), "No drops in %02lu:%02lu", (unsigned long)(now / 60000),
            (unsigned long)(now / 1000 % 60));
        canvas_draw_str(canvas, 2, 31, buf);
    }
    for(size_t i = 0; i < LOG_LINES && app->scroll + i < app->log_count; i++) {
        // newest first
        const Entry* e = log_at(app, app->log_count - 1 - app->scroll - i);
        const char* what = e->kind == EntryReplug ? "re-plug sent" : e->line == LineHpd ? "HPD low" : "+5V lost";
        if(e->kind == EntryDrop && e->length_ms == 0) {
            format_length(now - e->at_ms, len, sizeof(len));
            snprintf(
                buf, sizeof(buf), "%02lu:%02lu %s, still (%s)", (unsigned long)(e->at_ms / 60000),
                (unsigned long)(e->at_ms / 1000 % 60), what, len);
        } else {
            format_length(e->length_ms, len, sizeof(len));
            snprintf(
                buf, sizeof(buf), "%02lu:%02lu %s %s", (unsigned long)(e->at_ms / 60000),
                (unsigned long)(e->at_ms / 1000 % 60), what, len);
        }
        canvas_draw_str(canvas, 2, (int32_t)(31 + 9 * i), buf);
    }
    canvas_draw_line(canvas, 0, 53, 127, 53);
    if(app->unstable && now - app->window_start_ms < 3000) {
        canvas_draw_str(canvas, 2, 63, "Chattering: check the wires");
    } else {
        snprintf(buf, sizeof(buf), "Hold OK: re-plug %u ms (L/R)", pulse_ms[app->pulse_idx]);
        canvas_draw_str(canvas, 2, 63, buf);
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t hdmi_monitor_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->pulse_idx = 1;
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_cb, app);
    view_port_input_callback_set(view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    uint32_t last_draw = 0;
    bool running = true;
    while(running) {
        InputEvent event;
        // one tick (1 ms) per loop: the lines are sampled about a thousand times a second
        FuriStatus status = furi_message_queue_get(app->queue, &event, 1);
        bool do_replug = false, redraw = false;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->started) {
            uint32_t now = now_ms(app);
            size_t before = app->log_count;
            bool v5 = app->lines[Line5v].level, hpd = app->lines[LineHpd].level;
            sample_line(app, Line5v, furi_hal_gpio_read(PIN_5V), now);
            sample_line(app, LineHpd, furi_hal_gpio_read(PIN_HPD), now);
            redraw = before != app->log_count || v5 != app->lines[Line5v].level || hpd != app->lines[LineHpd].level;
        }
        if(status == FuriStatusOk) {
            bool step = event.type == InputTypeShort || event.type == InputTypeRepeat;
            redraw = true;
            if(event.key == InputKeyBack && event.type == InputTypeShort) {
                running = false;
            } else if(!app->started) {
                if(event.key == InputKeyOk && event.type == InputTypeShort) monitor_start(app);
            } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
                do_replug = true;
            } else if(event.key == InputKeyRight && step) {
                if(app->pulse_idx + 1u < N_PULSES) app->pulse_idx++;
            } else if(event.key == InputKeyLeft && step) {
                if(app->pulse_idx > 0) app->pulse_idx--;
            } else if(event.key == InputKeyDown && step) {
                if(app->scroll + LOG_LINES < app->log_count) app->scroll++;
            } else if(event.key == InputKeyUp && step) {
                if(app->scroll > 0) app->scroll--;
            }
        }
        furi_mutex_release(app->mutex);

        if(do_replug) {
            replug(app);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            // our own pulse is not a fault: resync without counting it
            app->lines[LineHpd].level = furi_hal_gpio_read(PIN_HPD);
            app->lines[LineHpd].open_entry = -1;
            furi_mutex_release(app->mutex);
            redraw = true;
        }
        uint32_t tick = furi_get_tick();
        if(redraw || tick - last_draw >= furi_ms_to_ticks(DRAW_PERIOD_MS)) {
            last_draw = tick;
            view_port_update(view_port);
        }
    }

    furi_hal_gpio_init(PIN_5V, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(PIN_HPD, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
