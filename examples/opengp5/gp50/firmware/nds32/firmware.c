#include "gp5_firmware.h"
#include "gp5_ui.h"
#include "fonts/sans_16.h"

static const uint8_t rows[] = {
    GP5_P_GAIN, GP5_P_LEVEL, GP5_P_MODEL, GP5_P_CAB, GP5_P_DELAY, GP5_P_REVERB, GP5_P_ON
};
static const int16_t steps[] = { 1, 2, 1, 1, 5, 5, 1 };
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

void fw_init(void) { show_on(); }
void fw_param_changed(unsigned id) { if (id == GP5_P_ON) show_on(); }
void fw_foot_down(void) { gp5_param_set(GP5_P_ON, !gp5_param(GP5_P_ON)); show_on(); }
void fw_para_turn(int detents)
{
    int on = gp5_param(GP5_P_ON);
    gp5_ui_page_turn(&page, detents);
    if (gp5_param(GP5_P_ON) != on) show_on();
}
void fw_para_click(void) { gp5_ui_page_click(&page); }
int fw_draw(int force) { return gp5_ui_page_draw(&page, force); }

#ifdef GP50_BOARD
void fw_gp50_encoder_turn(unsigned encoder, int detents)
{
    if (encoder > GP50_ENCODER_RIGHT) return;
    unsigned id = rows[encoder];
    gp5_param_set(id, gp5_param(id) + detents * steps[encoder]);
}

void fw_gp50_button(unsigned button, int down)
{
    if (down && button == GP50_BUTTON_FS2) {
        int next = gp5_param(GP5_P_MODEL) + 1;
        gp5_param_set(GP5_P_MODEL, next > gp5_param_table[GP5_P_MODEL].max ? 0 : next);
    }
}
#endif

int fw_param_text(unsigned id, int32_t value, char *text)
{
    if (id != GP5_P_GAIN) return 0;
    unsigned percent = ((unsigned)value * 100 + DOD250_GAIN_MAX / 2) / DOD250_GAIN_MAX;
    if (percent == 100) *text++ = '1';
    if (percent >= 10) *text++ = (char)('0' + (percent / 10) % 10);
    *text++ = (char)('0' + percent % 10);
    *text++ = ' ';
    *text++ = '%';
    *text = 0;
    return 1;
}
