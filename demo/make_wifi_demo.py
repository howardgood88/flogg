#!/usr/bin/env python3
"""Writes three Wi-Fi logs in the time formats of real Android / MediaTek captures.

    python3 demo/make_wifi_demo.py

wifi-logcat.txt  11-20 14:51:26.963151 16852 16852 I Tag: ...      (logcat, microseconds,
                                                                     no year)
wifi-driver.log  2026-11-20 05:58:25.120584,[ 2024.639186] ...     (wall clock and time
                                                                     since boot)
wifi-fw.log      11-20 05:58:25.758154 (    65276466) (2:RLM-S) ... (firmware ticks in
                                                                     parentheses)

Each one hides problems for the analyses; see demo/README.md, "Wi-Fi logs".
"""

import datetime as dt
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))


def write(name, lines):
    with open(os.path.join(HERE, name), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def us(t):
    return f"{t.microsecond:06d}"


# --------------------------------------------------------------------------------------
# 1. logcat with microseconds
# --------------------------------------------------------------------------------------

def logcat():
    random.seed(21)
    t = dt.datetime(2026, 11, 20, 14, 51, 26, 963151)
    lines = []

    def add(pid, tid, level, tag, text, at=None):
        when = at or t
        lines.append(f"{when.strftime('%m-%d %H:%M:%S')}.{us(when)} {pid:5d} {tid:5d} "
                     f"{level} {tag}: {text}")

    def tick(lo=10, hi=4000):
        nonlocal t
        t += dt.timedelta(microseconds=random.randint(lo, hi))

    p2p_states = ["P2pEnabledState", "P2pDisablingState", "P2pDisabledState"]
    networks = ['"HomeAP"', '"Office-5G"', '"Cafe_Free"']
    for cycle in range(60):
        # Toggling Wi-Fi off: the state machine and the supplicant, lines of two processes
        add(16852, 16852, "I", "MDEServiceBindingManager",
            f"sendMessage : Send message to Service. what = {random.randint(0, 7)}")
        tick()
        add(1768, 2095, "D", "WifiClientModeManager[14550:wlan0]",
            "ConnectModeState cmd = CMD_SWITCH_TO_SCAN_ONLY_MODE { when=0 what=1 "
            "target=com.android.server.wifi.ClientModeImpl }")
        tick()
        add(1768, 2095, "D", "WifiClientModeManager[14550:wlan0]",
            "Queuing broadcast=WIFI_STATE_CHANGED_ACTION EXTRA_WIFI_STATE=0 "
            "EXTRA_PREVIOUS_WIFI_STATE=3")
        tick()
        add(4350, 4350, "D", "wpa_supplicant", "wlan0: State: COMPLETED -> DISCONNECTED")
        tick()
        add(4350, 4350, "I", "wpa_supplicant", "wlan0: CTRL-EVENT-DSCP-POLICY clear_all")
        for a, b in zip(p2p_states, p2p_states[1:]):
            tick()
            add(1768, 2095, "D", "WifiP2pService", f"State: {a} -> {b}")
        tick(50000, 400000)

        # Turning it back on and connecting: CMD_START_CONNECT ... CTRL-EVENT-CONNECTED
        network = networks[cycle % 3]
        net_id = 10 + cycle % 3
        add(1768, 2095, "D", "WifiClientModeManager[14550:wlan0]",
            f"ConnectableState cmd = CMD_START_CONNECT netId={net_id} ssid={network}")
        tick()
        add(4350, 4350, "D", "wpa_supplicant", "wlan0: State: DISCONNECTED -> SCANNING")
        took = random.randint(800000, 2500000)
        if cycle in (17, 41):
            took = random.randint(6500000, 9000000)  # slow association
        failed = cycle in (29, 52)
        connected = t + dt.timedelta(microseconds=took)
        # Other processes keep logging meanwhile
        while t < connected - dt.timedelta(milliseconds=50):
            tick(20000, 200000)
            add(1768, 2207, "V", "SilentRoamingManager",
                "SilentRoamingManager$DefaultState what=CMD_WIFI_STATE_CHANGED screen=on")
            if random.random() < 0.3:
                add(16852, 16852, "D", "ConnectivityManager",
                    "StackLog: [android.net.ConnectivityManager.sendRequestForNetwork("
                    "ConnectivityManager.java:4512)]")
        if failed:
            add(4350, 4350, "W", "wpa_supplicant",
                f"wlan0: CTRL-EVENT-ASSOC-REJECT bssid=02:12:36:3a:3d:{cycle:02x} status_code=17")
            add(4350, 4350, "D", "wpa_supplicant", "wlan0: State: ASSOCIATING -> DISCONNECTED")
        else:
            t = connected
            add(4350, 4350, "I", "wpa_supplicant",
                f"wlan0: CTRL-EVENT-CONNECTED - Connection to 02:12:36:3a:3d:{cycle:02x} "
                f"completed [id={net_id} id_str=]")
            tick()
            add(4350, 4350, "D", "wpa_supplicant", "wlan0: State: GROUP_HANDSHAKE -> COMPLETED")
            tick()
            add(1768, 2095, "D", "WifiClientModeManager[14550:wlan0]",
                f"L2ConnectedState cmd = CMD_IP_CONFIGURATION_SUCCESSFUL netId={net_id}")
        tick(3000000, 9000000)

        # A queued message reported by another thread a little earlier: lines out of order
        if cycle % 7 == 3:
            add(1768, 2290, "D", "SemWifiP2pService",
                "InactiveState cmd = EVENT_WIFI_P2P_DISABLED { when=-2ms what=143362 }",
                at=t - dt.timedelta(milliseconds=3))

        # Five P2P state flips within a few seconds, once
        if cycle == 35:
            for flip in range(6):
                tick(200000, 600000)
                a, b = ("P2pEnabledState", "P2pDisabledState") if flip % 2 else \
                    ("P2pDisabledState", "P2pEnabledState")
                add(1768, 2290, "D", "SemWifiP2pService", f"State: {a} -> {b}")

        # Crossing midnight: the date changes, there is no year
        if cycle == 44:
            t = dt.datetime(2026, 11, 20, 23, 59, 57, 120004)
    write("wifi-logcat.txt", lines)


# --------------------------------------------------------------------------------------
# 2. Driver log: wall clock and [time since boot]
# --------------------------------------------------------------------------------------

def driver():
    random.seed(22)
    boot = 2024.639186
    wall = dt.datetime(2026, 11, 20, 5, 58, 25, 120584)
    lines = []

    def add(proc, pid, text):
        lines.append(f"{wall.strftime('%Y-%m-%d %H:%M:%S')}.{us(wall)},[{boot:12.6f}] "
                     f"{proc:<14}[{pid}]{text}")

    def tick(lo, hi):
        nonlocal boot, wall
        step = random.uniform(lo, hi)
        boot += step
        wall += dt.timedelta(seconds=step)

    mac = "ba:7e:25:5a:43:c8"
    for scan in range(400):
        channel = random.choice([1, 6, 11, 36, 53, 149])
        add("wificond", 1417, f"mtk_cfg80211_scan:(REQ DEBUG) n_ssid=(1->0) "
                              f"n_channel({channel}==>{channel}) wildcard=0x1 flag=0x4000")
        tick(0.00005, 0.0002)
        add("main_thread", 4049, "aisFsmSteps:(AIS STATE) [AIS0][0] TRANSITION: [IDLE] -> [SCAN]")
        tick(0.00005, 0.0002)
        add("main_thread", 4049, f"nicActivateNetworkEx:(RSN INFO) [BSS index]=0 Link=0 "
                                 f"OwnMac0={mac} BSSID=00:00:00:00:00:00")
        tick(0.00005, 0.0002)
        add("main_thread", 4049, "scnFsmSteps:(SCN STATE) [SCAN]TRANSITION: [IDLE] -> [SCANNING]")
        tick(0.00001, 0.00005)
        add("main_thread", 4049, f"SCANLOG:(SCN INFO) [SCN:200:D2F] ScanReqV2: ScanType=1,"
                                 f"BSS=0,SSIDType=1,Num=0,Ext=0,Chan={channel}")
        # A scan lasts 60 ms to 2.3 s; one never comes back to IDLE
        if scan == 260:
            tick(0.2, 0.4)
            add("main_thread", 4049, "scnFsmDumpScanDoneInfo:(SCN WARN) scan timeout, "
                                     "no response from firmware")
            tick(6.0, 7.0)
            add("main_thread", 4049, "kalSendUeventHandler:(INIT DEBUG) Send UEvent: "
                                     "Scan=Status:ABORT,DBDC:ENABLE")
            tick(0.0001, 0.0003)
            add("main_thread", 4049, "scnFsmSteps:(SCN STATE) [SCAN]TRANSITION: [SCANNING] -> [IDLE]")
            tick(1.0, 2.0)
            continue
        tick(0.06, 2.3 if scan % 50 else 4.6)
        add("main_thread", 4049, "scnFsmDumpScanDoneInfo:(SCN INFO) BAndPCnt :  "
                                 + "      ".join(str(random.randint(0, 9)) for _ in range(7)))
        tick(0.00003, 0.0001)
        add("main_thread", 4049, "scnFsmSteps:(SCN STATE) [SCAN]TRANSITION: [SCANNING] -> [IDLE]")
        tick(0.00003, 0.0001)
        add("main_thread", 4049, "nanAsccGetBackupCustomizedDw:(NAN DEBUG) Get backup "
                                 "customized DW=1")
        tick(0.0003, 0.0007)
        add("main_thread", 4049, "aisFsmSteps:(AIS STATE) [AIS0][0] TRANSITION: [SCAN] -> [IDLE]")
        tick(0.00002, 0.0001)
        add("main_thread", 4049, f"kalSendUeventHandler:(INIT DEBUG) Send UEvent: "
                                 f"Scan=Status:NORMAL,DBDC:ENABLE,Time:{random.randint(60, 2300)}")
        tick(2.0, 6.0)
        # P2P find on another interface now and then
        if scan % 9 == 4:
            add("wpa_supplicant", 17528, "mtk_p2p_cfg80211_scan:(P2P INFO) [6] n_channels: 3, "
                                         "bssid: ff:ff:ff:ff:ff:ff, n_ssid 1")
            tick(0.0001, 0.0003)
            add("main_thread", 4049, "p2pDevFsmStateTransition:(P2P STATE) [P2P_DEV]TRANSITION: "
                                     "[P2P_DEV_STATE_IDLE] -> [P2P_DEV_STATE_SCAN]")
            tick(0.1, 0.3)
            add("main_thread", 4049, "p2pDevFsmStateTransition:(P2P STATE) [P2P_DEV]TRANSITION: "
                                     "[P2P_DEV_STATE_SCAN] -> [P2P_DEV_STATE_IDLE]")
            tick(1.0, 3.0)
        # The network time arrives: the wall clock jumps back 2 hours, the time since
        # boot goes on
        if scan == 150:
            add("main_thread", 4049, "kalSyncTimeToFW:(INIT INFO) time synced from network, "
                                     "wall clock adjusted")
            wall -= dt.timedelta(hours=2, minutes=0, seconds=13, microseconds=420000)
    write("wifi-driver.log", lines)


# --------------------------------------------------------------------------------------
# 3. Firmware log: (ticks) (seq:MODULE-level)
# --------------------------------------------------------------------------------------

def firmware():
    random.seed(23)
    t = dt.datetime(2026, 11, 20, 5, 58, 25, 758154)
    ticks = 65276466
    seq = 2
    lines = []

    def add(module, text):
        nonlocal seq
        lines.append(f"{t.strftime('%m-%d %H:%M:%S')}.{us(t)} ({ticks:12d}) "
                     f"({seq}:{module}) {text}")
        seq = (seq + 1) % 16

    def tick(seconds):
        nonlocal t, ticks
        t += dt.timedelta(seconds=seconds)
        # A 32 kHz counter
        ticks += int(seconds * 32768)

    sleeps = 0
    cmd_seq = 254
    for burst in range(500):
        for channel in (100, 104, 108):
            add("RLM-S", f"PhySetCh:pri({channel})ofst(15)ext(0)s1({channel})s2(0)bw(0)"
                         f"cal(3)st1(2)st2(2)bnId(1)Ofreq(0)f(0)sx(0)en(1)")
            tick(0.0002)
            add("COEX-S", f"[BWC]BT 1,BSS4 4 4 4,Final 0,CUR {int(channel == 100)},SCAN 1,C_MD 0")
            tick(random.uniform(0.09, 0.12))
        for _ in range(random.randint(1, 4)):
            cmd_seq = (cmd_seq + 1) % 256
            add("CMD-W", f"UNI_CMD_ID:0x{random.choice([0x0e, 0x49, 0x01, 0x16]):04x} "
                         f"SEQ:{cmd_seq} LEN:{random.choice([32, 72, 108, 300])} option:0x06")
            tick(0.00006)
        add("RXM-S", f"UNI_EVT_ID:0x0b SEQ:{random.randint(1, 200)} LEN:24 FID:10 "
                     f"Type:0x3800 S2D:0")
        tick(0.007)

        # Deep sleep and wake up: (cnt N) pairs them; once the chip does not wake up
        sleeps += 1
        add("LP-S", "Wi-Fi may enter deep sleep")
        if burst == 333:
            tick(12.5)
            add("STATS-C", "LMAC_DBG: watchdog: no wake up from deep sleep, chip reset")
            tick(0.3)
            continue
        tick(random.uniform(0.02, 0.12) if burst % 40 else random.uniform(4.0, 4.7))
        add("LP-C", f"==== WAKE UP FROM DSLP (cnt {sleeps}) (rsn {random.choice([1, 103])}) ====")
        tick(0.0075)

        # TX paused and resumed for the queue of the station
        for reason in (2, 7):
            add("MEM-S", f"[STA_PAUSE][u2WlanIdx=0][ucWmmIdx=3][ucAcBitmap=15][eDbdcIdx=0]"
                         f"[Lp0=e00720d2][Reason={reason}][fgTxPause=1]")
            stuck = burst == 120 and reason == 7
            tick(1.8 if stuck else random.uniform(0.00002, 0.0003))
            add("MEM-S", f"[STA_PAUSE][u2WlanIdx=0][ucWmmIdx=3][ucAcBitmap=15][eDbdcIdx=0]"
                         f"[Lp0=e00720d2][Reason={reason}][fgTxPause=0]")
            tick(0.0001)
        add("COEX-S", f"COEX: NtfConSts BtSwitchOn[0] BtOn[1] Notify[0] SwIdx[0] Sta[0 0] "
                      f"Ap[0 0] Rsn[{random.choice([1, 2])}] Last[0] New[0]")
        tick(0.0006)
        add("TXM-S", "TPAQ[e0068d44]:p(1),w(0),d(0),om(0),wm(3),re(7),ta(1),m(0),"
                     "STATUS(00000000)")
        tick(random.uniform(0.5, 2.0))
    write("wifi-fw.log", lines)


def main():
    logcat()
    driver()
    firmware()
    print("Wi-Fi demo files written in", HERE)


if __name__ == "__main__":
    main()
