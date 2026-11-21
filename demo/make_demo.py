#!/usr/bin/env python3
"""Writes the demo logs of flogg's analysis features into this folder.

    python3 demo/make_demo.py            # the small demo files
    python3 demo/make_demo.py --big 3000000   # also big-app.log with that many lines

Everything is generated with a fixed seed: the same files every time.
See demo/README.md for what to try with them.
"""

import argparse
import datetime as dt
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
USERS = ["alice", "bob", "carol", "dave", "erin", "frank"]
PATHS = ["/api/pay", "/api/refund", "/api/balance", "/api/history"]


def ts(t):
    return t.strftime("%Y-%m-%d %H:%M:%S.") + f"{t.microsecond // 1000:03d}"


class Log:
    def __init__(self, start):
        self.t = start
        self.lines = []

    def tick(self, ms_min=5, ms_max=60):
        self.t += dt.timedelta(milliseconds=random.randint(ms_min, ms_max))

    def add(self, level, thread, text, at=None):
        self.lines.append(f"{ts(at or self.t)} {level:<5} [{thread}] {text}")

    def raw(self, text):
        self.lines.append(text)

    def write(self, name):
        with open(os.path.join(HERE, name), "w", newline="\n") as f:
            f.write("\n".join(self.lines) + "\n")


def service_day(start, bad, count):
    """A payment service; the bad day has the incident."""
    random.seed(7 if bad else 3)
    log = Log(start)
    log.add("INFO", "main", "service starting version=2.14.0 profile=prod")
    open_requests = {}
    never_ended = set()
    pending_logouts = []
    next_id = 1000
    conn = 0
    incident_at = count // 2

    for step in range(count):
        log.tick()
        thread = f"http-{random.randint(1, 8)}"

        # Requests: begin, then end after 80-250 ms (a few slow ones on the bad day)
        if random.random() < 0.35:
            next_id += 1
            user = random.choice(USERS)
            log.add("INFO", thread,
                    f"request begin id={next_id} path={random.choice(PATHS)} user={user}")
            took = random.randint(80, 250)
            if bad and step > incident_at and random.random() < 0.08:
                took = random.randint(2500, 4800)
            open_requests[next_id] = log.t + dt.timedelta(milliseconds=took)
            # Three requests never end on the bad day
            if bad and incident_at + 20 <= step and len(never_ended) < 3 and random.random() < 0.3:
                never_ended.add(next_id)

        for rid, end in sorted(open_requests.items(), key=lambda x: x[1]):
            if end <= log.t:
                if rid in never_ended:
                    del open_requests[rid]
                    continue
                log.add("INFO", thread, f"request end id={rid} status=200", at=end)
                del open_requests[rid]

        r = random.random()
        if r < 0.05:
            conn += 1
            log.add("DEBUG", "pool", f"db connect start conn={conn}")
            log.tick(3, 20)
            log.add("DEBUG", "pool", f"db connected conn={conn}")
        elif r < 0.12:
            miss_rate = 0.8 if bad and step > incident_at else 0.3
            word = "miss" if random.random() < miss_rate else "hit"
            log.add("DEBUG", "cache", f"cache {word} key=user:{random.choice(USERS)}")
        elif r < 0.14:
            user = random.choice(USERS)
            log.add("INFO", "auth", f"login user={user}")
            if random.random() < 0.2:
                # After the incident some sessions end right away (a broken token)
                quick = bad and step > incident_at and random.random() < 0.5
                logout = log.t + dt.timedelta(
                    milliseconds=random.randint(200, 900) if quick else random.randint(3000, 20000))
                pending_logouts.append((logout, user))
        elif r < 0.15:
            log.add("WARN", "gc", f"gc pause {random.randint(20, 90)} ms")

        for logout in [p for p in pending_logouts if p[0] <= log.t]:
            log.add("INFO", "auth", f"logout user={logout[1]}", at=logout[0])
            pending_logouts.remove(logout)

        # The incident, step by step: the requests under way end first
        if bad and step == incident_at:
            for rid, end in sorted(open_requests.items(), key=lambda x: x[1]):
                log.add("INFO", thread, f"request end id={rid} status=200", at=max(end, log.t))
            open_requests.clear()
            conn += 1
            log.add("DEBUG", "pool", f"db connect start conn={conn}")
            log.tick(5000, 6000)
            log.add("ERROR", "pool", "db error: connection refused host=ledger-db:5432")
            log.tick(100, 300)
            log.add("ERROR", "pool", "db error: connection refused host=ledger-db:5432")
            log.tick(1000, 2000)
            log.add("WARN", "breaker", "circuit breaker OPEN for service=ledger")
            for attempt in range(2, 7):
                log.tick(2000, 4000)
                log.add("WARN", "http-3", f"retry request=r77 attempt={attempt}")
        if bad and step == incident_at + 40:
            log.add("ERROR", "pool", "FATAL EXCEPTION: java.lang.OutOfMemoryError: Java heap space")
            for frame in ["com.pay.pool.Pool.alloc(Pool.java:88)",
                          "com.pay.pool.Pool.borrow(Pool.java:41)",
                          "com.pay.api.PayHandler.handle(PayHandler.java:120)",
                          "java.base/java.lang.Thread.run(Thread.java:1583)"]:
                log.raw(f"    at {frame}")
        if bad and step == incident_at + 80:
            log.add("INFO", "ntp", "clock adjusted by -30.000 s (server time.example.com)")
            log.t -= dt.timedelta(seconds=30)
        if not bad and step == incident_at:
            log.add("INFO", "breaker", "circuit breaker CLOSED for service=ledger")

    for rid, end in sorted(open_requests.items(), key=lambda x: x[1]):
        if rid not in never_ended:
            log.add("INFO", "http-1", f"request end id={rid} status=200", at=max(end, log.t))
    log.add("INFO", "main", "service stopping")
    return log


def device_boot():
    """Kernel lines with the time since boot and a wall clock set by NTP during boot."""
    log = Log(dt.datetime(2000, 1, 1))
    boot = 0.0
    # The board has no battery: its clock starts in 2000 until NTP sets it
    clock_start = dt.datetime(2000, 1, 1, 0, 0, 0)
    events = [
        "Booting Linux on physical CPU 0x0", "Machine model: Demo Board rev B",
        "init: starting service 'logd'", "wlan0: driver loaded", "wlan0: associated with demo-ap",
        "init: starting service 'netd'", "ntp: time synchronized with time.example.com",
        "init: starting service 'payment-agent'", "payment-agent: ready", "init: boot completed",
    ]
    for i, text in enumerate(events):
        boot += random.uniform(0.3, 1.5)
        if text.startswith("ntp:"):
            # The wall clock jumps 26 years forward, the time since boot does not
            clock_start = dt.datetime(2026, 11, 22, 8, 15, 3) - dt.timedelta(seconds=boot)
        wall = clock_start + dt.timedelta(seconds=boot)
        log.raw(f"[{boot:12.6f}] {ts(wall)} {text}")
    log.write("device-boot.log")


def logcat():
    """Android logcat (threadtime, no year): app launches and an ANR."""
    random.seed(11)
    t = dt.datetime(2026, 12, 31, 23, 59, 50)
    lines = []

    def add(level, tag, text, pid=1234, tid=1260):
        lines.append(f"{t.strftime('%m-%d %H:%M:%S.')}{t.microsecond // 1000:03d} "
                     f"{pid:5d} {tid:5d} {level} {tag}: {text}")

    for i, activity in enumerate(["MainActivity", "PayActivity", "HistoryActivity",
                                  "PayActivity", "SettingsActivity"]):
        add("I", "ActivityTaskManager", f"START u0 {{cmp=com.demo/.{activity}}} from uid 10123")
        launch = random.randint(300, 900) if i != 3 else 4200
        t += dt.timedelta(milliseconds=launch)
        add("I", "ActivityTaskManager", f"Displayed com.demo/.{activity}: +{launch}ms")
        for _ in range(random.randint(3, 8)):
            t += dt.timedelta(milliseconds=random.randint(50, 400))
            add("D", "PayClient", f"sync ok items={random.randint(1, 40)}", 4321, 4330)
        t += dt.timedelta(milliseconds=random.randint(500, 2000))
    add("E", "ActivityManager", "ANR in com.demo (com.demo/.PayActivity)", 1000, 1100)
    add("E", "ActivityManager", "Reason: Input dispatching timed out", 1000, 1100)
    with open(os.path.join(HERE, "logcat.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def rotated():
    """A log rotated in three files: service.log.2 is the oldest, service.log the newest."""
    folder = os.path.join(HERE, "rotated")
    os.makedirs(folder, exist_ok=True)
    start = dt.datetime(2026, 11, 22, 6, 0, 0)
    parts = [("service.log.2", 0), ("service.log.1", 1), ("service.log", 2)]
    for name, index in parts:
        t = start + dt.timedelta(hours=index)
        lines = [f"{ts(t + dt.timedelta(minutes=m))} INFO  [main] part {index + 1} of 3, "
                 f"minute {m}" for m in range(0, 60, 10)]
        data = "\n".join(lines)
        if name != "service.log.1":
            data += "\n"           # service.log.1 ends without a line end
        raw = data.encode()
        if name == "service.log.2":
            raw = b"\xef\xbb\xbf" + raw  # and the oldest starts with a byte order mark
        path = os.path.join(folder, name)
        with open(path, "wb") as f:
            f.write(raw)
        # Modified at the end of their hour, for the "Oldest first" sort of the file panel
        stamp = (t + dt.timedelta(minutes=59)).timestamp()
        os.utime(path, (stamp, stamp))


def big(lines):
    random.seed(5)
    t = dt.datetime(2026, 11, 22, 0, 0, 0)
    path = os.path.join(HERE, "big-app.log")
    rid = 0
    with open(path, "w", newline="\n") as f:
        pending = []
        for i in range(lines):
            t += dt.timedelta(microseconds=random.randint(200, 3000))
            if pending and pending[0][1] <= t:
                f.write(f"{ts(pending[0][1])} INFO  [http-2] request end id={pending[0][0]} "
                        f"status=200\n")
                pending.pop(0)
                continue
            r = random.random()
            if r < 0.3:
                rid += 1
                took = random.randint(20, 300) if random.random() > 0.0005 else 5000
                pending.append((rid, t + dt.timedelta(milliseconds=took)))
                pending.sort(key=lambda p: p[1])
                f.write(f"{ts(t)} INFO  [http-1] request begin id={rid} "
                        f"path={random.choice(PATHS)} user={random.choice(USERS)}\n")
            elif r < 0.6:
                f.write(f"{ts(t)} DEBUG [cache] cache {'hit' if r < 0.5 else 'miss'} "
                        f"key=user:{random.choice(USERS)}\n")
            elif r < 0.9999:
                f.write(f"{ts(t)} DEBUG [worker-{random.randint(1, 16)}] processed batch "
                        f"size={random.randint(1, 500)} in {random.randint(1, 90)} ms\n")
            else:
                f.write(f"{ts(t)} ERROR [pool] FATAL EXCEPTION: java.lang.OutOfMemoryError\n")
    print(f"wrote {path} ({os.path.getsize(path) // (1024 * 1024)} MB)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--big", type=int, default=0, help="lines of big-app.log")
    args = parser.parse_args()

    service_day(dt.datetime(2026, 11, 21, 9, 0, 0), False, 3000).write("app-good.log")
    service_day(dt.datetime(2026, 11, 22, 9, 0, 0), True, 3000).write("app-bad.log")
    random.seed(1)
    device_boot()
    logcat()
    rotated()
    if args.big:
        big(args.big)
    print("demo files written in", HERE)


if __name__ == "__main__":
    main()
