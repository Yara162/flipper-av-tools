// CEC Tool: watches HDMI-CEC traffic and sends common commands.
//
// Wiring (HDMI breakout -> Flipper GPIO header). The board can sit in line between a
// source and a display, or be connected to a display alone:
//   HDMI 13 (CEC)  -> pin 4  (A4)
//   HDMI 17 (GND)  -> pin 18 (GND)
//
// CEC is a single 3.3 V open-drain wire, so it connects directly. Received bits are timed
// in a pin interrupt; transmitted bits are bit-banged with the interrupt switched off.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include "cec.h"

// A4 is used because its interrupt line (4) is not taken by the firmware; line 7 (A7)
// belongs to expansion-module detection and the button pins hold several others.
#define PIN_CEC (&gpio_ext_pa4)
#define OWN_ADDRESS 0xE // "free use" logical address
#define LOG_SIZE 48
#define LIST_LINES 5
#define BUS_FREE_US 17000 // signal-free time before sending: 7 bit periods
#define BUS_WAIT_TIMEOUT_US 500000
#define IDLE_FLUSH_US 30000

typedef enum { ViewMonitor, ViewMenu, ViewScan } View;

typedef enum { EventInput, EventFrame } EventType;

typedef struct {
    EventType type;
    InputEvent input;
    CecFrame frame;
} Event;

typedef enum { CmdScan, CmdFrame, CmdKey, CmdInput } CmdKind;

typedef struct {
    const char* label;
    CmdKind kind;
    uint8_t dest;
    uint8_t opcode; // or key code for CmdKey
} Command;

static const Command commands[] = {
    {"Scan bus", CmdScan, 0, 0},
    {"TV: power on", CmdFrame, CEC_ADDR_TV, 0x04},
    {"TV: standby", CmdFrame, CEC_ADDR_TV, 0x36},
    {"Everything: standby", CmdFrame, CEC_ADDR_BROADCAST, 0x36},
    {"TV: go to HDMI", CmdInput, CEC_ADDR_BROADCAST, 0x82},
    {"TV: volume up", CmdKey, CEC_ADDR_TV, 0x41},
    {"TV: volume down", CmdKey, CEC_ADDR_TV, 0x42},
    {"TV: mute", CmdKey, CEC_ADDR_TV, 0x43},
    {"Audio: volume up", CmdKey, 5, 0x41},
    {"Audio: volume down", CmdKey, 5, 0x42},
    {"Ask TV power state", CmdFrame, CEC_ADDR_TV, 0x8F},
    {"Ask TV name", CmdFrame, CEC_ADDR_TV, 0x46},
    {"Ask active source", CmdFrame, CEC_ADDR_BROADCAST, 0x85},
};
#define N_COMMANDS COUNT_OF(commands)

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    View view;
    // receive side, touched by the pin interrupt
    CecDecoder decoder;
    uint32_t fall_time, last_edge, cycles_per_us;
    bool low;
    // log
    CecFrame log[LOG_SIZE];
    size_t log_count, log_head, cursor;
    bool follow;
    // menu and scan
    size_t menu_idx;
    uint8_t hdmi_input;
    uint16_t scan_found;
    const char* status;
} App;

static uint32_t now_cycles(void) {
    return furi_hal_cortex_timer_get(0).start;
}

// --- receive ---

static void cec_isr(void* context) {
    App* app = context;
    uint32_t now = now_cycles();
    bool level = furi_hal_gpio_read(PIN_CEC);
    if(!level) {
        app->fall_time = now;
        app->low = true;
    } else if(app->low) {
        app->low = false;
        uint32_t low_us = (now - app->fall_time) / app->cycles_per_us;
        Event event = {.type = EventFrame};
        if(cec_decoder_low_pulse(&app->decoder, low_us, &event.frame)) furi_message_queue_put(app->queue, &event, 0);
    }
    app->last_edge = now;
}

static void rx_start(App* app) {
    cec_decoder_reset(&app->decoder);
    app->low = false;
    furi_hal_gpio_init(PIN_CEC, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_add_int_callback(PIN_CEC, cec_isr, app);
}

static void rx_stop(void) {
    furi_hal_gpio_remove_int_callback(PIN_CEC);
}

// --- log ---

static void log_add(App* app, const CecFrame* frame) {
    size_t slot = (app->log_head + app->log_count) % LOG_SIZE;
    if(app->log_count == LOG_SIZE) {
        app->log_head = (app->log_head + 1) % LOG_SIZE;
        if(app->cursor > 0) app->cursor--;
    } else {
        app->log_count++;
    }
    app->log[slot] = *frame;
    if(app->follow) app->cursor = app->log_count - 1;
}

static const CecFrame* log_get(const App* app, size_t index) {
    return &app->log[(app->log_head + index) % LOG_SIZE];
}

// --- transmit ---

// Send one bit; returns the line level at the sample point (true = high)
static bool tx_bit(bool one) {
    bool level = true;
    FURI_CRITICAL_ENTER();
    furi_hal_gpio_write(PIN_CEC, false);
    furi_delay_us(one ? CEC_ONE_LOW_US : CEC_ZERO_LOW_US);
    furi_hal_gpio_write(PIN_CEC, true);
    if(one) {
        furi_delay_us(CEC_SAMPLE_US - CEC_ONE_LOW_US);
        level = furi_hal_gpio_read(PIN_CEC);
    }
    FURI_CRITICAL_EXIT();
    furi_delay_us(one ? CEC_BIT_US - CEC_SAMPLE_US : CEC_BIT_US - CEC_ZERO_LOW_US);
    return level;
}

static bool wait_bus_free(void) {
    uint32_t free_us = 0, waited = 0;
    while(free_us < BUS_FREE_US) {
        if(furi_hal_gpio_read(PIN_CEC))
            free_us += 100;
        else
            free_us = 0;
        furi_delay_us(100);
        waited += 100;
        if(waited > BUS_WAIT_TIMEOUT_US) return false;
    }
    return true;
}

// Transmit one frame with the pin already in open-drain mode. Fills `result` with what happened.
static bool tx_frame_once(const uint8_t* bytes, uint8_t len, CecFrame* result) {
    memset(result, 0, sizeof(*result));
    result->tx = true;
    if(!wait_bus_free()) return false;

    FURI_CRITICAL_ENTER();
    furi_hal_gpio_write(PIN_CEC, false);
    furi_delay_us(CEC_START_LOW_US);
    furi_hal_gpio_write(PIN_CEC, true);
    FURI_CRITICAL_EXIT();
    furi_delay_us(CEC_START_HIGH_US);

    for(uint8_t i = 0; i < len; i++) {
        for(int bit = 7; bit >= 0; bit--) {
            bool one = (bytes[i] >> bit) & 1;
            // reading low while sending a one means another device is talking: back off
            if(!tx_bit(one) && one) return false;
        }
        tx_bit(i == len - 1); // end of message
        bool ack_low = !tx_bit(true); // acknowledge slot
        result->data[i] = bytes[i];
        result->len = (uint8_t)(i + 1);
        if(ack_low) result->ack_mask |= (uint16_t)(1u << i);
        bool broadcast = (bytes[0] & 0x0F) == CEC_ADDR_BROADCAST;
        if(!broadcast && !ack_low) return false; // nobody there: stop, as the standard requires
    }
    result->complete = true;
    return true;
}

// Send a frame (one retry for directed messages) and log every attempt. Returns true if acknowledged.
static bool tx_frame(App* app, const uint8_t* bytes, uint8_t len, bool retry, bool log) {
    bool ok = false;
    rx_stop();
    furi_hal_gpio_write(PIN_CEC, true);
    furi_hal_gpio_init(PIN_CEC, GpioModeOutputOpenDrain, GpioPullUp, GpioSpeedLow);
    for(int attempt = 0; attempt < (retry ? 2 : 1) && !ok; attempt++) {
        CecFrame result;
        bool sent = tx_frame_once(bytes, len, &result);
        ok = sent && cec_frame_acked(&result);
        if(log && result.len) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            log_add(app, &result);
            furi_mutex_release(app->mutex);
        }
        if(!result.len) app->status = "Bus busy: nothing sent";
    }
    rx_start(app);
    return ok;
}

static void run_command(App* app, const Command* cmd) {
    uint8_t header = (uint8_t)((OWN_ADDRESS << 4) | cmd->dest);
    app->status = NULL;
    switch(cmd->kind) {
    case CmdScan: {
        uint16_t found = 0;
        for(uint8_t addr = 0; addr < 15; addr++) {
            if(addr == OWN_ADDRESS) continue;
            uint8_t poll = (uint8_t)((OWN_ADDRESS << 4) | addr);
            if(tx_frame(app, &poll, 1, false, false)) found |= (uint16_t)(1u << addr);
        }
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->scan_found = found;
        app->view = ViewScan;
        furi_mutex_release(app->mutex);
        return;
    }
    case CmdFrame: {
        uint8_t frame[2] = {header, cmd->opcode};
        tx_frame(app, frame, 2, true, true);
        break;
    }
    case CmdKey: {
        uint8_t press[3] = {header, 0x44, cmd->opcode}, release[2] = {header, 0x45};
        if(tx_frame(app, press, 3, true, true)) tx_frame(app, release, 2, true, true);
        break;
    }
    case CmdInput: {
        // Announce ourselves as the active source on that input; the TV follows
        uint8_t frame[4] = {header, cmd->opcode, (uint8_t)(app->hdmi_input << 4), 0x00};
        tx_frame(app, frame, 4, true, true);
        break;
    }
    }
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->view = ViewMonitor;
    app->follow = true;
    if(app->log_count) app->cursor = app->log_count - 1;
    furi_mutex_release(app->mutex);
}

// --- UI ---

static void draw_monitor(Canvas* canvas, App* app) {
    char line[40];
    canvas_set_font(canvas, FontSecondary);
    if(app->log_count == 0) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "CEC Tool");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "HDMI > Flipper");
        canvas_draw_line(canvas, 0, 12, 127, 12);
        canvas_draw_str(canvas, 2, 23, "13 CEC   >  pin 4 (A4)");
        canvas_draw_str(canvas, 2, 32, "17 GND  >  pin 18 (GND)");
        canvas_draw_str(canvas, 2, 44, app->status ? app->status : "Listening...");
        canvas_draw_line(canvas, 0, 52, 127, 52);
        canvas_draw_str(canvas, 2, 62, "OK: send a command");
        return;
    }
    size_t first = app->cursor >= LIST_LINES ? app->cursor - LIST_LINES + 1 : 0;
    for(size_t i = 0; i < LIST_LINES && first + i < app->log_count; i++) {
        const CecFrame* f = log_get(app, first + i);
        int32_t y = (int32_t)(8 + 9 * i);
        cec_describe(f, line, 26);
        if(first + i == app->cursor) {
            canvas_draw_box(canvas, 0, y - 8, 128, 9);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 1, y, line);
        if(!cec_frame_acked(f) || !f->complete) canvas_draw_str(canvas, 122, y, "x");
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_draw_line(canvas, 0, 46, 127, 46);
    const CecFrame* f = log_get(app, app->cursor);
    char hex[26];
    cec_hex(f, hex, sizeof(hex));
    canvas_draw_str(canvas, 1, 55, hex);
    const char* state = !f->complete ? "cut short" : cec_frame_acked(f) ? "ok" : "NO ACK";
    snprintf(
        line, sizeof(line), "%s%.9s>%.9s %s", f->tx ? "TX " : "", cec_address_name(f->data[0] >> 4),
        cec_address_name(f->data[0] & 15), state);
    canvas_draw_str(canvas, 1, 63, line);
}

static void draw_menu(Canvas* canvas, App* app) {
    char line[32];
    canvas_set_font(canvas, FontSecondary);
    size_t first = app->menu_idx >= 6 ? app->menu_idx - 5 : 0;
    for(size_t i = 0; i < 6 && first + i < N_COMMANDS; i++) {
        const Command* cmd = &commands[first + i];
        int32_t y = (int32_t)(8 + 9 * i);
        if(cmd->kind == CmdInput)
            snprintf(line, sizeof(line), "%s %u  (L/R)", cmd->label, app->hdmi_input);
        else
            snprintf(line, sizeof(line), "%s", cmd->label);
        if(first + i == app->menu_idx) {
            canvas_draw_box(canvas, 0, y - 8, 128, 9);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, line);
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_draw_str(canvas, 2, 63, "OK: send   Back: monitor");
}

static void draw_scan(Canvas* canvas, App* app) {
    char line[32];
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Devices on the bus");
    canvas_set_font(canvas, FontSecondary);
    int row = 0;
    for(uint8_t addr = 0; addr < 15 && row < 5; addr++) {
        if(!(app->scan_found & (1u << addr))) continue;
        snprintf(line, sizeof(line), "%X  %s", addr, cec_address_name(addr));
        canvas_draw_str(canvas, 2, 21 + 9 * row++, line);
    }
    if(row == 0) {
        canvas_draw_str(canvas, 2, 24, "Nothing answered.");
        canvas_draw_str(canvas, 2, 33, "Check the CEC wire, and that");
        canvas_draw_str(canvas, 2, 42, "CEC is enabled on the display.");
    }
}

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(app->view == ViewMonitor)
        draw_monitor(canvas, app);
    else if(app->view == ViewMenu)
        draw_menu(canvas, app);
    else
        draw_scan(canvas, app);
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* input, void* context) {
    App* app = context;
    Event event = {.type = EventInput, .input = *input};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

int32_t cec_tool_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->queue = furi_message_queue_alloc(16, sizeof(Event));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->cycles_per_us = furi_hal_cortex_instructions_per_microsecond();
    app->follow = true;
    app->hdmi_input = 1;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_cb, app);
    view_port_input_callback_set(view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    rx_start(app);

    bool running = true;
    while(running) {
        Event event;
        FuriStatus status = furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(100));
        const Command* to_run = NULL;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(status != FuriStatusOk) {
            // a message that stopped mid-way (nobody acknowledged) never completes: collect it
            CecFrame partial;
            bool have = false;
            FURI_CRITICAL_ENTER();
            if(app->decoder.in_frame && !app->low &&
               (now_cycles() - app->last_edge) / app->cycles_per_us > IDLE_FLUSH_US)
                have = cec_decoder_flush(&app->decoder, &partial);
            FURI_CRITICAL_EXIT();
            if(have) log_add(app, &partial);
        } else if(event.type == EventFrame) {
            log_add(app, &event.frame);
        } else {
            const InputEvent* in = &event.input;
            bool step = in->type == InputTypeShort || in->type == InputTypeRepeat;
            if(app->view == ViewMonitor) {
                if(in->key == InputKeyBack && in->type == InputTypeShort) running = false;
                if(in->key == InputKeyOk && in->type == InputTypeShort) app->view = ViewMenu;
                if(in->key == InputKeyUp && step && app->cursor > 0) {
                    app->cursor--;
                    app->follow = false;
                }
                if(in->key == InputKeyDown && step && app->cursor + 1 < app->log_count) app->cursor++;
                if(app->log_count && app->cursor + 1 == app->log_count) app->follow = true;
            } else if(app->view == ViewMenu) {
                if(in->key == InputKeyBack && in->type == InputTypeShort) app->view = ViewMonitor;
                if(in->key == InputKeyUp && step) app->menu_idx = (app->menu_idx + N_COMMANDS - 1) % N_COMMANDS;
                if(in->key == InputKeyDown && step) app->menu_idx = (app->menu_idx + 1) % N_COMMANDS;
                if(commands[app->menu_idx].kind == CmdInput && step) {
                    if(in->key == InputKeyRight && app->hdmi_input < 4) app->hdmi_input++;
                    if(in->key == InputKeyLeft && app->hdmi_input > 1) app->hdmi_input--;
                }
                if(in->key == InputKeyOk && in->type == InputTypeShort) to_run = &commands[app->menu_idx];
            } else {
                if(in->type == InputTypeShort && (in->key == InputKeyBack || in->key == InputKeyOk))
                    app->view = ViewMonitor;
            }
        }
        furi_mutex_release(app->mutex);

        if(to_run) run_command(app, to_run);
        view_port_update(view_port);
    }

    rx_stop();
    furi_hal_gpio_init(PIN_CEC, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
