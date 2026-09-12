#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动轮询任务，立刻取一次，之后每 CODEX_POLL_INTERVAL_MS 一轮 */
esp_err_t codex_client_start(void);

/** 立刻唤醒轮询任务取一次（触屏手动刷新用），不阻塞调用方 */
void codex_client_poll_now(void);

/**
 * 发一条媒体控制命令，并立刻唤醒媒体任务把它 POST 出去
 *
 * 给 UI 线程用的，不阻塞：在 LVGL 回调里直接发 HTTP 会让屏幕卡住整个超时时间。
 * 命令先进待发槽，任务醒来后发送，紧接着重取一次状态，所以触屏到画面更新之间
 * 只隔一个往返。
 *
 * value 的语义决定它排不排队：
 *   带值的命令（set_volume 传百分比、seek 传毫秒）是绝对量，同名命令在槽里合并成
 *   最后一条 —— 拖动音量条会连着触发几十次，每条都发出去只会把播放器淹了。
 *   不带值的传 -1，每条独立排队，因为连点两次 play_pause 必须真的切两下。
 *
 * 槽满了就丢掉这条并打日志：那意味着链路已经卡死，此时排队只会让命令在恢复后
 * 一次性炸出来，把播放器切到一个谁都没点过的状态。
 */
void codex_client_media_cmd(const char *action, int32_t value);

#ifdef __cplusplus
}
#endif
