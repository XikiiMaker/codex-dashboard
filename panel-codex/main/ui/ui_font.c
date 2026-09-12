#include "ui_font.h"

#include <stdio.h>

#include "lvgl.h"

#if LV_USE_TINY_TTF
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"
#endif

#ifdef PANEL_SIM
/* 模拟器没有分区，直接按路径读 tools 下的 otf */
#else
#include "esp_log.h"
#include "esp_partition.h"
#endif

/*
 * 中文字形来自完整的 NotoSansSC，运行时按需光栅化。
 *
 * 早先用 lv_font_conv 把源码里实际出现的汉字抓成子集位图编进固件，但工单里的
 * 「维修师傅」「初步诊断」是服务端下发的任意中文——子集的字表在编译期就定死了，
 * 现场换个师傅姓名就会渲染成方块。改成映射整份字体后这类缺字从原理上消失。
 *
 * 真机把 font 分区 mmap 到数据总线，字体数据始终留在 flash 里，不占 RAM；
 * 只有已光栅化的字形位图进缓存（LV_TINY_TTF_CACHE_GLYPH_CNT 条）。
 *
 * 字体没烧写时全部 getter 回退到 Montserrat：屏能亮、布局能看，但中文不可读，
 * panel_font_has_cjk() 会返回 false 让 UI 在屏上提示。
 */

#define FONT_SIZE_SMALL 16
#define FONT_SIZE_BODY  20
#define FONT_SIZE_TITLE 28
#define FONT_SIZE_DISPLAY 40

#ifndef PANEL_SIM
static const char *TAG = "panel_font";
#endif

static lv_font_t *s_small;
static lv_font_t *s_body;
static lv_font_t *s_title;
static lv_font_t *s_display;
static bool s_ready;

#if LV_USE_TINY_TTF

#ifdef PANEL_SIM

#ifndef PANEL_SIM_FONT_PATH
#define PANEL_SIM_FONT_PATH "tools/NotoSansSC-Regular.otf"
#endif

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>

/*
 * 走 _wfopen 而不是 fopen：仓库路径里有中文，而 MSVC 的 fopen 按 ANSI 代码页
 * 解析路径名，源码里 UTF-8 的字面量传进去会被当成 CP936 而直接打不开。
 */
static FILE *open_utf8(const char *path)
{
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wide_len <= 0) {
        return NULL;
    }
    wchar_t *wide = malloc((size_t)wide_len * sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, wide_len);
    FILE *f = _wfopen(wide, L"rb");
    free(wide);
    return f;
}
#else
static FILE *open_utf8(const char *path)
{
    return fopen(path, "rb");
}
#endif

static bool font_load(void)
{
    FILE *f = open_utf8(PANEL_SIM_FONT_PATH);
    if (f == NULL) {
        printf("W panel_font: 打不开 %s，中文将回退 Montserrat\n", PANEL_SIM_FONT_PATH);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        printf("W panel_font: %s 是空文件\n", PANEL_SIM_FONT_PATH);
        return false;
    }

    /* 常驻不释放：三个字号共用这一份数据，字体在整个运行期都要用 */
    void *data = malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        printf("W panel_font: 读 %s 失败\n", PANEL_SIM_FONT_PATH);
        return false;
    }
    fclose(f);

    s_small = lv_tiny_ttf_create_data(data, (size_t)size, FONT_SIZE_SMALL);
    s_body = lv_tiny_ttf_create_data(data, (size_t)size, FONT_SIZE_BODY);
    s_title = lv_tiny_ttf_create_data(data, (size_t)size, FONT_SIZE_TITLE);
    s_display = lv_tiny_ttf_create_data(data, (size_t)size, FONT_SIZE_DISPLAY);
    if (s_small == NULL || s_body == NULL || s_title == NULL || s_display == NULL) {
        printf("W panel_font: %s 不是有效的 ttf/otf\n", PANEL_SIM_FONT_PATH);
        return false;
    }
    return true;
}

#else /* 真机 */

static bool font_load(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "font");
    if (part == NULL) {
        ESP_LOGE(TAG, "分区表里没有 font 分区，中文将回退 Montserrat");
        return false;
    }

    /*
     * mmap 整个分区而不是只映射字体实际长度：分区里存的就是裸 otf，尾部空白
     * 区域 stb_truetype 不会去读（它按 sfnt 表目录寻址）。映射不用手动 munmap，
     * 字体在整个运行期都要用。
     */
    const void *data = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &data, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "font 分区 mmap 失败: %s", esp_err_to_name(err));
        return false;
    }

    /*
     * 空分区（没烧字体）全是 0xFF，stb_truetype 会在解析 sfnt 头时拒绝，
     * 但先自己挡一道能给出更清楚的日志——量产时漏烧字体是很容易犯的错。
     */
    const uint8_t *p = data;
    if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF) {
        ESP_LOGE(TAG, "font 分区是空的，未烧写字体（见 tools/flash-font.bat）");
        return false;
    }

    s_small = lv_tiny_ttf_create_data(data, part->size, FONT_SIZE_SMALL);
    s_body = lv_tiny_ttf_create_data(data, part->size, FONT_SIZE_BODY);
    s_title = lv_tiny_ttf_create_data(data, part->size, FONT_SIZE_TITLE);
    s_display = lv_tiny_ttf_create_data(data, part->size, FONT_SIZE_DISPLAY);
    if (s_small == NULL || s_body == NULL || s_title == NULL || s_display == NULL) {
        ESP_LOGE(TAG, "字体解析失败，font 分区内容可能不是有效的 ttf/otf");
        return false;
    }

    ESP_LOGI(TAG, "字体就绪：font 分区 %lu KB 已映射", (unsigned long)(part->size / 1024));
    return true;
}

#endif /* PANEL_SIM */

#else /* !LV_USE_TINY_TTF */

static bool font_load(void)
{
    return false;
}

#endif

void panel_font_init(void)
{
    if (s_ready) {
        return;
    }
    s_ready = font_load();
}

bool panel_font_has_cjk(void)
{
    return s_ready;
}

const lv_font_t *panel_font_small(void)
{
    return s_ready ? s_small : &lv_font_montserrat_16;
}

const lv_font_t *panel_font_body(void)
{
    return s_ready ? s_body : &lv_font_montserrat_20;
}

const lv_font_t *panel_font_title(void)
{
    return s_ready ? s_title : &lv_font_montserrat_28;
}

const lv_font_t *panel_font_display(void)
{
    /* 回退用 28 而不是 40：Montserrat 40 没在 lv_conf / sdkconfig 里启用，引用会链接失败 */
    return s_ready ? s_display : &lv_font_montserrat_28;
}
