"""边收边落盘的串口长时记录器。

用途：需要用户在现场做物理动作（拔插扫码枪、扫码）时抓日志。定时录制不行——
用户什么时候动手不可控，且开机那段日志洪流会把串口缓冲冲掉，必须长时挂着。
每行立即 flush，进程被杀掉也不丢已收到的内容。

用法：python serial_record.py COM4 out.log [baud]
"""

import sys

import serial

port = sys.argv[1]
out_path = sys.argv[2]
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

ser = serial.Serial(port, baud, timeout=1)
with open(out_path, "wb") as f:
    while True:
        chunk = ser.read(4096)
        if chunk:
            f.write(chunk)
            f.flush()
