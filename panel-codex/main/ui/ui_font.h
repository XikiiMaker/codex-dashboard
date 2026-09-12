#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 字体入口
 *
 * 面板 UI 全是中文，而 LVGL 内置的 Montserrat 只有拉丁字形。中文字形来自
 * font 分区里的完整 NotoSansSC，由 lv_tiny_ttf 在运行时按需光栅化——不再用
 * lv_font_conv 生成编译期位图子集，因为工单里的师傅姓名、初步诊断是服务端
 * 下发的任意中文，子集的字表在编译期定死，现场必然撞上缺字方块。
 *
 * 字体不随固件走，需单独烧到 font 分区一次（见 tools/flash-font.bat）。
 * 没烧写时回退到 Montserrat：屏能亮、布局能看，但中文不可读，
 * panel_font_has_cjk() 返回 false，UI 据此在屏上提示「字体缺失」。
 */

/**
 * 加载字体，必须在 LVGL 初始化之后、创建任何 UI 之前调用一次。
 * 重复调用无副作用。失败不阻塞启动，只是退到 Montserrat。
 */
void panel_font_init(void);

const lv_font_t *panel_font_small(void);
const lv_font_t *panel_font_body(void);
const lv_font_t *panel_font_title(void);
/** 大号数值（天数、次数、功率），只用在指标块上 */
const lv_font_t *panel_font_display(void);

/** 中文字形是否可用，UI 据此在屏上提示「字体缺失」 */
bool panel_font_has_cjk(void);

#ifdef __cplusplus
}
#endif
