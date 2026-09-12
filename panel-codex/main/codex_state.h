#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 面板全局状态快照
 *
 * 取数任务写、UI 任务读，中间用互斥量隔开。UI 永远拿整份拷贝，不持有指针，
 * 这样 LVGL 回调里读到的一定是自洽的一组值，不会出现「配额是新的、任务是旧的」。
 */

#define CODEX_MAX_WINDOWS 2
/** 必须与 bridge 的 MAX_TASKS 一致，否则多出来的会被静默丢弃 */
#define CODEX_MAX_TASKS 5
/** 48 个汉字的 UTF-8 上限加省略号，与 bridge 侧的截断长度对应 */
#define CODEX_TITLE_CAP 160

typedef enum {
    CODEX_LINK_NONE = 0,
    CODEX_LINK_ETH,
    CODEX_LINK_WIFI,
} codex_link_t;

typedef enum {
    CODEX_TASK_RUNNING = 0,
    CODEX_TASK_FAILED,
    CODEX_TASK_INTERRUPTED,
    CODEX_TASK_TURN_COMPLETED,
} codex_task_state_t;

typedef enum {
    CODEX_RADAR_UNAVAILABLE = 0,
    CODEX_RADAR_NO_WATCH,
    CODEX_RADAR_ACTIVE_WATCH,
    CODEX_RADAR_CONFIRMED,
} codex_radar_state_t;

typedef enum {
    CODEX_RESET_REGULAR = 0,
    CODEX_RESET_CREDIT,
} codex_reset_kind_t;

typedef struct {
    char name[24];       /* "7 天" —— bridge 已按窗口时长格式化好 */
    int32_t used_percent;
    int32_t remaining_percent;
    /** 取数那一刻距离重置还有多少秒，UI 靠本地单调时钟往下减 */
    int32_t resets_in_seconds;
} codex_window_t;

typedef struct {
    char title[CODEX_TITLE_CAP];
    codex_task_state_t state;
    /** 取数那一刻这条动态发生于多少秒前 */
    int32_t activity_ago_seconds;
} codex_task_t;

typedef struct {
    /** 内容真的变了才自增，UI 靠它判断要不要重绘 */
    uint32_t rev;

    codex_link_t link;
    char ip[16];

    /** 是否曾经成功取到过一份数据。false 时 UI 显示等待，而不是拿零值冒充 */
    bool data_valid;
    /** 上次成功取数时的 esp_timer 微秒数，UI 据此显示「更新于 N 秒前」 */
    int64_t fetched_at_us;
    int32_t fetch_fail_count;
    char fetch_error[64];

    bool privacy;

    bool quota_valid;
    int32_t window_count;
    codex_window_t windows[CODEX_MAX_WINDOWS];
    int32_t reset_credits;
    char plan_type[16];
    bool quota_stale;

    codex_radar_state_t radar;
    codex_reset_kind_t radar_kind;
    /** 上游没给概率时为 -1，此时只显示预测窗口，不显示信号尺刻度 */
    int32_t radar_chance_percent;
    char radar_window[64];
    /** 确认事件距今多少秒，仅 CODEX_RADAR_CONFIRMED 时有效 */
    int32_t radar_announced_ago_seconds;
    bool radar_stale;

    int32_t task_count;
    codex_task_t tasks[CODEX_MAX_TASKS];
    int32_t hidden_task_count;
    bool task_available;
    bool task_stale;
} codex_snapshot_t;

/** 必须在任何 setter 之前调用一次 */
void codex_state_init(void);

/** 取整份拷贝 */
void codex_state_get(codex_snapshot_t *out);

void codex_state_set_net(codex_link_t link, const char *ip);

/**
 * 用一份新解析出来的数据整体替换业务字段
 *
 * 只替换 bridge 提供的部分，链路与失败计数不动。内容与上次相同时不动 rev，
 * 避免 20 秒一轮的轮询把界面刷得一直在闪。
 */
void codex_state_apply(const codex_snapshot_t *fresh);

/**
 * 记一次取数失败
 *
 * 保留上次的有效数据：网络抖一下不该让屏幕上的额度归零。
 */
void codex_state_set_fetch_error(const char *reason);

/* ---------------------------------------------------------------- 媒体
 *
 * 音乐控制器的状态刻意不并入 codex_snapshot_t：
 *
 *   1. codex_state_apply 是拿 memcmp 整份快照来决定 rev 要不要自增的，而播放位置
 *      每 2 秒就变一次 —— 混进去会让雷达和任务动态每 2 秒都被判定为「内容变了」
 *      而重绘，白白闪。
 *   2. 歌词和封面是 KB 到几十 KB 量级，塞进快照就等于让 UI 每秒把它们拷一遍，
 *      而 UI 那份快照是 LVGL 任务栈上的局部变量，栈只有 16KB。
 *
 * 所以媒体单独一套存储、单独一个 rev，取数节奏也和看板的 20 秒分开。
 */

/** 中文一个字 UTF-8 占 3 字节，歌名按 30 个字封顶 */
#define CODEX_MEDIA_TITLE_CAP 96
#define CODEX_MEDIA_ARTIST_CAP 64
#define CODEX_MEDIA_ALBUM_CAP 64
/** source 是 Windows 的 AppUserModelID，UWP 应用那个是包全名，能到六十多字符 */
#define CODEX_MEDIA_SOURCE_CAP 80
#define CODEX_MEDIA_ERROR_CAP 48

/** 歌词行数按实测的 47 行留余量；面板只在切歌时整份加载一次 */
#define CODEX_LYRIC_MAX_LINES 64
#define CODEX_LYRIC_TEXT_CAP 128

/**
 * 与 media_server.py 的 COVER_SIZE 一致，改一边必须改另一边
 *
 * 面板是按字节数校验的（见 codex_media_set_cover），两边不一致时整张图被丢掉，
 * 屏幕上表现为「歌名有、封面永远是占位符」，串口里只有一行 error，很容易看漏。
 * 192×192×2 = 72KB，常驻 PSRAM。
 */
#define CODEX_COVER_SIZE 192
#define CODEX_COVER_BYTES (CODEX_COVER_SIZE * CODEX_COVER_SIZE * 2)

typedef enum {
    CODEX_MEDIA_INACTIVE = 0,
    CODEX_MEDIA_PLAYING,
    CODEX_MEDIA_PAUSED,
    /** stopped / changing 之类，面板统一按「有歌但没在放」处理 */
    CODEX_MEDIA_OTHER,
} codex_media_state_t;

typedef struct {
    /*
     * bridge_ok 与 available 是两件不同的事，合并成一个字段的话用户会把
     * 「PC 上的媒体服务没起」误当成「我没在听歌」：
     *   bridge_ok=false  PC 上的 media_server.py 没跑或崩了 → 提示去起服务
     *   available=false  服务在跑，但此刻没有播放器在放歌   → 正常空闲
     */
    bool bridge_ok;
    bool available;

    char source[CODEX_MEDIA_SOURCE_CAP];
    char title[CODEX_MEDIA_TITLE_CAP];
    char artist[CODEX_MEDIA_ARTIST_CAP];
    char album[CODEX_MEDIA_ALBUM_CAP];

    int32_t duration_ms;
    /** 取数那一刻的播放位置，UI 靠本地单调时钟往前推，不依赖 SNTP */
    int32_t position_ms;
    /**
     * 这份数据是什么时候取到的（本地 esp_timer 微秒），由 codex_media_apply 打戳，
     * 不由 bridge 提供 —— 与 codex_snapshot_t 的 fetched_at_us 同一套做法。
     * UI 用它把 position_ms 外推到渲染那一刻，2 秒拉一次也能让进度条平滑走动。
     */
    int64_t fetched_at_us;
    codex_media_state_t state;

    /*
     * 能力位。播放器没暴露的操作，面板必须禁用对应控件，而不是画一个按不动的
     * 按钮 —— 那是最难查的一种「坏了」。实测 QQ音乐 只给 play_pause/next/prev，
     * seek、shuffle、repeat 全是 false，换个播放器可能又不一样，所以由 bridge 下发。
     */
    bool can_play_pause;
    bool can_next;
    bool can_prev;
    bool can_seek;
    bool can_shuffle;
    bool can_repeat;

    /*
     * 能力位只说明播放器「支不支持」切换，按钮该画成开还是关得看当前值。
     * repeat_mode 是 media_server.py 下发的原文（none/track/list/unknown），
     * 由面板映射成中文，省一次往返。
     */
    bool shuffle_active;
    char repeat_mode[16];

    /**
     * bridge 说这首歌有封面，不等于像素已经下到面板上了（后者看
     * codex_media_cover）。取数任务用它决定要不要为这首歌发一次 /cover 请求，
     * 省掉一次注定落空的往返。
     */
    bool has_cover;
    bool volume_available;
    /** 这是 Windows 主音量、不是播放器自己的音量条 */
    int32_t volume_percent;
    bool muted;

    char error[CODEX_MEDIA_ERROR_CAP];
} codex_media_t;

typedef struct {
    int32_t time_ms;
    char text[CODEX_LYRIC_TEXT_CAP];
} codex_lyric_line_t;

/** 必须在任何 setter 之前调用一次 */
void codex_media_init(void);

/** 整份拷贝，约 400 字节，放栈上安全 */
void codex_media_get(codex_media_t *out);
void codex_media_apply(const codex_media_t *fresh);
void codex_media_set_error(bool bridge_ok, const char *reason);

/**
 * 歌曲内容变了才自增，UI 靠它决定要不要重排标题、换封面、重新加载歌词
 *
 * 播放位置和取数时间戳不算内容变化 —— 它们每 2 秒都在变，算进去就等于每次轮询
 * 都触发一遍重排，进度条会跟着一起闪。所以 rev 由 codex_media_apply 内部算，
 * 不进 codex_media_t，UI 单独取。
 */
uint32_t codex_media_rev(void);

/**
 * 歌词
 *
 * UI 要的是「现在该显示哪一句、它上下各是什么」，所以给的是定位加取窗，不是整份
 * 副本 —— 整份 8KB 拷出去既会啃穿 LVGL 那 16KB 的任务栈，又要在 UI 侧再存一遍，
 * 和这里的存储重复。一窗七行不到 1KB，放栈上安全。
 *
 * 窗口在锁内一次拷完，所以读不到半新半旧的歌词。find/count/window 三次调用之间
 * 理论上能被换歌插进来，但 UI 每轮都重新取、不缓存任何东西，真撞上了也只错半个
 * 秒，下一轮自己就对了。
 */
int32_t codex_media_lyrics_count(void);
/** 二分定位当前该显示哪一行；没有歌词、或还没唱到第一句时返回 -1 */
int32_t codex_media_lyric_find(int32_t position_ms);
/** 从 first 行起最多拷 max_count 行到 out，返回实际拷出的行数，越界自动截断 */
int32_t codex_media_lyrics_window(int32_t first, codex_lyric_line_t *out, int32_t max_count);
void codex_media_set_lyrics(const codex_lyric_line_t *lines, int32_t count);

/**
 * 封面
 *
 * 72KB 常驻 PSRAM，UI 直接拿指针喂 lv_image，不拷贝。指针在锁外也有效，因为
 * 缓冲在 init 时分配一次、之后不释放也不重分配，变的只有内容和 rev。
 *
 * UI 要同时看 media.has_cover：这首歌本来就没图时这里仍可能留着上一首的像素，
 * 只看返回值会把上一首的封面挂在新歌名下。rev 变了才需要重新 lv_image_set_src。
 */
bool codex_media_cover(const uint8_t **data, uint32_t *rev);
/** 写入由取数任务调，写完 rev 自增，UI 下一轮就能看到 */
bool codex_media_set_cover(const uint8_t *rgb565, int32_t bytes);

#ifdef __cplusplus
}
#endif
