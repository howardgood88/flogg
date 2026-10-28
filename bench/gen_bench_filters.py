#!/usr/bin/env python3
"""Generate a filter file with many filters, for the "[.timing]" tests and manual checks.

The filters match words of the logs from gen_bench_log.py: groups of plain text filters,
about 5% regular expressions, some excludes, colors on most of them.
Usage: python3 bench/gen_bench_filters.py [count] [output_file]
Default: 343 filters (a real user's filter file) in bench/many.flogg-filters.json
"""
import json
import os
import random
import sys
import uuid

count = int(sys.argv[1]) if len(sys.argv) > 1 else 343
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "many.flogg-filters.json")
random.seed(11)

words = ["wlan0", "wlan1", "hci0", "ERROR", "WARN", "crash", "timeout", "retry", "login",
         "cache", "state", "scan", "AP", "[auth]", "[db]", "[ui]", "[net]", "[kernel]",
         "heartbeat", "done in", "miss key", "changed", "connected"]
colors = ["#ffff0000", "#ff008000", "#ff0000ff", "#ff800080", "#ffff8c00", "#ff008080", ""]


def make_filter(i):
    regex = i % 20 == 0
    if regex:
        pattern = random.choice([r"in \d{2}ms", r"key=%d\d+" % random.randint(1, 9),
                                 r"tid=%d\d{3}" % random.randint(1, 9), r"(retry|timeout) \d"])
    else:
        pattern = "%s %d" % (random.choice(words), random.randint(0, 99999)) \
            if i % 3 else random.choice(words)
    return {
        "id": str(uuid.UUID(int=random.getrandbits(128))),
        "type": "filter",
        "enabled": i % 7 != 0,
        "pattern": pattern,
        "description": "filter %d" % i,
        "regex": regex,
        "caseSensitive": i % 2 == 0,
        "exclude": i % 25 == 0,
        "foreColor": random.choice(colors),
        "backColor": random.choice(colors[3:]),
        "colorMode": "match" if i % 4 == 0 else "line",
    }


children = []
group = None
for i in range(count):
    # Groups of about 20 filters, every fifth filter outside a group
    if i % 5 == 4:
        children.append(make_filter(i))
        continue
    if group is None or len(group["children"]) >= 16:
        group = {"id": str(uuid.UUID(int=random.getrandbits(128))), "type": "group",
                 "name": "group %d" % len(children), "children": []}
        children.append(group)
    group["children"].append(make_filter(i))

root = {"id": str(uuid.UUID(int=random.getrandbits(128))), "type": "group", "name": "root",
        "children": children}
with open(out, "w") as f:
    json.dump({"version": 1, "root": root}, f, indent=1)
print("%d filters written to %s" % (count, out))
