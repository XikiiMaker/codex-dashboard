#include "ui_dashboard.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "codex_client.h"
#include "codex_state.h"
#include "esp_timer.h"
#include "ui_font.h"
#include "ui_music.h"
#include "ui_theme.h"

/*
 * 600×1024 竖屏看板布局
 *
 * 顶层 CMakeLists 里 CODEX_PORTRAIT=1 是无条件开着的，画布是竖的 600×1024，
 * 不是横屏 1024×600 —— BSP 里连触摸坐标的旋转都是为竖屏单独补的。
 *
 * 原项目是给 400×300 黑白墨水屏画的，等比放到这块屏上会空得离谱，所以只沿用它的
 * 信息层级和优先级规则：额度占主位，雷达在旁，任务动态列成表。
 *
 * 高度分账（内容区合计 992 = 1024 - 上下 UI_PAD）：
 *   顶栏 56 + UI_GAP 12 + 中区 166 + UI_GAP 12 + 任务卡 315 + UI_GAP 12 + 音乐卡(余下)
 * 任务卡固定五行而不是撑满：bridge 最多下发 CODEX_MAX_TASKS 条动态，卡撑满的话
 * 两三条动态下面会留出半屏空白（实测就是用户抱怨的「下半部分比较空」）。
 * 剩下的整块高度给音乐控制器，它的歌词窗会吃掉所有余量，不怕多也不怕少。
 */
#define TOPBAR_H 56
/**
 * 中区高度。多出来的 6px 是给底行的 margin_top 付账的 —— 卡片不可滚动，
 * 行高加间距一旦超出 MID_H，末尾那行就直接看不见。
 */
#define MID_H 166
/**
 * 进度条（雷达卡是信号尺）与底行文字之间的额外间距
 *
 * 不加在卡片的 pad_row 上：那会同时拉开标题、大数字、进度条之间的每一段，
 * 只想让「条」和「条下面的字」透气一点的话，得单独给底行加 margin_top。
 * 两张卡都要加，否则左右底行不再齐平。
 */
#define MID_FOOT_GAP 6
/**
 * 中排两张卡的宽度：各占一半。页内容宽 600-16×2=568，减掉卡间距 12 再对半。
 * 写死而不是两张都给 flex_grow=1：grow 分的是「扣掉各自内容最小宽之后」的余量，
 * 额度卡的底行比雷达卡宽三十来像素，等 grow 分出来还是会一宽一窄。
 */
#define MID_CARD_W 278
#define TASK_ROW_H 48
/**
 * 任务卡高度：pad 24 + 标题行 26 + 分隔线 1 + (行数+1) 个行距 + 行高×行数。
 * 写死而不是 flex_grow，音乐卡才是那个吸收剩余高度的。
 */
#define TASK_CARD_H \
    (24 + 26 + 1 + 4 * (CODEX_MAX_TASKS + 1) + TASK_ROW_H * CODEX_MAX_TASKS)
/** 十格信号尺：每格代表 10%，与原项目一致，向上取整 */
#define RADAR_CELLS 10
/** 格高必须等于额度卡进度条那 12px，两张并排卡的底行才会落在同一条水平线上 */
#define RADAR_CELL_H 12
/**
 * 右上说明列定宽。雷达卡内容区只有 252px（278 减边框和 pad 24），不定宽的话
 * 上游那个最长 63 字符的 forecast_window 会把整行挤爆，大号数字被推出卡片。
 */
#define RADAR_SIDE_W 84
#define REFRESH_PERIOD_MS 1000

static struct {
    /* 额度 */
    lv_obj_t *quota_title;
    lv_obj_t *quota_value;
    lv_obj_t *quota_used;
    lv_obj_t *quota_bar_fill;
    lv_obj_t *quota_reset;
    lv_obj_t *quota_credits;
    /* 双窗口时才建，单窗口时隐藏 */
    lv_obj_t *quota_second;
    lv_obj_t *quota_second_value;
    lv_obj_t *quota_second_bar_fill;
    lv_obj_t *quota_second_reset;

    /* 雷达：与额度卡共用一套栅格，字段顺序就是行顺序 */
    lv_obj_t *radar_value;
    lv_obj_t *radar_when;
    lv_obj_t *radar_kind;
    lv_obj_t *radar_cells[RADAR_CELLS];
    lv_obj_t *radar_state;
    lv_obj_t *radar_flag;

    /* 任务 */
    lv_obj_t *task_hidden;
    lv_obj_t *task_rows[CODEX_MAX_TASKS];
    lv_obj_t *task_states[CODEX_MAX_TASKS];
    lv_obj_t *task_titles[CODEX_MAX_TASKS];
    lv_obj_t *task_times[CODEX_MAX_TASKS];
    lv_obj_t *task_empty;

    uint32_t drawn_rev;
} s_ui;

/* ---------------------------------------------------------------- 文案 */

static const char *task_state_label(codex_task_state_t state)
{
    switch (state) {
    case CODEX_TASK_RUNNING: return "执行中";
    case CODEX_TASK_FAILED: return "失败";
    case CODEX_TASK_INTERRUPTED: return "已中断";
    default: return "本轮完成";
    }
}

static uint32_t task_state_color(codex_task_state_t state)
{
    switch (state) {
    case CODEX_TASK_RUNNING: return UI_COL_INFO;
    case CODEX_TASK_FAILED: return UI_COL_DANGER;
    case CODEX_TASK_INTERRUPTED: return UI_COL_WARN;
    default: return UI_COL_OK;
    }
}

/** 剩余额度越少颜色越警惕，一眼就能判断还能不能开新任务 */
static uint32_t quota_color(int32_t remaining_percent)
{
    if (remaining_percent <= 10) return UI_COL_DANGER;
    if (remaining_percent <= 30) return UI_COL_WARN;
    return UI_COL_OK;
}

/** "6 天 21 小时" / "3 小时 12 分" / "8 分钟"，不足一分钟按一分钟算 */
static void format_duration(int32_t seconds, char *out, size_t cap)
{
    if (seconds < 0) {
        strlcpy(out, "--", cap);
        return;
    }
    int32_t days = seconds / 86400;
    int32_t hours = (seconds % 86400) / 3600;
    int32_t minutes = (seconds % 3600 + 59) / 60;
    if (days > 0) {
        snprintf(out, cap, "%" PRId32 " 天 %" PRId32 " 小时", days, hours);
    } else if (hours > 0) {
        snprintf(out, cap, "%" PRId32 " 小时 %" PRId32 " 分", hours, (seconds % 3600) / 60);
    } else {
        snprintf(out, cap, "%" PRId32 " 分钟", minutes > 0 ? minutes : 1);
    }
}

/** "刚刚" / "5 分钟前" / "2 小时前" / "3 天前" */
static void format_ago(int32_t seconds, char *out, size_t cap)
{
    if (seconds < 0) {
        strlcpy(out, "--", cap);
    } else if (seconds < 60) {
        strlcpy(out, "刚刚", cap);
    } else if (seconds < 3600) {
        snprintf(out, cap, "%" PRId32 " 分钟前", seconds / 60);
    } else if (seconds < 86400) {
        snprintf(out, cap, "%" PRId32 " 小时前", seconds / 3600);
    } else {
        snprintf(out, cap, "%" PRId32 " 天前", seconds / 86400);
    }
}

/* ---------------------------------------------------------------- 小控件 */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

/**
 * 右对齐 + 超长截断的说明标签
 *
 * 雷达卡右上那两行用。宽度由父容器定死，文字靠右压到卡片内边缘，这样才会和左边
 * 额度卡的「7天窗口」右对齐；上游的 forecast_window 可能长到六十多个字符，不截断
 * 会把整行连同大号数字一起顶出卡片。
 */
static lv_obj_t *make_side_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *label = make_label(parent, font, UI_COL_TEXT_SUB, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

/**
 * 进度条：外槽 + 内填充
 *
 * 不用 lv_bar：这里要跟着剩余额度换颜色，直接操作填充块的宽度和底色比配 bar 的
 * indicator 部件样式短。返回填充块，由调用方按百分比设宽。
 */
static lv_obj_t *make_bar(lv_obj_t *parent, lv_coord_t height)
{
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_width(track, LV_PCT(100));
    lv_obj_set_height(track, height);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(UI_COL_INSET), 0);
    lv_obj_set_style_radius(track, height / 2, 0);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_height(fill, height);
    lv_obj_set_width(fill, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, height / 2, 0);
    return fill;
}

static void bar_set(lv_obj_t *fill, int32_t percent, uint32_t color)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_obj_set_width(fill, LV_PCT(percent));
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
}

/**
 * 卡内的一行：宽度撑满、高度随内容
 *
 * ui_box() 在 grow=0 时既不设宽也不设高，高度会掉回 lv_obj 的默认值（约 100px）。
 * 后果是行与行之间撑出大片空白，而排在后面的行被挤出卡片直接裁掉 —— 额度卡的
 * 「重置」那行就是这么消失的。
 */
static lv_obj_t *make_row(lv_obj_t *parent, lv_flex_flow_t flow, lv_coord_t gap)
{
    lv_obj_t *row = ui_box(parent, flow, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(row, gap, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

/** 撑开中间，把后面的内容挤到右侧 */
static void add_spacer(lv_obj_t *parent)
{
    lv_obj_t *spacer = ui_box(parent, LV_FLEX_FLOW_ROW, 1);
    lv_obj_set_height(spacer, 1);
}

/* ---------------------------------------------------------------- 顶栏 */

/**
 * 顶栏只剩一个标题
 *
 * 这一行原来还挂着链路与 IP、「更新于多久前」、刷新按钮和一条「正在刷新…」的提示，
 * 现在全部去掉：这块屏是挂着不动的看板，链路和 IP 只在装机时有用，刷新按钮在
 * REFRESH_PERIOD_MS 一轮的自动刷新面前基本是装饰。
 *
 * 代价要记着：取数失败时画面会停在上一次的有效数据上，而「连续 N 次取数失败，
 * 画面为 X 分钟前」那句提示原来就挂在顶栏，现在没有任何地方能说明这件事 ——
 * 看板上的数字变陈旧时不会有提示。
 *
 * TOPBAR_H 保持 56 不动：下面所有卡片的高度分账都是按它算的。标题一行只占 40，
 * 多出来的变成上下留白。
 */
static void build_topbar(lv_obj_t *page)
{
    lv_obj_t *bar = ui_box(page, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(bar, LV_PCT(100), TOPBAR_H);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_label(bar, panel_font_title(), UI_COL_TEXT, "CodeX 工作看板");
}

/**
 * 补齐卡片在 cross 方向的尺寸并收紧内边距
 *
 * ui_card() 只设 flex_grow，cross 方向不设尺寸就会掉回 lv_obj 的默认值 ——
 * 横向排的卡会只剩 120px 高把内容裁掉。ui_theme.c 里的 ui_metric() 显式写
 * set_height(LV_PCT(100)) 就是这个原因。主题默认的 18/10 内边距在 220px 高度里
 * 装不下这一列内容，一并收紧。
 */
static void tighten_card(lv_obj_t *card, bool stretch_height)
{
    if (stretch_height) {
        lv_obj_set_height(card, LV_PCT(100));
    } else {
        lv_obj_set_width(card, LV_PCT(100));
    }
    /*
     * 12 / 4 是按最挤的那张卡（额度卡）算出来的：pad 24 之外还要放标题、大数字、
     * 进度条、底行（有第二窗口时再加一行），段与段之间只有 4px。再调大这两个数，
     * 末尾的「重置」那行就会被裁掉 —— 卡片不可滚动，超出 MID_H 的部分直接看不见。
     * 进度条和底行之间那点额外间距因此走 MID_FOOT_GAP（底行的 margin_top）。
     */
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
}

/* ---------------------------------------------------------------- 额度卡 */

static void build_quota_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 0);
    lv_obj_set_width(card, MID_CARD_W);
    tighten_card(card, true);
    ui_card_title(card, "官方额度", UI_COL_ACCENT);

    /* 主窗口：大数字 + 右侧窗口说明 */
    lv_obj_t *head = make_row(card, LV_FLEX_FLOW_ROW, 8);
    /* cross 用 END：让「% 剩余」和右侧说明沉到大数字的基线上 */
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    s_ui.quota_value = ui_value_unit(head);
    ui_value_set(s_ui.quota_value, "--", "% 剩余", UI_COL_TEXT_DIM);

    add_spacer(head);

    lv_obj_t *right = ui_box(head, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 2, 0);
    s_ui.quota_title = make_label(right, panel_font_small(), UI_COL_TEXT_SUB, "窗口 --");
    s_ui.quota_used = make_label(right, panel_font_body(), UI_COL_TEXT_SUB, "已用 --");

    s_ui.quota_bar_fill = make_bar(card, 12);

    lv_obj_t *foot = make_row(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_style_margin_top(foot, MID_FOOT_GAP, 0);
    /* 左下角和雷达卡的「普通重置已确认」、本行右侧的「重置额度」同款字号字色 */
    s_ui.quota_reset = make_label(foot, panel_font_small(), UI_COL_TEXT_SUB, "重置 --");
    add_spacer(foot);
    s_ui.quota_credits = make_label(foot, panel_font_small(), UI_COL_TEXT_SUB, "重置额度 0");

    /*
     * 第二个窗口（Plus 的 5 小时窗 / Pro 的短窗）只有账号真的有两档限流时才出现，
     * 单窗口时整块隐藏而不是显示一排 "--"。
     */
    s_ui.quota_second = make_row(card, LV_FLEX_FLOW_ROW, 8);
    s_ui.quota_second_value = make_label(s_ui.quota_second, panel_font_body(), UI_COL_TEXT, "--");
    lv_obj_t *second_bar_box = ui_box(s_ui.quota_second, LV_FLEX_FLOW_ROW, 1);
    lv_obj_set_height(second_bar_box, 8);
    s_ui.quota_second_bar_fill = make_bar(second_bar_box, 8);
    s_ui.quota_second_reset =
        make_label(s_ui.quota_second, panel_font_small(), UI_COL_TEXT_SUB, "--");
    lv_obj_add_flag(s_ui.quota_second, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------- 雷达卡 */

static void build_radar_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 0);
    lv_obj_set_width(card, MID_CARD_W);
    tighten_card(card, true);
    ui_card_title(card, "重置雷达", UI_COL_INFO);

    /*
     * 逐行镜像左边的额度卡：头行（大号数值 + 右侧两行说明）/ 信号尺 / 底行，
     * 行高、字号、行距一一对应。两张卡并排，栅格不同就没有「对齐」可言 ——
     * 右上的时间要和左卡的「7天窗口」压在同一条水平线上，底行的状态要和
     * 「重置 6天15小时」齐平，靠的是这两张卡用同一套行结构，而不是各自居中。
     */
    lv_obj_t *head = make_row(card, LV_FLEX_FLOW_ROW, 8);
    /* cross 用 END，和额度卡一样让右侧说明沉到大数字的基线上 */
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    s_ui.radar_value = ui_value_unit(head);
    ui_value_set(s_ui.radar_value, "--", "", UI_COL_TEXT_DIM);

    add_spacer(head);

    lv_obj_t *right = ui_box(head, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_size(right, RADAR_SIDE_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 2, 0);
    s_ui.radar_when = make_side_label(right, panel_font_small(), "--");
    s_ui.radar_kind = make_side_label(right, panel_font_body(), "");

    /* 十格信号尺：每格 10%，向上取整。占的是额度卡进度条那一行 */
    lv_obj_t *ruler = ui_box(card, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(ruler, LV_PCT(100));
    lv_obj_set_height(ruler, RADAR_CELL_H);
    lv_obj_set_style_pad_column(ruler, 4, 0);
    for (int i = 0; i < RADAR_CELLS; i++) {
        lv_obj_t *cell = lv_obj_create(ruler);
        lv_obj_remove_style_all(cell);
        lv_obj_set_flex_grow(cell, 1);
        lv_obj_set_height(cell, RADAR_CELL_H);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(cell, 3, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(UI_COL_BORDER_HI), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COL_INSET), 0);
        s_ui.radar_cells[i] = cell;
    }

    lv_obj_t *foot = make_row(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_style_margin_top(foot, MID_FOOT_GAP, 0);
    /*
     * 「数据旧」在左、状态在右，和额度卡底行（左「重置 …」右「重置额度」）互为镜像。
     * 状态吃掉全部余量再右对齐，文字才会压在卡片内容区的右边线上。标记为空时必须
     * 整块隐藏：LVGL 的 flex 跳过 hidden 项，但一个空标签仍是布局项，会白占一个
     * 列间距，把状态文字顶离右边缘 8px。
     */
    s_ui.radar_flag = make_label(foot, panel_font_small(), UI_COL_TEXT_SUB, "");
    lv_obj_add_flag(s_ui.radar_flag, LV_OBJ_FLAG_HIDDEN);
    s_ui.radar_state = make_label(foot, panel_font_small(), UI_COL_TEXT_SUB, "信号不可用");
    lv_obj_set_flex_grow(s_ui.radar_state, 1);
    lv_obj_set_style_text_align(s_ui.radar_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_ui.radar_state, LV_LABEL_LONG_DOT);
}

/* ---------------------------------------------------------------- 任务卡 */

static void build_task_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 0);
    tighten_card(card, false);
    lv_obj_set_height(card, TASK_CARD_H);

    lv_obj_t *head = make_row(card, LV_FLEX_FLOW_ROW, UI_GAP);
    make_label(head, panel_font_body(), UI_COL_TEXT_SUB, "任务动态");
    add_spacer(head);
    s_ui.task_hidden = make_label(head, panel_font_small(), UI_COL_TEXT_DIM, "");

    ui_divider(card);

    for (int i = 0; i < CODEX_MAX_TASKS; i++) {
        lv_obj_t *row = ui_box(card, LV_FLEX_FLOW_ROW, 0);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, TASK_ROW_H);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, UI_GAP, 0);

        s_ui.task_states[i] = ui_dot_text(row, panel_font_body(), true);
        /* 状态标签定宽，三行的标题才会左对齐成一列 */
        lv_obj_set_width(s_ui.task_states[i], 132);

        s_ui.task_titles[i] = make_label(row, panel_font_body(), UI_COL_TEXT, "");
        lv_label_set_long_mode(s_ui.task_titles[i], LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(s_ui.task_titles[i], 1);

        s_ui.task_times[i] = make_label(row, panel_font_small(), UI_COL_TEXT_DIM, "");

        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        s_ui.task_rows[i] = row;
    }

    s_ui.task_empty = make_label(card, panel_font_body(), UI_COL_TEXT_DIM, "24 小时内没有任务动态");
}

/* ---------------------------------------------------------------- 刷新 */

static void refresh_quota(const codex_snapshot_t *snap, int32_t elapsed)
{
    if (!snap->quota_valid || snap->window_count == 0) {
        return;
    }
    const codex_window_t *primary = &snap->windows[0];
    char buf[64];

    snprintf(buf, sizeof(buf), "%" PRId32, primary->remaining_percent);
    ui_value_set(s_ui.quota_value, buf, "% 剩余", quota_color(primary->remaining_percent));
    bar_set(s_ui.quota_bar_fill, primary->remaining_percent,
            quota_color(primary->remaining_percent));

    snprintf(buf, sizeof(buf), "%s窗口%s", primary->name, snap->quota_stale ? " · 数据旧" : "");
    lv_label_set_text(s_ui.quota_title, buf);

    snprintf(buf, sizeof(buf), "已用 %" PRId32 "%%", primary->used_percent);
    lv_label_set_text(s_ui.quota_used, buf);

    char duration[40];
    format_duration(primary->resets_in_seconds - elapsed, duration, sizeof(duration));
    snprintf(buf, sizeof(buf), "重置 %s", duration);
    lv_label_set_text(s_ui.quota_reset, buf);

    snprintf(buf, sizeof(buf), "重置额度 %" PRId32, snap->reset_credits);
    lv_label_set_text(s_ui.quota_credits, buf);

    if (snap->window_count > 1) {
        const codex_window_t *second = &snap->windows[1];
        snprintf(buf, sizeof(buf), "%s %" PRId32 "%%", second->name, second->remaining_percent);
        lv_label_set_text(s_ui.quota_second_value, buf);
        bar_set(s_ui.quota_second_bar_fill, second->remaining_percent,
                quota_color(second->remaining_percent));
        format_duration(second->resets_in_seconds - elapsed, duration, sizeof(duration));
        snprintf(buf, sizeof(buf), "重置 %s", duration);
        lv_label_set_text(s_ui.quota_second_reset, buf);
        lv_obj_clear_flag(s_ui.quota_second, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.quota_second, LV_OBJ_FLAG_HIDDEN);
    }
}

static void radar_set_cells(int32_t filled, uint32_t color)
{
    for (int i = 0; i < RADAR_CELLS; i++) {
        bool on = i < filled;
        lv_obj_set_style_bg_color(s_ui.radar_cells[i],
                                  lv_color_hex(on ? color : UI_COL_INSET), 0);
    }
}

/**
 * 四种雷达状态映射到额度卡那套栅格上
 *
 * 大号数值就是信号尺的数字化：活跃预测用上游给的概率，已确认按定义是满格 100，
 * 上游明确说没有预测是 0，拿不到数据则老老实实 "--"，不用刻度冒充一个不存在的数。
 * 右上小字放时间（预测窗口 / 多久前宣布），和左卡「7天窗口」同位同字号；下面那行
 * 放限定语，和「已用 13%」同位同字号；状态挪到左下，对齐「重置 6天15小时」。
 */
static void refresh_radar(const codex_snapshot_t *snap)
{
    char value[12];
    char when[72];
    char kind[24];
    char state_text[24];
    uint32_t color;
    int32_t filled = 0;

    switch (snap->radar) {
    case CODEX_RADAR_ACTIVE_WATCH:
        color = UI_COL_WARN;
        if (snap->radar_chance_percent >= 0) {
            /* 每格 10%，向上取整：1%-10% 一格，61%-70% 七格 */
            filled = (snap->radar_chance_percent + 9) / 10;
            snprintf(value, sizeof(value), "%" PRId32, snap->radar_chance_percent);
        } else {
            strlcpy(value, "--", sizeof(value));
        }
        /* 上游这个窗口串最长 63 字符，标签自己会截断，这里不裁 */
        strlcpy(when, snap->radar_window[0] ? snap->radar_window : "--", sizeof(when));
        strlcpy(kind, "预测中", sizeof(kind));
        strlcpy(state_text, "活跃预测", sizeof(state_text));
        break;
    case CODEX_RADAR_CONFIRMED: {
        char ago[24];
        format_ago(snap->radar_announced_ago_seconds, ago, sizeof(ago));
        strlcpy(value, "100", sizeof(value));
        strlcpy(when, ago, sizeof(when));
        filled = RADAR_CELLS;
        if (snap->radar_kind == CODEX_RESET_CREDIT) {
            color = UI_COL_INFO;
            /* 「已确认」不等于「已经补满」，这句留着，否则会被读成额度到账了 */
            strlcpy(kind, "可稍后用", sizeof(kind));
            strlcpy(state_text, "重置额度已确认", sizeof(state_text));
        } else {
            color = UI_COL_OK;
            strlcpy(kind, "已宣布", sizeof(kind));
            strlcpy(state_text, "普通重置已确认", sizeof(state_text));
        }
        break;
    }
    case CODEX_RADAR_NO_WATCH:
        color = UI_COL_TEXT_SUB;
        strlcpy(value, "0", sizeof(value));
        strlcpy(when, "--", sizeof(when));
        strlcpy(kind, "上游回报", sizeof(kind));
        strlcpy(state_text, "暂无活跃预测", sizeof(state_text));
        break;
    default:
        color = UI_COL_TEXT_DIM;
        strlcpy(value, "--", sizeof(value));
        strlcpy(when, "--", sizeof(when));
        strlcpy(kind, "无可验证", sizeof(kind));
        strlcpy(state_text, "信号不可用", sizeof(state_text));
        break;
    }

    /* 数值不存在时不能留个孤零零的 % 在那儿 */
    ui_value_set(s_ui.radar_value, value, value[0] == '-' ? "" : "%", color);
    radar_set_cells(filled, color);
    lv_label_set_text(s_ui.radar_when, when);
    /*
     * 限定语任何状态都非空：右上那列是「小字 + 正文」两行，空掉一行，小字就会往下沉，
     * 和左卡的「7天窗口」错开半个字高 —— 正是要避免的那种不对齐。
     */
    lv_label_set_text(s_ui.radar_kind, kind);
    lv_label_set_text(s_ui.radar_state, state_text);
    bool stale = snap->radar_stale && snap->radar != CODEX_RADAR_UNAVAILABLE;
    lv_label_set_text(s_ui.radar_flag, stale ? "数据旧" : "");
    /* 不留空标签在布局里占位，否则状态文字被顶离右边线 */
    if (stale) {
        lv_obj_clear_flag(s_ui.radar_flag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.radar_flag, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_tasks(const codex_snapshot_t *snap, int32_t elapsed)
{
    for (int i = 0; i < CODEX_MAX_TASKS; i++) {
        if (i >= snap->task_count) {
            lv_obj_add_flag(s_ui.task_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const codex_task_t *task = &snap->tasks[i];
        ui_dot_text_set(s_ui.task_states[i], task_state_label(task->state),
                        task_state_color(task->state));
        lv_label_set_text(s_ui.task_titles[i], task->title);

        char ago[24];
        format_ago(task->activity_ago_seconds + elapsed, ago, sizeof(ago));
        lv_label_set_text(s_ui.task_times[i], ago);
        lv_obj_clear_flag(s_ui.task_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (snap->hidden_task_count > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "另有 %" PRId32 " 项", snap->hidden_task_count);
        lv_label_set_text(s_ui.task_hidden, buf);
    } else {
        lv_label_set_text(s_ui.task_hidden, "");
    }

    if (snap->task_count == 0) {
        lv_label_set_text(s_ui.task_empty,
                          snap->task_available ? "24 小时内没有任务动态" : "任务动态暂不可用");
        lv_obj_clear_flag(s_ui.task_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.task_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * 每秒一次的刷新
 *
 * 倒计时和「更新于」必须每秒走动，所以这里不能只在 rev 变化时才做事。取数那一刻
 * 之后过去的秒数由本地单调时钟补偿：面板不依赖 SNTP，bridge 给的全是相对秒数。
 */
static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    codex_snapshot_t snap;
    codex_state_get(&snap);

    int32_t elapsed = 0;
    if (snap.data_valid) {
        elapsed = (int32_t)((esp_timer_get_time() - snap.fetched_at_us) / 1000000);
        if (elapsed < 0) elapsed = 0;
    }

    refresh_quota(&snap, elapsed);

    /* 静态内容只在快照真的变过时重排，避免每秒重设一遍十格信号尺和三行标题 */
    if (snap.rev != s_ui.drawn_rev) {
        s_ui.drawn_rev = snap.rev;
        refresh_radar(&snap);
    }
    refresh_tasks(&snap, elapsed);
}

/* ---------------------------------------------------------------- 入口 */

void ui_dashboard_create(void)
{
    panel_font_init();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *page = ui_page_create(screen);
    build_topbar(page);

    lv_obj_t *mid = ui_box(page, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_size(mid, LV_PCT(100), MID_H);
    lv_obj_set_style_pad_column(mid, UI_GAP, 0);
    build_quota_card(mid);
    build_radar_card(mid);

    build_task_card(page);
    ui_music_create(page);

    if (!panel_font_has_cjk()) {
        /* 字体分区没烧时中文全是方块，得让人知道该去烧字体，而不是以为屏坏了 */
        lv_obj_t *warn = make_label(screen, panel_font_small(), UI_COL_DANGER,
                                    "CJK font missing: flash tools/flash-font.bat");
        lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    lv_timer_create(refresh_timer_cb, REFRESH_PERIOD_MS, NULL);
}
