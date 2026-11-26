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

enum { PLAY, MODEL, CABINET, AMBIENCE, CIRCUIT };
enum { HEADER = 1, MODEL_NAME = 2, CONTEXT = 4, KNOBS = 8, ALL = 15 };
static int16_t patches[PATCH_COUNT][GP5_NPARAMS];
static uint8_t patch_record[GP5_STORE_MAX];
static uint32_t patch_record_id;
static unsigned patch, screen, redraw;
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
    if (id == GP5_P_MODEL && screen >= CIRCUIT && (screen - CIRCUIT) * 2 >= count)
        show_screen(count ? CIRCUIT : MODEL);
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

static unsigned circuit_pages(void)
{
#ifdef LIVEWDF_GP50_CATALOG
    return (catalog_control_count[current_model()] + 1) / 2;
#else
    return 1;
#endif
}

static unsigned edit_page(unsigned index)
{
    unsigned tone_pages = circuit_pages();
    return index < tone_pages ? CIRCUIT + index : MODEL + index - tone_pages;
}

void fw_para_turn(int detents)
{
    if (screen == PLAY) {
        unsigned next = wrap(patch, detents, PATCH_COUNT);
        if (next != patch) recall_patch(next);
    } else {
        unsigned tone_pages = circuit_pages();
        unsigned index = screen >= CIRCUIT ? screen - CIRCUIT : tone_pages + screen - MODEL;
        show_screen(edit_page(wrap(index, detents, tone_pages + 3)));
    }
}

void fw_para_click(void)
{
    if (screen == PLAY) show_screen(edit_page(0));
    else fw_para_turn(1);
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

static unsigned named_control(unsigned model, const char *const *names, unsigned exclude)
{
    for (; *names; ++names)
        for (unsigned i = 0; i < catalog_control_count[model]; ++i)
            if (i != exclude && starts_with(catalog_controls[model][i].name, *names)) return i;
    return CATALOG_CONTROLS;
}
#endif

#define NO_CONTROL GP5_NPARAMS

#ifdef LIVEWDF_GP50_CATALOG
static unsigned pot_id(unsigned control)
{
    return control < catalog_control_count[current_model()] ? GP5_P_POT_0 + control : NO_CONTROL;
}

static int pot_position(unsigned id)
{
    int value = gp5_param(id);
    return value < 0 ? catalog_controls[current_model()][id - GP5_P_POT_0].position : value;
}
#endif

static unsigned knob_param(unsigned encoder)
{
    if (encoder == GP50_ENCODER_MIDDLE) return GP5_P_LEVEL;
    int left = encoder == GP50_ENCODER_LEFT;
    switch (screen) {
    case MODEL: return left ? GP5_P_MODEL : GP5_P_INPUT;
    case CABINET: return left ? GP5_P_CAB : GP5_P_ON;
    case AMBIENCE: return left ? GP5_P_DELAY : GP5_P_REVERB;
#ifdef LIVEWDF_GP50_CATALOG
    case PLAY: {
        static const char *const drives[] = {
            "drive", "gain", "distortion", "lead gain", "lead drive", "pre gain", "preamp volume",
            "overdrive", "fuzz", "boost", 0
        };
        static const char *const tones[] = {
            "treadle", "tone", "filter", "presence", "treble", "sensitivity", "threshold", "sustain", "depth", "blend", 0
        };
        unsigned model = current_model();
        unsigned drive = named_control(model, drives, CATALOG_CONTROLS);
        if (left) return drive < CATALOG_CONTROLS ? pot_id(drive) : GP5_P_INPUT;
        unsigned tone = named_control(model, tones, drive);
        if (tone == CATALOG_CONTROLS) tone = drive == 0 ? 1 : 0;
        unsigned id = pot_id(tone);
        return id == NO_CONTROL ? GP5_P_DELAY : id;
    }
    default: return pot_id((screen - CIRCUIT) * 2 + !left);
#else
    case PLAY: return left ? GP5_P_GAIN : GP5_P_DELAY;
    default: return left ? GP5_P_GAIN : NO_CONTROL;
#endif
    }
}

void fw_gp50_encoder_turn(unsigned encoder, int detents)
{
    if (encoder > GP50_ENCODER_RIGHT || !detents) return;
    unsigned id = knob_param(encoder);
    if (id == NO_CONTROL) return;
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

#ifdef LIVEWDF_GP50_CATALOG
static const char *pot_label(const char *name)
{
    static const struct { const char *full, *short_label; } labels[] = {
        { "160 hz gain", "160 Hz" }, { "2.5 khz gain", "2.5 kHz" }, { "40 hz gain", "40 Hz" },
        { "650 hz gain", "650 Hz" }, { "sub 10 hz gain", "Sub 10" },
        { "bbd bias", "BBD Bs" }, { "color trim", "Col Trim" }, { "distortion", "Distort" },
        { "hf frequency", "HF Hz" }, { "lf frequency", "LF Hz" },
        { "high bell bandwidth", "Hi B Q" }, { "high bell frequency", "Hi B Hz" }, { "high bell gain", "Hi B G" },
        { "low bell bandwidth", "Lo B Q" }, { "low bell frequency", "Lo B Hz" }, { "low bell gain", "Lo B G" },
        { "mid bell bandwidth", "Mid B Q" }, { "mid bell frequency", "Mid B Hz" }, { "mid bell gain", "Mid B G" },
        { "high frequency", "High Hz" }, { "low frequency", "Low Hz" },
        { "high gain", "Hi Gain" }, { "low gain", "Lo Gain" },
        { "high mid frequency", "HM Freq" }, { "low mid frequency", "LM Freq" },
        { "high mid gain", "Hi Mid G" }, { "low mid gain", "LM Gain" },
        { "high pass frequency", "HP Freq" }, { "low pass frequency", "LP Freq" },
        { "high shelf", "Hi Shelf" }, { "low shelf", "Lo Shelf" },
        { "high shelf gain", "Hi Shelf" }, { "low shelf gain", "Lo Shelf" },
        { "high treble volume", "HiTr Vol" }, { "instrument volume", "Inst Vol" },
        { "lead drive", "Ld Drive" }, { "lead gain", "Ld Gain" },
        { "lead master", "Ld Mstr" }, { "lead volume", "Ld Vol" },
        { "hmf frequency", "HMF Hz" }, { "lmf frequency", "LMF Hz" },
        { "hmf gain", "HMF G" }, { "lmf gain", "LMF G" },
        { "master volume", "Mstr Vol" }, { "microphone volume", "Mic Vol" },
        { "normal volume", "Nrm Vol" }, { "post gain", "Post G" }, { "preamp volume", "Pre Vol" },
        { "presence", "Pres" }, { "resonance", "Reson" }, { "spectrum", "Spectr" },
        { "volume 1", "Vol 1" }, { "level", "Ckt Vol" },
    };
    for (unsigned i = 0; i < sizeof labels / sizeof labels[0]; ++i) {
        const char *full = labels[i].full;
        unsigned length = 0;
        while (full[length]) ++length;
        if (starts_with(name, full) && !name[length]) return labels[i].short_label;
    }
    return name;
}
#endif

static void draw_knob(unsigned encoder)
{
    unsigned id = knob_param(encoder);
    if (id == NO_CONTROL) return;
    const char *label = gp5_param_table[id].name;
    char value[16];
    if (id == GP5_P_MODEL) {
        gp5_fmt(value, sizeof value, "%u/%u", (unsigned)current_model() + 1, (unsigned)PATCH_COUNT);
    }
#ifdef LIVEWDF_GP50_CATALOG
    else if (id >= GP5_P_POT_0) {
        label = pot_label(catalog_controls[current_model()][id - GP5_P_POT_0].name);
        gp5_fmt(value, sizeof value, "%u%%", (unsigned)(pot_position(id) + 5) / 10);
    }
#endif
    else if (id == GP5_P_DELAY || id == GP5_P_REVERB || id == GP5_P_LEVEL || id == GP5_P_INPUT) {
        gp5_fmt(value, sizeof value, "%u%%", (unsigned)gp5_param(id));
    } else {
        gp5_param_format(id, gp5_param(id), value);
    }
    int x = (int)(encoder * GP5_SCREEN_W / 3) + 1;
    screen_text(&gp5_font_sans_12, label, x, 0, 50, muted, 1);
    uint32_t color = id == GP5_P_LEVEL && gp5_param(GP5_P_ON) ? accent : ink;
    if (!gp5_param(GP5_P_ON) && id != GP5_P_ON && id != GP5_P_MODEL) color = muted;
    screen_text(&gp5_font_sans_16, value, x, 11, 50, color, 1);
}

int fw_draw(int force)
{
    if (force) redraw = ALL;
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
        gp5_fmt(text, sizeof text, "Patch %03u%s", patch + 1, patch_dirty() ? " *" : "");
        screen_text(&gp5_font_sans_12, text, 8, 8, 104, muted, 0);
        if (!gp5_param(GP5_P_ON))
            screen_text(&gp5_font_sans_12, "BYPASS", 108, 8, 45, amber, 0);
    } else if (band == MODEL_NAME) {
        y = 32;
        model_name(text, sizeof text);
        const gp5_font_t *font = gp5_text_width(&gp5_font_sans_20, text) <= 144 ?
                                 &gp5_font_sans_20 : &gp5_font_sans_16;
        screen_text(font, text, 8, 0, 144, ink, 0);
    } else if (band == CONTEXT) {
        y = 64;
        const char *label = message;
        if (!label) {
            switch (screen) {
            case PLAY: label = "Press PARA to edit"; break;
            case MODEL: label = "Model & input"; break;
            case CABINET: label = "Cab & bypass"; break;
            case AMBIENCE: label = "Delay & reverb"; break;
            default:
                gp5_fmt(text, sizeof text, "Circuit %u/%u", screen - CIRCUIT + 1, circuit_pages());
                label = text;
            }
        }
        screen_text(&gp5_font_sans_12, label, 8, 6, 144, message ? amber : muted, 0);
        if (screen != PLAY && !message)
            screen_text(&gp5_font_sans_12, "PARA >", 112, 6, 42, accent, 0);
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
        message = save_bank() ? "Patch saved" : "Saved in RAM";
        message_until = gp5_ms() + 1100;
        redraw |= HEADER | CONTEXT;
        break;
    case GP50_BUTTON_EXIT: show_screen(PLAY); break;
    case GP50_BUTTON_GLOBAL: show_screen(CABINET); break;
    case GP50_BUTTON_DRUM: show_screen(AMBIENCE); break;
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
