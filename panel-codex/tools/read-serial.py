"""Dump raw panel serial output to a file.

idf.py monitor loses the tail when its stdout is a pipe (block buffering), which
hides exactly the panic backtrace we need. This reads the port directly and
flushes every chunk.

Pass "reset" as the 4th argument to pulse DTR/RTS first. Without it you only see
whatever the board happens to print from now on -- the boot log, and with it the
ethernet link and DHCP events, is long gone on a board that has been up a while.

Usage: python read-serial.py COM4 20 out.log [reset]
"""
import sys
import time

import serial

port = sys.argv[1]
seconds = float(sys.argv[2])
out_path = sys.argv[3]
do_reset = len(sys.argv) > 4 and sys.argv[4] == "reset"

ser = serial.Serial(port, 115200, timeout=0.2)
if do_reset:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)

deadline = time.time() + seconds
with open(out_path, "wb") as fh:
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            fh.write(data)
            fh.flush()
ser.close()
