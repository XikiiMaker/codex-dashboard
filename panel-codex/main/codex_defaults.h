#pragma once

/**
 * 编译期配置
 *
 * 这块面板是放在桌上给自己看的，不像车间面板那样要逐台配机位号，所以没有设置页，
 * 也不读 NVS：所有参数在烧写时用 -D 传进来一次即可。
 *
 *   build.bat build -DCODEX_BRIDGE_HOST=192.168.31.194 -DCODEX_WIFI_SSID=xxx -DCODEX_WIFI_PASS=yyy
 *
 * 转发地址指向 PC 上跑的 bridge/codex-bridge.js。配额与任务动态只能由那台机器上的
 * codex app-server 提供，面板自己拿不到，所以这个地址是必填项。
 */

/** bridge 所在主机，可带端口（如 "192.168.31.194:8787"）。留空则只显示未配置提示 */
#ifndef CODEX_BRIDGE_HOST
#define CODEX_BRIDGE_HOST ""
#endif

#ifndef CODEX_BRIDGE_PORT
#define CODEX_BRIDGE_PORT 8787
#endif

/** bridge 若用 --token 启动，这里要填同一个值，会以 X-Panel-Token 发送 */
#ifndef CODEX_BRIDGE_TOKEN
#define CODEX_BRIDGE_TOKEN ""
#endif

/** WiFi 兜底链路。留空则只用有线 */
#ifndef CODEX_WIFI_SSID
#define CODEX_WIFI_SSID ""
#endif

#ifndef CODEX_WIFI_PASS
#define CODEX_WIFI_PASS ""
#endif

/** 拉取间隔：bridge 侧任务动态是 30 秒一轮，面板比它略快没有意义 */
#ifndef CODEX_POLL_INTERVAL_MS
#define CODEX_POLL_INTERVAL_MS 20000
#endif

/**
 * 媒体轮询间隔，刻意和看板分开
 *
 * 播放位置是每轮都变的，跟着 20 秒走的话进度条就是一格一格跳。2 秒一轮只是用来
 * 校正漂移 —— 位置的平滑前进靠面板本地单调时钟外推（codex_media_t.fetched_at_us），
 * 所以这个值改大改小都不会让进度条卡顿，只影响暂停/切歌后多久能被面板看到。
 */
#ifndef CODEX_MEDIA_POLL_INTERVAL_MS
#define CODEX_MEDIA_POLL_INTERVAL_MS 2000
#endif
