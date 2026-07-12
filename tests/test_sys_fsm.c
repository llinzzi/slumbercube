/*
 * test_sys_fsm.c — Step 1 skeleton test。
 * Step 4 起替换为完整转换矩阵。
 */
#include <assert.h>
#include <stdio.h>
#include "../main/regions/sys_fsm.h"

int main(void)
{
    sys_state_t s = SYS_BOOT;
    app_input_t inp = {0};
    fsm_actions_t a = sys_fsm_step(&s, SYS_EVT_BOOT_DONE, &inp);
    assert(a.count == 0);
    assert(s == SYS_BOOT);

    s = SYS_NORMAL;
    a = sys_fsm_step(&s, SYS_EVT_BTN_SLEEP_PRESS, &inp);
    assert(a.count == 0);
    assert(s == SYS_NORMAL);

    printf("ok sys_fsm identity (Step 1 skeleton)\n");
    return 0;
}
