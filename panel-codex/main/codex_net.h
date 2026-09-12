#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动上行链路（以太网必开，编译期配了 SSID 才开 WiFi）
 *
 * 非阻塞：网线没插也照常返回，链路事件会持续刷新 codex_state 里的 link/ip，
 * 面板因此能在断网时先把界面显示出来并提示，而不是卡在启动画面。
 *
 * 两条链路同时在线时以太网胜出（route_prio 更高）。WiFi 经板载 ESP32-C6 走
 * esp-hosted，起不来只记日志不算失败。
 */
esp_err_t codex_net_start(void);

/** 链路是否已拿到 IP */
bool codex_net_is_up(void);

/** 阻塞等待拿到 IP，超时返回 false */
bool codex_net_wait_up(int timeout_ms);

#ifdef __cplusplus
}
#endif
