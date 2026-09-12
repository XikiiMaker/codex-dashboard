#include "ui_music.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "codex_client.h"
#include "codex_state.h"
#include "esp_timer.h"
#include "ui_font.h"
#include "ui_theme.h"

/*
 * 竖屏音乐控制器：迷你卡 + 全屏两套排版
 *
 * 这块屏是 600×1024 竖着的（顶层 CMakeLists 里 CODEX_PORTRAIT=1 无条件开着），
 * 任务卡固定五行之后下半截整块给迷你卡。点封面或歌名切到全屏：封面和三个名字
 * 提到最上面，中间整片给歌词，进度条和控制按钮沉到底。两套排版共用一份数据、
 * 一套刷新函数 —— 差别只是控件指针和尺寸，所以 refresh_* 全部收 music_view_t。
 *
 * 控件的可用性全部来自 bridge 下发的能力位，而不是画出来再按了没反应：实测
 * QQ音乐 的 SMTC 只给 play_pause/next/prev，seek/shuffle/repeat 都是 false，
 * 换个播放器可能又不一样，所以这里一律跟着 caps 走。
 *
 * 符号（播放/暂停/音量）必须用 Montserrat：tiny_ttf 光栅化的 NotoSansSC 里没有
 * LVGL 私有区的符号字形，用中文字体会画成一排方块。
 */

/** 封面按 192×192 原样显示，和 media_server.py 的 COVER_SIZE 一致，不缩放 */
#define COVER_SHOW 192
/** 迷你卡右侧歌词行数；加上歌名和歌手-专辑两行，五行合起来对齐封面高度 */
#define MINI_LYRIC_LINES 3
/**
 * 全屏歌词行数
 *
 * 11 而不是「能塞多少塞多少」：歌词窗的内容区是 554（见 build_fullscreen 的分账），
 * NotoSansSC 在 28px 下每行占 40（tiny_ttf 的 line_height = size × 1.448，取自字体的
 * ascent/descent），11 行 440，SPACE_EVENLY 还剩 9~10px 行距。字号还在 20px 的时候
 * 这里是 13 行，但 28px 再摆 13 行就只剩 2px 行距，整页会糊成一块。
 *
 * 行数只是取景窗口大小，refresh_lyrics 拿它把当前句夹在中间，改大改小都不影响定位。
 * 11 行 × sizeof(codex_lyric_line_t) 132 = 1.45KB 落在 LVGL 任务栈上，那个栈在
 * main.c 里给的是 16KB，加上 codex_media_t 那 400 字节仍然宽裕。
 */
#define FULL_LYRIC_LINES 11
/** 两个视图的歌词数组都按大的那个开，迷你卡只用前 MINI_LYRIC_LINES 个 */
#define LYRIC_MAX FULL_LYRIC_LINES

#define MINI_ROW_GAP    14
#define MINI_PROGRESS_H 30
#define MINI_CONTROL_H  64

#define FULL_PAD        16
#define FULL_ROW_GAP    16
#define FULL_PROGRESS_H 34
#define FULL_CONTROL_H  72

/** 音量触摸条弹出后的自动关闭时长；拖动会重新计时 */
#define VOL_POP_MS 5000

/**
 * 本地认定的音量最多顶这么久
 *
 * 拖动即发之后，2 秒一轮的状态回来的还是旧音量，得让本地值先顶着。但万一命令
 * 根本没送出去（bridge 断了、播放器不接受），一直顶着就是一个永远不更正的假数字，
 * 所以给个保质期，到点交还给轮询结果。
 */
#define VOL_LOCAL_HOLD_MS 3000

#define REFRESH_PERIOD_MS 500

typedef struct {
    /** 显隐切换的单位：迷你视图是卡片本身，全屏视图是盖住整屏的覆盖层 */
    lv_obj_t *root;

    lv_obj_t *status;
    lv_obj_t *source;

    lv_obj_t *cover;
    lv_obj_t *cover_ph;

    lv_obj_t *title;
    /**
     * 只有迷你视图有：歌手和专辑挤在第二行，中间靠它隔开。全屏是各占一行。
     * 用 NULL 判断而不是 if (fullscreen)：读代码的人不必回去翻搭建那段。
     */
    lv_obj_t *artist;
    lv_obj_t *sep_album;
    lv_obj_t *album;

    lv_obj_t *elapsed;
    lv_obj_t *total;
    lv_obj_t *progress;

    lv_obj_t *btn_shuffle;
    lv_obj_t *btn_mode;
    lv_obj_t *mode_text;
    lv_obj_t *btn_prev;
    lv_obj_t *btn_play;
    lv_obj_t *play_icon;
    lv_obj_t *btn_next;

    /**
     * 音量只常驻一个小芯片（喇叭图标 + 百分比），点它才弹出触摸条。弹出层挂在
     * root 上而不是控制行里 —— 控制行只有几十像素高，装不下一根能拖的滑杆。
     */
    lv_obj_t *mute_icon;
    lv_obj_t *volume_pct;
    lv_obj_t *vol_chip;
    lv_obj_t *vol_pop;
    lv_obj_t *vol_slider;
    bool vol_pop_open;
    /** lv_tick 毫秒截止时间，拖一下重新计；到点由刷新回合收起来 */
    uint32_t vol_pop_deadline;
    /** 拖动即发时本地认定的音量，-1 表示没有（静态零值会被读成「音量 0%」，必须初始化） */
    int32_t vol_local;
    /** vol_local 的保质期起点，见 VOL_LOCAL_HOLD_MS */
    uint32_t vol_local_tick;
    /** 上一次发出去的音量，挡掉值没变的重复 VALUE_CHANGED */
    int32_t vol_sent;

    /** 无边框之后随机开关只能靠染符号颜色表达，得留着符号 label 的指针 */
    lv_obj_t *shuffle_icon;

    lv_obj_t *lyric_box;
    lv_obj_t *lyrics[LYRIC_MAX];
    int32_t lyric_lines;
    lv_obj_t *hint;

    lv_image_dsc_t cover_dsc;

    uint32_t drawn_media_rev;
    uint32_t drawn_cover_rev;
    int32_t drawn_lyric_index;
} music_view_t;

static music_view_t s_mini;
static music_view_t s_full;
static bool s_fullscreen;

/* ---------------------------------------------------------------- 小工具 */

/** "3:07" / "61:05"，负数或超长按 "--:--" */
static void format_ms(int32_t ms, char *out, size_t cap)
{
    if (ms < 0) {
        strlcpy(out, "--:--", cap);
        return;
    }
    int32_t total_seconds = ms / 1000;
    snprintf(out, cap, "%" PRId32 ":%02d", total_seconds / 60, (int)(total_seconds % 60));
}

/** 文字没变就不重设：每半秒重设一遍会让 LVGL 白算一次布局 */
static void set_label(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 按一个音量值画芯片：百分比跟着走，图标按静音/大小换档 */
static void volume_chip_set(music_view_t *v, int32_t percent, bool muted)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%" PRId32 "%%", percent);
    set_label(v->volume_pct, buf);
    const char *want = muted ? LV_SYMBOL_MUTE
                             : (percent >= 50 ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_VOLUME_MID);
    set_label(v->mute_icon, want);
}

/** 透明无边框的横向行，宽 100%、cross 居中；要 SIZE_CONTENT 宽的调用方自己改 */
static lv_obj_t *make_row(lv_obj_t *parent, lv_coord_t height, lv_coord_t col_gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, col_gap, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_spacer(lv_obj_t *parent)
{
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return spacer;
}

/**
 * 滑杆配色
 *
 * 不用默认主题那套：默认轨道是浅灰，在深色卡上比填充还亮，一眼看反。
 * 触摸区用 ext_click_area 往外扩，轨道只有 8px 高，靠手指是点不准的。
 */
static void style_slider(lv_obj_t *slider, uint32_t indicator)
{
    lv_obj_set_height(slider, 8);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_INSET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(indicator), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 14);
}

/**
 * 无边框按钮底子：透明底、无框、按下时透一点淡底做反馈
 *
 * 播放键两侧原来是一排 GHOST 描边方框，五个框并排太「遥控器」；去掉框只留图标，
 * 间距才读得出来。按下反馈不能一起去掉，否则按了像没按。
 */
static lv_obj_t *plain_button(lv_obj_t *parent, lv_coord_t h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_BORDER_HI), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    return btn;
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb,
                             const void *action, lv_coord_t side)
{
    lv_obj_t *btn = plain_button(parent, side);
    lv_obj_set_width(btn, side);
    /*
     * 动作名必须走 add_event_cb 的第四参：on_action 里 lv_event_get_user_data()
     * 取的是**事件回调**那一层的 user_data，存进对象的 lv_obj_set_user_data() 是
     * 另一层，读不到 —— 早先就是这么写的，按钮按下去只有动画、命令一条都没发。
     */
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COL_TEXT_SUB), 0);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);
    return btn;
}

/* ---------------------------------------------------------------- 事件 */

static void music_set_fullscreen(bool on);

/**
 * 播放/切歌/静音共用一个回调，动作名放在 user_data 里
 *
 * 只投递不等待：HTTP 在媒体任务里发，这里要是阻塞，屏幕就会卡住整个超时时间。
 */
static void on_action(lv_event_t *e)
{
    const char *action = lv_event_get_user_data(e);
    if (action) {
        codex_client_media_cmd(action, -1);
    }
}

/**
 * 点喇叭芯片：开关音量触摸条
 *
 * 触摸条不常驻：控制行里最常按的是播放键，一根永远横在那儿的滑杆既抢位置又
 * 容易被误碰。弹出后 5 秒自己收，拖一下重新计时，不用专门去点关闭。
 */
static void on_vol_chip(lv_event_t *e)
{
    music_view_t *v = lv_event_get_user_data(e);
    v->vol_pop_open = !v->vol_pop_open;
    if (v->vol_pop_open) {
        v->vol_pop_deadline = lv_tick_get() + VOL_POP_MS;
        lv_obj_clear_flag(v->vol_pop, LV_OBJ_FLAG_HIDDEN);
        /*
         * 每开一次重新对齐：位置取决于芯片的最终坐标和弹出层装了滑杆之后的最终
         * 宽度，两者都是布局跑完才定的，只在打开这一刻算才一定准。
         */
        lv_obj_align_to(v->vol_pop, v->vol_chip, LV_ALIGN_OUT_TOP_RIGHT, 0, -8);
    } else {
        lv_obj_add_flag(v->vol_pop, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * 拖动即发，不等松手
 *
 * 原来只在 RELEASED 发命令，手指拖到底得等松手才开始变，加上 2 秒一轮的状态回显，
 * 感觉就是「延迟很高」。带值的命令在 codex_client 里会覆盖掉还在排队的同名那条
 * （拖音量条不该排出一串请求），所以连发的实际速率由 HTTP 往返自己限住，这里只
 * 挡掉值没变的重复事件。
 */
static void on_vol_slider(lv_event_t *e)
{
    music_view_t *v = lv_event_get_user_data(e);
    v->vol_pop_deadline = lv_tick_get() + VOL_POP_MS;

    int32_t value = lv_slider_get_value(v->vol_slider);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED && value == v->vol_sent) {
        return;
    }
    v->vol_sent = value;
    v->vol_local = value;
    v->vol_local_tick = lv_tick_get();
    codex_client_media_cmd("set_volume", value);

    /* 芯片上的百分比立刻跟着手指走；静音状态只有轮询知道，先沿用当前的 */
    codex_media_t m;
    codex_media_get(&m);
    volume_chip_set(v, value, m.muted);
}

static void on_seek_released(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t percent = lv_slider_get_value(slider);

    codex_media_t m;
    codex_media_get(&m);
    if (m.duration_ms <= 0) {
        return;
    }
    codex_client_media_cmd("seek", (int32_t)((int64_t)m.duration_ms * percent / 100));
}

/** 封面和歌名都是入口，全屏里的封面则是出口 —— 同一个回调来回切 */
static void on_toggle_fullscreen(lv_event_t *e)
{
    (void)e;
    music_set_fullscreen(!s_fullscreen);
}

/* ---------------------------------------------------------------- 搭建 */

static void build_cover(music_view_t *v, lv_obj_t *parent, lv_coord_t side)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, side, side);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COL_INSET), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    /* 图是方的、盒子是圆角的，不裁的话四个角会露出直角像素盖住圆角 */
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, on_toggle_fullscreen, LV_EVENT_CLICKED, NULL);

    v->cover = lv_image_create(box);
    lv_obj_set_size(v->cover, side, side);
    lv_obj_center(v->cover);
    lv_obj_clear_flag(v->cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->cover, LV_OBJ_FLAG_HIDDEN);

    /* 封面和占位符叠在同一个格子里，按有没有图切换显隐 */
    v->cover_ph = lv_label_create(box);
    lv_obj_set_style_text_font(v->cover_ph, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(v->cover_ph, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_label_set_text(v->cover_ph, LV_SYMBOL_AUDIO);
    lv_obj_center(v->cover_ph);
    lv_obj_clear_flag(v->cover_ph, LV_OBJ_FLAG_CLICKABLE);
}

static void build_progress(music_view_t *v, lv_obj_t *parent, lv_coord_t height, lv_coord_t gap)
{
    lv_obj_t *row = make_row(parent, height, gap);

    /*
     * 两个时间标签定宽：秒数每跳一下字宽就变一次，不锁宽度的话中间的滑杆会被
     * 推着左右晃，看着像进度在倒退。
     */
    v->elapsed = make_label(row, panel_font_small(), UI_COL_TEXT_DIM, "--:--");
    lv_obj_set_width(v->elapsed, 44);

    v->progress = lv_slider_create(row);
    lv_obj_set_flex_grow(v->progress, 1);
    lv_slider_set_range(v->progress, 0, 1000);
    style_slider(v->progress, UI_COL_ACCENT);
    lv_obj_add_event_cb(v->progress, on_seek_released, LV_EVENT_RELEASED, NULL);

    v->total = make_label(row, panel_font_small(), UI_COL_TEXT_DIM, "--:--");
    lv_obj_set_width(v->total, 44);
    lv_obj_set_style_text_align(v->total, LV_TEXT_ALIGN_RIGHT, 0);
}

/**
 * 播放键左右那两个等宽盒子
 *
 * grow=1 且都不设 min_width：flex 把「行宽 − 播放键 − 两个间距」对半分给它们
 * （lv_flex.c 里 grow 项的尺寸 = 余量 × 权重 / 权重和，不叠加内容宽，下限只取
 * 样式里的 min_width），所以两盒严格等宽，夹在中间的定宽圆正好压在行的中线上。
 */
static lv_obj_t *make_side_box(lv_obj_t *parent, lv_coord_t height, lv_coord_t gap,
                               lv_flex_align_t main_place)
{
    lv_obj_t *box = make_row(parent, height, gap);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_align(box, main_place, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return box;
}

/**
 * 传输键：两个等宽盒子夹住中间一个正圆描边的播放键
 *
 * 播放键是这一行唯一带框的控件 —— 2px 蓝环正圆，一眼就知道该按哪儿；其余只留
 * 图标，间距统一由 gap 给。垂直居中不用额外做：make_row 的 cross 就是 CENTER，
 * 圆的直径等于行高时自然上下贴齐。
 *
 * 水平居中不能靠 spacer 推：左边三个键、右边一个键加音量芯片，两侧内容宽不同，
 * spacer 只会把圆顶到某个由内容宽决定的位置，看着就是「偏左」。得让两侧各自
 * 变成一个等宽的弹性盒子（见 make_side_box）。
 *
 * 返回右盒，音量芯片由调用方挂进去 —— 它要贴在整行最右，所以右盒里 next 之后
 * 还得放一个 spacer 把它推过去。
 */
static lv_obj_t *build_transport(music_view_t *v, lv_obj_t *parent, lv_coord_t icon_side,
                                 lv_coord_t play_side, lv_coord_t gap)
{
    lv_obj_t *left = make_side_box(parent, play_side, gap, LV_FLEX_ALIGN_END);

    v->btn_shuffle = icon_button(left, LV_SYMBOL_SHUFFLE, on_action, "shuffle", icon_side);
    v->shuffle_icon = lv_obj_get_child(v->btn_shuffle, 0);

    /* 循环模式是文字而不是符号：列表/单曲/关三种状态用符号区分不出来 */
    v->btn_mode = plain_button(left, icon_side);
    lv_obj_set_width(v->btn_mode, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(v->btn_mode, 8, 0);
    lv_obj_add_event_cb(v->btn_mode, on_action, LV_EVENT_CLICKED, (void *)"repeat");
    v->mode_text = lv_label_create(v->btn_mode);
    lv_obj_set_style_text_font(v->mode_text, panel_font_small(), 0);
    lv_obj_set_style_text_color(v->mode_text, lv_color_hex(UI_COL_TEXT_SUB), 0);
    lv_label_set_text(v->mode_text, "--");
    lv_obj_center(v->mode_text);

    v->btn_prev = icon_button(left, LV_SYMBOL_PREV, on_action, "prev", icon_side);

    v->btn_play = lv_button_create(parent);
    lv_obj_set_size(v->btn_play, play_side, play_side);
    lv_obj_set_style_radius(v->btn_play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(v->btn_play, 0, 0);
    lv_obj_set_style_pad_all(v->btn_play, 0, 0);
    lv_obj_set_style_bg_color(v->btn_play, lv_color_hex(UI_COL_BORDER_HI), 0);
    lv_obj_set_style_bg_opa(v->btn_play, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(v->btn_play, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(v->btn_play, 2, 0);
    lv_obj_set_style_border_color(v->btn_play, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_add_event_cb(v->btn_play, on_action, LV_EVENT_CLICKED, (void *)"play_pause");
    v->play_icon = lv_label_create(v->btn_play);
    lv_obj_set_style_text_font(v->play_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(v->play_icon, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(v->play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(v->play_icon);

    lv_obj_t *right = make_side_box(parent, play_side, gap, LV_FLEX_ALIGN_START);
    v->btn_next = icon_button(right, LV_SYMBOL_NEXT, on_action, "next", icon_side);
    return right;
}

/**
 * 音量：常驻只有「喇叭 + 百分比」一个小芯片，点它弹出触摸条
 *
 * 弹出层挂在 v->root 上、对齐到芯片正上方：控制行只有芯片那么高，滑杆放在行里
 * 会被行裁掉；挂 root 才能浮在进度条和歌词上面。默认隐藏，显隐和 5 秒自动关闭
 * 见 on_vol_chip / refresh_volume。
 */
static void build_volume(music_view_t *v, lv_obj_t *parent, lv_coord_t chip_h,
                         lv_coord_t slider_w)
{
    lv_obj_t *chip = make_row(parent, chip_h, 8);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(chip, 6);
    lv_obj_add_event_cb(chip, on_vol_chip, LV_EVENT_CLICKED, v);
    v->vol_chip = chip;

    /*
     * 喇叭和 icon_button 用同一个 montserrat_24，芯片里的图标才和左右那些键（上一首、
     * 下一首）一样高；百分比配 20px 的 body —— 数字的字面高约等于 24px 符号的墨迹高，
     * 再大就会盖过歌名那一级文字。芯片高度由 chip_h 定死，字号变大不撑行。
     */
    v->mute_icon = make_label(chip, &lv_font_montserrat_24, UI_COL_TEXT_SUB, LV_SYMBOL_VOLUME_MAX);
    v->volume_pct = make_label(chip, panel_font_body(), UI_COL_TEXT_SUB, "--");

    lv_obj_t *pop = lv_obj_create(v->root);
    lv_obj_set_size(pop, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_bg_color(pop, lv_color_hex(UI_COL_INSET), 0);
    lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pop, lv_color_hex(UI_COL_BORDER_HI), 0);
    lv_obj_set_style_border_width(pop, 1, 0);
    lv_obj_set_style_radius(pop, 12, 0);
    lv_obj_set_style_pad_hor(pop, 14, 0);
    lv_obj_clear_flag(pop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
    /*
     * 必须 IGNORE_LAYOUT：pop 挂在 root 这个 flex 列容器上，位置由 align_to 手工给。
     * flex 会排所有非隐藏的普通子项，而清掉 HIDDEN 那一刻父容器布局就被标脏
     * （lv_obj.c 的 lv_obj_remove_flag），弹出层会被当成新的一行塞进卡片列里 ——
     * 既抢走歌词窗的高度，又把手工坐标冲掉。
     */
    lv_obj_add_flag(pop, LV_OBJ_FLAG_IGNORE_LAYOUT);
    v->vol_pop = pop;

    v->vol_slider = lv_slider_create(pop);
    lv_obj_set_width(v->vol_slider, slider_w);
    lv_slider_set_range(v->vol_slider, 0, 100);
    style_slider(v->vol_slider, UI_COL_ACCENT);
    lv_obj_center(v->vol_slider);
    lv_obj_add_event_cb(v->vol_slider, on_vol_slider, LV_EVENT_VALUE_CHANGED, v);
    lv_obj_add_event_cb(v->vol_slider, on_vol_slider, LV_EVENT_RELEASED, v);
    /* 位置不在这里定：见 on_vol_chip，每次打开时按最终几何重新对齐 */
}

/**
 * 歌词窗
 *
 * boxed：迷你卡那三行贴着一列左对齐的歌名/歌手，需要一块 INSET 灰底（卡片是
 * 0x0B0B0B 的近黑）才能和封面、文字分出层次。全屏那一屏整块都是歌词，灰框反而
 * 像贴了张卡片在页面上，所以不要框、直接浮在纯底色上。
 *
 * centered：同理，全屏 13 行居中排下来才像一页歌词；迷你卡跟着上面的歌名左对齐。
 *
 * 行高不写死：每行 SIZE_CONTENT，主轴 SPACE_EVENLY 把余量均分。字体度量变了
 * （比如中文字体分区没烧、整体退回 Montserrat）也只是行距变化，不会溢出或挤成一坨。
 */
static void build_lyrics(music_view_t *v, lv_obj_t *parent, int32_t lines,
                         const lv_font_t *font, lv_coord_t pad,
                         bool boxed, bool centered)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COL_INSET), 0);
    lv_obj_set_style_bg_opa(box, boxed ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, pad, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    v->lyric_box = box;
    v->lyric_lines = lines;

    const lv_text_align_t align = centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
    for (int32_t i = 0; i < lines; i++) {
        v->lyrics[i] = make_label(box, font, UI_COL_TEXT_DIM, "");
        lv_label_set_long_mode(v->lyrics[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(v->lyrics[i], LV_PCT(100));
        lv_obj_set_style_text_align(v->lyrics[i], align, 0);
    }

    v->hint = make_label(box, font, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(v->hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v->hint, LV_PCT(100));
    lv_obj_set_style_text_align(v->hint, align, 0);
    lv_obj_add_flag(v->hint, LV_OBJ_FLAG_HIDDEN);
}

static void build_head(music_view_t *v, lv_obj_t *parent, lv_coord_t height, lv_coord_t gap,
                       lv_coord_t source_max_w, bool fullscreen)
{
    lv_obj_t *head = make_row(parent, height, gap);

    /*
     * 全屏的返回是左上角一颗裸 ←，不是原来右上角那个「返回看板」描边框：这一屏没有
     * 别的导航，退出是最常按的操作，该放在视线起点；图标比四个字省一半宽度，右上角
     * 才腾得出来给播放器标识。on_toggle_fullscreen 不看 user_data，传 NULL 即可。
     * 44 和原来那个框同高，头部的高度分账不变。
     */
    if (fullscreen) {
        icon_button(head, LV_SYMBOL_LEFT, on_toggle_fullscreen, NULL, 44);
    }

    v->status = ui_dot_text(head, panel_font_small(), true);
    ui_dot_text_set(v->status, "媒体未连接", UI_COL_TEXT_DIM);

    make_spacer(head);

    v->source = make_label(head, panel_font_small(), UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(v->source, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v->source, source_max_w, 0);
    lv_obj_set_style_text_align(v->source, LV_TEXT_ALIGN_RIGHT, 0);

    /*
     * 全屏右上角这颗是「在用什么播放器放」的标识，迷你卡那份只是行尾的一句脚注，
     * 所以这里放大一号、提亮一档。真正的厂牌图形得有图片资源才画得出来（SMTC 只给
     * 得到进程名），先用文字顶着，见 source_display_name。
     */
    if (fullscreen) {
        lv_obj_set_style_text_font(v->source, panel_font_body(), 0);
        lv_obj_set_style_text_color(v->source, lv_color_hex(UI_COL_TEXT_SUB), 0);
    }
}

/**
 * 迷你卡：封面 | 歌名 / 歌手-专辑 / 三行歌词
 *
 * 行高给 grow 而不是写死 COVER_SHOW：卡片是页面上吸收剩余高度的那一个，任务卡
 * 行数一变这里就多几十像素。封面和右侧文字块都是定高 192 并垂直居中，多出来的
 * 高度变成上下对称的留白，「文字块对齐封面」这件事不会被撑坏。
 */
static void build_song_row(music_view_t *v, lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), COVER_SHOW);
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_cover(v, row, COVER_SHOW);

    lv_obj_t *mid = lv_obj_create(row);
    lv_obj_set_size(mid, LV_PCT(100), COVER_SHOW);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_style_pad_row(mid, 8, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /*
     * 两行：歌名独占一行，歌手-专辑第二行。早先三个名字挤一行，专辑名稍长就
     * 折成两行把整块顶乱；拆开后每行宽度都够，LONG_DOT 只在真的超长时截断。
     * 宽度必须显式给（PCT / grow）：SIZE_CONTENT 的 label 配 LONG_DOT 拿不到
     * 截断宽度，实测会退化成换行。
     */
    lv_obj_t *names = lv_obj_create(mid);
    lv_obj_set_size(names, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(names, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(names, 0, 0);
    lv_obj_set_style_pad_all(names, 0, 0);
    lv_obj_set_style_pad_row(names, 2, 0);
    lv_obj_clear_flag(names, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(names, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    v->title = make_label(names, panel_font_body(), UI_COL_TEXT, "等待媒体数据");
    lv_label_set_long_mode(v->title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->title, LV_PCT(100));
    lv_obj_add_flag(v->title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(v->title, 6);
    lv_obj_add_event_cb(v->title, on_toggle_fullscreen, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sub = make_row(names, LV_SIZE_CONTENT, 4);
    v->artist = make_label(sub, panel_font_small(), UI_COL_TEXT_SUB, "");
    lv_label_set_long_mode(v->artist, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(v->artist, 1);
    lv_obj_set_style_min_width(v->artist, 40, 0);

    v->sep_album = make_label(sub, panel_font_small(), UI_COL_TEXT_DIM, "-");
    v->album = make_label(sub, panel_font_small(), UI_COL_TEXT_SUB, "");
    lv_label_set_long_mode(v->album, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(v->album, 2);
    lv_obj_set_style_min_width(v->album, 40, 0);

    /* 第一轮刷新在 500ms 之后，在那之前名字都是空串，第二行先整行藏着免得挂一个孤零零的 "-" */
    lv_obj_add_flag(v->artist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(v->sep_album, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(v->album, LV_OBJ_FLAG_HIDDEN);

    build_lyrics(v, mid, MINI_LYRIC_LINES, panel_font_small(), 10, true, false);
}

static void build_mini(lv_obj_t *page)
{
    music_view_t *v = &s_mini;

    /*
     * 这张卡是页面上唯一 grow 的，高度由看板剩下的量决定（任务卡五行时是 419，
     * 减边框和 pad 12×2 后内容区 393）。逐段扣掉：
     *   头部 34（状态药丸 = 16px 字号一行 22 + pad_ver 6×2）
     *   + 分隔线 1 + 进度行 30 + 控制行 64 + 四段间距 14×4 = 56
     * 剩 208 给封面行 —— 比封面本身高 16，多出来的变成上下各 8px 的留白，
     * 「文字块对齐封面」这件事由 build_song_row 里的定高 192 兜住，不受影响。
     */
    lv_obj_t *card = ui_card(page, 1);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, MINI_ROW_GAP, 0);
    v->root = card;

    build_head(v, card, LV_SIZE_CONTENT, 8, 150, false);
    ui_divider(card);
    build_song_row(v, card);
    build_progress(v, card, MINI_PROGRESS_H, 10);

    /*
     * 播放键居中靠左右两个等宽弹性盒子，不靠 spacer 推（见 build_transport）。
     * 内容区 542 的核算：圆 64 + 两个盒间距 40 = 104，余 438 对半分，每盒 219。
     * 那个 20 的 pad_column 就是「切歌键离播放圆键多远」—— 它只作用在 左盒↔圆 和
     * 圆↔右盒 两处，两边同步变大，所以居中不受影响。
     * 左盒最坏装 168（图标 44×2 + 模式约 48 + 两个间距 32）—— 随机键与模式键在
     * 播放器不支持时会整块隐藏，实际只会更窄；右盒装 160（图标 44 + 芯片约 84 +
     * 两个间距 32），都不会被挤到换行。
     */
    lv_obj_t *control = make_row(card, MINI_CONTROL_H, 20);
    lv_obj_t *right = build_transport(v, control, 44, MINI_CONTROL_H, 16);
    make_spacer(right);
    build_volume(v, right, 44, 200);
}

static void build_fullscreen(lv_obj_t *screen)
{
    music_view_t *v = &s_full;

    /*
     * 覆盖层而不是另一个 screen：lv_screen_load 会把看板那套定时器留着但整屏重画，
     * 切回来还要重建一遍。做成 screen 的子节点，HIDDEN 一开一关就行，看板底下
     * 那份状态一个字都不用动。默认 lv_obj 就是 CLICKABLE，所以盖上去之后触摸不会
     * 漏到下层的看板上。
     */
    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, FULL_PAD, 0);
    lv_obj_set_style_pad_row(root, FULL_ROW_GAP, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    v->root = root;

    build_head(v, root, LV_SIZE_CONTENT, 12, 170, true);

    lv_obj_t *top = make_row(root, COVER_SHOW, 20);
    build_cover(v, top, COVER_SHOW);

    lv_obj_t *names = lv_obj_create(top);
    lv_obj_set_size(names, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(names, 1);
    lv_obj_set_style_bg_opa(names, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(names, 0, 0);
    lv_obj_set_style_pad_all(names, 0, 0);
    lv_obj_set_style_pad_row(names, 12, 0);
    lv_obj_clear_flag(names, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(names, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /*
     * 全屏各占一行，字号比迷你卡逐级放大：歌名 40、歌手 28、专辑 20。
     * 高度核算：三行的行高 57 / 40 / 28（tiny_ttf 的 line_height = size × 1.448）
     * 加两段 pad_row 12 = 149，仍在封面行的 192 之内，names 靠 cross CENTER 上下留白。
     * 宽度要留意：names 是 grow 出来的 356（568 − 封面 192 − 间距 20），40px 的中文
     * 歌名大约 8~9 个字就到头，再长会被 LONG_DOT 截成省略号 —— 点歌名能进/出全屏，
     * 但看不到全称，这是放大字号换来的代价。
     */
    v->title = make_label(names, panel_font_display(), UI_COL_TEXT, "等待媒体数据");
    lv_label_set_long_mode(v->title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->title, LV_PCT(100));
    lv_obj_add_flag(v->title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v->title, on_toggle_fullscreen, LV_EVENT_CLICKED, NULL);

    v->artist = make_label(names, panel_font_title(), UI_COL_TEXT_SUB, "");
    lv_label_set_long_mode(v->artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->artist, LV_PCT(100));

    v->album = make_label(names, panel_font_body(), UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(v->album, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->album, LV_PCT(100));

    /*
     * 歌词窗是这一屏唯一 grow 的元素，宽高都由它兜底：
     *   宽 568 = 600 - pad 16×2
     *   高 992 = 1024 - pad 16×2，减掉头部 44、封面行 192、进度 34、控制 72
     *      和四段间距 64，剩下 586 全给歌词窗；再减自己的 pad 16×2，内容区 554
     *      —— FULL_LYRIC_LINES 就是按这 554 定的
     * 控制行核算：圆 72 + 两个盒间距 44 = 116，余 452 对半分每盒 226（那个 22 的
     * pad_column 就是切歌键离播放圆键的距离，两边同步，居中不受影响）；
     * 左盒最坏装 180（图标 48×2 + 模式约 48 + 间距 36，随机与模式键不支持时整块
     * 隐藏，实际更窄），右盒装 168（图标 48 + 芯片约 84 + 间距 36 + spacer），
     * 都放得下。
     */
    build_lyrics(v, root, FULL_LYRIC_LINES, panel_font_title(), 16, false, true);
    build_progress(v, root, FULL_PROGRESS_H, 12);

    lv_obj_t *control = make_row(root, FULL_CONTROL_H, 22);
    lv_obj_t *right = build_transport(v, control, 48, FULL_CONTROL_H, 18);
    make_spacer(right);
    build_volume(v, right, 48, 300);
}

/* ---------------------------------------------------------------- 刷新 */

/**
 * 播放器进程名 → 展示名
 *
 * SMTC 只给得到 exe 名，"QQMusic.exe" 摆在全屏右上角那个标识位上不像个名字。已知的
 * 换成常用叫法，没见过的原样显示 —— 宁可露出真实进程名，也不要把它标成别家。
 * 真正的厂牌图形得有图片资源才画得出来，那不在 SMTC 的能力范围内。
 */
static const char *source_display_name(const char *source)
{
    if (strcmp(source, "QQMusic.exe") == 0) {
        return "QQ音乐";
    }
    return source;
}

static const char *repeat_mode_text(const char *mode)
{
    if (strcmp(mode, "track") == 0) return "单曲";
    if (strcmp(mode, "list") == 0) return "列表";
    if (strcmp(mode, "none") == 0) return "关";
    return "--";
}

static void refresh_names(music_view_t *v, const codex_media_t *m)
{
    const char *title;
    const char *artist;
    const char *album;
    if (m->available) {
        title = m->title[0] ? m->title : "未知曲目";
        artist = m->artist;
        album = m->album;
    } else {
        title = m->bridge_ok ? "没有在播放的媒体" : "媒体服务没有响应";
        artist = m->bridge_ok ? "" : m->error;
        album = "";
    }

    set_label(v->title, title);
    set_label(v->artist, artist);
    set_label(v->album, album);
    /* 名字空了要连它前面那个连字符一起藏，否则行首/行尾挂着一个孤零零的 "-" */
    if (v->sep_album != NULL) {
        set_hidden(v->artist, artist[0] == '\0');
        set_hidden(v->sep_album, artist[0] == '\0' || album[0] == '\0');
        set_hidden(v->album, album[0] == '\0');
    }
}

/** 歌名、能力位这些只在快照真的换过时重设，避免每半秒重排一遍 */
static void refresh_static(music_view_t *v, const codex_media_t *m)
{
    if (!m->bridge_ok) {
        ui_dot_text_set(v->status, "媒体服务未连接", UI_COL_DANGER);
    } else if (!m->available) {
        ui_dot_text_set(v->status, "没有在播放", UI_COL_TEXT_DIM);
    } else if (m->state == CODEX_MEDIA_PLAYING) {
        ui_dot_text_set(v->status, "播放中", UI_COL_OK);
    } else if (m->state == CODEX_MEDIA_PAUSED) {
        ui_dot_text_set(v->status, "已暂停", UI_COL_WARN);
    } else {
        ui_dot_text_set(v->status, "已停止", UI_COL_TEXT_DIM);
    }

    set_label(v->source,
              m->available ? (m->source[0] ? source_display_name(m->source) : "未知播放器") : "");
    refresh_names(v, m);

    ui_button_set_enabled(v->btn_play, m->available && m->can_play_pause);
    ui_button_set_enabled(v->btn_prev, m->available && m->can_prev);
    ui_button_set_enabled(v->btn_next, m->available && m->can_next);

    /*
     * 随机键和循环模式键：播放器没暴露这个能力就整个藏掉，不留一个压暗的死键。
     * QQ音乐 的 SMTC 里 is_shuffle_enabled / is_repeat_enabled 都是 false，命令到了
     * media_server 会被能力位直接挡回来（"当前播放器不支持此操作"），按下去永远没
     * 反应；模式键的标签在 repeat 为 unknown 时又只显示 "--"，用户认不出这是什么。
     * 禁用态说明的是「现在不行」，而这里是「这个播放器压根不行」，藏起来才对。
     *
     * 藏掉不影响播放键居中：左右两盒 flex_grow=1 且都不设 min_width，宽度只由
     * 「行宽 − 播放键 − 间距」对半分决定，与里面装了几个键无关，且 flex 会完全跳过
     * hidden 子项（连列间距都不占）。
     */
    set_hidden(v->btn_shuffle, !(m->available && m->can_shuffle));
    set_hidden(v->btn_mode, !(m->available && m->can_repeat));

    /*
     * 播放器不支持拖动进度时把滑杆的点击关掉，而不是禁用：禁用态会被主题压暗，
     * 进度条是这块卡上最主要的「正在放什么」指示，不能看起来像坏了一半。
     */
    if (m->available && m->can_seek) {
        lv_obj_add_flag(v->progress, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(v->progress, LV_OPA_COVER, LV_PART_KNOB);
    } else {
        lv_obj_clear_flag(v->progress, LV_OBJ_FLAG_CLICKABLE);
        /*
         * 拖柄一起藏掉：没有那颗圆点，这条读起来就是个纯进度指示器，不会再让人伸手
         * 去拖它、拖了没反应又以为坏了。QQ音乐 这一位在现场永远是 false ——
         * is_playback_position_enabled=false，timeline 的 max_seek_time/min_seek_time
         * 都是 0，它自绘的窗口用 UI Automation 扫也找不到任何 Slider/ProgressBar。
         * 换个真支持 seek 的播放器，拖柄会自己回来。
         */
        lv_obj_set_style_bg_opa(v->progress, LV_OPA_TRANSP, LV_PART_KNOB);
    }
    if (m->volume_available) {
        lv_obj_add_flag(v->vol_chip, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(v->vol_chip, LV_OBJ_FLAG_CLICKABLE);
        v->vol_pop_open = false;
        lv_obj_add_flag(v->vol_pop, LV_OBJ_FLAG_HIDDEN);
    }

    set_label(v->mode_text, repeat_mode_text(m->repeat_mode));

    /* 随机播放开着时把符号染成主色：按钮无边框无底色，只有符号颜色能表达开关 */
    bool shuffle_on = m->available && m->shuffle_active;
    lv_obj_set_style_text_color(v->shuffle_icon,
                                lv_color_hex(shuffle_on ? UI_COL_ACCENT : UI_COL_TEXT_SUB), 0);
}

static void refresh_cover(music_view_t *v, const codex_media_t *m)
{
    const uint8_t *data = NULL;
    uint32_t rev = 0;
    bool have = m->available && m->has_cover && codex_media_cover(&data, &rev);

    if (!have) {
        lv_obj_add_flag(v->cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(v->cover_ph, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(v->cover_ph, LV_OBJ_FLAG_HIDDEN);

    /*
     * 像素在 PSRAM 里常驻、指针永远有效，所以只有 rev 变了才需要重新挂 src；
     * 每半秒重挂一次会让 LVGL 每轮都重画这一大块（还带一次圆角裁剪）。
     */
    if (rev != v->drawn_cover_rev) {
        v->drawn_cover_rev = rev;
        memset(&v->cover_dsc, 0, sizeof(v->cover_dsc));
        v->cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        v->cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        v->cover_dsc.header.w = CODEX_COVER_SIZE;
        v->cover_dsc.header.h = CODEX_COVER_SIZE;
        v->cover_dsc.header.stride = CODEX_COVER_SIZE * 2;
        v->cover_dsc.data_size = CODEX_COVER_BYTES;
        v->cover_dsc.data = data;
        lv_image_set_src(v->cover, &v->cover_dsc);
    }
    lv_obj_clear_flag(v->cover, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_progress(music_view_t *v, const codex_media_t *m, int32_t position_ms)
{
    char buf[16];
    format_ms(position_ms, buf, sizeof(buf));
    set_label(v->elapsed, buf);
    format_ms(m->duration_ms, buf, sizeof(buf));
    set_label(v->total, buf);

    int32_t permille = 0;
    if (m->duration_ms > 0) {
        permille = (int32_t)(((int64_t)position_ms * 1000) / m->duration_ms);
        if (permille < 0) permille = 0;
        if (permille > 1000) permille = 1000;
    }
    /* 手指按着的时候不抢滑杆，否则拖到哪被弹回哪 */
    if (!lv_obj_has_state(v->progress, LV_STATE_PRESSED)) {
        lv_slider_set_value(v->progress, permille, LV_ANIM_OFF);
    }
}

static void refresh_volume(music_view_t *v, const codex_media_t *m)
{
    /*
     * 本地值优先。拖动即发之后，轮询回来的还是旧音量（最长滞后一个 2 秒周期），
     * 拿它画芯片、设滑杆，手指刚拖到的位置就会被弹回去 —— 看着就像「调了没用」。
     * 服务端一旦报出和本地相同的值就说明追上了，立刻撤掉覆盖；保质期到了也撤，
     * 免得命令根本没送出去时永远显示一个假数字。
     */
    bool local_live = v->vol_local >= 0 &&
                      (int32_t)(lv_tick_get() - v->vol_local_tick) < VOL_LOCAL_HOLD_MS;
    if (local_live && m->volume_available && m->volume_percent == v->vol_local) {
        v->vol_local = -1;
        local_live = false;
    }

    if (local_live) {
        volume_chip_set(v, v->vol_local, m->muted);
    } else if (m->volume_available) {
        volume_chip_set(v, m->volume_percent, m->muted);
    } else {
        set_label(v->volume_pct, "--");
    }

    /* 手指按着的时候不抢滑杆，否则拖到哪被弹回哪 */
    if (!local_live && !lv_obj_has_state(v->vol_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(v->vol_slider, m->volume_available ? m->volume_percent : 0,
                            LV_ANIM_OFF);
    }

    /* 5 秒自动关闭放在刷新回合里做：不为它单开一个定时器，也不用担心忘了删 */
    if (v->vol_pop_open && (int32_t)(lv_tick_get() - v->vol_pop_deadline) >= 0) {
        v->vol_pop_open = false;
        lv_obj_add_flag(v->vol_pop, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_play_icon(music_view_t *v, const codex_media_t *m)
{
    const char *want =
        (m->available && m->state == CODEX_MEDIA_PLAYING) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    set_label(v->play_icon, want);
}

static void set_hint(music_view_t *v, bool show, const char *text, uint32_t color)
{
    for (int32_t i = 0; i < v->lyric_lines; i++) {
        set_hidden(v->lyrics[i], show);
    }
    if (show) {
        lv_obj_set_style_text_color(v->hint, lv_color_hex(color), 0);
        set_label(v->hint, text);
        lv_obj_clear_flag(v->hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_lyrics(music_view_t *v, const codex_media_t *m, int32_t position_ms)
{
    const int32_t lines = v->lyric_lines;

    if (!m->available) {
        set_hint(v, true, m->bridge_ok ? "没有在播放的媒体" : m->error,
                 m->bridge_ok ? UI_COL_TEXT_DIM : UI_COL_WARN);
        return;
    }

    int32_t total = codex_media_lyrics_count();
    if (total <= 0) {
        set_hint(v, true, "这首歌没有歌词", UI_COL_TEXT_DIM);
        return;
    }
    set_hint(v, false, NULL, 0);

    int32_t cur = codex_media_lyric_find(position_ms);

    /* 当前句放中间，上下各留一半；靠头靠尾时夹到边界 */
    int32_t first = cur - lines / 2;
    if (first < 0) first = 0;
    if (total - first < lines) first = total - lines;
    if (first < 0) first = 0;

    codex_lyric_line_t win[LYRIC_MAX];
    int32_t n = codex_media_lyrics_window(first, win, lines);

    for (int32_t i = 0; i < lines; i++) {
        set_label(v->lyrics[i], i < n ? win[i].text : "");
    }

    /* 只有当前句换行时才重设颜色，其余时间一个字都不动 */
    if (cur == v->drawn_lyric_index) {
        return;
    }
    v->drawn_lyric_index = cur;
    for (int32_t i = 0; i < lines; i++) {
        bool active = (first + i) == cur;
        lv_obj_set_style_text_color(v->lyrics[i],
                                    lv_color_hex(active ? UI_COL_TEXT : UI_COL_TEXT_DIM), 0);
    }
}

static void refresh_view(music_view_t *v, const codex_media_t *m, int32_t position_ms)
{
    uint32_t rev = codex_media_rev();
    if (rev != v->drawn_media_rev) {
        v->drawn_media_rev = rev;
        v->drawn_lyric_index = INT32_MIN;
        refresh_static(v, m);
    }

    refresh_cover(v, m);
    refresh_progress(v, m, position_ms);
    refresh_volume(v, m);
    refresh_play_icon(v, m);
    refresh_lyrics(v, m, position_ms);
}

static void music_refresh_now(void)
{
    codex_media_t m;
    codex_media_get(&m);

    /*
     * 播放位置按本地单调时钟往前推：媒体 2 秒才轮询一次，不外推的话进度条和歌词
     * 都会一格一格跳。暂停时不外推，位置就停在 bridge 给的数值上。
     */
    int32_t position_ms = m.position_ms;
    if (m.state == CODEX_MEDIA_PLAYING && m.fetched_at_us > 0) {
        int64_t since_us = esp_timer_get_time() - m.fetched_at_us;
        if (since_us > 0) {
            position_ms += (int32_t)(since_us / 1000);
        }
    }
    if (m.duration_ms > 0 && position_ms > m.duration_ms) {
        position_ms = m.duration_ms;
    }

    /*
     * 只刷看得见的那个。另一个整块 HIDDEN，重设文字确实不会触发重绘，但歌词那段
     * 每轮都要拷一窗出来、逐个 strcmp，白烧 CPU。代价是切换时必须把缓存作废。
     */
    refresh_view(s_fullscreen ? &s_full : &s_mini, &m, position_ms);
}

static void music_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    music_refresh_now();
}

static void music_set_fullscreen(bool on)
{
    if (s_fullscreen == on) {
        return;
    }
    s_fullscreen = on;

    if (on) {
        lv_obj_clear_flag(s_full.root, LV_OBJ_FLAG_HIDDEN);
        /*
         * 每次显示都重新提到最前：看板在 ui_music_create 之后还会往 screen 上挂
         * 「字体缺失」那条提示，后挂的默认在上层，不提一句就会被它压住。
         */
        lv_obj_move_to_index(s_full.root, -1);
    } else {
        lv_obj_add_flag(s_full.root, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * 目标视图的缓存全部作废再立刻刷一次。隐藏期间它一轮都没刷过，drawn_* 停在
     * 上次显示时的值，不作废的话切进来那一屏还是旧歌名。
     */
    music_view_t *v = on ? &s_full : &s_mini;
    v->drawn_media_rev = 0;
    v->drawn_cover_rev = 0;
    v->drawn_lyric_index = INT32_MIN;

    /* 弹出层浮在各自 root 上，切视图还留着会盖在新视图上，两边都收掉 */
    s_mini.vol_pop_open = false;
    s_full.vol_pop_open = false;
    lv_obj_add_flag(s_mini.vol_pop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_full.vol_pop, LV_OBJ_FLAG_HIDDEN);
    music_refresh_now();
}

/* ---------------------------------------------------------------- 入口 */

void ui_music_create(lv_obj_t *parent)
{
    build_mini(parent);
    build_fullscreen(lv_obj_get_screen(parent));
    lv_obj_move_to_index(s_full.root, -1);

    /* -1 = 没有本地音量 / 一次都没发过；留着静态零值会挡掉拖到 0% 的那次事件 */
    s_mini.vol_local = -1;
    s_mini.vol_sent = -1;
    s_full.vol_local = -1;
    s_full.vol_sent = -1;

    lv_timer_create(music_refresh_cb, REFRESH_PERIOD_MS, NULL);
}
