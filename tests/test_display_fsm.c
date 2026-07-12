/*
 * test_display_fsm.c — Step 1 skeleton test。
 * Step 7 起替换为完整转换矩阵。
 */
#include <assert.h>
#include <stdio.h>
#include "../main/regions/display_fsm.h"

int main(void)
{
    display_state_t s = DISP_DAY;
    app_input_t inp = {0};
    fsm_actions_t a = display_fsm_step(&s, DISP_EVT_TICK_1HZ, &inp);
    assert(a.count == 0);
    assert(s == DISP_DAY);

    s = DISP_NIGHT_AUTO;
    a = display_fsm_step(&s, DISP_EVT_BTN_TOGGLE, &inp);
    assert(a.count == 0);
    assert(s == DISP_NIGHT_AUTO);

    printf("ok display_fsm identity (Step 1 skeleton)\n");
    return 0;
}
