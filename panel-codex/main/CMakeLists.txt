# 中文字体不编进固件：完整的 NotoSansSC 单独烧到 font 分区，由 ui_font.c 经
# esp_partition_mmap 映射后交给 lv_tiny_ttf 运行时光栅化。烧写见 tools/flash-font.bat。

idf_component_register(
    SRCS
        "main.c"
        "lv_mem_psram.c"
        "codex_state.c"
        "codex_net.c"
        "codex_client.c"
        "ui/ui_dashboard.c"
        "ui/ui_music.c"
        "ui/ui_font.c"
        "ui/ui_theme.c"
    INCLUDE_DIRS "." "ui"
    # lv_mem_psram.c 里的 lv_malloc_core 等符号只被 LVGL 库反向引用，本组件自己一处
    # 都不调。静态库是按需拉取的：ld 扫到 libmain.a 时还没有这些未决引用，那个 obj
    # 就不会入链，于是编译全过、链接期却报一串 undefined reference to `lv_malloc_core`。
    # WHOLE_ARCHIVE 让 libmain.a 整体入链。--gc-sections 仍在，不会因此变胖。
    WHOLE_ARCHIVE
    REQUIRES
        nvs_flash
        esp_eth
        esp_netif
        esp_wifi
        esp_http_client
        esp_app_format
        json
        driver
        esp_timer
        lwip
        spi_flash
        esp32_p4_function_ev_board
)

# idf.py 的 -D 只定义 CMake 变量，不会变成 C 宏。想让 -DPANEL_DISPLAY_SELFTEST=1 /
# -DCODEX_BRIDGE_PORT=8787 真的生效，必须在这里显式转发一次，否则命令行看起来
# 生效了、代码里却一直是头文件里的默认值。注意 CMakeCache 会保留上次的值，
# 要关掉某项得显式传 =0 而不是省略它。
foreach(_opt PANEL_DISPLAY_SELFTEST CODEX_BRIDGE_PORT CODEX_POLL_INTERVAL_MS
             CODEX_MEDIA_POLL_INTERVAL_MS)
    if(DEFINED ${_opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_opt}=${${_opt}})
    endif()
endforeach()

# 字符串型的要单独来一遍：上面那种裸展开会变成 C 里的标识符而编译不过，必须补引号。
# WiFi 密码和 token 走命令行而不是写进 codex_defaults.h，免得提交进仓库：
#   build.bat build -DCODEX_BRIDGE_HOST=192.168.31.194 -DCODEX_WIFI_SSID=xxx -DCODEX_WIFI_PASS=yyy
foreach(_opt CODEX_BRIDGE_HOST CODEX_BRIDGE_TOKEN CODEX_WIFI_SSID CODEX_WIFI_PASS)
    if(DEFINED ${_opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_opt}="${${_opt}}")
    endif()
endforeach()
