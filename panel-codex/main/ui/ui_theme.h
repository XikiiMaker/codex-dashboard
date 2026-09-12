#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 面板 UI 的设计规范与共用组件
 *
 * 版式沿用控制机端 GpuTestStation（细边框卡片、大号彩色数值），配色是纯黑底加
 * 近黑灰的中性色阶。看板页与音乐卡都从这里取样式，避免同一种卡片各写一遍后慢慢长歪。
 */

/* ---------- 配色 ---------- */

/*
 * 纯黑底 + 极深的近黑灰，整套中性色不带色相。
 * 明度层级必须保持 底 < 卡 < 嵌入 < 边框 < 高亮边框：卡片和页面底只差 11 个灰阶，
 * 拉开层次靠的是那 1px 边框，边框再暗下去卡片就和背景糊成一片了。
 */
#define UI_COL_BG        0x000000 /* 页面底 */
#define UI_COL_CARD      0x0B0B0B /* 卡片底 */
#define UI_COL_INSET     0x151515 /* 卡片内的嵌入块、输入框 */
#define UI_COL_BORDER    0x212121
#define UI_COL_BORDER_HI 0x303030
#define UI_COL_TEXT      0xF2F2F2 /* 主文字 */
#define UI_COL_TEXT_SUB  0x9E9E9E /* 标签 */
#define UI_COL_TEXT_DIM  0x6A6A6A /* 单位、占位 */
#define UI_COL_ACCENT    0x3B82F6 /* 蓝：主动作、工单号 */
#define UI_COL_OK        0x22C55E /* 绿：在位、待机 */
#define UI_COL_INFO      0x22D3EE /* 青：测试中 */
#define UI_COL_WARN      0xF59E0B /* 琥珀：未绑定、槽位空 */
#define UI_COL_DANGER    0xEF4444 /* 红：关机、故障 */

/* ---------- 间距 ---------- */

#define UI_PAD 16 /* 页面四周 */
#define UI_GAP 12 /* 卡片之间 */

/* ---------- 组件 ---------- */

typedef enum {
    UI_BTN_PRIMARY,
    UI_BTN_SUCCESS,
    UI_BTN_DANGER,
    UI_BTN_GHOST,
} ui_btn_kind_t;

/** 页容器：透明、pad UI_PAD、纵向 flex、行距 UI_GAP */
lv_obj_t *ui_page_create(lv_obj_t *parent);

/** 透明无边框的纯布局容器。grow 为 0 时不参与主轴分配，需自行设尺寸 */
lv_obj_t *ui_box(lv_obj_t *parent, lv_flex_flow_t flow, uint8_t grow);

/** 卡片：卡片底 + 1px 边框 + 圆角，纵向 flex */
lv_obj_t *ui_card(lv_obj_t *parent, uint8_t grow);

/** 卡片小标题：左侧一道竖色条 + 次要色文字，整行不参与高度分配 */
void ui_card_title(lv_obj_t *card, const char *text, uint32_t accent);

/** 卡片里的一行「标签 + 值」，同卡片内各行等分高度，返回值标签 */
lv_obj_t *ui_kv_row(lv_obj_t *card, const char *caption, lv_coord_t caption_w);

/**
 * 大号数值 + 小号单位，单位沉到数值基线
 *
 * 返回容器，用 ui_value_set 写值 —— 数值和单位是两个不同字号的 label，
 * 一个 label 里做不到。
 */
lv_obj_t *ui_value_unit(lv_obj_t *parent);
void ui_value_set(lv_obj_t *obj, const char *value, const char *unit, uint32_t color);

/** 指标块：内嵌底 + 小标题 + ui_value_unit，用于并排的统计格 */
lv_obj_t *ui_metric(lv_obj_t *parent, const char *caption, uint8_t grow);
void ui_metric_set(lv_obj_t *metric, const char *value, const char *unit, uint32_t color);

/**
 * 圆点 + 文字的状态显示，pill=true 时额外加同色半透明底与药丸外形
 *
 * 返回容器，用 ui_dot_text_set 一次改掉圆点和文字的颜色 —— 状态色变化时两者
 * 必须同步，分开设必然有一处漏改。
 */
lv_obj_t *ui_dot_text(lv_obj_t *parent, const lv_font_t *font, bool pill);
void ui_dot_text_set(lv_obj_t *obj, const char *text, uint32_t color);

/** 卡内 1px 分隔线，把主值区与明细区分开 */
void ui_divider(lv_obj_t *parent);

/** 按钮：kind 决定配色，按下加深、禁用整体变暗 */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, ui_btn_kind_t kind, lv_event_cb_t cb);
void ui_button_set_enabled(lv_obj_t *btn, bool enabled);

/** 条码图标，画在扫码按钮上（LVGL 的符号字体里没有条形码字形） */
void ui_barcode_icon(lv_obj_t *parent, lv_coord_t height);

/** 键盘配色，扫码页与设置页共用 */
void ui_keyboard_style(lv_obj_t *kb);

#ifdef __cplusplus
}
#endif
