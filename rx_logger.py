#!/usr/bin/env python3
"""
Outdoor RSSI logger — reads RX board serial output and writes to CSV.
Usage: python3 rx_logger.py [port] [output.csv]
"""
import sys
import re
import csv
import serial
from datetime import datetime

PORT    = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
OUTFILE = sys.argv[2] if len(sys.argv) > 2 else f"rssi_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
BAUD    = 115200

# Matches both firmware versions:
#   new: [RX] CMD:START:28 rssi=-87.0 snr=9.2
#   old: [RX] START 28s rssi=-87.0 snr=9.2
PATTERN = re.compile(
    r"\[RX\] (?:CMD:)?(\w+)[: ](\d+)s?\s+rssi=([-\d.]+)(?:\s+snr=([-\d.]+))?"
)

print(f"Logging {PORT} -> {OUTFILE}  (Ctrl+C to stop)")

with serial.Serial(PORT, BAUD, timeout=2) as ser, \
     open(OUTFILE, "w", newline="") as f:

    writer = csv.writer(f)
    writer.writerow(["timestamp", "cmd", "seconds", "rssi_dbm", "snr_db"])
    f.flush()

    count = 0
    try:
        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            print(line)
            m = PATTERN.search(line)
            if m:
                ts   = datetime.now().isoformat(timespec="milliseconds")
                cmd  = m.group(1)
                sec  = int(m.group(2))
                rssi = float(m.group(3))
                snr  = float(m.group(4)) if m.group(4) else None
                writer.writerow([ts, cmd, sec, rssi, snr])
                f.flush()
                count += 1
                print(f"  -> logged #{count}: rssi={rssi} dBm  snr={snr} dB")
    except KeyboardInterrupt:
        print(f"\nDone. {count} entries saved to {OUTFILE}")
