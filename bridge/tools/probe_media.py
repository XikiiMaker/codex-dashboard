"""
媒体链路探针

media_server.py 起不来、或面板显示「无媒体会话」时先跑这个分流，能区分出问题在
SMTC 侧（播放器没注册会话）、控制能力侧（会话只暴露元数据不给控制）还是音量侧。

    .venv-media\\Scripts\\python.exe tools\\probe_media.py

只读，不会改动播放状态：控制能力只看 is_*_enabled 能力位，音量写路径用「设回当前
值」验证，所以跑这个脚本时音乐不会被暂停。
"""

import asyncio
import datetime
import sys

# Windows 控制台默认 GBK，中文诊断信息会输出成乱码，探针自己先变得不可读就失去意义了
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TIMESPAN_TICKS_PER_MS = 10_000


def ms(value) -> int:
    """
    时长/位置转毫秒

    winsdk 1.0.0b10 的 timeline 给的是 Python datetime.timedelta，更早版本给的是
    WinRT TimeSpan（100ns 刻度，读 .duration）。两种都得认：只认后者的话
    getattr 会静默拿到 None，时长和位置全变成 0 ms，看起来像"这首歌没有时长"，
    而不是报个错让人知道是类型没对上。
    """
    if isinstance(value, datetime.timedelta):
        return int(value.total_seconds() * 1000)
    duration = getattr(value, "duration", None)
    return int(duration / TIMESPAN_TICKS_PER_MS) if duration else 0


def read_vector(view) -> list:
    """
    WinRT IVectorView → list

    SMTC 的 genres 之类是 IVectorView，不是 Python 序列，list() 拿不到内容，
    得按 size/get_at 逐项取。读不出来就给空列表，别让探针在这儿崩掉。
    """
    if view is None:
        return []
    try:
        return [view.get_at(i) for i in range(view.size)]
    except Exception:
        return []


async def probe_smtc() -> bool:
    print("=== SMTC 会话 ===")
    try:
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as Manager,
        )
    except ImportError as exc:
        print(f"[FAIL] winsdk 导入失败: {exc}")
        print("       装法: .venv-media\\Scripts\\python.exe -m pip install winsdk")
        return False

    try:
        manager = await asyncio.wait_for(Manager.request_async(), timeout=8.0)
    except Exception as exc:
        print(f"[FAIL] 拿不到 SessionManager（超时或系统拒绝）: {exc!r}")
        return False

    sessions = manager.get_sessions()
    count = len(sessions) if sessions else 0
    print(f"已注册会话数: {count}")
    if sessions:
        for item in sessions:
            print(f"  - {item.source_app_user_model_id}")

    session = manager.get_current_session()
    if session is None:
        print("[WARN] 没有当前会话。这不是 bug —— 先在 PC 上播一首歌再跑本脚本。")
        print("       注意：播放器必须注册 SMTC 会话，本地播放器一般都可以，")
        print("       但部分绿色版/老版本播放器不会注册。")
        return False

    print(f"\n当前会话来源: {session.source_app_user_model_id}")

    media = await session.try_get_media_properties_async()
    timeline = session.get_timeline_properties()
    playback = session.get_playback_info()

    print("\n--- 元数据 ---")
    print(f"  标题: {media.title!r}")
    print(f"  歌手: {media.artist!r}")
    print(f"  专辑: {media.album_title!r}")
    print(f"  时长: {ms(timeline.end_time)} ms")
    print(f"  位置: {ms(timeline.position)} ms")
    print(f"  状态: {playback.playback_status}")
    print(f"  随机: {getattr(playback, 'is_shuffle_active', 'n/a')}")
    print(f"  循环: {getattr(playback, 'auto_repeat_mode', 'n/a')}")
    print(f"  封面: {'有缩略图' if media.thumbnail is not None else '无（面板会显示占位）'}")
    genres = read_vector(getattr(media, "genres", None))
    if genres:
        print(f"  genres: {genres}")
    else:
        print("  genres: 空")
        print("       → 拿不到 BetterLyrics 注入的 track_id，歌词只能按标题+歌手在线搜，")
        print("         同名歌曲可能匹配到错的版本。")

    print("\n--- 控制能力 ---")
    # 能力位挂在 playback_info.controls 上，不在 playback_info 顶层；播放/暂停那位
    # 的真名是 is_play_pause_toggle_enabled。读错层级或读错名字，getattr 都会静默
    # 给出 False，把一个明明可控的播放器误报成「只暴露元数据」—— 所以这里用
    # hasattr 显式区分「属性不存在」和「属性为假」。
    controls = getattr(playback, "controls", None)
    if controls is None:
        print("[FAIL] playback_info 上没有 controls 子对象，winsdk 版本与本探针不符。")
        return False

    cap_names = {
        "play_pause": "is_play_pause_toggle_enabled",
        "next": "is_next_enabled",
        "prev": "is_previous_enabled",
        "seek": "is_playback_position_enabled",
        "shuffle": "is_shuffle_enabled",
        "repeat": "is_repeat_enabled",
    }
    caps = {}
    for label, attr in cap_names.items():
        if not hasattr(controls, attr):
            print(f"[FAIL] controls 上没有 {attr}，属性名可能随 winsdk 版本变了。")
            return False
        caps[label] = bool(getattr(controls, attr))
        print(f"  {label:11s}: {'可用' if caps[label] else '不可用'}")

    if not (caps["play_pause"] or caps["next"] or caps["prev"]):
        print("\n[WARN] 这个会话只暴露元数据、不给控制。面板能显示歌名，但按钮按了没反应。")
        print("       换一个播放器试试，或确认它没有以「仅后台播放」模式运行。")
        return False

    # 下面三项不可用时，面板必须把对应控件降级成只读或直接隐藏，
    # 不能画一个按不动的按钮骗人 —— 那是最难查的一种"坏了"。
    if not caps["seek"]:
        print("\n[NOTE] seek 不可用：进度条只能显示、不能拖动，面板会做成只读。")
    if not caps["shuffle"]:
        print("[NOTE] shuffle 不可用：面板不显示随机播放开关。")
    if not caps["repeat"]:
        print("[NOTE] repeat 不可用：面板不显示循环模式开关。")

    print("\n[OK] SMTC 读取与基础控制可用。")
    return True


def probe_volume() -> bool:
    print("\n=== 系统音量（IAudioEndpointVolume）===")
    print("SMTC 不提供音量接口，音量必须走 COM 的 endpoint volume。")
    try:
        from pycaw.utils import AudioUtilities
    except ImportError as exc:
        print(f"[FAIL] pycaw 导入失败: {exc}")
        print("       装法: .venv-media\\Scripts\\python.exe -m pip install pycaw comtypes")
        return False

    try:
        device = AudioUtilities.GetSpeakers()
        # 新版 pycaw 的 GetSpeakers() 返回 AudioDevice 包装对象，EndpointVolume 已经
        # 封好了；老教程那套 device.Activate(IAudioEndpointVolume._iid_, ...) 在它上面
        # 根本不存在，会抛 AttributeError('AudioDevice' object has no attribute
        # 'Activate')。两条路都留着，免得 pycaw 版本一变探针就废。
        volume = getattr(device, "EndpointVolume", None)
        if volume is None:
            from ctypes import POINTER, cast

            from comtypes import CLSCTX_ALL
            from pycaw.pycaw import IAudioEndpointVolume

            volume = cast(
                device.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None),
                POINTER(IAudioEndpointVolume),
            )
        print(f"  输出设备: {getattr(device, 'FriendlyName', '?')}")
    except Exception as exc:
        print(f"[FAIL] 拿不到默认输出设备: {exc!r}")
        return False

    try:
        scalar = volume.GetMasterVolumeLevelScalar()
        muted = volume.GetMute()
    except Exception as exc:
        print(f"[FAIL] 读音量失败: {exc!r}")
        return False

    print(f"  当前音量: {scalar:.3f} ({scalar * 100:.0f}%)")
    print(f"  静音: {bool(muted)}")

    # 写路径用设回同一个值验证，听感上没有任何变化
    try:
        volume.SetMasterVolumeLevelScalar(scalar, None)
        print("  写路径: 可用（已用设回原值的方式验证，未改变音量）")
    except Exception as exc:
        print(f"[WARN] 读到了但写不进去: {exc!r}")
        print("       面板上音量滑块会做成只读显示。")
        return False

    print("\n[OK] 音量可读可写。")
    return True


async def main() -> int:
    print(f"Python {sys.version.split()[0]}  ({sys.executable})\n")
    smtc_ok = await probe_smtc()
    vol_ok = probe_volume()

    print("\n=== 结论 ===")
    print(f"  SMTC 媒体读取/控制: {'可用' if smtc_ok else '不可用'}")
    print(f"  系统音量读写:       {'可用' if vol_ok else '不可用'}")
    if not smtc_ok:
        print("\n  SMTC 不可用时先确认 PC 上真的在放歌，再确认播放器注册了 SMTC。")
    return 0 if (smtc_ok and vol_ok) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
