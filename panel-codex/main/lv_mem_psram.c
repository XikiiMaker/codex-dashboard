/*
 * LVGL 的堆后端：全部从 PSRAM 分配。
 *
 * 换掉 CLIB malloc 的理由：IDF 在 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 下会把
 * 小于 16KB 的分配一律留在内部 RAM，而 LVGL 的对象、样式、tiny_ttf 字形位图单块都远
 * 小于 16KB —— 于是 32MB PSRAM 一块也用不上，388KB 内部 RAM 却要同时喂 LVGL、TLS
 * 握手缓冲和 esp-hosted 的 SDIO 接收缓冲。进扫码页光栅化字形就把内部 RAM 抢空，SDIO
 * 拿不到 64 字节对齐的内部 DMA 块，assert 在 sdio_drv.c:928，现象是「输入时白屏重启」。
 *
 * 不能改用全局降 ALWAYSINTERNAL 来解决：那样 SDIO 自己的缓冲也会落进 PSRAM，而 SDMMC
 * 的 DMA 访问不了 PSRAM，2026-09-02 实测当场崩。只能把 LVGL 单独摘出去。
 */
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lv_mem";

void lv_mem_init(void)
{
    ESP_LOGI(TAG, "LVGL 堆指向 PSRAM，此刻内部 RAM 余 %u B / PSRAM 余 %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

/* size==0 / &zero_mem / NULL 三种边界 LVGL 已在 lv_malloc、lv_free、lv_realloc 里拦掉，
 * 传不到这里，所以下面三个直通即可。 */
void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    /* LV_USE_MEM_MONITOR 只在 BUILTIN 后端下可选，这里不会被调到 */
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif /* LV_STDLIB_CUSTOM */
