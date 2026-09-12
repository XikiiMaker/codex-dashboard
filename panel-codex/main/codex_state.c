#include "codex_state.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *MEDIA_TAG = "codex_media";

static codex_snapshot_t s_snap;
static SemaphoreHandle_t s_lock;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

void codex_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.rev = 1;
    s_snap.radar_chance_percent = -1;
    s_snap.radar_announced_ago_seconds = -1;
}

void codex_state_get(codex_snapshot_t *out)
{
    LOCK();
    memcpy(out, &s_snap, sizeof(*out));
    UNLOCK();
}

void codex_state_set_net(codex_link_t link, const char *ip)
{
    LOCK();
    if (s_snap.link != link || strcmp(s_snap.ip, ip) != 0) {
        s_snap.link = link;
        strlcpy(s_snap.ip, ip, sizeof(s_snap.ip));
        s_snap.rev++;
    }
    UNLOCK();
}

void codex_state_apply(const codex_snapshot_t *fresh)
{
    LOCK();
    /*
     * 比较时排除 rev、链路和取数时间戳：前者是输出，后两者不由 bridge 决定，
     * 而 fetched_at_us 每轮都在变，把它算进比较就等于每轮都判定「内容变了」。
     */
    codex_snapshot_t candidate = *fresh;
    candidate.rev = s_snap.rev;
    candidate.link = s_snap.link;
    strlcpy(candidate.ip, s_snap.ip, sizeof(candidate.ip));
    candidate.fetched_at_us = s_snap.fetched_at_us;
    candidate.fetch_fail_count = 0;
    candidate.fetch_error[0] = '\0';

    bool changed = memcmp(&candidate, &s_snap, sizeof(candidate)) != 0;

    s_snap = candidate;
    s_snap.data_valid = true;
    s_snap.fetched_at_us = esp_timer_get_time();
    if (changed) {
        s_snap.rev++;
    }
    UNLOCK();
}

void codex_state_set_fetch_error(const char *reason)
{
    LOCK();
    s_snap.fetch_fail_count++;
    strlcpy(s_snap.fetch_error, reason ? reason : "", sizeof(s_snap.fetch_error));
    s_snap.rev++;
    UNLOCK();
}

/* ---------------------------------------------------------------- 媒体 */

static codex_media_t s_media;
static uint32_t s_media_rev;
static SemaphoreHandle_t s_media_lock;

static codex_lyric_line_t s_lyrics[CODEX_LYRIC_MAX_LINES];
static int32_t s_lyric_count;

static uint8_t *s_cover;
static bool s_cover_valid;
static uint32_t s_cover_rev;

#define MLOCK() xSemaphoreTake(s_media_lock, portMAX_DELAY)
#define MUNLOCK() xSemaphoreGive(s_media_lock)

void codex_media_init(void)
{
    s_media_lock = xSemaphoreCreateMutex();
    memset(&s_media, 0, sizeof(s_media));
    memset(s_lyrics, 0, sizeof(s_lyrics));

    /*
     * 32KB 只能放 PSRAM：P4 v1.x 的内部可执行 RAM 才 179KB，连 LVGL 的绘制缓冲
     * 都是靠 buff_dma=false 才落到 PSRAM 的，封面没有理由去挤内部 RAM。
     * 分配一次就再不释放，指针因此可以交到锁外给 LVGL 慢慢画。
     */
    s_cover = heap_caps_malloc(CODEX_COVER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cover) {
        ESP_LOGE(MEDIA_TAG, "PSRAM 里分不出 %d 字节给封面，音乐控制器不显示封面", CODEX_COVER_BYTES);
    }
}

void codex_media_get(codex_media_t *out)
{
    MLOCK();
    memcpy(out, &s_media, sizeof(*out));
    MUNLOCK();
}

uint32_t codex_media_rev(void)
{
    MLOCK();
    uint32_t rev = s_media_rev;
    MUNLOCK();
    return rev;
}

void codex_media_apply(const codex_media_t *fresh)
{
    MLOCK();
    /*
     * 同 codex_state_apply 的套路，但排除项不一样：这里排除 position_ms 和
     * fetched_at_us。前者每 2 秒往前走一截，后者每次都是当下时刻，任何一个算进
     * 比较都会让 rev 每轮自增，UI 于是每 2 秒重排一次标题、封面和歌词。
     *
     * memcmp 连结构体的填充字节一起比，所以调用方传进来的 fresh 必须是 memset 过
     * 再逐字段填的，否则没清零的填充会把「内容没变」判成「变了」。
     */
    codex_media_t candidate = *fresh;
    candidate.position_ms = s_media.position_ms;
    candidate.fetched_at_us = s_media.fetched_at_us;

    bool changed = memcmp(&candidate, &s_media, sizeof(candidate)) != 0;

    candidate.position_ms = fresh->position_ms;
    candidate.fetched_at_us = esp_timer_get_time();
    s_media = candidate;
    if (changed) {
        s_media_rev++;
    }
    MUNLOCK();
}

void codex_media_set_error(bool bridge_ok, const char *reason)
{
    MLOCK();
    /*
     * 和 codex_state_set_fetch_error 一样保留上次的歌曲信息：PC 上的媒体服务重启
     * 一下，不该让屏上的标题和封面凭空消失再回来。UI 靠 available=false 决定显示
     * 提示还是显示歌名。
     */
    s_media.bridge_ok = bridge_ok;
    s_media.available = false;
    strlcpy(s_media.error, reason ? reason : "", sizeof(s_media.error));
    s_media_rev++;
    MUNLOCK();
}

int32_t codex_media_lyrics_count(void)
{
    MLOCK();
    int32_t n = s_lyric_count;
    MUNLOCK();
    return n;
}

int32_t codex_media_lyric_find(int32_t position_ms)
{
    MLOCK();
    /*
     * 找最后一句 time_ms 不大于播放位置的歌行。二分而不是线性扫：一行 132 字节，
     * 64 行线性扫要摸 8KB 的 PSRAM/.bss，二分七次就够，而这个函数每半秒调一次。
     */
    int32_t lo = 0;
    int32_t hi = s_lyric_count - 1;
    int32_t found = -1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (s_lyrics[mid].time_ms <= position_ms) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    MUNLOCK();
    return found;
}

int32_t codex_media_lyrics_window(int32_t first, codex_lyric_line_t *out, int32_t max_count)
{
    if (max_count < 0 || (max_count > 0 && !out)) {
        return 0;
    }
    MLOCK();
    if (first < 0) {
        first = 0;
    }
    int32_t n = s_lyric_count - first;
    if (n > max_count) {
        n = max_count;
    }
    if (n > 0) {
        memcpy(out, &s_lyrics[first], sizeof(codex_lyric_line_t) * (size_t)n);
    }
    MUNLOCK();
    return n;
}

void codex_media_set_lyrics(const codex_lyric_line_t *lines, int32_t count)
{
    if (count < 0 || (count > 0 && !lines)) {
        return;
    }
    if (count > CODEX_LYRIC_MAX_LINES) {
        ESP_LOGW(MEDIA_TAG, "歌词 %d 行超出上限 %d，多出来的丢弃", (int)count, CODEX_LYRIC_MAX_LINES);
        count = CODEX_LYRIC_MAX_LINES;
    }

    MLOCK();
    if (count > 0) {
        memcpy(s_lyrics, lines, sizeof(codex_lyric_line_t) * (size_t)count);
    }
    s_lyric_count = count;
    MUNLOCK();
}

bool codex_media_cover(const uint8_t **data, uint32_t *rev)
{
    MLOCK();
    bool ok = s_cover && s_cover_valid;
    if (ok) {
        *data = s_cover;
        if (rev) {
            *rev = s_cover_rev;
        }
    }
    MUNLOCK();
    return ok;
}

bool codex_media_set_cover(const uint8_t *rgb565, int32_t bytes)
{
    if (!s_cover) {
        return false;
    }
    /*
     * 尺寸不对就整张丢掉，并且要说出来：media_server.py 改了 COVER_SIZE 而面板
     * 这边没跟着改的话，收下来的字节数是对的、但每行宽度对不上，屏上会是一片
     * 斜纹 —— 那是极难往「尺寸常量不同步」上想的一种坏法。
     */
    if (!rgb565 || bytes != CODEX_COVER_BYTES) {
        ESP_LOGE(MEDIA_TAG, "封面 %d 字节，不等于期望的 %d，已丢弃", (int)bytes, CODEX_COVER_BYTES);
        return false;
    }

    MLOCK();
    memcpy(s_cover, rgb565, CODEX_COVER_BYTES);
    s_cover_valid = true;
    s_cover_rev++;
    MUNLOCK();
    return true;
}
