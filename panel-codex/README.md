# Codex 看板固件（ESP32-P4 触屏）

把 [codex-zectrix-dashboard](https://github.com/BarryBarrywu/codex-zectrix-dashboard) 的看板搬到
JC1060P470C_I_W_Y 面板上（ESP32-P4 + 1024×600 IPS，固件里旋转成竖屏 600×1024）：不用切到 Codex
窗口，也能看到剩余额度、重置倒计时、第三方重置雷达和最近的任务动态。竖屏下半屏改成 PC 本机
播放器的音乐控制器，额度看板和播放控制在同一块屏上。

硬件、BSP 和构建脚手架沿用同一块板子上已经跑通的 `panel-firmware`，但这是**独立工程**，
不改动那份车间生产固件。

## 为什么需要 PC 侧的 bridge

官方配额和任务动态只能从本机 `codex app-server` 的 stdio JSON-RPC 和本地 rollout 文件里拿，
没有任何可远程调用的接口，面板自己够不着。所以链路是两段：

```
codex app-server (stdio JSON-RPC)  ┐
~/.codex/sessions/**/rollout-*.jsonl ├─→ bridge/codex-bridge.js ─(局域网 HTTP JSON)─→ 面板 LVGL
https://codex-resets.com/api/v1/status ┘        (PC 常驻)        ↑ /api/media* 反向代理
                                        bridge/media_server.py ─┘ (只听 127.0.0.1)
                                        ├─ SMTC：本机播放器
                                        └─ IAudioEndpointVolume：系统音量
```

| 数据 | 观察频率 | 来源 |
| --- | ---: | --- |
| 官方配额 | 10 分钟 | `account/rateLimits/read` |
| 任务动态 | 30 秒 | `thread/list` 取标题 + rollout 里的 `task_started` / `task_complete` / `turn_aborted` |
| 重置雷达 | 1 小时 | codex-resets.com 公开 API，无鉴权 |
| 播放状态 / 封面 / 歌词 | 2 秒 | `media_server.py` 读 SMTC 会话，封面转 RGB565 裸数据 |
| 系统音量 | 2 秒 | `pycaw` 走 COM 读 `IAudioEndpointVolume`（SMTC 没有音量接口） |

时间换算全部在 bridge 里做完，下发的是相对秒数（`resets_in_seconds`、`activity_ago_seconds`），
面板只用本地单调时钟往下减 —— 因此**面板不依赖 SNTP**，内网不放行 NTP 也不影响倒计时。

雷达是第三方对公开 X 帖子的概率推测，既不是 OpenAI 的承诺，也不是本账号的重置时间。

## 跑 bridge

```sh
cd ../bridge
node codex-bridge.js                  # 默认 :8787，监听 0.0.0.0
node codex-bridge.js --once           # 只取一次并打印 JSON，用来验证链路
node codex-bridge.js --privacy        # 任务标题替换为「隐私任务」，仍保留状态与计数
node codex-bridge.js --token <值>     # 要求 X-Panel-Token，面板侧用 -DCODEX_BRIDGE_TOKEN 对应
node codex-bridge.js --no-media       # 整块关掉媒体桥，面板音乐卡显示「媒体服务未启用」
node codex-bridge.js --no-media-spawn # 不自动拉起 media_server.py，自己另开窗口跑
node codex-bridge.js --media-port 8788  # media_server.py 的端口，默认 8788
```

启动日志会列出面板该用哪个地址。首次可能需要在 Windows 防火墙上放行该端口，否则面板连不上而
PC 上用 127.0.0.1 却是通的。

默认画面包含任务标题，会经明文 HTTP 在局域网里传输。

**已经有一个 bridge 在 8787 上跑着的话，必须重启它**：`/api/media*` 这几条路由是后加的，
老进程里没有，面板只会一直收到 404。

### PC 侧媒体桥

`media_server.py` 只在 127.0.0.1 上监听，不直接对局域网开放 —— 面板一律经 bridge 的
`/api/media*` 转发，这样 `X-Panel-Token` 那一层鉴权对媒体接口同样生效，不用配两遍。
bridge 启动时会自动把它拉起来，前提是 venv 装好了：

```sh
cd ../bridge
py -m venv .venv-media
.venv-media\Scripts\python.exe -m pip install -r requirements-media.txt
```

依赖版本在 `requirements-media.txt` 里钉死并注明了理由（`winsdk` 是只有 cp312 wheel 的预发布包，
换 Python 大版本前先确认 PyPI 上有对应包）。

播放状态和封面起不来时，先跑探针分流，它能区分问题在 SMTC 侧（播放器没注册会话）、控制能力侧
（会话只给元数据不给控制）还是音量侧：

```sh
.venv-media\Scripts\python.exe tools\probe_media.py
```

探针是**只读**的：控制能力只看 `is_*_enabled` 能力位，音量写路径用「设回当前值」验证，
所以跑它不会把你正在听的音乐暂停掉。

音源是**这台 PC 上的播放器**（QQ音乐、Spotify 之类），走 Windows SMTC。手机、网页视频、
别的机器上的播放都不在范围内。能力位由播放器自己决定，面板按位启停控件：QQ音乐实测只给
播放/暂停、上一首、下一首，`seek` / `shuffle` / `repeat` 全是 false，所以进度条拖不动、
播放模式按钮是灰的 —— 这是播放器的限制，不是链路故障。

## 构建与烧写

**所有 `idf.py` 命令都要经 `build.bat` 转发，不要直接调 `idf.py`。** 仓库路径带空格，
ESP-IDF 在 Windows 上过不去（细节见 `build.bat` 顶部注释）；`build.bat` 会先把源码镜像到
`C:\espcodex`，在那里构建，再把可烧写产物拷回本目录的 `build/`。判据是 CMake 打印
`Build files have been written to: C:/espcodex`。

工具链已装在 `C:\Espressif`（ESP-IDF 5.5.5），target 已是 esp32p4。

```sh
# bridge 所在 PC 的局域网 IP 是必填项，不填则屏上只显示「未配置 bridge 地址」
build.bat build -DCODEX_BRIDGE_HOST=192.168.31.194

# 走 WiFi（板载 ESP32-C6，esp-hosted）时补上无线参数；不填就只用有线
build.bat build -DCODEX_BRIDGE_HOST=192.168.31.194 -DCODEX_WIFI_SSID=xxx -DCODEX_WIFI_PASS=yyy

build.bat -p COM4 flash
tools\read-serial.py COM4 25 boot.log
```

`-D` 传进来的值会留在 `C:\espcodex\build\CMakeCache.txt` 里，所以要关掉某项得显式传 `=0`
而不是省略它。改过 `sdkconfig.defaults` 后必须删掉 `sdkconfig`（含 `C:\espcodex\sdkconfig`），
否则静默不生效。

### 中文字体要单独烧一次

中文字形不编进固件：完整的 NotoSansSC 烧在 `font` 分区（0x620000），由 `lv_tiny_ttf` 运行时
光栅化。分区表与 `panel-firmware` 完全一致，所以**这块板子刷过那份固件的字体后不必重烧**。
新板子要烧一次：

```sh
tools\flash-font.bat COM4
```

没烧时中文全是方块，屏幕底部会显示 `CJK font missing`。

### 屏幕出问题时先分流

```sh
build.bat build -DPANEL_DISPLAY_SELFTEST=1 -DCODEX_BRIDGE_HOST=192.168.31.194
```

八条纯色竖带，不依赖字体和布局。纯色带内部出现条纹或渐变，问题在 DSI 时序 / PSRAM 带宽 /
帧缓冲；色带边界锐利、颜色均匀，则出图链路是好的，异常来自 UI 自身。

另外注意：**网络任务栈溢出的表象是屏幕定格在半张画面**，极容易误判成花屏，`codex_client.c`
的栈按 8192 给就是这个原因。

## 界面

竖屏 600×1024，自上而下五段：

| 区域 | 内容 |
| --- | --- |
| 顶栏 | 只有「CodeX 工作看板」标题（链路与 IP、更新于多久前、刷新按钮都已去掉） |
| 中区左 | 官方配额：剩余百分比大字、进度条、窗口时长、已用、重置倒计时、重置额度；账号有两档限流时下方多一行紧凑的第二窗口 |
| 中区右 | 重置雷达：概率大字、更新时间与限定语、十格信号尺（每格 10%，向上取整）、信号状态 |
| 任务卡 | 任务动态固定五行，按 执行中 → 失败 → 已中断 → 本轮完成 排序，颜色区分 |
| 音乐卡 | 播放状态与来源、192×192 封面、右侧五行文字（歌名一行、歌手 - 专辑一行、三行歌词，当前行高亮）、整行进度条、控制行 |
| 全屏音乐 | 点封面或歌名进入，点封面、歌名或左上角 ← 退出；头部左 ← 右播放器名，封面与歌名歌手专辑在顶部，中间 11 行居中歌词（无底框、28px），进度条与控制行沉底 |

中区两张卡**各占一半**（内容区 556，每卡 278）：`MID_CARD_W` 是写死的像素值而不是
`flex_grow`，因为 LVGL 的 grow 只分配「扣掉各项内容最小宽度之后的余量」，两边 grow 相同
也会因为内容长短不同而宽窄不一。两卡的行结构（头行 / 尺或条 / 底行）一一对应，所以右上的
更新时间与左卡的「7天窗口」压在同一条水平线，两张卡的底行也齐平：额度卡是「重置 6天14小时」
在左、「重置额度」在右，雷达卡反过来，信号状态居右、`数据旧` 标记在左。条与底行文字之间另有
`MID_FOOT_GAP` 6px（卡片不可滚动，这 6px 由 `MID_H` 付账）。

控制行没有边框：随机、播放模式、上一首、下一首都是裸图标（随机开启时靠图标染色表示），
只有播放/暂停是正圆外框、垂直且水平居中。水平居中不是靠 spacer 推的 —— 左右两侧按钮数量
不同，spacer 只会把圆顶到某个由内容宽决定的位置；做法是两侧各放一个 `flex_grow=1` 且都不设
`min_width` 的盒子，余量对半分，中间那个定宽的圆自然压在中线上。切歌键离那颗圆多远由控制行
自己的 `pad_column` 决定（迷你卡 20、全屏 22）—— 它只作用在 左盒↔圆 和 圆↔右盒 两处，
两边同步变化，所以调它不会破坏居中。右端是「小喇叭 + 百分比」的
音量胶囊（喇叭和左右那些键同为 24px，百分比 20px），点它才弹出触摸条，5 秒不碰自动收起
（拖动会重新计时）。

随机键和播放模式键**按能力位整块隐藏**，不是压暗禁用：QQ音乐 的 `is_shuffle_enabled` /
`is_repeat_enabled` 都是 false，命令到了 `media_server.py` 会被能力位直接挡回来，按下去永远
没反应，而模式键在 `repeat` 未知时只显示一个 `--`，谁也认不出那是什么。禁用态表达的是
「现在不行」，能力位缺失表达的是「这个播放器压根不行」，只有藏起来才对。所以现场实际只看得到
上一首、播放/暂停、下一首三个键 —— 隐藏不影响播放键居中，两个盒子的宽度只由行宽对半分决定，
且 flex 会完全跳过 hidden 子项（连列间距都不占）。换个支持这些能力的播放器，键会自己回来。

音量走的是**拖动即发**，不等松手：带值命令在 `codex_client` 里会覆盖掉还在排队的同名那条，
所以连发不会堆成一串请求，实际速率由 HTTP 往返自己限住。芯片上的百分比同时按本地值先显示，
等 2 秒一轮的状态报出同一个数字再交还 —— 不然手指刚拖到的位置会被上一轮的旧值弹回去。

进度条**能不能拖取决于播放器**：`seek` 这一位来自 SMTC 的 `is_playback_position_enabled`，
QQ音乐 实测是 false，而且 `timeline_properties` 的 `max_seek_time` / `min_seek_time` 都是 0
—— 可拖动区间被播放器自己显式声明为零；`try_change_playback_position_async` 虽然返回 True、
位置却根本不动（跳 20 秒实测只前进了正常播放的那 1 秒多）。绕过 SMTC 去驱动 QQ音乐 自己的
界面也不行：它的窗口类名是 `TXGuiFoundation`（腾讯自绘框架），用 UI Automation 扫过主窗口和
桌面歌词窗口共 129 个节点，没有任何 Slider / ProgressBar / Thumb，也没有一个支持
RangeValue 模式。剩下唯一的路是按屏幕坐标模拟鼠标去拖 QQ音乐 窗口里的进度条 —— 那要求窗口
可见且没被最小化、会抢走焦点和鼠标指针，还会随皮肤/窗口尺寸/版本漂移，所以没做。
因此能力位不允许时面板把滑杆的点击关掉、**圆形拖柄也一起设为透明**（`LV_PART_KNOB`），
让它读起来就是个纯进度指示器 —— 不然一颗拖柄摆在那儿，谁都会伸手去拖，拖了没反应又以为坏了。
但整条不压暗：进度条是这块卡上最主要的「正在放什么」指示，压暗看着像坏了一半。
音量不受此限：它走 `pycaw` 的 `IAudioEndpointVolume`，是系统音量而不是播放器的。

全屏音乐那一屏的歌词**不带底框、整体居中、28px**：整块都是歌词的时候灰框像贴了张卡片在页面上，
居中也才读得出一页歌词而不是三行附注。字号从 20px 提到 28px 之后行数从 13 降到 11 —— 歌词窗
内容区 554，tiny_ttf 的 `line_height = size × 1.448`（取自 NotoSansSC 的 ascent/descent），
28px 每行占 40，13 行只剩 2px 行距会糊成一块，11 行还能留 9~10px。行数只是取景窗口大小，
`refresh_lyrics` 拿它把当前句夹在中间，改大改小不影响定位。

头部右上角那个「QQ音乐」是**文字不是厂牌图形**：SMTC 只给得到进程名（`QQMusic.exe`），
`source_display_name` 把已知的换成常用叫法、没见过的原样显示，宁可露出真实进程名也不标成别家。
要真图形得自带图片资源，那是另一件事。

剩余额度按 30% / 10% 两档变琥珀和红。取数失败时保留上次的有效画面，不用零值冒充当前结果 ——
但**顶栏精简之后没有任何地方能说明这件事了**：原来那句「连续 N 次取数失败，画面为 X 分钟前」
挂在顶栏，随 `refresh_updated` 一起删掉了。所以画面上的数字变陈旧时不会有提示，排查时只能看
串口日志。这是「顶栏只留标题」这个要求换来的代价，不是漏掉的。

高度分账（内容区 992 = 1024 − 上下 `UI_PAD`）：顶栏 56 + 12 + 中区 166 + 12 + 任务卡 315 +
12 + 音乐卡吃掉余下约 419。任务卡**写死 315 而不是 `flex_grow`**：bridge 最多下发五条动态，
卡片撑满的话两三条动态下面会空出半屏。音乐卡才是吸收剩余高度的那个，卡内弹性的只有封面行
（192 封面 + 右侧五行文字），状态行、进度条、控制行都是定高。

## 已验证边界

- bridge 侧：配额、任务动态、雷达三条链路都在这台 Windows 上取到过真实数据；HTTP 与 ETag 条件请求可用
- 媒体侧：`media_server.py` 在这台 Windows 上实测过 SMTC 会话读取、封面转 RGB565、`pycaw` 音量读写、
  QQ音乐的能力位；bridge 的 `/api/media*` 代理与降级应答也验过
- 固件侧：2026-09-13 在 JC1060P470C 上实机烧录运行过多轮 —— 屏幕点亮、竖屏 600×1024 出图、
  中文字体分区正常、WiFi（esp-hosted 经 C6）拿到 192.168.31.196、面板到 bridge 的链路通、
  封面 / 歌词 / 音量 / 播放控制都在真机上跑通，触摸也已生效
- **触摸坐标由 LVGL 独占旋转**，`esp_lcd_touch` 那层一个字都不改（`swap_xy`/`mirror_*` 全 0，
  也不挂 `process_coordinates`）。这里踩过两遍旋转的坑，细节写在 `bsp_touch_new()` 的注释里
- **封面在 WiFi 抖动时会读超时**（72KB 裸数据碰上 5760 字节的 LWIP 接收窗口，要十来个来回），
  已按 3s / 6s / 9s 退避重试、最多四次，超时也单独放宽到 10 秒。有线会稳得多
- **有线以太网这次没实测**：代码本来就是有线优先，但板子没插网线，以太网初始化了却没拿到 IP
- **长时间运行稳定性未验证**：跑过的都是几十分钟量级，没做过通宵观测
- QQ音乐 不支持 `seek`，进度条在它上面拖不动（见「界面」一节）；`shuffle` / `repeat` 的能力位
  同样是 false，那两个键是灰的
- 不同步 Codex Desktop 的未读蓝点，没有「待你」「检查」状态，不能修改或结束任务
