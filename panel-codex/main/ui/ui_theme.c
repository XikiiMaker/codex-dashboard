#include "ui_theme.h"

#include "ui_font.h"

lv_obj_t *ui_page_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_GAP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    return page;
}

lv_obj_t *ui_box(lv_obj_t *parent, lv_flex_flow_t flow, uint8_t grow)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_gap(box, UI_GAP, 0);
    /*
     * 基础对象默认就是 CLICKABLE 且不冒泡，所以盖在按钮上的装饰容器会把触摸吃掉，
     * 按钮再也收不到 CLICKED —— 首页扫码按钮中央那块图标+文字曾整片点不动。
     */
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, flow);
    if (grow > 0) {
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_flex_grow(box, grow);
    }
    return box;
}

lv_obj_t *ui_card(lv_obj_t *parent, uint8_t grow)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    /* 设置页的表单卡会重新打开滚动，默认主题那根浅色滚动条在深色卡上太扎眼 */
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_BORDER_HI), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(card, 6, LV_PART_SCROLLBAR);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if (grow > 0) {
        lv_obj_set_flex_grow(card, grow);
    }
    return card;
}

void ui_card_title(lv_obj_t *card, const char *text, uint32_t accent)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_set_size(bar, 3, 16);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_font(label, panel_font_small(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT_SUB), 0);
    lv_label_set_text(label, text);
}

lv_obj_t *ui_kv_row(lv_obj_t *card, const char *caption, lv_coord_t caption_w)
{
    lv_obj_t *row = ui_box(card, LV_FLEX_FLOW_ROW, 1);
    lv_obj_set_style_pad_gap(row, 10, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(row);
    /* 标签列定宽，各行的值才会左对齐成一列 */
    lv_obj_set_width(cap, caption_w);
    lv_obj_set_style_text_font(cap, panel_font_small(), 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(UI_COL_TEXT_SUB), 0);
    lv_label_set_text(cap, caption);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, panel_font_body(), 0);
    lv_obj_set_style_text_color(val, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_flex_grow(val, 1);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_label_set_text(val, "--");
    return val;
}

lv_obj_t *ui_value_unit(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_pad_gap(line, 6, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    /* 单位贴着数值基线：END 对齐让小字沉到大字底部，而不是垂直居中 */
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t *value = lv_label_create(line);
    lv_obj_set_style_text_font(value, panel_font_display(), 0);
    lv_obj_set_style_text_color(value, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(value, "--");

    lv_obj_t *unit = lv_label_create(line);
    lv_obj_set_style_text_font(unit, panel_font_body(), 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_style_pad_bottom(unit, 4, 0);
    lv_label_set_text(unit, "");

    return line;
}

void ui_value_set(lv_obj_t *obj, const char *value, const char *unit, uint32_t color)
{
    lv_obj_t *value_label = lv_obj_get_child(obj, 0);
    lv_obj_t *unit_label = lv_obj_get_child(obj, 1);

    lv_label_set_text(value_label, value);
    lv_obj_set_style_text_color(value_label, lv_color_hex(color), 0);
    lv_label_set_text(unit_label, unit ? unit : "");
}

lv_obj_t *ui_metric(lv_obj_t *parent, const char *caption, uint8_t grow)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_height(box, LV_PCT(100));
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COL_INSET), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (grow > 0) {
        lv_obj_set_flex_grow(box, grow);
    }

    lv_obj_t *cap = lv_label_create(box);
    lv_obj_set_style_text_font(cap, panel_font_small(), 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(UI_COL_TEXT_SUB), 0);
    lv_label_set_text(cap, caption);

    ui_value_unit(box);
    return box;
}

void ui_metric_set(lv_obj_t *metric, const char *value, const char *unit, uint32_t color)
{
    ui_value_set(lv_obj_get_child(metric, 1), value, unit, color);
}

lv_obj_t *ui_dot_text(lv_obj_t *parent, const lv_font_t *font, bool pill)
{
    const lv_coord_t dot = font->line_height >= 28 ? 12 : 8;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_gap(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (pill) {
        lv_obj_set_style_bg_opa(box, LV_OPA_20, 0);
        lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(box, 12, 0);
        lv_obj_set_style_pad_ver(box, 6, 0);
    } else {
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
    }

    lv_obj_t *led = lv_obj_create(box);
    lv_obj_set_size(led, dot, dot);
    lv_obj_set_style_border_width(led, 0, 0);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(led, 0, 0);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(box);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, "--");

    return box;
}

void ui_dot_text_set(lv_obj_t *obj, const char *text, uint32_t color)
{
    const lv_color_t c = lv_color_hex(color);
    lv_obj_set_style_bg_color(obj, c, 0);
    lv_obj_set_style_bg_color(lv_obj_get_child(obj, 0), c, 0);

    lv_obj_t *label = lv_obj_get_child(obj, 1);
    lv_obj_set_style_text_color(label, c, 0);
    lv_label_set_text(label, text);
}

void ui_divider(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, ui_btn_kind_t kind, lv_event_cb_t cb)
{
    uint32_t bg = UI_COL_ACCENT;
    uint32_t fg = 0xFFFFFF;
    switch (kind) {
    case UI_BTN_SUCCESS:
        bg = UI_COL_OK;
        break;
    case UI_BTN_DANGER:
        bg = UI_COL_DANGER;
        break;
    case UI_BTN_GHOST:
        bg = UI_COL_INSET;
        fg = UI_COL_TEXT_SUB;
        break;
    case UI_BTN_PRIMARY:
    default:
        break;
    }

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(fg), 0);
    lv_obj_set_style_text_font(btn, panel_font_body(), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (kind == UI_BTN_GHOST) {
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_BORDER_HI), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    }
    lv_obj_set_style_bg_color(btn, lv_color_darken(lv_color_hex(bg), LV_OPA_30), LV_STATE_PRESSED);
    /*
     * 禁用态整体压暗到 35%：默认样式只把颜色调灰，在这块屏上和可用态几乎分不出来，
     * 而「关机被联锁禁用」必须一眼可辨，否则现场会反复戳同一个按钮。
     */
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    if (text != NULL) {
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    }
    return btn;
}

void ui_button_set_enabled(lv_obj_t *btn, bool enabled)
{
    if (enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

void ui_barcode_icon(lv_obj_t *parent, lv_coord_t height)
{
    static const uint8_t k_bars[] = { 4, 2, 7, 2, 3, 3, 8, 2, 4, 2, 2, 6, 3, 2, 5 };

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_SIZE_CONTENT, height);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_gap(box, 3, 0);
    /* 图标画在按钮里面，可点击的话就把按钮的触摸吃掉了，见 ui_box 的说明 */
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < sizeof(k_bars) / sizeof(k_bars[0]); i++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_set_size(bar, k_bars[i], LV_PCT(100));
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

void ui_keyboard_style(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_border_color(kb, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_radius(kb, 12, 0);
    /*
     * 键上只有 ASCII 和 LV_SYMBOL_*，不能继承中文字体：tiny_ttf 光栅化的 NotoSansSC
     * 里没有 LVGL 私有区的符号字形，退格/回车/Shift 会全变方块。
     */
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, 0);

    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COL_INSET), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(UI_COL_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    /*
     * 功能键（切换、退格、回车…）带 CHECKED 状态，默认主题给这个状态单独加了灰底，
     * 而状态权重比 DEFAULT 的局部样式高 —— 只设 DEFAULT 的话功能键仍是白底。
     */
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COL_BORDER_HI),
                              (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COL_TEXT),
                                (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
}
