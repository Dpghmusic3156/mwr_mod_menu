#!/usr/bin/env python3
"""Bundle arm64-v8a external Read-Only overlay binary (mwr_ext) into a single .sh."""

import base64
import gzip
import io
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.abspath(__file__))
EXT_BIN = os.path.join(ROOT, "build", "arm64-v8a", "mwr_ext")
OUT_SH = os.path.join(ROOT, "build", "mwr_ext.sh")

HEADER = r'''#!/system/bin/sh
# Mini World Royale — External Read-Only ESP Overlay (Zero Game Injection)
# Chay bang quyen ROOT:
#   sh /data/local/tmp/mwr_ext.sh

if [ "$(id -u)" -ne 0 ]; then
    echo "[!] Can quyen ROOT! Hay chay script nay bang root (su / MT Manager / KernelSU)."
    exit 1
fi

pkill -9 -f "/data/local/tmp/.mwr_ext_" 2>/dev/null || true
rm -rf /data/local/tmp/.mwr_ext_* 2>/dev/null || true

WORK_DIR="/data/local/tmp/.mwr_ext_$$"
cleanup_work() {
    rm -rf "$WORK_DIR" 2>/dev/null || true
}
trap cleanup_work EXIT INT TERM

mkdir -p "$WORK_DIR" || { echo "[!] Khong tao duoc $WORK_DIR"; exit 1; }

MARKER="__MWR_EXT_ARCHIVE__"
LINE="$(grep -n "^${MARKER}$" "$0" | head -n 1 | cut -d: -f1)"
if [ -z "$LINE" ]; then
    echo "[!] Khong tim thay payload trong $0"
    exit 1
fi

tail -n +"$((LINE + 1))" "$0" | base64 -d | gzip -d | tar -xf - -C "$WORK_DIR"
if [ ! -f "$WORK_DIR/mwr_ext" ]; then
    echo "[!] Giai nen mwr_ext that bai."
    exit 1
fi
chmod 755 "$WORK_DIR/mwr_ext"

PKG="${1:-}"
if [ -z "$PKG" ]; then
    for cand in com.vn.miniworld.royale.sparkgame com.sea.miniworldroyale.sparkgame; do
        if pm path "$cand" >/dev/null 2>&1; then
            PKG="$cand"
            break
        fi
    done
fi

if [ -z "$PKG" ]; then
    echo "[!] Khong tim thay Mini World Royale (VN/SEA) tren may."
    exit 1
fi

PID="$(pidof "$PKG" | awk '{print $1}')"
if [ -z "$PID" ]; then
    echo "[*] Dang mo game $PKG..."
    MAIN_ACT="$(cmd package resolve-activity --brief "$PKG" 2>/dev/null | tail -n 1)"
    if [ -n "$MAIN_ACT" ] && echo "$MAIN_ACT" | grep -q "/"; then
        am start -n "$MAIN_ACT" >/dev/null 2>&1 || monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    else
        monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    fi
fi

echo "[*] Khoi chay External Read-Only Overlay cho $PKG (khong inject vao game)..."
"$WORK_DIR/mwr_ext" "$PKG"
exit $?
__MWR_EXT_ARCHIVE__
'''


def main():
    if not os.path.exists(EXT_BIN):
        print(f"[!] Missing {EXT_BIN}", file=sys.stderr)
        sys.exit(1)

    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        tf.add(EXT_BIN, arcname="mwr_ext")

    gz_bytes = gzip.compress(tar_buf.getvalue(), compresslevel=9)
    b64_bytes = base64.encodebytes(gz_bytes)

    os.makedirs(os.path.dirname(OUT_SH), exist_ok=True)
    with open(OUT_SH, "wb") as f:
        f.write(HEADER.encode("utf-8"))
        f.write(b64_bytes)

    sz = os.path.getsize(OUT_SH)
    print(f"[+] Packed external script: {OUT_SH} ({sz} bytes)")


if __name__ == "__main__":
    main()
