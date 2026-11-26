#include "gp5_firmware.h"
#include "gp5_ui.h"
#include "fonts/sans_16.h"

#ifdef GP50_BOARD
#include "fonts/sans_12.h"
#include "fonts/sans_20.h"
#include "gp5_fmt.h"

#ifdef LIVEWDF_GP50_CATALOG
#define PATCH_COUNT CATALOG_MODELS
#else
#define PATCH_COUNT 2u
#endif

enum { PLAY, MODEL, CABINET, BYPASS, DELAY, REVERB, FIRST_POT };
enum { HEADER = 1, MODEL_NAME = 2, CONTEXT = 4, KNOBS = 8, ALL = 15 };
static int16_t patches[PATCH_COUNT][GP5_NPARAMS];
static uint8_t patch_record[GP5_STORE_MAX];
static uint32_t patch_record_id;
static unsigned patch, screen, redraw;
static int adjusting;
static const char *message;
static uint32_t message_until;
#else
#ifdef LIVEWDF_GP50_CATALOG
static const uint8_t rows[] = {
    GP5_P_VALUE, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CONTROL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON
};
static const int16_t steps[] = { 10, 2, 1, 1, 1, 5, 5, 1 };
#else
static const uint8_t rows[] = { GP5_P_GAIN, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON };
static const int16_t steps[] = { 1, 2, 1, 1, 5, 5, 1 };
#endif
#endif

#ifdef LIVEWDF_GP50_CATALOG
static int16_t values[CATALOG_MODELS][CATALOG_CONTROLS];

static void reset_model_edits(void)
{
    for (unsigned m = 0; m < CATALOG_MODELS; ++m)
        for (unsigned i = 0; i < CATALOG_CONTROLS; ++i) values[m][i] = -1;
}
#endif
#ifndef GP50_BOARD
static const gp5_ui_theme_t theme = { 0x18130e, 0xfff1d8, 0xa59072, 0xf4b665, 0x24170a };
static gp5_ui_page_t page = {
    .rows = rows, .n = sizeof rows, .steps = steps, .font = &gp5_font_sans_16,
    .theme = &theme, .title = "LiveWDF", .row_h = GP5_SCREEN_H / 5, .upper = 1,
};
#endif

static void show_on(void)
{
    int on = gp5_param(GP5_P_ON);
    gp5_led(on ? GP5_LED_ON : GP5_LED_BYPASS);
#ifndef GP50_BOARD
    page.faded = !on;
#endif
}

#ifdef GP50_BOARD
/* One journal record keeps every slot in the same recoverable generation. */
static uint32_t hash_text(uint32_t hash, const char *text)
{
    if (text) while (*text) hash = (hash ^ (uint8_t)*text++) * 16777619u;
    return hash * 16777619u;
}

static uint32_t bank_id(void)
{
    uint32_t hash = hash_text(2166136261u, "LiveWDF GP50 patches v1");
    hash = (hash ^ PATCH_COUNT) * 16777619u;
    for (unsigned i = 0; i < GP5_NPARAMS; ++i) {
        const gp5_param_desc_t *d = &gp5_param_table[i];
        hash = (hash ^ (uint16_t)d->min) * 16777619u;
        hash = (hash ^ (uint16_t)d->max) * 16777619u;
        hash = hash_text(hash_text(hash, d->name), d->choices);
    }
#ifdef LIVEWDF_GP50_CATALOG
    for (unsigned m = 0; m < CATALOG_MODELS; ++m)
        for (unsigned i = 0; i < CATALOG_CONTROLS; ++i)
            hash = hash_text(hash, catalog_controls[m][i].name);
#endif
    return hash;
}

static uint32_t record_crc(unsigned size)
{
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 4; i < size; ++i) {
        crc ^= patch_record[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Widths come from the parameter ranges so all 47 slots fit without rounding.
 * Byte 4 is the last explicitly saved slot. Parameters follow low bit first. */
static unsigned transfer_bank(int save, unsigned stored_size)
{
    uint8_t widths[GP5_NPARAMS];
    unsigned bank_bits = 0;
    for (unsigned i = 0; i < GP5_NPARAMS; ++i) {
        unsigned range = (unsigned)((int)gp5_param_table[i].max - gp5_param_table[i].min);
        widths[i] = 0;
        for (; range; range >>= 1) ++widths[i];
        bank_bits += widths[i];
    }
    unsigned size = 5 + (PATCH_COUNT * bank_bits + 7) / 8;
    if (size > sizeof patch_record) return 0;
    if (save) {
        for (unsigned i = 0; i < size; ++i) patch_record[i] = 0;
        patch_record[4] = (uint8_t)patch;
    } else {
        if (stored_size != size || patch_record[4] >= PATCH_COUNT) return 0;
        uint32_t crc = record_crc(size);
        for (unsigned i = 0; i < 4; ++i)
            if (patch_record[i] != (uint8_t)(crc >> (i * 8))) return 0;
    }
    unsigned offset = 40;
    for (unsigned slot = 0; slot < PATCH_COUNT; ++slot)
        for (unsigned i = 0; i < GP5_NPARAMS; ++i) {
            const gp5_param_desc_t *d = &gp5_param_table[i];
            unsigned value = save ? (unsigned)((int)patches[slot][i] - d->min) : 0;
            for (unsigned bit = 0; bit < widths[i]; ++bit, ++offset) {
                uint8_t *byte = &patch_record[offset / 8];
                unsigned mask = 1u << (offset % 8);
                if (save) *byte |= (uint8_t)(((value >> bit) & 1u) * mask);
                else if (*byte & mask) value |= 1u << bit;
            }
            if (!save) {
                int parameter = d->min + (int)value;
                patches[slot][i] = (int16_t)(parameter > d->max ? d->max : parameter);
            }
        }
    if (save) {
        uint32_t crc = record_crc(size);
        for (unsigned i = 0; i < 4; ++i) patch_record[i] = (uint8_t)(crc >> (i * 8));
    }
    return size;
}

static int save_bank(void)
{
    unsigned size = transfer_bank(1, 0);
    return size && gp5_store_save(patch_record_id, patch_record, size) == 0;
}

static void show_screen(unsigned next)
{
    screen = next;
    adjusting = 0;
    message = 0;
    redraw = ALL;
}

static int patch_dirty(void)
{
    for (unsigned i = 0; i < GP5_NPARAMS; ++i) {
#ifdef LIVEWDF_GP50_CATALOG
        if (i == GP5_P_CONTROL || i == GP5_P_VALUE) continue;
#endif
        if (gp5_param(i) != patches[patch][i]) return 1;
    }
    return 0;
}

static void recall_patch(unsigned slot)
{
    patch = slot;
#ifdef LIVEWDF_GP50_CATALOG
    reset_model_edits();
    unsigned model = (unsigned)patches[slot][GP5_P_MODEL];
    for (unsigned i = 0; i < CATALOG_CONTROLS; ++i)
        values[model][i] = patches[slot][GP5_P_POT_0 + i];
#endif
    for (unsigned i = 0; i < GP5_NPARAMS; ++i)
        gp5_param_set(i, patches[slot][i]);
#ifdef LIVEWDF_GP50_CATALOG
    fw_param_changed(GP5_P_CONTROL);
#endif
    show_on();
    show_screen(PLAY);
}
#endif

void fw_init(void)
{
#ifdef GP50_BOARD
    unsigned model = (unsigned)gp5_param(GP5_P_MODEL);
    for (unsigned slot = 0; slot < PATCH_COUNT; ++slot) {
        for (unsigned i = 0; i < GP5_NPARAMS; ++i)
            patches[slot][i] = (int16_t)gp5_param(i);
        patches[slot][GP5_P_MODEL] = (int16_t)((model + slot) % PATCH_COUNT);
    }
    patch_record_id = bank_id();
    unsigned size = gp5_store_load(patch_record_id, patch_record, sizeof patch_record);
    recall_patch(transfer_bank(0, size) ? patch_record[4] : 0);
#else
#ifdef LIVEWDF_GP50_CATALOG
    reset_model_edits();
    fw_param_changed(GP5_P_MODEL);
#endif
    show_on();
#endif
}

void fw_param_changed(unsigned id)
{
    if (id == GP5_P_ON) show_on();
#ifdef GP50_BOARD
    redraw |= id == GP5_P_MODEL ? ALL : HEADER | KNOBS;
    if (screen != PLAY) redraw |= MODEL_NAME;
#endif
#ifdef LIVEWDF_GP50_CATALOG
    unsigned model = (unsigned)gp5_param(GP5_P_MODEL);
    if (id == GP5_P_MODEL) {
        gp5_param_set(GP5_P_CONTROL, 0);
        for (unsigned i = 0; i < CATALOG_CONTROLS; ++i)
            gp5_param_set(GP5_P_POT_0 + i, values[model][i]);
    }
    unsigned count = catalog_control_count[model];
#ifdef GP50_BOARD
    if (id == GP5_P_MODEL && screen >= FIRST_POT && screen - FIRST_POT >= count)
        show_screen(count ? FIRST_POT : MODEL);
#endif
    unsigned control = (unsigned)gp5_param(GP5_P_CONTROL);
    if (control >= count) {
        control = count ? count - 1 : 0;
        gp5_param_set(GP5_P_CONTROL, (int)control);
    }
    if (!count) {
        gp5_param_set(GP5_P_VALUE, 0);
        return;
    }
    if (id >= GP5_P_POT_0 && id < GP5_P_POT_0 + CATALOG_CONTROLS) {
        unsigned pot = id - GP5_P_POT_0;
        values[model][pot] = (int16_t)gp5_param(id);
        if (pot != control) return;
        id = GP5_P_CONTROL;
    }
    if (id == GP5_P_MODEL || id == GP5_P_CONTROL) {
        int previous = values[model][control];
        gp5_param_set(GP5_P_VALUE, previous < 0 ? catalog_controls[model][control].position : previous);
    } else if (id == GP5_P_VALUE) {
        int value = gp5_param(GP5_P_VALUE);
        values[model][control] = (int16_t)value;
        gp5_param_set(GP5_P_POT_0 + control, value);
    }
#endif
}

void fw_foot_down(void)
{
#ifdef GP50_BOARD
    recall_patch((patch + PATCH_COUNT - 1) % PATCH_COUNT);
#else
    gp5_param_set(GP5_P_ON, !gp5_param(GP5_P_ON));
    show_on();
#endif
}
#ifndef GP50_BOARD
void fw_para_turn(int detents)
{
    unsigned id = gp5_ui_page_row(&page);
    int before = gp5_param(id);
    gp5_ui_page_turn(&page, detents);
    if (gp5_param(id) != before) fw_param_changed(id);
}

void fw_para_click(void) { gp5_ui_page_click(&page); }
int fw_draw(int force) { return gp5_ui_page_draw(&page, force); }
#else

static unsigned wrap(unsigned position, int detents, unsigned count)
{
    return (unsigned)(((int)position + detents % (int)count + (int)count) % (int)count);
}

static unsigned current_model(void) { return (unsigned)gp5_param(GP5_P_MODEL); }

static unsigned circuit_controls(void)
{
#ifdef LIVEWDF_GP50_CATALOG
    return catalog_control_count[current_model()];
#else
    return 1;
#endif
}

static unsigned view_for_parameter(unsigned index)
{
    unsigned count = circuit_controls();
    return index < count ? FIRST_POT + index : MODEL + index - count;
}

static unsigned parameter_index(void)
{
    return screen >= FIRST_POT ? screen - FIRST_POT : circuit_controls() + screen - MODEL;
}

static unsigned parameter_count(void) { return circuit_controls() + FIRST_POT - MODEL; }

static unsigned edit_param(unsigned view)
{
    static const uint8_t params[] = { GP5_P_MODEL, GP5_P_CAB, GP5_P_ON, GP5_P_DELAY, GP5_P_REVERB };
    if (view < FIRST_POT) return params[view - MODEL];
#ifdef LIVEWDF_GP50_CATALOG
    return GP5_P_POT_0 + view - FIRST_POT;
#else
    return GP5_P_GAIN;
#endif
}

static void turn_param(unsigned id, int detents);

void fw_para_turn(int detents)
{
    if (adjusting) {
        turn_param(edit_param(screen), detents);
    } else if (detents) {
        int index = screen == PLAY ? detents - 1 : (int)parameter_index() + detents;
        int last = (int)parameter_count() - 1;
        show_screen(view_for_parameter((unsigned)(index < 0 ? 0 : index > last ? last : index)));
    }
}

void fw_para_click(void)
{
    if (screen == PLAY) show_screen(view_for_parameter(0));
    else {
        adjusting = !adjusting;
        message = 0;
        redraw |= MODEL_NAME | CONTEXT;
    }
}

#ifdef LIVEWDF_GP50_CATALOG
static int starts_with(const char *name, const char *prefix)
{
    for (; *prefix; ++prefix, ++name) {
        int c = *name;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *prefix) return 0;
    }
    return 1;
}

static unsigned named_control(unsigned model, const char *const *names)
{
    for (; *names; ++names)
        for (unsigned i = 0; i < catalog_control_count[model]; ++i)
            if (starts_with(catalog_controls[model][i].name, *names)) return i;
    return CATALOG_CONTROLS;
}
#endif

#ifdef LIVEWDF_GP50_CATALOG
static int pot_position(unsigned id)
{
    int value = gp5_param(id);
    return value < 0 ? catalog_controls[current_model()][id - GP5_P_POT_0].position : value;
}
#endif

static unsigned knob_param(unsigned encoder)
{
    if (encoder == GP50_ENCODER_LEFT) return GP5_P_INPUT;
    if (encoder == GP50_ENCODER_MIDDLE) return GP5_P_LEVEL;
#ifdef LIVEWDF_GP50_CATALOG
    static const char *const quick_controls[] = {
        "drive", "gain", "distortion", "lead gain", "lead drive", "pre gain", "preamp volume",
        "overdrive", "fuzz", "boost", "volume", "normal volume", "instrument volume",
        "treadle", "tone", "treble", 0
    };
    unsigned model = current_model();
    if (!catalog_control_count[model]) return GP5_P_DELAY;
    unsigned control = named_control(model, quick_controls);
    return GP5_P_POT_0 + (control == CATALOG_CONTROLS ? 0 : control);
#else
    return GP5_P_GAIN;
#endif
}

static void turn_param(unsigned id, int detents)
{
    if (!detents) return;
    int value = gp5_param(id);
    if (id == GP5_P_MODEL) value = (int)wrap((unsigned)value, detents, PATCH_COUNT);
    else if (id == GP5_P_LEVEL || id == GP5_P_INPUT) value += 2 * detents;
    else if (id == GP5_P_DELAY || id == GP5_P_REVERB) value += 5 * detents;
#ifdef LIVEWDF_GP50_CATALOG
    else if (id >= GP5_P_POT_0) {
        value = pot_position(id) + 10 * detents;
        if (value < 0) value = 0;
    }
#endif
    else value += detents;
    if (gp5_param_set(id, value)) fw_param_changed(id);
}

void fw_gp50_encoder_turn(unsigned encoder, int detents)
{
    if (encoder <= GP50_ENCODER_RIGHT) turn_param(knob_param(encoder), detents);
}

static void model_name(char *out, unsigned capacity)
{
    const char *name = gp5_param_table[GP5_P_MODEL].choices;
    for (unsigned model = current_model(); model; --model)
        while (*name++ != '|') {}
    while (--capacity && *name && *name != '|') *out++ = *name++;
    *out = 0;
}

enum { STRIP_H = 32 };
static uint16_t strip[GP5_SCREEN_W * STRIP_H];
static const uint32_t background = 0x14191e, panel = 0x202a31, ink = 0xf5f0e7;
static const uint32_t muted = 0xb9c3c8, accent = 0x7ee0bd, amber = 0xffc180;

static void screen_text(const gp5_font_t *font, const char *text, int x, int y, int width,
                        uint32_t color, int center)
{
    char fitted[32];
    unsigned length = 0;
    while (text[length] && length < sizeof fitted - 1) {
        fitted[length] = text[length];
        ++length;
    }
    fitted[length] = 0;
    if (gp5_text_width(font, fitted) > width && length > 3) {
        do {
            fitted[--length] = 0;
            fitted[length - 2] = '.';
            fitted[length - 1] = '.';
        } while (length > 2 && gp5_text_width(font, fitted) > width);
    }
    if (center) x += (width - gp5_text_width(font, fitted)) / 2;
    gp5_draw_text(strip, GP5_SCREEN_W, STRIP_H, x, y, font, fitted, color);
}

#ifdef GP50_HAS_STATUS
static gp50_status_t displayed_status;

static void draw_status(void)
{
    const gp5_font_t *font = &gp5_font_sans_12;
    unsigned bluetooth = displayed_status.bluetooth;
    const char *wireless = bluetooth == GP50_BT_PAIRED ? "BT Paired" :
                           bluetooth == GP50_BT_UNPAIRED ? "BT Unpaired" : "BT Off";
    screen_text(font, wireless, 6, 16, 72, bluetooth == GP50_BT_PAIRED ? accent : muted, 0);

    enum { X = 135, Y = 20, WIDTH = 20, HEIGHT = 10 };
    int battery = displayed_status.battery;
    uint32_t color = battery == 0 ? 0xff8580 : battery > 0 ? ink : muted;
    uint16_t pixel = gp5_color(color);
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH + 2; ++x) {
            int outline = x < WIDTH ? x == 0 || x == WIDTH - 1 || y == 0 || y == HEIGHT - 1 :
                                      y >= 3 && y < 7;
            int bar = x >= 2 && x < 2 + 4 * (unsigned)(battery > 0 ? battery : 0) &&
                      (x - 2) % 4 < 3 && y >= 2 && y < HEIGHT - 2;
            if (outline || bar) strip[(Y + y) * GP5_SCREEN_W + X + x] = pixel;
        }
    if (battery < 0) screen_text(font, "?", X + 7, 17, 8, muted, 0);
    if (displayed_status.charging > 0) {
        int width = gp5_text_width(font, "Charging");
        screen_text(font, "Charging", X - width - 5, 16, width, accent, 0);
    }
}
#endif

static const char *param_label(unsigned id)
{
#ifdef LIVEWDF_GP50_CATALOG
    if (id >= GP5_P_POT_0) {
        const char *name = catalog_controls[current_model()][id - GP5_P_POT_0].name;
        return starts_with(name, "level") && !name[5] ? "Volume" : name;
    }
#endif
    return gp5_param_table[id].name;
}

static const char *quick_label(const char *name)
{
#ifdef LIVEWDF_GP50_CATALOG
    static const struct { const char *full, *short_label; } labels[] = {
        { "sub 10 hz gain", "Sub 10" }, { "high shelf", "Hi Shelf" },
        { "high pass frequency", "HP Freq" }, { "low shelf gain", "Lo Shelf" },
        { "lf gain", "LF Gain" }, { "lead drive", "Ld Drive" }, { "lead gain", "Ld Gain" },
        { "instrument volume", "Inst Vol" }, { "normal volume", "Nrm Vol" },
        { "preamp volume", "Pre Vol" }, { "distortion", "Distort" }, { "volume 1", "Vol 1" },
        { "bbd bias", "BBD Bs" },
    };
    for (unsigned i = 0; i < sizeof labels / sizeof labels[0]; ++i)
        if (starts_with(name, labels[i].full)) return labels[i].short_label;
#endif
    return name;
}

static void param_value(unsigned id, char *value)
{
    if (id == GP5_P_MODEL) {
        gp5_fmt(value, 16, "%u/%u", current_model() + 1, (unsigned)PATCH_COUNT);
    }
#ifdef LIVEWDF_GP50_CATALOG
    else if (id >= GP5_P_POT_0) {
        gp5_fmt(value, 16, "%u%%", (unsigned)(pot_position(id) + 5) / 10);
    }
#endif
    else if (gp5_param_table[id].unit == GP5_U_PERCENT) {
        gp5_fmt(value, 16, "%u%%", (unsigned)gp5_param(id));
    } else {
        gp5_param_format(id, gp5_param(id), value);
    }
}

static void draw_knob(unsigned encoder)
{
    unsigned id = knob_param(encoder);
    char value[16];
    param_value(id, value);
    int x = (int)(encoder * GP5_SCREEN_W / 3) + 1;
    screen_text(&gp5_font_sans_12, quick_label(param_label(id)), x, 0, 50, muted, 1);
    uint32_t color = !gp5_param(GP5_P_ON) ? muted : id == GP5_P_LEVEL ? accent : ink;
    screen_text(&gp5_font_sans_16, value, x, 11, 50, color, 1);
}

static void draw_parameter(unsigned index, int y, int selected)
{
    unsigned id = edit_param(view_for_parameter(index));
    const gp5_font_t *font = &gp5_font_sans_12;
    char value[16];
    param_value(id, value);
    uint32_t color = muted;
    if (selected) {
        uint16_t fill = gp5_color(panel);
        for (int row = y; row < y + 16; ++row)
            for (unsigned x = 4; x < GP5_SCREEN_W - 4; ++x)
                strip[row * GP5_SCREEN_W + x] = fill;
        color = adjusting ? accent : amber;
    }
    int value_w = gp5_text_width(font, value);
    screen_text(font, param_label(id), 8, y, 138 - value_w, color, 0);
    screen_text(font, value, 152 - value_w, y, value_w, color, 0);
}

int fw_draw(int force)
{
    if (force) redraw = ALL;
#ifdef GP50_HAS_STATUS
    gp50_status_t status = gp50_status();
    if (status.battery != displayed_status.battery || status.charging != displayed_status.charging ||
        status.bluetooth != displayed_status.bluetooth) {
        displayed_status = status;
        redraw |= CONTEXT;
    }
#endif
    if (message && (int32_t)(gp5_ms() - message_until) >= 0) {
        message = 0;
        redraw |= CONTEXT;
    }
    if (!redraw) return 0;

    unsigned band = redraw & -redraw;
    unsigned y = 0;
    uint16_t fill = gp5_color(band == KNOBS ? panel : background);
    for (unsigned i = 0; i < sizeof strip / sizeof strip[0]; ++i) strip[i] = fill;
    char text[32];
    if (band == HEADER) {
        int bypass = !gp5_param(GP5_P_ON);
        const char *dirty = patch_dirty() ? "*" : "";
        if (screen == PLAY) {
            gp5_fmt(text, sizeof text, "Patch %03u%s", patch + 1, dirty);
        } else {
            char name[32];
            model_name(name, sizeof name);
            gp5_fmt(text, sizeof text, "%03u%s  %s", patch + 1, dirty, name);
        }
        screen_text(&gp5_font_sans_12, text, 8, screen == PLAY ? 8 : 0, bypass ? 100 : 144, muted, 0);
        if (bypass) screen_text(&gp5_font_sans_12, "BYPASS", 108, screen == PLAY ? 8 : 0, 45, amber, 0);
        if (screen != PLAY && parameter_index()) draw_parameter(parameter_index() - 1, 16, 0);
    } else if (band == MODEL_NAME) {
        y = 32;
        if (screen == PLAY) {
            model_name(text, sizeof text);
            const gp5_font_t *font = gp5_text_width(&gp5_font_sans_20, text) <= 144 ?
                                     &gp5_font_sans_20 : &gp5_font_sans_16;
            screen_text(font, text, 8, 0, 144, ink, 0);
        } else {
            unsigned index = parameter_index();
            draw_parameter(index, 0, 1);
            if (index + 1 < parameter_count()) draw_parameter(index + 1, 16, 0);
        }
    } else if (band == CONTEXT) {
        y = 64;
        const char *hint = message;
        if (!hint) {
            if (screen == PLAY) hint = "Turn PARA to edit";
            else hint = adjusting ? "Press PARA to finish" : "Press PARA to adjust";
        }
#ifdef GP50_HAS_STATUS
        screen_text(&gp5_font_sans_12, hint, 8, 0, 144, message ? amber : muted, 0);
        draw_status();
#else
        screen_text(&gp5_font_sans_12, hint, 8, 7, 144, message ? amber : muted, 0);
#endif
    } else {
        y = 96;
        for (unsigned encoder = 0; encoder <= GP50_ENCODER_RIGHT; ++encoder) draw_knob(encoder);
    }
    gp5_blit(0, y, GP5_SCREEN_W, STRIP_H, strip);
    redraw &= ~band;
    return 1;
}

void fw_gp50_button(unsigned button, int down)
{
    if (!down) return;
    switch (button) {
    case GP50_BUTTON_FS2:
        recall_patch((patch + 1) % PATCH_COUNT);
        break;
    case GP50_BUTTON_SAVE:
        for (unsigned i = 0; i < GP5_NPARAMS; ++i)
            patches[patch][i] = (int16_t)gp5_param(i);
        adjusting = 0;
        message = save_bank() ? "Patch saved" : "Saved in RAM";
        message_until = gp5_ms() + 1100;
        redraw |= HEADER | MODEL_NAME | CONTEXT;
        break;
    case GP50_BUTTON_EXIT: show_screen(PLAY); break;
    case GP50_BUTTON_GLOBAL: show_screen(CABINET); break;
    case GP50_BUTTON_DRUM: show_screen(DELAY); break;
    }
}

#ifdef LIVEWDF_GP50_CATALOG
void fw_gp50_expression(unsigned value)
{
    int position = (int)((value * 1000u + 127u) / 255u);
    values[CATALOG_WAH][0] = position;
    if (gp5_param(GP5_P_MODEL) == CATALOG_WAH) {
        if (gp5_param_set(GP5_P_POT_0, position)) fw_param_changed(GP5_P_POT_0);
    }
}
#endif
#endif

int fw_param_text(unsigned id, int32_t value, char *text)
{
#ifdef LIVEWDF_GP50_CATALOG
    if (id == GP5_P_CONTROL) {
        unsigned model = (unsigned)gp5_param(GP5_P_MODEL);
        const char *name = (unsigned)value < catalog_control_count[model] ?
                           catalog_controls[model][value].name : "None";
        unsigned length = 0;
        for (; length < 15 && name[length]; ++length) text[length] = name[length];
        text[length] = 0;
        return 1;
    }
    if (id != GP5_P_VALUE) return 0;
    unsigned percent = ((unsigned)value + 5) / 10;
#else
    if (id != GP5_P_GAIN) return 0;
    unsigned percent = ((unsigned)value * 100 + DOD250_GAIN_MAX / 2) / DOD250_GAIN_MAX;
#endif
    if (percent == 100) *text++ = '1';
    if (percent >= 10) *text++ = (char)('0' + (percent / 10) % 10);
    *text++ = (char)('0' + percent % 10);
    *text++ = ' ';
    *text++ = '%';
    *text = 0;
    return 1;
}
