#!/usr/bin/env python3
"""Generate the synthetic benchmark logs used by the "[.benchmark]" test.

big.log: 30M lines (~1.8 GB), mid.log: first 5M lines (~300 MB).
Usage: python3 bench/gen_bench_log.py [output_dir]
"""
import os
import random
import sys

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
random.seed(7)
mods = ["wlan0", "wlan1", "hci0", "bt", "app", "net", "ui", "db", "auth", "kernel"]
lvls = ["DEBUG", "INFO", "INFO", "INFO", "WARN", "ERROR"]
msgs = ["heartbeat ok", "scan started", "connected to AP", "timeout waiting for response",
        "retry 3/5", "user login id=%d", "crash in module %d", "request done in %dms",
        "cache miss key=%d", "state changed to %d"]

lines = []
for i in range(1000000):
    m = random.choice(msgs)
    if "%d" in m:
        m = m % random.randint(0, 99999)
    lines.append(f"2026-10-22 10:{i//60000%60:02d}:{i//1000%60:02d}.{i%1000:03d} "
                 f"{random.choice(lvls):5} [{random.choice(mods)}] "
                 f"tid={random.randint(1000,9999)} {m}\n")
chunk = "".join(lines).encode()

with open(os.path.join(out, "big.log"), "wb") as f:
    for _ in range(30):
        f.write(chunk)
with open(os.path.join(out, "mid.log"), "wb") as f:
    for _ in range(5):
        f.write(chunk)
