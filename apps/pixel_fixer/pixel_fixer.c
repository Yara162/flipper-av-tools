// Pixel Fixer: drives the rapid colour-noise pattern of the combined Video Game Module firmware.
// Nothing is sent to the module until the photosensitivity warning has been confirmed.
// Link: USART pins 13/14 at 9600 (the idle baud rate of the expansion link).
//   run:   P,<mode>,<chunk>,<hold>\n     pause: S,<mode>,6,0,\n (black)     reply: K,...
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <expansion/expansion.h>

#define BAUD 9600
#define TICK_MS 250
#define RESEND_TICKS 2
#define LINK_TIMEOUT_MS 1500
#define PATTERN_BLACK 6

// Block sizes in the module's half-resolution pixels (shown doubled, as display pixels)
static const uint8_t chunks[] = {2, 4, 8, 16, 32, 64};
// Frames each noise image is held
static const uint8_t holds[] = {1, 2, 4, 8, 15, 30};
static const char* const mode_names[] = {"640x480 60Hz", "720x480 60Hz", "1280x720 30Hz*"};
static const uint8_t mode_fps[] = {60, 60, 30};
#define N_CHUNKS COUNT_OF(chunks)
#define N_HOLDS COUNT_OF(holds)
#define N_MODES COUNT_OF(mode_names)

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    Expansion* expansion;
    FuriHalSerialHandle* serial;
    bool confirmed, link_open, paused;
    uint8_t mode, chunk_idx, hold_idx;
    uint32_t run_ticks;
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

// Take the UART away from the expansion-module service (this ends screen mirroring)
static void link_open(App* app) {
    if(app->link_open) return;
    expansion_disable(app->expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, BAUD);
        furi_hal_serial_async_rx_start(app->serial, rx_cb, app, false);
    }
    app->link_open = true;
}

static void link_close(App* app) {
    if(!app->link_open) return;
    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
        app->serial = NULL;
    }
    expansion_enable(app->expansion);
    app->link_open = false;
}

static void send_state(App* app) {
    if(!app->serial) return;
    char line[32];
    int n;
    if(app->paused) {
        n = snprintf(line, sizeof(line), "S,%u,%u,0,\n", app->mode, PATTERN_BLACK);
    } else {
        n = snprintf(
            line, sizeof(line), "P,%u,%u,%u\n", app->mode, chunks[app->chunk_idx], holds[app->hold_idx]);
    }
    furi_hal_serial_tx(app->serial, (const uint8_t*)line, (size_t)n);
}

static void draw_warning(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "SEIZURE WARNING");
    canvas_draw_line(canvas, 0, 12, 127, 12);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 15, AlignCenter, AlignTop, "Rapid flashing colours.");
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "Can trigger seizures in");
    canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignTop, "photosensitive people.");
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignTop, "Do not watch the display.");
    canvas_draw_line(canvas, 0, 53, 127, 53);
    canvas_draw_str_aligned(canvas, 2, 63, AlignLeft, AlignBottom, "Back: exit");
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "Hold OK: start");
}

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(!app->confirmed) {
        draw_warning(canvas);
        furi_mutex_release(app->mutex);
        return;
    }

    char buf[40];
    bool linked = app->got_reply && (furi_get_tick() - app->last_reply) < furi_ms_to_ticks(LINK_TIMEOUT_MS);
    uint32_t secs = app->run_ticks * TICK_MS / 1000;

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, app->paused ? "Paused (black)" : "Running");
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", secs / 3600, secs / 60 % 60, secs % 60);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    snprintf(buf, sizeof(buf), "Chunk: %u px  (L/R)", chunks[app->chunk_idx] * 2);
    canvas_draw_str(canvas, 2, 24, buf);
    uint32_t tenths = (uint32_t)mode_fps[app->mode] * 10 / holds[app->hold_idx];
    snprintf(buf, sizeof(buf), "Speed: %lu.%lu changes/s  (U/D)", tenths / 10, tenths % 10);
    canvas_draw_str(canvas, 2, 34, buf);
    snprintf(buf, sizeof(buf), "Res: %s", mode_names[app->mode]);
    canvas_draw_str(canvas, 2, 44, buf);
    canvas_draw_str(
        canvas, 2, 54, !app->serial ? "Serial port busy" : (linked ? "Module: connected" : "Module: no reply"));
    canvas_draw_str(canvas, 2, 63, "OK pause  hold OK res  Back exit");
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t pixel_fixer_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->chunk_idx = 2; // 16 px blocks
    app->hold_idx = 0; // new image every frame
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->expansion = furi_record_open(RECORD_EXPANSION);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_cb, app);
    view_port_input_callback_set(view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    uint32_t ticks = 0;
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
            } else if(!app->confirmed) {
                // The flashing pattern starts only after a deliberate long press
                if(event.key == InputKeyOk && event.type == InputTypeLong) {
                    app->confirmed = true;
                    link_open(app);
                    changed = true;
                }
            } else if(event.key == InputKeyRight && step) {
                if(app->chunk_idx + 1u < N_CHUNKS) app->chunk_idx++;
                changed = true;
            } else if(event.key == InputKeyLeft && step) {
                if(app->chunk_idx > 0) app->chunk_idx--;
                changed = true;
            } else if(event.key == InputKeyUp && step) {
                if(app->hold_idx > 0) app->hold_idx--;
                changed = true;
            } else if(event.key == InputKeyDown && step) {
                if(app->hold_idx + 1u < N_HOLDS) app->hold_idx++;
                changed = true;
            } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                app->paused = !app->paused;
                changed = true;
            } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
                app->mode = (app->mode + 1) % N_MODES;
                changed = true;
            }
        } else if(app->confirmed) {
            ticks++;
            if(!app->paused) app->run_ticks++;
            // Resend regularly: the module returns to mirroring if the commands stop
            if(ticks % RESEND_TICKS == 0) changed = true;
        }
        if(changed && app->confirmed) send_state(app);
        furi_mutex_release(app->mutex);
        view_port_update(view_port);
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    link_close(app);
    furi_record_close(RECORD_EXPANSION);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
