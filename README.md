# Codex Dashboard Panel

把 Codex 的额度、重置倒计时、任务动态和 Windows 媒体控制搬到一块 ESP32-P4 竖屏触控面板上。

本项目包含两部分：

- `panel-codex/`：面向 JC1060P470C_I_W_Y（ESP32-P4 + 1024×600 IPS）的 ESP-IDF / LVGL 固件。
- `bridge/`：运行在 Windows PC 上的本地桥接服务，读取 Codex 状态与 Windows SMTC 媒体信息，通过局域网 HTTP 提供给面板。

> 项目基于开源项目 [codex-zectrix-dashboard](https://github.com/BarryBarrywu/codex-zectrix-dashboard) 的状态读取思路扩展。上游源码不重复收录在本仓库中。

## 功能

- 展示 Codex 官方配额、窗口时长、重置倒计时与重置额度
- 展示最近的任务动态，并支持隐藏任务标题的隐私模式
- 展示第三方 Codex 重置雷达（仅供参考，不代表 OpenAI 官方承诺）
- 读取 Windows 当前播放器的歌曲、封面、歌词、进度与控制能力
- 控制播放/暂停、上一首、下一首和系统音量
- 支持以太网，以及通过板载 ESP32-C6 使用 Wi-Fi
- Bridge token 可选鉴权；媒体服务只监听本机回环地址

## 系统结构

```text
Codex app-server ─┐
Codex rollout 文件 ├─ bridge/codex-bridge.js ──局域网 HTTP──> ESP32-P4 面板
重置雷达 API ─────┘              │
                                 └─ bridge/media_server.py
                                    ├─ Windows SMTC 播放器
                                    └─ Windows 系统音量
```

## 快速开始

### 1. 启动 Windows Bridge

需要 Node.js、Python 3.12，以及本机可用的 Codex CLI。

```powershell
cd bridge
py -m venv .venv-media
.venv-media\Scripts\python.exe -m pip install -r requirements-media.txt
node codex-bridge.js
```

启动日志会显示面板应连接的局域网地址。默认监听 `8787` 端口；如需鉴权，可使用：

```powershell
node codex-bridge.js --token <你的随机令牌>
```

### 2. 构建面板固件

当前构建环境为 Windows + ESP-IDF 5.5.5。仓库路径包含空格时，请始终通过 `build.bat` 转发构建：

```powershell
cd panel-codex
build.bat build -DCODEX_BRIDGE_HOST=192.168.1.100
build.bat -p COM4 flash
```

使用 Wi-Fi 或 Bridge token 时，在构建命令中增加：

```text
-DCODEX_WIFI_SSID=你的SSID
-DCODEX_WIFI_PASS=你的密码
-DCODEX_BRIDGE_TOKEN=你的随机令牌
```

这些值只应通过命令行传入，切勿写进源码或提交到仓库。

### 3. 烧写中文字体

新面板需单独烧写一次中文字体分区：

```powershell
tools\flash-font.bat COM4
```

更完整的硬件、界面、调试和媒体桥说明请阅读 [`panel-codex/README.md`](./panel-codex/README.md)。

## 隐私与安全

- 默认情况下，任务标题会通过局域网明文 HTTP 发送给面板；敏感网络建议启用 `--privacy` 和 `--token`。
- Bridge 不读取或展示提示词、回复、推理内容和工具参数。
- Wi-Fi 密码、Bridge token、日志、构建缓存和本地虚拟环境均已从版本控制中排除。
- 第三方重置雷达只是公开信号推测，不是账户额度或官方重置通知。

## 开源协议

本项目采用 [MIT License](./LICENSE)。所包含的第三方 BSP 与组件保留各自许可证。

