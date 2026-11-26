#include "gp5_firmware.h"
#include "gp5_ui.h"
#include "fonts/sans_16.h"

static const uint8_t rows[] = {
#ifdef LIVEWDF_GP50_CATALOG
    GP5_P_VALUE, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CONTROL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON
};
static const int16_t steps[] = { 10, 2, 1, 1, 1, 5, 5, 1 };
static int values[CATALOG_MODELS][CATALOG_CONTROLS];
#else
    GP5_P_GAIN, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON
};
static const int16_t steps[] = { 1, 2, 1, 1, 5, 5, 1 };
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

void fw_init(void)
{
#ifdef LIVEWDF_GP50_CATALOG
    for (unsigned m = 0; m < CATALOG_MODELS; ++m)
        for (unsigned i = 0; i < CATALOG_CONTROLS; ++i) values[m][i] = -1;
    fw_param_changed(GP5_P_MODEL);
#endif
    show_on();
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
    unsigned control = (unsigned)gp5_param(GP5_P_CONTROL);
    if (id == GP5_P_CONTROL && control >= catalog_control_count[model]) {
        control = catalog_control_count[model] - 1;
        gp5_param_set(GP5_P_CONTROL, (int)control);
    }
    if (id == GP5_P_MODEL || id == GP5_P_CONTROL) {
        int previous = values[model][control];
        gp5_param_set(GP5_P_VALUE, previous < 0 ? catalog_controls[model][control].position : previous);
    } else if (id == GP5_P_VALUE) {
        int value = gp5_param(GP5_P_VALUE);
        values[model][control] = value;
        gp5_param_set(GP5_P_POT_0 + control, value);
    }
#endif
}

void fw_foot_down(void) { gp5_param_set(GP5_P_ON, !gp5_param(GP5_P_ON)); show_on(); }
void fw_para_turn(int detents)
{
    unsigned id = gp5_ui_page_row(&page);
    int before = gp5_param(id);
    gp5_ui_page_turn(&page, detents);
    if (gp5_param(id) != before) fw_param_changed(id);
}
void fw_para_click(void) { gp5_ui_page_click(&page); }
int fw_draw(int force) { return gp5_ui_page_draw(&page, force); }

#ifdef GP50_BOARD
void fw_gp50_encoder_turn(unsigned encoder, int detents)
{
    if (encoder > GP50_ENCODER_RIGHT) return;
    unsigned id = rows[encoder];
    if (gp5_param_set(id, gp5_param(id) + detents * steps[encoder])) fw_param_changed(id);
}

void fw_gp50_button(unsigned button, int down)
{
    if (down && button == GP50_BUTTON_FS2) {
        int next = gp5_param(GP5_P_MODEL) + 1;
        if (gp5_param_set(GP5_P_MODEL, next > gp5_param_table[GP5_P_MODEL].max ? 0 : next))
            fw_param_changed(GP5_P_MODEL);
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
        if ((unsigned)value >= catalog_control_count[model]) return 0;
        const char *name = catalog_controls[model][value].name;
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
