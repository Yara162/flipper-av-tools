// Flipper Zero remote control for the Video Game Module test pattern firmware.
// Sends "S,<mode>,<pattern>,<overlay>,<label>\n" on the USART pins (13/14) at 9600 (the idle baud rate of the expansion link).
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <expansion/expansion.h>

#define BAUD 9600
#define TICK_MS 250
#define RESEND_TICKS 2
#define AUTO_TICKS 20
#define LINK_TIMEOUT_MS 1500

// Order must match the enum in the module firmware
static const char* const pattern_names[] = {
    "Colour bars", "Bars 100%", "White", "Red", "Green", "Blue", "Black",
    "Grey 50%", "Grey ramp", "Crosshatch", "Border/safe", "Checkerboard", "Motion",
};
static const char* const mode_names[] = {"640x480 60Hz", "720x480 60Hz", "1280x720 30Hz*"};
#define N_PATTERNS COUNT_OF(pattern_names)
#define N_MODES COUNT_OF(mode_names)

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    FuriHalSerialHandle* serial;
    uint8_t mode, pattern, input;
    bool overlay, autocycle;
    volatile uint32_t last_reply;
    volatile bool got_reply;
} App;

static void rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    App* app = context;
    if(event & FuriHalSerialRxEventData) {
        if(furi_hal_serial_async_rx(handle) == 'K') {
            app->last_reply = furi_get_tick();
            app->got_reply = true;
        }
    }
}

static void send_state(App* app) {
    if(!app->serial) return;
    char line[40];
    int n = snprintf(
        line, sizeof(line), "S,%u,%u,%u,INPUT %u\n", app->mode, app->pattern, app->overlay ? 1 : 0, app->input);
    furi_hal_serial_tx(app->serial, (const uint8_t*)line, (size_t)n);
}

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    char buf[40];
    bool linked = app->got_reply && (furi_get_tick() - app->last_reply) < furi_ms_to_ticks(LINK_TIMEOUT_MS);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, pattern_names[app->pattern]);
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%u/%u", app->pattern + 1, (unsigned)N_PATTERNS);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    snprintf(buf, sizeof(buf), "Res: %s", mode_names[app->mode]);
    canvas_draw_str(canvas, 2, 24, buf);
    snprintf(buf, sizeof(buf), "Label: INPUT %u (%s)", app->input, app->overlay ? "shown" : "hidden");
    canvas_draw_str(canvas, 2, 34, buf);
    snprintf(buf, sizeof(buf), "Auto-cycle: %s", app->autocycle ? "on" : "off");
    canvas_draw_str(canvas, 2, 44, buf);
    canvas_draw_str(canvas, 2, 54, !app->serial ? "Serial port busy" : (linked ? "Module: connected" : "Module: no reply"));
    canvas_draw_str(canvas, 2, 63, "U/D pat  L/R in  OK lbl  hold: res/auto");
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t vgm_tpg_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->input = 1;
    app->overlay = true;
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    // Take the UART away from the expansion-module service while the app runs
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, BAUD);
        furi_hal_serial_async_rx_start(app->serial, rx_cb, app, false);
    }

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_cb, app);
    view_port_input_callback_set(view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    send_state(app);
    uint32_t ticks = 0, auto_ticks = 0;
    bool running = true;
    while(running) {
        InputEvent event;
        bool changed = false;
        FuriStatus status = furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(TICK_MS));
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(status == FuriStatusOk) {
            bool step = event.type == InputTypeShort || event.type == InputTypeRepeat;
            if(event.key == InputKeyBack && event.type == InputTypeShort) {
                running = false;
            } else if(event.key == InputKeyUp && step) {
                app->pattern = (app->pattern + N_PATTERNS - 1) % N_PATTERNS;
                changed = true;
            } else if(event.key == InputKeyDown && event.type == InputTypeShort) {
                app->pattern = (app->pattern + 1) % N_PATTERNS;
                changed = true;
            } else if(event.key == InputKeyDown && event.type == InputTypeLong) {
                app->autocycle = !app->autocycle;
                auto_ticks = 0;
            } else if(event.key == InputKeyRight && step) {
                app->input = app->input >= 99 ? 1 : app->input + 1;
                changed = true;
            } else if(event.key == InputKeyLeft && step) {
                app->input = app->input <= 1 ? 99 : app->input - 1;
                changed = true;
            } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                app->overlay = !app->overlay;
                changed = true;
            } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
                app->mode = (app->mode + 1) % N_MODES;
                changed = true;
            }
        } else {
            ticks++;
            if(app->autocycle && ++auto_ticks >= AUTO_TICKS) {
                auto_ticks = 0;
                app->pattern = (app->pattern + 1) % N_PATTERNS;
                changed = true;
            }
            // Resend regularly so the module recovers its state after a reboot or replug
            if(ticks % RESEND_TICKS == 0) changed = true;
        }
        if(changed) send_state(app);
        furi_mutex_release(app->mutex);
        view_port_update(view_port);
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }
    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
