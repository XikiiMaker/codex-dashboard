"""
Windows 媒体桥（只监听本机）

面板够不着 PC 上的播放器：Windows 只通过 SMTC（系统媒体传输控件）暴露「正在播什么」
和「反向控制播放」，而那必须在 PC 本机调。这个服务就是那层适配，只监听 127.0.0.1，
由 codex-bridge.js 代理给面板 —— 面板仍然只认 bridge 一个地址、一套 token。

    .venv-media\\Scripts\\python.exe media_server.py               # 默认 127.0.0.1:8788
    .venv-media\\Scripts\\python.exe media_server.py --port 9000
    .venv-media\\Scripts\\python.exe media_server.py --no-cover    # 排查封面路径时用
    .venv-media\\Scripts\\python.exe media_server.py --no-lyrics   # 排查歌词路径时用

为什么是 Python 而不是让 bridge 直接调：SMTC 是 WinRT 异步接口，Node 侧没有能用的
绑定（@nodert 系列对 Node 24 的预编译基本是坏的），而 winsdk 有 cp312 的 wheel。

端点
    GET  /media     歌曲信息 + 进度 + 播放状态 + 能力位 + 系统音量
    GET  /cover     封面裸 RGB565（不是 JPEG，见下）+ ETag
    GET  /lyrics    按标题+歌手在线查到的同步歌词
    POST /control   {"action": "play_pause" | "next" | ... }
    GET  /health    存活探测，bridge 用它判断该不该报 media_available

封面为什么下发 RGB565 裸数据而不是 JPEG：面板那份 LVGL 9.5.0 的 sdkconfig 里
TJPGD / libjpeg / lodepng 一个都没开，收到 JPEG 也解不出来。为了这个去改
sdkconfig 不划算 —— README 里写了改完必须删 sdkconfig 否则静默不生效，是个坑。
PC 侧转好、面板零解码零配置改动，192×192 也才 72KB，切歌时传一次而已。

实测边界（QQ音乐，tools/probe_media.py 可复现）：play_pause / next / prev 可用，
seek、shuffle、repeat 播放器根本没暴露。所以能力位由 /media 下发，面板据此把进度条
做成只读、把那两个开关隐藏 —— 换个播放器能力不同也能自适应，不必重烧固件。
"""

import argparse
import asyncio
import datetime
import hashlib
import io
import json
import logging
import re
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    # Windows 控制台默认 GBK，歌名是中文，日志里全是乱码就没法排查了
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

import requests

log = logging.getLogger("media")

HOST_DEFAULT = "127.0.0.1"
PORT_DEFAULT = 8788

POLL_INTERVAL_S = 1.0
SMTC_TIMEOUT_S = 8.0

# 必须和面板 codex_state.h 的 CODEX_COVER_SIZE 一致：面板按边长校验字节数，
# 对不上就整张丢掉，表现为「有歌名没封面」，串口里只有一行 error。
COVER_SIZE = 192
LYRIC_TIMEOUT_S = 6.0
LYRIC_TTL_S = 6 * 3600

# 面板一次要收完，body 上限只是防手滑发个巨大请求把内存吃掉
MAX_BODY_BYTES = 4096
MAX_HEADER_LINES = 64

TIMESPAN_TICKS_PER_MS = 10_000

# SMTC 的 playback_status 枚举值。winsdk 给的是 PLAYING=4 / PAUSED=5，
# 跟网上流传的 3/4 对不上 —— 按名字取，别按数字猜。
STATUS_NAMES = {0: "closed", 1: "opened", 2: "changing", 3: "stopped",
                4: "playing", 5: "paused"}


def to_ms(value) -> int:
    """
    timeline 的时长/位置转毫秒

    winsdk 1.0.0b10 给的是 Python datetime.timedelta，更早的版本给 WinRT TimeSpan
    （100ns 刻度，读 .duration）。只认后者的话 getattr 会静默拿到 None，时长和位置
    全变成 0，看起来像「这首歌没有时长」而不是报个错 —— 探针就是这么被骗过一次的。
    """
    if value is None:
        return 0
    if isinstance(value, datetime.timedelta):
        return int(value.total_seconds() * 1000)
    duration = getattr(value, "duration", None)
    return int(duration / TIMESPAN_TICKS_PER_MS) if duration else 0


def read_vector(view) -> list:
    """WinRT IVectorView → list。不是 Python 序列，list() 拿不到内容。"""
    if view is None:
        return []
    try:
        return [view.get_at(i) for i in range(view.size)]
    except Exception:
        return []


def parse_lrc(text: str) -> list:
    """
    LRC → [{"time_ms":…, "text":…}]

    同一行可能挂多个时间标签（副歌重复），全部展开。纯音乐段常是空文本行，
    丢掉 —— 面板上滚过一片空白比不显示歌词更难看。
    """
    if not text:
        return []
    out = []
    for raw in text.splitlines():
        raw = raw.strip()
        if not raw:
            continue
        stamps = re.findall(r"\[(\d{1,3}):(\d{1,2}(?:[.:]\d{1,3})?)\]", raw)
        if not stamps:
            continue
        lyric = re.sub(r"\[\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?\]", "", raw).strip()
        if not lyric:
            continue
        for minutes, seconds in stamps:
            sec = float(seconds.replace(":", "."))
            out.append({"time_ms": int((int(minutes) * 60 + sec) * 1000), "text": lyric})
    out.sort(key=lambda item: item["time_ms"])
    return out


# ---------------------------------------------------------------- 歌曲快照

class Song:
    """一轮 SMTC 读取的结果。key 用来判断是不是换歌了。"""

    __slots__ = ("title", "artist", "album", "duration_ms", "position_ms",
                 "state", "shuffle", "repeat", "source", "caps", "has_thumbnail")

    def __init__(self, **kw):
        for name in self.__slots__:
            setattr(self, name, kw.get(name))

    @property
    def key(self) -> str:
        return f"{self.source}|{self.title}|{self.artist}|{self.album}|{self.duration_ms}"

    def to_payload(self, position_ms: int, volume) -> dict:
        return {
            "available": True,
            "source": self.source or "",
            "title": self.title or "",
            "artist": self.artist or "",
            "album": self.album or "",
            "duration_ms": self.duration_ms,
            "position_ms": position_ms,
            "state": self.state,
            "shuffle": self.shuffle,
            "repeat": self.repeat,
            "caps": dict(self.caps),
            "has_cover": bool(self.has_thumbnail),
            "volume": volume,
        }


# ---------------------------------------------------------------- SMTC 轮询

class MediaWatcher:
    """
    每秒读一次 SMTC，把最新状态缓存在内存里

    HTTP 请求不直接去碰 SMTC：那样每个请求都要走一遍 WinRT 异步调用，慢而且
    并发起来容易撞上 COM 的单线程套间。这里读一次缓存一份，位置用 monotonic
    往前外推，请求路径上全是纯内存操作。
    """

    def __init__(self):
        self.manager = None
        self.song = None
        self.caps = self._empty_caps()
        self.read_at = time.monotonic()
        self.thumbnail = None
        self.last_key = None

    @staticmethod
    def _empty_caps() -> dict:
        # 全部 False 而不是缺字段：面板按能力位决定控件显隐，字段缺失和「不可用」
        # 必须是同一件事，否则换个播放器就画出按不动的按钮。
        return {"play_pause": False, "next": False, "prev": False,
                "seek": False, "shuffle": False, "repeat": False}

    async def ensure_manager(self):
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as Manager,
        )
        if self.manager is None:
            self.manager = await asyncio.wait_for(Manager.request_async(),
                                                 timeout=SMTC_TIMEOUT_S)
        return self.manager

    async def poll_once(self):
        manager = await self.ensure_manager()
        session = manager.get_current_session()
        if session is None:
            # 播放器关了 / 没在放。保留上次的歌名没有意义，明确报不可用，
            # 面板显示「没有正在播放的媒体」，而不是拿旧数据冒充当前状态。
            if self.song is not None:
                log.info("SMTC 会话消失，媒体不可用")
            self.song = None
            self.thumbnail = None
            self.last_key = None
            self.caps = self._empty_caps()
            return

        media = await session.try_get_media_properties_async()
        timeline = session.get_timeline_properties()
        playback = session.get_playback_info()

        # 能力位在 playback_info.controls 上，不在 playback_info 顶层；播放/暂停那位
        # 的真名是 is_play_pause_toggle_enabled。层级或名字读错，getattr 会静默给出
        # False，把可控的播放器报成不可控。用 getattr 带默认值是因为不同播放器
        # 暴露的字段确实有差异，缺字段就该当不可用。
        controls = getattr(playback, "controls", None)
        caps = self._empty_caps()
        if controls is not None:
            caps["play_pause"] = bool(getattr(controls, "is_play_pause_toggle_enabled", False))
            caps["next"] = bool(getattr(controls, "is_next_enabled", False))
            caps["prev"] = bool(getattr(controls, "is_previous_enabled", False))
            caps["seek"] = bool(getattr(controls, "is_playback_position_enabled", False))
            caps["shuffle"] = bool(getattr(controls, "is_shuffle_enabled", False))
            caps["repeat"] = bool(getattr(controls, "is_repeat_enabled", False))

        status = getattr(playback, "playback_status", None)
        state = STATUS_NAMES.get(int(getattr(status, "value", -1)), "unknown")

        repeat_raw = getattr(playback, "auto_repeat_mode", None)
        repeat = str(repeat_raw).split(".")[-1].lower() if repeat_raw is not None else "unknown"
        shuffle = getattr(playback, "is_shuffle_active", None)

        song = Song(
            title=str(media.title or ""),
            artist=str(media.artist or ""),
            album=str(media.album_title or ""),
            duration_ms=to_ms(timeline.end_time),
            position_ms=to_ms(timeline.position),
            state=state,
            shuffle=bool(shuffle) if shuffle is not None else None,
            repeat=repeat,
            source=str(getattr(session, "source_app_user_model_id", "") or ""),
            caps=caps,
            has_thumbnail=media.thumbnail is not None,
        )
        self.caps = caps
        self.read_at = time.monotonic()

        if song.key != self.last_key:
            log.info("切歌: %s - %s (%s, %d ms)", song.artist, song.title, state,
                     song.duration_ms)
            self.last_key = song.key
            # 换歌了旧封面必须丢，否则面板会先看到上一首的图
            self.thumbnail = None
        self.song = song

    def current_position_ms(self) -> int:
        """
        从上次读取的时刻往前外推

        1 秒轮询一次的话进度条会一跳一跳，面板又是 20 秒才拉一次 dashboard，
        所以两边都靠外推补：这里按 monotonic 补到当前，面板再按自己的时钟补到
        渲染那一刻。暂停/停止时不外推，否则暂停的曲子进度还在走。
        """
        song = self.song
        if song is None or song.state != "playing":
            return song.position_ms if song else 0
        elapsed = int((time.monotonic() - self.read_at) * 1000)
        position = song.position_ms + elapsed
        if song.duration_ms > 0:
            position = min(position, song.duration_ms)
        return max(position, 0)

    async def poll_loop(self):
        while True:
            try:
                await self.poll_once()
            except asyncio.TimeoutError:
                log.warning("SMTC 读取超时（%.1fs）", SMTC_TIMEOUT_S)
            except Exception as exc:
                # WinRT 调用会因为播放器退出、会话切换抛各种奇怪的错。
                # 一次失败不该让整个服务死掉，manager 置空下轮重建。
                log.warning("SMTC 轮询失败: %r", exc)
                self.manager = None
                self.song = None
            await asyncio.sleep(POLL_INTERVAL_S)

    async def session(self):
        manager = await self.ensure_manager()
        return manager.get_current_session()


# ---------------------------------------------------------------- 封面

def to_rgb565(img) -> bytes:
    """
    PIL Image → RGB565 小端裸数据，LVGL 9 的 LV_COLOR_FORMAT_RGB565 就是这个布局

    低字节 RRRRRGGG、高字节 GGGBBBBB。192×192 是 36864 像素，纯 Python 循环几十毫秒，
    只在切歌时跑一次，犯不上为此拉一个 numpy 依赖进来。
    """
    raw = img.convert("RGB").tobytes()
    out = bytearray(len(raw) // 3 * 2)
    j = 0
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[j] = v & 0xFF
        out[j + 1] = (v >> 8) & 0xFF
        j += 2
    return bytes(out)


class CoverCache:
    """SMTC 缩略图 → 缩放后的 RGB565，按歌曲 key 缓存一份"""

    def __init__(self, watcher: MediaWatcher, enabled: bool = True):
        self.watcher = watcher
        self.enabled = enabled
        self.key = None
        self.rgb = None
        self.etag = None

    async def get(self):
        song = self.watcher.song
        if not self.enabled or song is None or not song.has_thumbnail:
            return None
        if self.key == song.key and self.rgb is not None:
            return self.rgb

        session = await self.watcher.session()
        if session is None:
            return None
        media = await session.try_get_media_properties_async()
        if media.thumbnail is None:
            return None

        stream = await media.thumbnail.open_read_async()
        size = int(stream.size)
        if size <= 0 or size > 4 * 1024 * 1024:
            log.warning("缩略图尺寸异常: %d", size)
            return None

        from winsdk.windows.storage.streams import DataReader
        reader = DataReader(stream)
        loaded = await reader.load_async(size)
        blob = bytearray(loaded)
        reader.read_bytes(blob)

        rgb = await asyncio.get_running_loop().run_in_executor(
            None, self._convert, bytes(blob))
        if rgb is None:
            return None
        self.key = song.key
        self.rgb = rgb
        # ETag 必须是纯 ASCII：song.key 里是中文歌名和歌手，直接塞进响应头会在
        # 编码时抛 UnicodeEncodeError（实测炸过一次）。取摘要既绕开编码问题，
        # 也比一长串歌名短，内容一变摘要就变，语义是一样的。
        digest = hashlib.sha1(f"{song.key}|{len(rgb)}".encode("utf-8")).hexdigest()
        self.etag = digest[:32]
        log.info("封面就绪 %d×%d, %d 字节, etag %s", COVER_SIZE, COVER_SIZE,
                 len(rgb), self.etag[:12])
        return rgb

    @staticmethod
    def _convert(blob: bytes):
        """解码和缩放是 CPU 活，扔到线程池，别把 asyncio loop 卡住"""
        try:
            from PIL import Image
            with Image.open(io.BytesIO(blob)) as img:
                img = img.convert("RGB")
                # 缩略图长宽比不一定是 1:1，居中裁成正方形再缩放，
                # 直接 resize 会把人脸拉宽
                w, h = img.size
                side = min(w, h)
                left, top = (w - side) // 2, (h - side) // 2
                img = img.crop((left, top, left + side, top + side))
                img = img.resize((COVER_SIZE, COVER_SIZE), Image.LANCZOS)
                return to_rgb565(img)
        except Exception as exc:
            log.warning("封面转换失败: %r", exc)
            return None


# ---------------------------------------------------------------- 歌词

class LyricFetcher:
    """
    按标题+歌手在线查同步歌词，两级：网易云 → lrclib

    desk-beam 是先拿 QQ 音乐的精确 track_id，但那要求第三方 BetterLyrics 插件往
    SMTC 的 genres 里注入 ID。实测 QQ音乐的 genres 是空的，这条路不通，只能按
    标题+歌手搜 —— 所以同名歌曲有可能匹配到错的版本，面板上得让人看得出来这是
    搜来的而不是曲库自带的。
    """

    def __init__(self, enabled: bool = True):
        self.enabled = enabled
        self.cache = {}

    async def get(self, title: str, artist: str, duration_ms: int):
        if not self.enabled or not title:
            return None
        key = (title.lower().strip(), artist.lower().strip())
        hit = self.cache.get(key)
        if hit and time.monotonic() - hit[0] < LYRIC_TTL_S:
            return hit[1]

        loop = asyncio.get_running_loop()
        for name, fn in (("netease", self._from_netease), ("lrclib", self._from_lrclib)):
            try:
                lines = await asyncio.wait_for(
                    loop.run_in_executor(None, fn, title, artist, duration_ms),
                    timeout=LYRIC_TIMEOUT_S)
            except asyncio.TimeoutError:
                log.warning("歌词 %s 超时", name)
                lines = None
            except Exception as exc:
                log.warning("歌词 %s 失败: %r", name, exc)
                lines = None
            if lines:
                result = {"source": name, "lines": lines}
                self.cache[key] = (time.monotonic(), result)
                log.info("歌词命中 %s: %d 行", name, len(lines))
                return result
        log.info("歌词没查到: %s - %s", artist, title)
        return None

    @staticmethod
    def _from_netease(title, artist, duration_ms):
        headers = {"Referer": "https://music.163.com/",
                   "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
        query = f"{title} {artist}".strip()
        resp = requests.post("https://music.163.com/api/search/get/web",
                             data={"s": query, "type": 1, "offset": 0, "limit": 5},
                             headers=headers, timeout=LYRIC_TIMEOUT_S)
        resp.raise_for_status()
        songs = ((resp.json().get("result") or {}).get("songs")) or []
        if not songs:
            return None
        # 有 duration 就挑时长最接近的那首，能挡掉大部分同名翻唱和 live 版
        pick = songs[0]
        if duration_ms > 0:
            pick = min(songs, key=lambda s: abs(int(s.get("duration") or 0) - duration_ms))
        resp = requests.get("https://music.163.com/api/song/lyric",
                            params={"id": pick["id"], "lv": -1, "kv": -1, "tv": -1},
                            headers=headers, timeout=LYRIC_TIMEOUT_S)
        resp.raise_for_status()
        text = ((resp.json().get("lrc") or {}).get("lyric")) or ""
        return parse_lrc(text) or None

    @staticmethod
    def _from_lrclib(title, artist, duration_ms):
        params = {"track_name": title, "artist_name": artist}
        if duration_ms > 0:
            params["duration"] = int(round(duration_ms / 1000))
        resp = requests.get("https://lrclib.net/api/get", params=params,
                            headers={"User-Agent": "codex-panel/1.0"},
                            timeout=LYRIC_TIMEOUT_S)
        if resp.status_code == 404:
            return None
        resp.raise_for_status()
        return parse_lrc(resp.json().get("syncedLyrics") or "") or None


# ---------------------------------------------------------------- 音量

class VolumeControl:
    """
    系统主音量

    SMTC 压根没有音量接口（desk-beam 因此把音量键挪去调 LED 亮度了），要读写音量
    只能走 COM 的 IAudioEndpointVolume。注意这是 Windows 主音量、不是 QQ音乐自己的
    音量条，面板上的滑块得让人知道这一点。

    pycaw 新版 GetSpeakers() 返回 AudioDevice 包装对象、直接给 EndpointVolume；
    老教程那套 device.Activate(IAudioEndpointVolume._iid_,…) 在它上面不存在，会抛
    AttributeError。两条路都留着，免得 pycaw 版本一变就废。
    """

    def __init__(self):
        self.volume = None
        self.broken = None
        self.last = {"available": False, "scalar": 0.0, "percent": 0, "muted": False}

    def _acquire(self):
        if self.volume is not None or self.broken:
            return self.volume
        try:
            from pycaw.utils import AudioUtilities
            device = AudioUtilities.GetSpeakers()
            endpoint = getattr(device, "EndpointVolume", None)
            if endpoint is None:
                from ctypes import POINTER, cast
                from comtypes import CLSCTX_ALL
                from pycaw.pycaw import IAudioEndpointVolume
                endpoint = cast(device.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None),
                                POINTER(IAudioEndpointVolume))
            self.volume = endpoint
            log.info("音量设备: %s", getattr(device, "FriendlyName", "?"))
        except Exception as exc:
            self.broken = repr(exc)
            log.warning("音量不可用: %s", self.broken)
        return self.volume

    def read(self) -> dict:
        """COM 调用是阻塞的，一律经线程池，别卡住 asyncio loop"""
        volume = self._acquire()
        if volume is None:
            return self.last
        try:
            scalar = float(volume.GetMasterVolumeLevelScalar())
            muted = bool(volume.GetMute())
            self.last = {"available": True, "scalar": round(scalar, 3),
                         "percent": int(round(scalar * 100)), "muted": muted}
        except Exception as exc:
            log.warning("读音量失败: %r", exc)
            self.last = dict(self.last, available=False)
        return self.last

    def set_percent(self, percent: int) -> dict:
        volume = self._acquire()
        if volume is None:
            return self.last
        percent = max(0, min(100, int(percent)))
        try:
            volume.SetMasterVolumeLevelScalar(percent / 100.0, None)
            if percent > 0:
                # 拖到非零音量时顺手解除静音，否则滑块动了却没声音，像是坏了
                volume.SetMute(0, None)
        except Exception as exc:
            log.warning("设音量失败: %r", exc)
        return self.read()

    def toggle_mute(self) -> dict:
        volume = self._acquire()
        if volume is None:
            return self.last
        try:
            volume.SetMute(0 if volume.GetMute() else 1, None)
        except Exception as exc:
            log.warning("切换静音失败: %r", exc)
        return self.read()

    async def aread(self):
        return await asyncio.get_running_loop().run_in_executor(None, self.read)

    async def aset_percent(self, percent):
        return await asyncio.get_running_loop().run_in_executor(None, self.set_percent, percent)

    async def atoggle_mute(self):
        return await asyncio.get_running_loop().run_in_executor(None, self.toggle_mute)


# ---------------------------------------------------------------- 控制命令

class Controller:
    def __init__(self, watcher: MediaWatcher, volume: VolumeControl):
        self.watcher = watcher
        self.volume = volume

    async def handle(self, action: str, params: dict) -> dict:
        if action in ("set_volume",):
            return {"ok": True, "action": action,
                    "volume": await self.volume.aset_percent(int(params.get("percent", 0)))}
        if action == "toggle_mute":
            return {"ok": True, "action": action, "volume": await self.volume.atoggle_mute()}

        session = await self.watcher.session()
        if session is None:
            return {"ok": False, "action": action, "error": "没有正在播放的媒体会话"}

        caps = self.watcher.caps
        if not caps.get(action, False) and action in ("play_pause", "next", "prev",
                                                      "seek", "shuffle", "repeat"):
            # 能力位说不可用就别发：SMTC 的 try_* 返回 False 而不是抛错，
            # 面板只会看到「按了没反应」，不如直接告诉它这个播放器不支持。
            return {"ok": False, "action": action, "error": "当前播放器不支持此操作"}

        calls = {
            "play_pause": session.try_toggle_play_pause_async,
            "next": session.try_skip_next_async,
            "prev": session.try_skip_previous_async,
            "previous": session.try_skip_previous_async,
        }
        try:
            if action in calls:
                accepted = bool(await calls[action]())
            elif action == "seek":
                position = int(params.get("position_ms", 0))
                # SMTC 的位置参数是 100ns 刻度，不是毫秒
                accepted = bool(await session.try_change_playback_position_async(
                    position * TIMESPAN_TICKS_PER_MS))
            elif action == "shuffle":
                current = self.watcher.song.shuffle if self.watcher.song else False
                accepted = bool(await session.try_change_shuffle_active_async(not current))
            elif action == "repeat":
                accepted = bool(await self._cycle_repeat(session))
            else:
                return {"ok": False, "action": action, "error": f"未知动作 {action}"}
        except Exception as exc:
            log.warning("命令 %s 执行失败: %r", action, exc)
            return {"ok": False, "action": action, "error": repr(exc)}

        if not accepted:
            log.info("命令 %s 被播放器拒绝（try_* 返回 False）", action)
        # 命令发完立刻重读一次，让面板的下一次拉取就能拿到新状态，
        # 不必等下一个 1 秒轮询周期
        try:
            await self.watcher.poll_once()
        except Exception:
            pass
        return {"ok": bool(accepted), "action": action,
                "position_ms": self.watcher.current_position_ms()}

    @staticmethod
    async def _cycle_repeat(session):
        from winsdk.windows.media import MediaPlaybackAutoRepeatMode as Mode
        playback = session.get_playback_info()
        current = getattr(playback, "auto_repeat_mode", None)
        current = int(getattr(current, "value", 0)) if current is not None else 0
        # 枚举: 0=None 1=List 2=Track，循环切换
        return bool(await session.try_change_auto_repeat_mode_async(Mode((current + 1) % 3)))


# ---------------------------------------------------------------- HTTP

STATUS_PHRASE = {200: "OK", 304: "Not Modified", 400: "Bad Request",
                 404: "Not Found", 413: "Payload Too Large", 422: "Unprocessable Entity",
                 500: "Internal Server Error"}


def status_line(status: int) -> str:
    return f"HTTP/1.1 {status} {STATUS_PHRASE.get(status, 'Error')}"


def json_response(payload: dict, status: int = 200, etag: str = None) -> bytes:
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    head = [status_line(status),
            "Content-Type: application/json; charset=utf-8",
            f"Content-Length: {len(body)}",
            "Cache-Control: no-store",
            "Connection: close"]
    if etag:
        head.append(f'ETag: "{etag}"')
    # 头部用 ISO-8859-1 而不是 ASCII：HTTP 规范里字段值就是 Latin-1，而且万一有谁
    # 往头里塞了中文，也不该让整个请求变成 500。
    return ("\r\n".join(head) + "\r\n\r\n").encode("iso-8859-1") + body


def binary_response(body: bytes, etag: str, extra: dict = None) -> bytes:
    head = [status_line(200),
            "Content-Type: application/octet-stream",
            f"Content-Length: {len(body)}",
            "Cache-Control: no-store",
            "Connection: close",
            f'ETag: "{etag}"']
    for key, value in (extra or {}).items():
        head.append(f"{key}: {value}")
    return ("\r\n".join(head) + "\r\n\r\n").encode("iso-8859-1") + body


class HttpServer:
    """
    手写极简 HTTP/1.1

    不引 aiohttp/fastapi：这个服务只有五个端点、唯一客户端是本机的 bridge，
    而 SMTC 是 asyncio 接口，用标准库 http.server 反而要额外开一个后台 loop 线程
    来回投递协程。Connection: close，不做 keep-alive。
    """

    def __init__(self, watcher, cover, lyrics, controller, volume):
        self.watcher = watcher
        self.cover = cover
        self.lyrics = lyrics
        self.controller = controller
        self.volume = volume

    async def start(self, host: str, port: int):
        server = await asyncio.start_server(self._handle, host, port)
        log.info("媒体桥就绪 http://%s:%d  (GET /media /cover /lyrics /health, POST /control)",
                 host, port)
        return server

    async def _handle(self, reader, writer):
        try:
            await self._serve(reader, writer)
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        except Exception as exc:
            log.warning("请求处理异常: %r", exc)
            try:
                writer.write(json_response({"ok": False, "error": repr(exc)}, 500))
                await writer.drain()
            except Exception:
                pass
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    async def _serve(self, reader, writer):
        try:
            request_line = await asyncio.wait_for(reader.readline(), timeout=10.0)
        except asyncio.TimeoutError:
            return
        if not request_line:
            return
        parts = request_line.decode("iso-8859-1").split()
        if len(parts) < 2:
            return
        method, target = parts[0].upper(), parts[1]

        headers = {}
        for _ in range(MAX_HEADER_LINES):
            line = await reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            decoded = line.decode("iso-8859-1").rstrip("\r\n")
            if ":" in decoded:
                key, value = decoded.split(":", 1)
                headers[key.strip().lower()] = value.strip()

        body = b""
        length = int(headers.get("content-length") or 0)
        if length > 0:
            if length > MAX_BODY_BYTES:
                writer.write(json_response({"ok": False, "error": "body 过大"}, 413))
                await writer.drain()
                return
            body = await reader.readexactly(length)

        path = target.split("?", 1)[0]
        query = dict(re.findall(r"([^&=?]+)=([^&]*)", target.split("?", 1)[1])) if "?" in target else {}

        if method == "GET" and path == "/health":
            song = self.watcher.song
            writer.write(json_response({
                "ok": True,
                "media_available": song is not None,
                "title": song.title if song else "",
                "cover": self.cover.enabled,
                "lyrics": self.lyrics.enabled,
            }))
        elif method == "GET" and path == "/media":
            writer.write(json_response(await self._media_payload()))
        elif method == "GET" and path == "/cover":
            writer.write(await self._cover_response(headers))
        elif method == "GET" and path == "/lyrics":
            writer.write(await self._lyrics_response())
        elif method == "POST" and path == "/control":
            writer.write(await self._control_response(body))
        else:
            writer.write(json_response({"ok": False, "error": "not found"}, 404))
        await writer.drain()

    async def _media_payload(self) -> dict:
        song = self.watcher.song
        volume = await self.volume.aread()
        if song is None:
            # 明确报不可用 + 带上能力位，面板据此显示「没有正在播放的媒体」
            # 并把按钮全部禁用，而不是拿上一首歌冒充当前状态。
            return {"available": False, "caps": dict(self.watcher.caps), "volume": volume}
        return song.to_payload(self.watcher.current_position_ms(), volume)

    async def _cover_response(self, headers) -> bytes:
        if self.watcher.song is None:
            return json_response({"ok": False, "error": "没有正在播放的媒体"}, 404)
        rgb = await self.cover.get()
        if rgb is None or not self.cover.etag:
            return json_response({"ok": False, "error": "这首歌没有封面或转换失败"}, 404)
        if headers.get("if-none-match") == f'"{self.cover.etag}"':
            return (status_line(304) +
                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").encode("iso-8859-1")
        return binary_response(rgb, self.cover.etag, {
            "X-Cover-Width": str(COVER_SIZE),
            "X-Cover-Height": str(COVER_SIZE),
            "X-Cover-Format": "rgb565le",
        })

    async def _lyrics_response(self) -> bytes:
        song = self.watcher.song
        if song is None:
            return json_response({"ok": False, "available": False,
                                  "error": "没有正在播放的媒体"}, 404)
        result = await self.lyrics.get(song.title, song.artist, song.duration_ms)
        if result is None:
            return json_response({"ok": True, "available": False, "source": "",
                                  "lines": [], "title": song.title, "artist": song.artist})
        return json_response({"ok": True, "available": True, "source": result["source"],
                              "lines": result["lines"], "title": song.title,
                              "artist": song.artist})

    async def _control_response(self, body: bytes) -> bytes:
        try:
            payload = json.loads(body.decode("utf-8")) if body else {}
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            return json_response({"ok": False, "error": f"JSON 解析失败: {exc}"}, 400)
        action = str(payload.get("action") or "").strip()
        if not action:
            return json_response({"ok": False, "error": "缺少 action"}, 400)
        result = await self.controller.handle(action, payload)
        return json_response(result, 200 if result.get("ok") else 422)


# ---------------------------------------------------------------- 入口

async def run(host: str, port: int, with_cover: bool, with_lyrics: bool):
    watcher = MediaWatcher()
    volume = VolumeControl()
    cover = CoverCache(watcher, with_cover)
    lyrics = LyricFetcher(with_lyrics)
    controller = Controller(watcher, volume)
    server = HttpServer(watcher, cover, lyrics, controller, volume)

    try:
        await watcher.ensure_manager()
        log.info("SMTC SessionManager 就绪")
    except Exception as exc:
        log.warning("SMTC 初始化失败（服务照常起，/media 会报不可用）: %r", exc)

    await watcher.poll_once()
    await volume.aread()

    http_server = await server.start(host, port)
    poll_task = asyncio.create_task(watcher.poll_loop())
    try:
        async with http_server:
            await http_server.serve_forever()
    finally:
        poll_task.cancel()


def main() -> int:
    parser = argparse.ArgumentParser(description="Windows 媒体桥（SMTC + 系统音量）")
    parser.add_argument("--host", default=HOST_DEFAULT,
                        help=f"监听地址，默认 {HOST_DEFAULT}。面板不直连这里，别改成 0.0.0.0")
    parser.add_argument("--port", type=int, default=PORT_DEFAULT)
    parser.add_argument("--no-cover", action="store_true", help="不下发封面，排查封面路径时用")
    parser.add_argument("--no-lyrics", action="store_true", help="不查在线歌词，排查歌词路径时用")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S",
        stream=sys.stdout)

    try:
        asyncio.run(run(args.host, args.port, not args.no_cover, not args.no_lyrics))
    except KeyboardInterrupt:
        log.info("收到中断，退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
