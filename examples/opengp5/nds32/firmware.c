#include "gp5_firmware.h"
#include "gp5_params.h"
#include "gp5_ui.h"
#include "fonts/sans_16.h"

static gp5_ui_page_t page = {
    .title = "LiveWDF / RC",
    .font = &gp5_font_sans_16,
    .row_h = 20,
};

int fw_draw(int force)
{
    page.faded = !gp5_param(GP5_P_ON);
    return gp5_ui_page_draw(&page, force);
}

void fw_para_turn(int detents) { gp5_ui_page_turn(&page, detents); }
void fw_para_click(void) { gp5_ui_page_click(&page); }
void fw_foot_down(void) { gp5_param_set(GP5_P_ON, !gp5_param(GP5_P_ON)); }
