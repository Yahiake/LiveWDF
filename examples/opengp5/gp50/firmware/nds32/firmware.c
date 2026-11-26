#include "gp5_firmware.h"
#include "gp5_ui.h"
#include "fonts/sans_16.h"

static const uint8_t rows[] = {
#ifdef GP50_BOARD
#ifdef LIVEWDF_GP50_CATALOG
    GP5_P_MODEL, GP5_P_CONTROL, GP5_P_VALUE, GP5_P_LEVEL, GP5_P_INPUT
#else
    GP5_P_MODEL, GP5_P_GAIN, GP5_P_LEVEL, GP5_P_INPUT
#endif
};
#ifdef LIVEWDF_GP50_CATALOG
static const int16_t steps[] = { 1, 1, 10, 2, 2 };
#define PATCH_COUNT CATALOG_MODELS
#else
static const int16_t steps[] = { 1, 1, 2, 2 };
#define PATCH_COUNT 2u
#endif
static const uint8_t output_rows[] = { GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON };
static const int16_t output_steps[] = { 1, 5, 5, 1 };
static int16_t patches[PATCH_COUNT][GP5_NPARAMS];
static uint8_t patch_record[GP5_STORE_MAX];
static uint32_t patch_record_id;
static unsigned patch;
static unsigned edit_id;
static int32_t edit_start;
static int redraw;
static char patch_title[] = "PATCH 001 ";
#elif defined(LIVEWDF_GP50_CATALOG)
    GP5_P_VALUE, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CONTROL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON
};
static const int16_t steps[] = { 10, 2, 1, 1, 1, 5, 5, 1 };
#else
    GP5_P_GAIN, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON
};
static const int16_t steps[] = { 1, 2, 1, 1, 5, 5, 1 };
#endif
#ifdef LIVEWDF_GP50_CATALOG
static int16_t values[CATALOG_MODELS][CATALOG_CONTROLS];

static void reset_model_edits(void)
{
    for (unsigned m = 0; m < CATALOG_MODELS; ++m)
        for (unsigned i = 0; i < CATALOG_CONTROLS; ++i) values[m][i] = -1;
}
#endif
static const gp5_ui_theme_t theme = { 0x18130e, 0xfff1d8, 0xa59072, 0xf4b665, 0x24170a };
static gp5_ui_page_t page = {
    .rows = rows, .n = sizeof rows, .steps = steps, .font = &gp5_font_sans_16,
    .theme = &theme, .title = "LiveWDF", .row_h = GP5_SCREEN_H / 5, .upper = 1,
};

static void show_on(void)
{
    int on = gp5_param(GP5_P_ON);
    gp5_led(on ? GP5_LED_ON : GP5_LED_BYPASS);
    page.faded = !on;
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

static void show_page(int output, unsigned cursor)
{
    page.rows = output ? output_rows : rows;
    page.n = output ? sizeof output_rows : sizeof rows;
    page.steps = output ? output_steps : steps;
    page.title = output ? "OUTPUT" : patch_title;
    page.cursor = (uint8_t)cursor;
    page.top = page.adjusting = page.msg_on = 0;
    redraw = 1;
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
    show_page(0, 0);
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
#ifdef LIVEWDF_GP50_CATALOG
    unsigned model = (unsigned)gp5_param(GP5_P_MODEL);
    if (id == GP5_P_MODEL) {
        gp5_param_set(GP5_P_CONTROL, 0);
        for (unsigned i = 0; i < CATALOG_CONTROLS; ++i)
            gp5_param_set(GP5_P_POT_0 + i, values[model][i]);
    }
    unsigned count = catalog_control_count[model];
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
void fw_para_turn(int detents)
{
    unsigned id = gp5_ui_page_row(&page);
    int before = gp5_param(id);
    gp5_ui_page_turn(&page, detents);
    if (gp5_param(id) != before) fw_param_changed(id);
}
void fw_para_click(void)
{
#ifdef GP50_BOARD
    if (!page.adjusting) {
        unsigned id = gp5_ui_page_row(&page);
        if (id == GP5_P_ON) {
            gp5_param_set(id, !gp5_param(id));
            show_on();
            return;
        }
#ifdef LIVEWDF_GP50_CATALOG
        if (id == GP5_P_VALUE && catalog_control_count[gp5_param(GP5_P_MODEL)])
            id = GP5_P_POT_0 + (unsigned)gp5_param(GP5_P_CONTROL);
#endif
        edit_id = id;
        edit_start = gp5_param(id);
    }
#endif
    gp5_ui_page_click(&page);
}

int fw_draw(int force)
{
#ifdef GP50_BOARD
    if (page.rows == rows) {
        unsigned number = patch + 1;
        char dirty = patch_dirty() ? '*' : ' ';
        if (patch_title[6] != '0' + number / 100 || patch_title[7] != '0' + number / 10 % 10 ||
            patch_title[8] != '0' + number % 10 || patch_title[9] != dirty) {
            patch_title[6] = '0' + number / 100;
            patch_title[7] = '0' + number / 10 % 10;
            patch_title[8] = '0' + number % 10;
            patch_title[9] = dirty;
            redraw = 1;
        }
    }
    force |= redraw;
    redraw = 0;
#endif
    return gp5_ui_page_draw(&page, force);
}

#ifdef GP50_BOARD
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

void fw_gp50_encoder_turn(unsigned encoder, int detents)
{
    if (encoder > GP50_ENCODER_RIGHT) return;
    unsigned id = encoder == GP50_ENCODER_MIDDLE ? GP5_P_LEVEL : GP5_P_DELAY;
    int step = encoder == GP50_ENCODER_MIDDLE ? 2 : 5;
#ifdef LIVEWDF_GP50_CATALOG
    if (encoder != GP50_ENCODER_MIDDLE) {
        static const char *const drive_names[] = { "drive", "gain", "distortion", "overdrive", "fuzz", 0 };
        static const char *const right_names[] = {
            "treadle", "tone", "filter", "presence", "treble", "sensitivity", "threshold",
            "sustain", "depth", "blend", "level", 0
        };
        unsigned model = (unsigned)gp5_param(GP5_P_MODEL);
        unsigned count = catalog_control_count[model];
        unsigned drive = named_control(model, drive_names, CATALOG_CONTROLS);
        unsigned control = drive;
        if (encoder == GP50_ENCODER_LEFT) {
            id = GP5_P_INPUT;
            step = 2;
        } else {
            control = named_control(model, right_names, drive);
            if (control == CATALOG_CONTROLS && count > (drive < count))
                control = drive == 0 ? 1 : 0;
        }
        if (control < count) {
            if (!page.adjusting && gp5_param_set(GP5_P_CONTROL, (int)control)) fw_param_changed(GP5_P_CONTROL);
            int value = values[model][control];
            if (value < 0) value = catalog_controls[model][control].position;
            value += detents * 10;
            id = GP5_P_POT_0 + control;
            if (gp5_param_set(id, value < 0 ? 0 : value)) fw_param_changed(id);
            return;
        }
    }
#else
    if (encoder == GP50_ENCODER_LEFT) { id = GP5_P_GAIN; step = 1; }
#endif
    if (gp5_param_set(id, gp5_param(id) + detents * step)) fw_param_changed(id);
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
        page.adjusting = 0;
        gp5_ui_page_message(&page, save_bank() ? "PATCH SAVED" : "SAVED IN RAM", 900);
        break;
    case GP50_BUTTON_EXIT:
        if (page.adjusting) {
            page.adjusting = 0;
            if (gp5_param_set(edit_id, edit_start)) fw_param_changed(edit_id);
        } else if (page.rows == output_rows) {
            show_page(0, 0);
        } else if (patch_dirty()) {
            recall_patch(patch);
        }
        break;
    case GP50_BUTTON_GLOBAL:
        show_page(1, 0);
        break;
    case GP50_BUTTON_DRUM:
        show_page(1, 1);
        fw_para_click();
        break;
    }
}

#ifdef LIVEWDF_GP50_CATALOG
void fw_gp50_expression(unsigned value)
{
    int position = (int)((value * 1000u + 127u) / 255u);
    values[CATALOG_WAH][0] = position;
    if (gp5_param(GP5_P_MODEL) == CATALOG_WAH) {
        gp5_param_set(GP5_P_VALUE, position);
        gp5_param_set(GP5_P_POT_0, position);
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
