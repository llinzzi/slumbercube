#include "ui.h"

void ui_init(void) {
    create_screens();
}

void ui_tick(void) {
    tick_screen_main();
}