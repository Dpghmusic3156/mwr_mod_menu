#!/usr/bin/env python3
"""
Mini World Royale Dear ImGui In-Game Mod Menu Launcher
Supports both:
  - Real rooted Android phones (arm64-v8a, su -c or in-process Frida .so staging, SELinux MCS preserve, Frida USB/TCP)
  - Emulators like MuMu (x86_64 + libhoudini, Frida TCP forward)
"""

import argparse
import os
import signal
import subprocess
import sys
import time
import frida

CANDIDATE_PKGS = [
    "com.vn.miniworld.royale.sparkgame",
    "com.sea.miniworldroyale.sparkgame",
]
PKG = "com.sea.miniworldroyale.sparkgame"
ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPT_PATH = os.path.join(ROOT_DIR, "mwr_mod.js")
BUILD_DIR = os.path.join(ROOT_DIR, "build")

_SELECTED_SERIAL = os.environ.get("MWR_ADB_SERIAL", "").strip() or None
_SELECTED_PKG = os.environ.get("MWR_PKG", "").strip() or None
_SU_CMD = None
_ROOT_CHECKED = False


def list_adb_devices():
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=10)
    devs = []
    for line in r.stdout.splitlines()[1:]:
        parts = line.strip().split()
        if len(parts) >= 2 and parts[1] == "device":
            devs.append(parts[0])
    return devs


def get_serial():
    global _SELECTED_SERIAL
    if _SELECTED_SERIAL:
        return _SELECTED_SERIAL
    devs = list_adb_devices()
    if not devs:
        _SELECTED_SERIAL = "127.0.0.1:16384"
        return _SELECTED_SERIAL
    # Prefer physical USB device (non-emulator / non-localhost) if connected alongside MuMu
    for d in devs:
        if not d.startswith("127.0.0.1:") and not d.startswith("emulator-"):
            _SELECTED_SERIAL = d
            return _SELECTED_SERIAL
    _SELECTED_SERIAL = devs[0]
    return _SELECTED_SERIAL


def adb_cmd(args, timeout=15):
    return subprocess.run(
        ["adb", "-s", get_serial()] + args,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def adb_shell(cmd, timeout=10):
    return adb_cmd(["shell", cmd], timeout=timeout).stdout.strip()


def probe_root_cmd():
    """Return ('direct' | su_path | None) for running root commands via ADB."""
    global _SU_CMD, _ROOT_CHECKED
    if _ROOT_CHECKED:
        return _SU_CMD
    _ROOT_CHECKED = True

    if adb_shell("id -u") == "0":
        _SU_CMD = "direct"
        return _SU_CMD

    for cand in (
        "su",
        "/data/adb/ksu/bin/su",
        "/data/adb/ap/bin/su",
        "/sbin/su",
        "/debug_ramdisk/su",
    ):
        r = adb_cmd(["shell", f"{cand} -c 'id -u' 2>/dev/null"], timeout=6)
        if r.returncode == 0 and r.stdout.strip() == "0":
            _SU_CMD = cand
            return _SU_CMD

    _SU_CMD = None
    return None


def adb_root_shell(cmd, timeout=10):
    root_cmd = probe_root_cmd()
    if root_cmd == "direct":
        return adb_shell(cmd, timeout=timeout)
    if root_cmd:
        escaped = cmd.replace("'", "'\"'\"'")
        return adb_shell(f"{root_cmd} -c '{escaped}'", timeout=timeout)
    return ""


def detect_target_abi():
    abi = adb_shell("getprop ro.product.cpu.abi")
    if "arm64" in abi or "aarch64" in abi:
        return "arm64-v8a"
    if "x86_64" in abi:
        return "x86_64"
    return "arm64-v8a"


def detect_pkg():
    global _SELECTED_PKG, PKG
    if _SELECTED_PKG:
        PKG = _SELECTED_PKG
        return _SELECTED_PKG

    # 1. Prefer a currently running MWR package
    for cand in CANDIDATE_PKGS:
        if adb_shell(f"pidof {cand}"):
            _SELECTED_PKG = cand
            PKG = cand
            return cand

    # 2. Otherwise pick whichever is installed
    for cand in CANDIDATE_PKGS:
        if adb_shell(f"pm path {cand}"):
            _SELECTED_PKG = cand
            PKG = cand
            return cand

    _SELECTED_PKG = PKG
    return PKG


def get_local_so(abi=None):
    if abi is None:
        abi = detect_target_abi()
    abi_so = os.path.join(BUILD_DIR, abi, "libmwr_imgui.so")
    if os.path.exists(abi_so):
        return abi_so, abi
    legacy_so = os.path.join(BUILD_DIR, "libmwr_imgui.so")
    return legacy_so, abi


def ensure_frida_forward():
    adb_cmd(["forward", "tcp:27045", "tcp:27045"])


def push_imgui_so(abi=None, pkg=None):
    """Push libmwr_imgui.so via ADB root when available, or return (remote_so, local_so, stage_needed)."""
    if pkg is None:
        pkg = detect_pkg()
    local_so, detected_abi = get_local_so(abi)
    if not os.path.exists(local_so):
        print(f"[!] Missing {local_so}. Run build.ps1 first.")
        sys.exit(1)

    mtime = int(os.path.getmtime(local_so))
    remote_so = f"/data/data/{pkg}/cache/libmwr_imgui_{mtime}.so"

    if probe_root_cmd() is None:
        # Caller in main() will stage via Frida inside app UID & SELinux context
        return remote_so

    r = adb_cmd(["push", local_so, "/data/local/tmp/libmwr_imgui.so"])
    if r.returncode != 0:
        print(f"[!] adb push failed: {r.stderr}")
        sys.exit(1)

    meta = adb_root_shell(f"stat -c '%u:%g %C' /data/data/{pkg}/cache")
    parts = meta.split()
    uid = parts[0] if len(parts) >= 1 and ":" in parts[0] else "1000:1000"
    ctx = parts[1] if len(parts) >= 2 else "u:object_r:app_data_file:s0"

    adb_root_shell(
        f"rm -f /data/data/{pkg}/cache/libmwr_imgui*.so && "
        f"cp /data/local/tmp/libmwr_imgui.so {remote_so} && "
        f"chown {uid} {remote_so} && chmod 555 {remote_so} && "
        f"chcon {ctx} {remote_so} 2>/dev/null || true"
    )
    print(f"[+] Deployed [{detected_abi}] {remote_so} (owner {uid}, ctx {ctx})")
    return remote_so


def get_game_pid(pkg=None):
    if pkg is None:
        pkg = detect_pkg()
    out = adb_shell(f"pidof {pkg}")
    if not out:
        out = adb_root_shell(f"pidof {pkg}")
    if not out:
        return None
    return int(out.split()[0])


def connect_frida(serial, frida_addr):
    if frida_addr:
        print(f"[*] Connecting Frida remote @ {frida_addr}...")
        return frida.get_device_manager().add_remote_device(frida_addr)

    is_emu = serial.startswith("127.0.0.1:") or serial.startswith("emulator-")
    if is_emu:
        ensure_frida_forward()
        try:
            dev = frida.get_device_manager().add_remote_device("127.0.0.1:27045")
            dev.enumerate_processes()
            return dev
        except Exception:
            pass

    # Real USB phone: try matching ADB serial via Frida USB transport first
    try:
        dev = frida.get_device(serial, timeout=4)
        print(f"[*] Connected via Frida USB ({dev.id})")
        return dev
    except Exception:
        try:
            dev = frida.get_usb_device(timeout=4)
            print(f"[*] Connected via Frida USB ({dev.id})")
            return dev
        except Exception:
            pass

    # Fallback: ADB forward to frida-server TCP 27042 or 27045
    for port in ("27042", "27045"):
        adb_cmd(["forward", f"tcp:{port}", f"tcp:{port}"])
        try:
            dev = frida.get_device_manager().add_remote_device(f"127.0.0.1:{port}")
            dev.enumerate_processes()
            print(f"[*] Connected via Frida TCP forward 127.0.0.1:{port}")
            return dev
        except Exception:
            continue

    print("[!] Cannot reach frida-server. Start root frida-server on the device first.")
    sys.exit(1)


def main():
    global _SELECTED_SERIAL, _SELECTED_PKG, PKG
    parser = argparse.ArgumentParser(description="MWR Dear ImGui Mod Menu Launcher")
    parser.add_argument("--serial", "-s", help="ADB device serial (auto-detected if omitted)")
    parser.add_argument("--pkg", help="Override target package name")
    parser.add_argument("--frida", help="Explicit Frida remote host:port (e.g. 127.0.0.1:27045)")
    parser.add_argument("--abi", choices=["arm64-v8a", "x86_64"], help="Override target ABI")
    args = parser.parse_args()

    if args.serial:
        _SELECTED_SERIAL = args.serial
    if args.pkg:
        _SELECTED_PKG = args.pkg
        PKG = args.pkg

    serial = get_serial()
    pkg = detect_pkg()
    local_so, detected_abi = get_local_so(args.abi)
    print(f"[*] Target ADB device: {serial} | ABI: {detected_abi} | PKG: {pkg}")

    pid = get_game_pid(pkg)
    if not pid:
        print(f"[!] {pkg} is not running. Launch Mini World Royale on the device first.")
        sys.exit(1)

    need_frida_stage = (probe_root_cmd() is None)
    remote_so = push_imgui_so(args.abi, pkg)
    if need_frida_stage:
        print("[*] ADB `su` not enabled for shell (KernelSU/APatch); will stage .so in-process via Frida.")

    frida_addr = args.frida or os.environ.get("MWR_FRIDA_ADDR", "").strip() or None
    device = connect_frida(serial, frida_addr)

    print(f"[*] Attaching to {pkg} (PID {pid}) [client Frida {frida.__version__}]...")
    try:
        session = device.attach(pid)
    except Exception as e:
        print(f"[!] Frida attach failed: {e}")
        print(f"[!] Ensure root `frida-server` matching PC Frida {frida.__version__} is running on {serial}.")
        sys.exit(1)

    with open(SCRIPT_PATH, "r", encoding="utf-8") as f:
        source = f.read().replace(
            "/data/data/com.sea.miniworldroyale.sparkgame/cache/libmwr_imgui.so",
            remote_so,
        )
        if need_frida_stage:
            source = source.replace("waitForStage: false", "waitForStage: true", 1)

    script = session.create_script(source)
    stop = False

    def on_message(msg, data):
        nonlocal stop
        if msg["type"] == "send":
            payload = msg["payload"]
            if isinstance(payload, dict) and payload.get("type") == "kill_menu":
                print("[*] Unload pressed — detaching and exiting...")
                stop = True
            else:
                print("[js]", payload, flush=True)
        elif msg["type"] == "error":
            print("[js-err]", msg.get("description", msg), flush=True)
            if "stack" in msg:
                print(msg["stack"], flush=True)

    script.on("message", on_message)
    script.load()
    if need_frida_stage:
        with open(local_so, "rb") as sf:
            so_bytes = sf.read()
        script.post({"type": "stage_so", "path": remote_so}, so_bytes)
        print(f"[+] Sent {len(so_bytes)} bytes ({detected_abi}) for in-process cache staging.")

    print("[+] Dear ImGui Mod Menu loaded! Tap [MWR] pill in game.")
    print("[*] Press Ctrl+C or tap [Unload] in System tab to cleanly unload.")

    def handle_sig(sig, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    try:
        while not stop:
            time.sleep(0.25)
    finally:
        print("\n[*] Cleaning up Dear ImGui and restoring game memory...")
        try:
            script.exports_sync.cleanup()
            time.sleep(0.3)
        except Exception as e:
            print(f"[!] Cleanup warning: {e}")
        try:
            session.detach()
        except Exception:
            pass
        print("[+] Detached cleanly.")


if __name__ == "__main__":
    main()
