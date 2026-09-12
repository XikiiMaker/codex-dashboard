#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 建立看板界面。必须在 LVGL 已初始化、且持有显示锁的情况下调用一次 */
void ui_dashboard_create(void);

/**
 * 按最新快照刷新界面
 *
 * 由内部的 1 秒定时器自己驱动，外部无需调用。倒计时和「更新于」要每秒走动，
 * 所以刷新周期比取数周期密得多，只改文本不重建控件。
 */

#ifdef __cplusplus
}
#endif
