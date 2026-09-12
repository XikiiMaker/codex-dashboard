#include "bsp/esp-bsp.h"
#include "codex_client.h"
#include "codex_defaults.h"
#include "codex_net.h"
#include "codex_state.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ui_dashboard.h"

#ifndef PANEL_DISPLAY_SELFTEST
#define PANEL_DISPLAY_SELFTEST 0
#endif

static const char *TAG = "codex_main";

/**
 * 启动顺序
 *
 * 先备好状态再点屏：屏一亮就能看到链路进度和「等待数据」，而不是先黑屏几秒。
 * 网络和取数都是异步的，不阻塞点屏。
 */
void app_main(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "Codex 看板固件 %s 启动", desc->version);

    codex_state_init();
    /* 封面缓冲在这里从 PSRAM 分配，必须早于取数任务起来 */
    codex_media_init();

    /* esp_wifi 要求 NVS 可用；本工程自己不存任何配置，参数都在编译期 -D 里 */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    if (CODEX_BRIDGE_HOST[0] == '\0') {
        ESP_LOGW(TAG, "未配置 bridge 地址，用 -DCODEX_BRIDGE_HOST=<PC 的局域网 IP> 重新构建");
    } else {
        ESP_LOGI(TAG, "bridge http://%s:%d", CODEX_BRIDGE_HOST, CODEX_BRIDGE_PORT);
    }

    ESP_ERROR_CHECK(codex_net_start());

    /*
     * 不能用无参的 bsp_display_start()：它在 RGB565 下硬编码 buff_dma=true，缓冲就
     * 得要内部 DMA 连续内存，而 P4 v1.x 只有 179KB 可执行 RAM，分不出来（报
     * "Not enough memory for LVGL buffer" 后崩溃重启）。buff_dma=false 落到 PSRAM。
     *
     * 竖屏只能走 sw_rotate：面板 DSI 是 1024×600 横向扫描，DPI 面板不支持硬件
     * swap_xy（BSP 原文注释 "Only SW rotation is supported for 90° and 270°"）。
     * 旋转由 CPU 做（lv_draw_sw_rotate），但一次只转 BSP_LCD_DRAW_BUFF_SIZE 那 50 行
     * 一块，不是整屏，这个看板每秒只重画几个文字标签，开销无所谓。
     * 真嫌慢就在 sdkconfig 里开 CONFIG_LVGL_PORT_ENABLE_PPA=y 换成 P4 的 PPA 加速器
     * （默认关，开了启动日志会多一句 "Setting PPA context for SW rotation"）。
     */
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            .task_stack = 16384,
            .task_affinity = -1,
            .task_max_sleep_ms = 500,
            .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
            .timer_period_ms = 5,
        },
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = false,
            .sw_rotate = CODEX_PORTRAIT ? true : false,
        },
    };
    lv_disp_t *disp = bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
#if CODEX_PORTRAIT
    /*
     * sw_rotate 只是让 lvgl_port 备好旋转通路，角度默认仍是 0，这一行不能省。
     * 之后 lv_display_get_horizontal_resolution() 才会返回 600。
     */
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
#endif
#if PANEL_DISPLAY_SELFTEST
    /*
     * 显示链路自检图案（-DPANEL_DISPLAY_SELFTEST=1 启用，正常构建不含此段）
     *
     * 八条等宽纯色竖带，静态、不依赖字体和布局。用来判别条纹的归属：纯色带内部
     * 出现条纹或渐变就是 DSI 时序 / PSRAM 带宽 / 帧缓冲层面的问题；色带边界锐利、
     * 每条颜色均匀，则出图链路是好的，屏上的异常来自 UI 自身。
     */
    static const uint32_t bands[] = {
        0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00,
        0x0000FF, 0xFFFF00, 0x00FFFF, 0x808080,
    };
    const int32_t hres = lv_display_get_horizontal_resolution(disp);
    const int32_t vres = lv_display_get_vertical_resolution(disp);
    const int32_t band_w = hres / (int32_t)(sizeof(bands) / sizeof(bands[0]));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
        lv_obj_t *band = lv_obj_create(scr);
        lv_obj_remove_style_all(band);
        lv_obj_set_size(band, band_w, vres);
        lv_obj_set_pos(band, (int32_t)i * band_w, 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(band, lv_color_hex(bands[i]), 0);
    }
    ESP_LOGW(TAG, "显示自检色带已启用，LVGL 画布 %d x %d", (int)hres, (int)vres);
#else
    (void)disp;
    ui_dashboard_create();
#endif
    bsp_display_unlock();

    ESP_ERROR_CHECK(codex_client_start());

    ESP_LOGI(TAG, "看板就绪");
}
