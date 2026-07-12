/*
 * test_event_router.c — Step 1 skeleton test。
 * 当前 skeleton 实现:route_event() 返回全 *_EVT_NONE。等 Step 8 补完整 fan-out。
 */
#include <assert.h>
#include <stdio.h>
#include "../main/event_router.h"

int main(void)
{
    app_input_t inp = {0};
    routed_events_t r = route_event(EVT_TICK_1HZ, &inp);
    /* skeleton 实现:全部为 *_EVT_NONE */
    assert(r.wake   == WAKE_EVT_NONE);
    assert(r.sys    == SYS_EVT_NONE);
    assert(r.net    == NET_EVT_NONE);
    assert(r.audio  == AUDIO_EVT_NONE);
    assert(r.display == DISP_EVT_NONE);

    r = route_event(EVT_BTN_SLEEP_PRESS, &inp);
    assert(r.wake == WAKE_EVT_NONE);
    assert(r.sys  == SYS_EVT_NONE);

    printf("ok event_router identity (Step 1 skeleton)\n");
    return 0;
}
