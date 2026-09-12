cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 卖家 demo 里这几个 -Wno-* 是必须的：BSP 与 LVGL 在 -Werror 默认档下有大量
# format / qualifier 告警，去掉后 components/espressif__esp32_p4_function_ev_board 编不过。
add_compile_options(-Wno-format)
add_compile_options(-Wno-ignored-qualifiers)
add_compile_options(-Wno-attributes)
add_compile_options(-Wno-unused-function)

# 固件版本：只用于启动日志，本工程不注册云端也不走 OTA
set(PROJECT_VER "0.1.0")

# 竖屏（600×1024）
#
# 用 add_compile_definitions 而不是 main 组件的 -D 转发：这个宏必须对 components/
# 下的 BSP 也生效。sw_rotate 打开后 esp_lvgl_port 的 lvgl_port_disp_rotation_update
# 会直接 return，触摸坐标一点都不转，只能在 bsp_touch_new 里自己转。
add_compile_definitions(CODEX_PORTRAIT=1)

project(codex_panel)
