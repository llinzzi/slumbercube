/*
 * test_net_fsm.c — Step 1 skeleton test。
 * Step 5 起替换为完整转换矩阵。
 */
#include <assert.h>
#include <stdio.h>
#include "../main/regions/net_fsm.h"

int main(void)
{
    net_state_t s = NET_OFFLINE;
    app_input_t inp = {0};
    fsm_actions_t a = net_fsm_step(&s, NET_EVT_NO_CREDS_AT_BOOT, &inp);
    assert(a.count == 0);
    assert(s == NET_OFFLINE);

    s = NET_CONNECTING;
    a = net_fsm_step(&s, NET_EVT_IP_GOT, &inp);
    assert(a.count == 0);
    assert(s == NET_CONNECTING);

    printf("ok net_fsm identity (Step 1 skeleton)\n");
    return 0;
}
