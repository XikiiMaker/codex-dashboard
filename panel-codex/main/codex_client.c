#include "codex_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "codex_defaults.h"
#include "codex_net.h"
#include "codex_state.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "codex_client";

#define CODEX_HTTP_TIMEOUT_MS 6000
/**
 * 封面单独给更长的超时
 *
 * 它是 72KB 裸 RGB565，而 LWIP 的接收窗口只有 5760 字节（CONFIG_LWIP_TCP_WND_DEFAULT），
 * 一张图要十来个窗口来回。实测 WiFi 上收到一半卡住 6 秒以上不止一次（串口抓到
 * "Incomlete data received, ret=0, 27060/73728"，ret=0 是读超时不是对端关闭），
 * 小 JSON 那档 6 秒对它就太紧了。
 */
#define COVER_TIMEOUT_MS 10000
/** 连试这么多次还不行就放弃这首歌，别把媒体轮询一直堵在封面上 */
#define COVER_TRIES_MAX 4
/** 重试间隔按次数线性拉长：3s、6s、9s */
#define COVER_RETRY_GAP_US (3 * 1000 * 1000)
/**
 * 控制命令单独给更短的超时
 *
 * 拖音量、按播放这类操作要的是「立刻有反应」，而这条链路上真正卡延迟的不是服务端
 * （curl 实测 3ms）而是 WiFi：32 秒的串口抓取里就出现过两次
 * "Connection timed out before data was ready"。命令在媒体任务里是串行发的，
 * 一次 6 秒超时会把排在后面的命令全堵在那儿 —— 手指早松了音量才变。
 *
 * 缩到 2 秒，丢了也不重试：拖动即发意味着下一个 VALUE_CHANGED 会带来更新的值，
 * RELEASED 还会把终值补发一次，自己就愈合了。
 */
#define CONTROL_TIMEOUT_MS 2000
/** 五条任务标题各 160 字节封顶，加配额与雷达字段约 3KB，留一倍余量 */
#define CODEX_RESP_MAX 6144
/**
 * 网络任务栈
 *
 * esp_http_client 加 cJSON 在 5120 上实测会溢出，而栈溢出的表象是屏幕定格在半张
 * 画面，极容易误判成花屏。这里按 panel-firmware 里验证过的 8192 档给。
 */
#define CODEX_TASK_STACK 8192

static TaskHandle_t s_task;

typedef struct {
    char *buf;
    int len;
    int cap;
    /** 响应里的 ETag，原样存（含引号），下次拿去发 If-None-Match */
    char etag[40];
} resp_sink_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_sink_t *sink = evt->user_data;
    if (sink == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        /*
         * ETag 只能在收响应头时抓。这个事件每来一个头触发一次，key/value 已经放在
         * evt 里了，不用再去问客户端 —— perform() 返回后响应头往往已经清掉了，
         * 那时再取就是空，封面会退化成每次都重传 32KB。
         */
        if (evt->header_key && evt->header_value && strcasecmp(evt->header_key, "ETag") == 0) {
            strlcpy(sink->etag, evt->header_value, sizeof(sink->etag));
        }
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    int room = sink->cap - 1 - sink->len;
    int copy = evt->data_len < room ? evt->data_len : room;
    if (copy > 0) {
        memcpy(sink->buf + sink->len, evt->data, copy);
        sink->len += copy;
        sink->buf[sink->len] = '\0';
    }
    return ESP_OK;
}

/**
 * 一次 HTTP 往返的入参与出参
 *
 * buf/cap 是调用方备好的响应体缓冲。出参里 len 是实收字节数，status 是状态码，
 * etag 只有服务端给了才有内容。
 */
typedef struct {
    const char *path;                 /* 入："/api/dashboard" */
    esp_http_client_method_t method;  /* 入 */
    const char *post_body;            /* 入：可为 NULL，非空则按 JSON 发 */
    const char *if_none_match;        /* 入：可为 NULL，非空就是条件请求 */
    /** 入：0 表示用 CODEX_HTTP_TIMEOUT_MS，大响应体（封面）单独放宽 */
    int timeout_ms;
    char *buf;                        /* 入 */
    int cap;                          /* 入 */
    int len;                          /* 出 */
    int status;                       /* 出 */
    char etag[40];                    /* 出 */
} http_req_t;

/**
 * 返回 ESP_OK 只代表链路通了，成没成要看 req->status
 *
 * 304 是封面的正常路径而不是错误；媒体服务没起时 bridge 回的也是 200 带
 * media_bridge:false。非 2xx 一律判失败的话这两种情况都会被误当成链路故障。
 */
static esp_err_t http_roundtrip(http_req_t *req)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d%s", CODEX_BRIDGE_HOST, CODEX_BRIDGE_PORT, req->path);

    resp_sink_t sink = { .buf = req->buf, .len = 0, .cap = req->cap, .etag = {0} };
    req->buf[0] = '\0';
    req->len = 0;
    req->status = 0;
    req->etag[0] = '\0';

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = req->method,
        .timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : CODEX_HTTP_TIMEOUT_MS,
        .event_handler = http_event,
        .user_data = &sink,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    if (CODEX_BRIDGE_TOKEN[0]) {
        esp_http_client_set_header(client, "X-Panel-Token", CODEX_BRIDGE_TOKEN);
    }
    if (req->if_none_match && req->if_none_match[0]) {
        esp_http_client_set_header(client, "If-None-Match", req->if_none_match);
    }
    if (req->post_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        /* set_post_field 配 perform() 就够，不必自己 open/write/fetch_headers 走流式那套 */
        esp_http_client_set_post_field(client, req->post_body, (int)strlen(req->post_body));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        req->status = esp_http_client_get_status_code(client);
        req->len = sink.len;
        strlcpy(req->etag, sink.etag, sizeof(req->etag));
    }
    esp_http_client_cleanup(client);
    return err;
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool json_bool(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(item);
}

static void json_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out, item->valuestring, cap);
    } else {
        out[0] = '\0';
    }
}

static codex_task_state_t parse_task_state(const char *text)
{
    if (text == NULL) return CODEX_TASK_TURN_COMPLETED;
    if (strcmp(text, "running") == 0) return CODEX_TASK_RUNNING;
    if (strcmp(text, "failed") == 0) return CODEX_TASK_FAILED;
    if (strcmp(text, "interrupted") == 0) return CODEX_TASK_INTERRUPTED;
    return CODEX_TASK_TURN_COMPLETED;
}

static void parse_quota(const cJSON *root, codex_snapshot_t *out)
{
    const cJSON *quota = cJSON_GetObjectItemCaseSensitive(root, "quota");
    if (!cJSON_IsObject(quota)) {
        return;
    }
    const cJSON *windows = cJSON_GetObjectItemCaseSensitive(quota, "windows");
    if (!cJSON_IsArray(windows)) {
        return;
    }
    const cJSON *window = NULL;
    cJSON_ArrayForEach(window, windows) {
        if (out->window_count >= CODEX_MAX_WINDOWS) break;
        if (!cJSON_IsObject(window)) continue;
        codex_window_t *slot = &out->windows[out->window_count];
        json_str(window, "name", slot->name, sizeof(slot->name));
        slot->used_percent = json_int(window, "used_percent", 0);
        slot->remaining_percent = json_int(window, "remaining_percent", 0);
        slot->resets_in_seconds = json_int(window, "resets_in_seconds", -1);
        out->window_count++;
    }
    if (out->window_count == 0) {
        return;
    }
    out->reset_credits = json_int(quota, "reset_credits", 0);
    json_str(quota, "plan_type", out->plan_type, sizeof(out->plan_type));
    out->quota_stale = json_bool(quota, "stale");
    out->quota_valid = true;
}

static void parse_radar(const cJSON *root, codex_snapshot_t *out)
{
    out->radar = CODEX_RADAR_UNAVAILABLE;
    out->radar_chance_percent = -1;
    out->radar_announced_ago_seconds = -1;

    const cJSON *radar = cJSON_GetObjectItemCaseSensitive(root, "radar");
    if (!cJSON_IsObject(radar)) {
        return;
    }
    char state[24];
    json_str(radar, "state", state, sizeof(state));
    if (strcmp(state, "active_watch") == 0) {
        out->radar = CODEX_RADAR_ACTIVE_WATCH;
        out->radar_chance_percent = json_int(radar, "reset_chance_percent", -1);
        json_str(radar, "forecast_window", out->radar_window, sizeof(out->radar_window));
    } else if (strcmp(state, "confirmed") == 0) {
        out->radar = CODEX_RADAR_CONFIRMED;
        char kind[24];
        json_str(radar, "kind", kind, sizeof(kind));
        out->radar_kind =
            strcmp(kind, "reset_credit") == 0 ? CODEX_RESET_CREDIT : CODEX_RESET_REGULAR;
        out->radar_announced_ago_seconds = json_int(radar, "announced_ago_seconds", -1);
    } else if (strcmp(state, "no_active_watch") == 0) {
        out->radar = CODEX_RADAR_NO_WATCH;
    }
    out->radar_stale = json_bool(root, "radar_stale");
}

static void parse_tasks(const cJSON *root, codex_snapshot_t *out)
{
    out->task_available = json_bool(root, "task_activity_available");
    out->task_stale = json_bool(root, "task_activity_stale");
    out->hidden_task_count = json_int(root, "hidden_task_count", 0);

    const cJSON *tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (!cJSON_IsArray(tasks)) {
        return;
    }
    const cJSON *task = NULL;
    cJSON_ArrayForEach(task, tasks) {
        if (out->task_count >= CODEX_MAX_TASKS) break;
        if (!cJSON_IsObject(task)) continue;
        codex_task_t *slot = &out->tasks[out->task_count];
        json_str(task, "title", slot->title, sizeof(slot->title));
        char state[24];
        json_str(task, "state", state, sizeof(state));
        slot->state = parse_task_state(state);
        slot->activity_ago_seconds = json_int(task, "activity_ago_seconds", -1);
        out->task_count++;
    }
}

/** 解析整份响应。任何一段缺失都只让那一段留空，不丢弃整份数据 */
static bool parse_payload(const char *body, codex_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->privacy = json_bool(root, "privacy");
    parse_quota(root, out);
    parse_radar(root, out);
    parse_tasks(root, out);
    cJSON_Delete(root);
    /* 配额是这块屏的主角，它都没解析出来就当这轮失败，保留上次的画面 */
    return out->quota_valid;
}

static void poll_once(char *buf)
{
    if (CODEX_BRIDGE_HOST[0] == '\0') {
        codex_state_set_fetch_error("未配置 bridge 地址");
        return;
    }
    if (!codex_net_is_up()) {
        codex_state_set_fetch_error("网络未就绪");
        return;
    }

    http_req_t req = {
        .path = "/api/dashboard",
        .method = HTTP_METHOD_GET,
        .buf = buf,
        .cap = CODEX_RESP_MAX,
    };
    esp_err_t err = http_roundtrip(&req);
    if (err != ESP_OK) {
        codex_state_set_fetch_error(esp_err_to_name(err));
        return;
    }
    if (req.status < 200 || req.status >= 300) {
        /*
         * 把状态码本身写进错误，而不是笼统一句「失败」：401 是 token 不对、
         * 404 是 bridge 版本比面板旧、500 是 bridge 内部炸了，三者的查法完全不同，
         * 屏上能直接看到就不用回去翻串口日志。
         */
        char reason[32];
        snprintf(reason, sizeof(reason), "HTTP %d", req.status);
        ESP_LOGW(TAG, "bridge 返回 HTTP %d", req.status);
        codex_state_set_fetch_error(reason);
        return;
    }

    codex_snapshot_t fresh;
    if (!parse_payload(buf, &fresh)) {
        codex_state_set_fetch_error("响应无法解析");
        return;
    }
    codex_state_apply(&fresh);
}

static void poll_task(void *arg)
{
    (void)arg;
    /* 响应缓冲放在任务堆上而不是栈上：6KB 会把 8192 的栈啃掉大半 */
    char *buf = malloc(CODEX_RESP_MAX);
    if (buf == NULL) {
        ESP_LOGE(TAG, "响应缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }

    codex_net_wait_up(15000);

    for (;;) {
        poll_once(buf);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CODEX_POLL_INTERVAL_MS));
    }
}

/* ---------------------------------------------------------------- 媒体 */

/**
 * 三个媒体端点共用一块响应缓冲
 *
 * /api/media 不到 1KB，/api/media/lyrics 实测 47 行约 12KB，/api/media/cover 是
 * 72KB 裸 RGB565。按最大的那个给，放 PSRAM —— 内部 RAM 连 LVGL 的绘制缓冲都
 * 挤不下，这块又只在媒体任务里用，慢一点无所谓。
 */
#define MEDIA_RESP_MAX (CODEX_COVER_BYTES + 4096)
/** 和看板任务同档：esp_http_client 加 cJSON 在 5120 上实测会溢出 */
#define MEDIA_TASK_STACK 8192
/** 带值的命令会合并，真正能堆积的只有连点的播放/切歌，4 个够了 */
#define MEDIA_CMD_SLOTS 4
#define MEDIA_CMD_ACTION_CAP 16
/** 四段字段拼起来当歌曲指纹，加 32 给分隔符和时长整数 */
#define MEDIA_KEY_CAP                                                          \
    (CODEX_MEDIA_SOURCE_CAP + CODEX_MEDIA_TITLE_CAP + CODEX_MEDIA_ARTIST_CAP + \
     CODEX_MEDIA_ALBUM_CAP + 32)

typedef struct {
    char action[MEDIA_CMD_ACTION_CAP];
    /** <0 表示这条命令不带值 */
    int32_t value;
    bool used;
} media_cmd_t;

typedef struct {
    char *resp;
    /** 解析歌词的临时数组，整份交给 codex_media_set_lyrics 后就没用了 */
    codex_lyric_line_t *lyrics;
    /** 上一轮那首歌的指纹，用来判断是不是切歌了 */
    char song_key[MEDIA_KEY_CAP];
    /** 封面 ETag，原样存原样发，切歌时清空 */
    char cover_etag[40];
    /** 这首歌的封面已经了结（拿到了，或重试到上限放弃了），不再发请求 */
    bool cover_done;
    int cover_tries;
    /** esp_timer 微秒时刻，到这个点才允许再试一次 */
    int64_t cover_next_us;
} media_ctx_t;

static media_cmd_t s_cmds[MEDIA_CMD_SLOTS];
static SemaphoreHandle_t s_cmd_lock;
static TaskHandle_t s_media_task;

static codex_media_state_t parse_media_state(const char *text)
{
    if (strcmp(text, "playing") == 0) return CODEX_MEDIA_PLAYING;
    if (strcmp(text, "paused") == 0) return CODEX_MEDIA_PAUSED;
    if (text[0] == '\0') return CODEX_MEDIA_INACTIVE;
    /* closed / opened / changing / stopped / unknown 一律按「有歌但没在放」 */
    return CODEX_MEDIA_OTHER;
}

static void parse_media(const cJSON *root, codex_media_t *out)
{
    /*
     * 必须先 memset：codex_media_apply 拿 memcmp 整份比来决定 rev 要不要自增，
     * 结构体填充字节里的脏值会让「内容没变」被判成「变了」，界面每 2 秒重排一次。
     */
    memset(out, 0, sizeof(*out));
    out->bridge_ok = true;
    out->available = json_bool(root, "available");

    json_str(root, "source", out->source, sizeof(out->source));
    json_str(root, "title", out->title, sizeof(out->title));
    json_str(root, "artist", out->artist, sizeof(out->artist));
    json_str(root, "album", out->album, sizeof(out->album));
    out->duration_ms = json_int(root, "duration_ms", 0);
    out->position_ms = json_int(root, "position_ms", 0);
    /* fetched_at_us 由 codex_media_apply 自己打戳，bridge 给的时间面板信不过 */

    char state[16];
    json_str(root, "state", state, sizeof(state));
    out->state = parse_media_state(state);

    /*
     * 能力位缺失必须当「不可用」而不是当「可用」：播放器没暴露的操作，SMTC 的
     * try_* 只返回 False 不报错，画出来的按钮按下去毫无反应，那是最难查的一种坏。
     */
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(root, "caps");
    if (cJSON_IsObject(caps)) {
        out->can_play_pause = json_bool(caps, "play_pause");
        out->can_next = json_bool(caps, "next");
        out->can_prev = json_bool(caps, "prev");
        out->can_seek = json_bool(caps, "seek");
        out->can_shuffle = json_bool(caps, "shuffle");
        out->can_repeat = json_bool(caps, "repeat");
    }

    out->has_cover = json_bool(root, "has_cover");
    out->shuffle_active = json_bool(root, "shuffle");
    json_str(root, "repeat", out->repeat_mode, sizeof(out->repeat_mode));

    /* 这是 Windows 主音量，SMTC 根本没有音量 API，只能走 COM 端点音量 */
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if (cJSON_IsObject(volume)) {
        out->volume_available = json_bool(volume, "available");
        out->volume_percent = json_int(volume, "percent", 0);
        out->muted = json_bool(volume, "muted");
    }
}

static void fetch_lyrics(media_ctx_t *ctx)
{
    if (!ctx->lyrics) {
        codex_media_set_lyrics(NULL, 0);
        return;
    }

    http_req_t req = {
        .path = "/api/media/lyrics",
        .method = HTTP_METHOD_GET,
        .buf = ctx->resp,
        .cap = MEDIA_RESP_MAX,
    };
    /*
     * 取不到就清空，而不是留着上一首的歌词：歌词跟着进度条走，挂错了会一句一句
     * 明明白白地对不上，比干脆不显示更让人以为面板坏了。
     */
    if (http_roundtrip(&req) != ESP_OK || req.status != 200) {
        ESP_LOGW(TAG, "歌词没取到（HTTP %d），本首不显示歌词", req.status);
        codex_media_set_lyrics(NULL, 0);
        return;
    }

    cJSON *root = cJSON_Parse(req.buf);
    if (!root) {
        codex_media_set_lyrics(NULL, 0);
        return;
    }

    int32_t count = 0;
    const cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "lines");
    if (cJSON_IsArray(lines)) {
        const cJSON *line = NULL;
        cJSON_ArrayForEach(line, lines) {
            if (count >= CODEX_LYRIC_MAX_LINES) break;
            if (!cJSON_IsObject(line)) continue;
            codex_lyric_line_t *slot = &ctx->lyrics[count];
            slot->time_ms = json_int(line, "time_ms", 0);
            json_str(line, "text", slot->text, sizeof(slot->text));
            /* 空文本行多半是间奏，bridge 那边已经滤过一道，这里再兜一次 */
            if (slot->text[0] == '\0') continue;
            count++;
        }
    }
    cJSON_Delete(root);
    codex_media_set_lyrics(ctx->lyrics, count);
}

/**
 * 取一次封面。返回 true 表示「这件事了结了」，false 表示链路失败、值得再试
 *
 * 304 和尺寸不符都算了结：前者说明面板上已经是这张图，后者重试一百次也是同样
 * 的结果，只有传输层失败（超时、收到一半断掉）才该重来。
 */
static bool fetch_cover(media_ctx_t *ctx)
{
    http_req_t req = {
        .path = "/api/media/cover",
        .method = HTTP_METHOD_GET,
        .if_none_match = ctx->cover_etag,
        .timeout_ms = COVER_TIMEOUT_MS,
        .buf = ctx->resp,
        .cap = MEDIA_RESP_MAX,
    };
    if (http_roundtrip(&req) != ESP_OK) {
        return false;
    }
    /* 304 就是「面板上已经是这张了」，连 72KB 都不用传 */
    if (req.status == 304) {
        return true;
    }
    if (req.status != 200) {
        ESP_LOGW(TAG, "封面取回 HTTP %d", req.status);
        return false;
    }

    /*
     * 先入库再记 ETag：尺寸对不上时 codex_media_set_cover 会整张丢掉，此时要是
     * 已经把 ETag 记下了，下一轮就只会收到 304，永远拿不到那张图。
     */
    if (!codex_media_set_cover((const uint8_t *)ctx->resp, req.len)) {
        return true;
    }
    if (req.etag[0]) {
        strlcpy(ctx->cover_etag, req.etag, sizeof(ctx->cover_etag));
    }
    return true;
}

/**
 * 每轮都问一次：这首歌的封面到手了没有
 *
 * 原来只在切歌那一刻取一次，WiFi 抖一下整首歌就再没有图了 —— 而链路抖动在这台
 * 设备上是常态（面板走的是 C6 的 esp-hosted WiFi，以太网没插线）。失败就按
 * 3s/6s/9s 退避重发，连 COVER_TRIES_MAX 次都不行才放弃这首歌。
 */
static void cover_tick(media_ctx_t *ctx, const codex_media_t *song)
{
    if (ctx->cover_done || !song->has_cover) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now < ctx->cover_next_us) {
        return;
    }
    if (fetch_cover(ctx)) {
        ctx->cover_done = true;
        return;
    }

    ctx->cover_tries++;
    if (ctx->cover_tries >= COVER_TRIES_MAX) {
        ESP_LOGW(TAG, "封面连试 %d 次都没取到，这首歌不再重试", ctx->cover_tries);
        ctx->cover_done = true;
        return;
    }
    ctx->cover_next_us = now + (int64_t)COVER_RETRY_GAP_US * ctx->cover_tries;
}

static void refresh_song_assets(media_ctx_t *ctx, const codex_media_t *song)
{
    /* 和 media_server.py 的 Song.key 同一套字段，两边判断「切歌」的口径要一致 */
    char key[MEDIA_KEY_CAP];
    snprintf(key, sizeof(key), "%s|%s|%s|%s|%d", song->source, song->title, song->artist,
             song->album, (int)song->duration_ms);

    if (strcmp(key, ctx->song_key) == 0) {
        return;
    }
    strlcpy(ctx->song_key, key, sizeof(ctx->song_key));
    /* 上一首的 ETag 再拿去问只会换来一个错的 304 */
    ctx->cover_etag[0] = '\0';
    /* 封面交给 cover_tick：这一轮就会发第一次，失败了后面几轮还能接着试 */
    ctx->cover_done = false;
    ctx->cover_tries = 0;
    ctx->cover_next_us = 0;

    ESP_LOGI(TAG, "切歌：%s - %s", song->title, song->artist);
    fetch_lyrics(ctx);
}

static void poll_media(media_ctx_t *ctx)
{
    if (CODEX_BRIDGE_HOST[0] == '\0') {
        codex_media_set_error(false, "未配置 bridge 地址");
        return;
    }
    if (!codex_net_is_up()) {
        codex_media_set_error(false, "网络未就绪");
        return;
    }

    http_req_t req = {
        .path = "/api/media",
        .method = HTTP_METHOD_GET,
        .buf = ctx->resp,
        .cap = MEDIA_RESP_MAX,
    };
    if (http_roundtrip(&req) != ESP_OK) {
        codex_media_set_error(false, "取媒体状态失败");
        return;
    }
    if (req.status < 200 || req.status >= 300) {
        codex_media_set_error(true, "媒体接口状态异常");
        return;
    }

    cJSON *root = cJSON_Parse(req.buf);
    if (!root) {
        codex_media_set_error(true, "媒体响应无法解析");
        return;
    }

    /*
     * media_bridge:false 是 bridge 在说「PC 上的 media_server.py 根本没跑」，和
     * 「服务在跑但此刻没在放歌」是两件完全不同的事：前者用户该去起服务，后者是
     * 正常空闲。混成一个提示的话，用户会盯着「没有音乐」查半天自己的播放器。
     */
    const cJSON *bridge_flag = cJSON_GetObjectItemCaseSensitive(root, "media_bridge");
    if (cJSON_IsBool(bridge_flag) && !cJSON_IsTrue(bridge_flag)) {
        char why[CODEX_MEDIA_ERROR_CAP];
        json_str(root, "error", why, sizeof(why));
        cJSON_Delete(root);
        codex_media_set_error(false, why[0] ? why : "媒体服务没有响应");
        return;
    }

    codex_media_t fresh;
    parse_media(root, &fresh);
    cJSON_Delete(root);
    codex_media_apply(&fresh);

    if (!fresh.available) {
        /* 没在放歌就没有「这首歌」的概念，指纹和 ETag 都要清掉 */
        ctx->song_key[0] = '\0';
        ctx->cover_etag[0] = '\0';
        ctx->cover_done = true;
        return;
    }
    refresh_song_assets(ctx, &fresh);
    cover_tick(ctx, &fresh);
}

static void send_control(media_ctx_t *ctx, const char *action, int32_t value)
{
    char body[96];
    if (value >= 0) {
        /* 服务端按动作取不同的键名：seek 要 position_ms，其余带值的只有音量要 percent */
        const char *key = strcmp(action, "seek") == 0 ? "position_ms" : "percent";
        snprintf(body, sizeof(body), "{\"action\":\"%s\",\"%s\":%d}", action, key, (int)value);
    } else {
        snprintf(body, sizeof(body), "{\"action\":\"%s\"}", action);
    }

    http_req_t req = {
        .path = "/api/media/control",
        .method = HTTP_METHOD_POST,
        .post_body = body,
        .buf = ctx->resp,
        .cap = MEDIA_RESP_MAX,
        .timeout_ms = CONTROL_TIMEOUT_MS,
    };
    if (http_roundtrip(&req) != ESP_OK) {
        ESP_LOGW(TAG, "控制命令 %s 没发出去", action);
        return;
    }

    /*
     * 「播放器不支持这个操作」服务端回的也是 200，只有 body 里的 ok 能说明命令到底
     * 生效没有 —— QQ音乐 的 seek/shuffle/repeat 就都走这条路。
     */
    cJSON *root = cJSON_Parse(req.buf);
    if (!root) {
        return;
    }
    if (!json_bool(root, "ok")) {
        char why[64];
        json_str(root, "error", why, sizeof(why));
        ESP_LOGW(TAG, "控制命令 %s 被拒：%s", action, why[0] ? why : "未知原因");
    }
    cJSON_Delete(root);
}

/** 把槽里排着的命令挨个发出去。发完紧接着就会重取一次状态，所以画面立刻跟上 */
static void drain_commands(media_ctx_t *ctx)
{
    for (;;) {
        xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
        int idx = -1;
        for (int i = 0; i < MEDIA_CMD_SLOTS; i++) {
            if (s_cmds[i].used) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            xSemaphoreGive(s_cmd_lock);
            return;
        }
        media_cmd_t cmd = s_cmds[idx];
        s_cmds[idx].used = false;
        xSemaphoreGive(s_cmd_lock);

        send_control(ctx, cmd.action, cmd.value);
    }
}

static void media_poll_task(void *arg)
{
    (void)arg;
    media_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "媒体上下文分配失败");
        vTaskDelete(NULL);
        return;
    }
    ctx->resp = heap_caps_malloc(MEDIA_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->lyrics = heap_caps_malloc(sizeof(codex_lyric_line_t) * CODEX_LYRIC_MAX_LINES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->resp == NULL) {
        ESP_LOGE(TAG, "媒体响应缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }
    if (ctx->lyrics == NULL) {
        /* 歌词缓冲分不出来只关掉歌词，状态和封面还得照常 */
        ESP_LOGW(TAG, "歌词缓冲分配失败，本固件不显示歌词");
    }

    codex_net_wait_up(15000);

    for (;;) {
        /* 命令先于取数：服务端执行完会立刻重读一次 SMTC，这一轮就能拿到新状态 */
        drain_commands(ctx);
        poll_media(ctx);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CODEX_MEDIA_POLL_INTERVAL_MS));
    }
}

esp_err_t codex_client_start(void)
{
    s_cmd_lock = xSemaphoreCreateMutex();
    if (s_cmd_lock == NULL) {
        return ESP_FAIL;
    }

    if (xTaskCreate(poll_task, "codex_poll", CODEX_TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        return ESP_FAIL;
    }

    /*
     * 媒体任务失败不当成整体失败：额度和任务动态是主链路，音乐控制器是附加功能，
     * 不该因为它起不来就让整块屏什么都不显示。
     */
    if (xTaskCreate(media_poll_task, "codex_media", MEDIA_TASK_STACK, NULL, 4, &s_media_task) !=
        pdPASS) {
        s_media_task = NULL;
        ESP_LOGE(TAG, "媒体任务创建失败，音乐控制器不会有数据");
    }
    return ESP_OK;
}

void codex_client_poll_now(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void codex_client_media_cmd(const char *action, int32_t value)
{
    if (action == NULL || action[0] == '\0' || s_cmd_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    media_cmd_t *slot = NULL;
    if (value >= 0) {
        /* 带值的是绝对量，覆盖掉还在排队的同名那条即可，拖音量条不该排出一串 */
        for (int i = 0; i < MEDIA_CMD_SLOTS; i++) {
            if (s_cmds[i].used && strcmp(s_cmds[i].action, action) == 0) {
                slot = &s_cmds[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        for (int i = 0; i < MEDIA_CMD_SLOTS; i++) {
            if (!s_cmds[i].used) {
                slot = &s_cmds[i];
                break;
            }
        }
    }
    if (slot) {
        strlcpy(slot->action, action, sizeof(slot->action));
        slot->value = value;
        slot->used = true;
    }
    xSemaphoreGive(s_cmd_lock);

    if (slot == NULL) {
        ESP_LOGW(TAG, "命令槽满了，丢掉 %s", action);
        return;
    }
    if (s_media_task) {
        xTaskNotifyGive(s_media_task);
    }
}
