/*
 * test_audio_fsm.c — Step 1 skeleton test。
 * Step 6 起替换为完整转换矩阵,关键边界包括 PENDING 不亮 indicator。
 */
#include <assert.h>
#include <stdio.h>
#include "../main/regions/audio_fsm.h"

int main(void)
{
    audio_state_t s = AUDIO_IDLE;
    app_input_t inp = {0};
    inp.net_connected = true;
    fsm_actions_t a = audio_fsm_step(&s, AUDIO_EVT_BTN_TOGGLE, &inp);
    assert(a.count == 0);
    assert(s == AUDIO_IDLE);

    s = AUDIO_PLAYING;
    a = audio_fsm_step(&s, AUDIO_EVT_PLAYER_IDLE, &inp);
    assert(a.count == 0);
    assert(s == AUDIO_PLAYING);

    printf("ok audio_fsm identity (Step 1 skeleton)\n");
    return 0;
}
