/*
 * test_wake_fsm.c — Step 1 skeleton test:
 *   - 链接生产 regions/wake_fsm.c (不是镜像复制)
 *   - 验证对任意 (state, evt) 输入,identity 实现返回 {0 actions},且不修改 *cur
 *
 * Step 3 起会逐步替换为完整转换矩阵断言(见 plan "测试矩阵 -> wake_fsm")。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/regions/wake_fsm.h"

static int passed = 0;
#define CHECK(expr) do { assert(expr); passed++; } while (0)

static void test_identity_no_event(void)
{
    wake_state_t s = WAKE_DORMANT;
    app_input_t inp = {0};
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_NONE, &inp);
    CHECK(a.count == 0);
    CHECK(s == WAKE_DORMANT);
}

static void test_identity_all_events(void)
{
    /* skeleton 实现:任何事件都是 identity */
    wake_state_t s = WAKE_FROM_RTC;
    app_input_t inp = {0};
    fsm_actions_t a = wake_fsm_step(&s, WAKE_EVT_DETECT_SOURCE, &inp);
    CHECK(a.count == 0);
    CHECK(s == WAKE_FROM_RTC);
}

int main(void)
{
    test_identity_no_event();
    test_identity_all_events();
    printf("ok wake_fsm identity (Step 1 skeleton)\n");
    return 0;
}
