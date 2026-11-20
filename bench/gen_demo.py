#!/usr/bin/env python3
"""Generate the folder of device logs and the filters of the README screenshot.

Writes <output_dir>/device_logs (a few days of kernel, wifi and system logs) and
<output_dir>/demo.flogg-filters.json: filters coloring whole lines and filters coloring only
the matched text, in groups.
Usage: python3 bench/gen_demo.py [output_dir]   (default: bench/demo)
Then: flogg --filters <output_dir>/demo.flogg-filters.json <output_dir>/device_logs \\
            <output_dir>/device_logs/2026-11-15/system.log
"""
import json
import os
import random
import sys
import uuid

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "demo")
random.seed(5)

mods = {
    "system": ["app", "net", "ui", "db", "auth", "kernel", "bt", "wlan0", "wlan1", "hci0"],
    "kernel": ["kernel", "kernel", "hci0", "bt", "wlan0"],
    "wifi": ["wlan0", "wlan1", "wlan0", "net"],
}
lvls = ["DEBUG"] * 2 + ["INFO"] * 12 + ["WARN"] * 3 + ["ERROR"]
msgs = ["heartbeat ok", "scan started", "connected to AP", "timeout waiting for response",
        "retry 3/5", "user login id=%d", "crash in module %d", "request done in %dms",
        "cache miss key=%d", "state changed to %d"]


def write_log(path, day, kind, count):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        for i in range(count):
            m = random.choice(msgs)
            if "%d" in m:
                m = m % random.randint(0, 99999)
            f.write(f"{day} 10:{i // 60000 % 60:02d}:{i // 1000 % 60:02d}.{i % 1000:03d} "
                    f"{random.choice(lvls):5} [{random.choice(mods[kind])}] "
                    f"tid={random.randint(1000, 9999)} {m}\n")


logs = os.path.join(out, "device_logs")
for day in ["2026-11-14", "2026-11-15"]:
    for kind in ["kernel", "wifi", "system"]:
        write_log(os.path.join(logs, day, kind + ".log"), day, kind, 20000)
    write_log(os.path.join(logs, day, "crash", "tombstone_00.log"), day, "system", 300)
os.makedirs(os.path.join(logs, "config"), exist_ok=True)
with open(os.path.join(logs, "config", "device.json"), "w") as f:
    json.dump({"device": "demo", "wifi": {"band": "5GHz", "country": "TW"}}, f, indent=2)

ids = random.Random(9)


def node_id():
    return str(uuid.UUID(int=ids.getrandbits(128)))


def flt(pattern, back="", fore="", mode="line", regex=False, enabled=True, exclude=False,
        case=False):
    return {"id": node_id(), "type": "filter", "enabled": enabled, "pattern": pattern,
            "description": "", "regex": regex, "caseSensitive": case, "exclude": exclude,
            "foreColor": fore, "backColor": back, "colorMode": mode}


def group(name, children):
    return {"id": node_id(), "type": "group", "name": name, "enabled": True,
            "children": children}


filters = {
    "version": 1,
    "root": group("root", [
        group("Errors", [
            flt("ERROR", back="#fff28b82", case=True),
            flt("crash in module", back="#ffffcdd2"),
            flt("timeout waiting", back="#ffffe0b2"),
        ]),
        group("Wi-Fi", [
            flt(r"wlan\d", fore="#ff1565c0", back="#ffbbdefb", mode="match", regex=True),
            flt("connected to AP", back="#ffc8e6c9"),
            flt("scan started", back="#fffff176", mode="match"),
        ]),
        flt("state changed", back="#ff2e7d32", fore="#ffffffff"),
        flt(r"tid=\d+", fore="#ffc62828", mode="match", regex=True),
        flt(r"user login id=\d+", back="#ffe1bee7", mode="match", regex=True),
        flt("heartbeat ok", enabled=False),
        flt("DEBUG", exclude=True, case=True),
    ]),
}
with open(os.path.join(out, "demo.flogg-filters.json"), "w") as f:
    json.dump(filters, f, indent=2)
print("Wrote", logs, "and", os.path.join(out, "demo.flogg-filters.json"))
