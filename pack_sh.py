#!/usr/bin/env python3
"""Bundle arm64-v8a mwr_inject + libmwr_imgui.so into a single self-contained .sh file."""

import base64
import gzip
import io
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.abspath(__file__))
INJECT_BIN = os.path.join(ROOT, "build", "arm64-v8a", "mwr_inject")
SO_BIN = os.path.join(ROOT, "build", "arm64-v8a", "libmwr_imgui.so")
OUT_SH = os.path.join(ROOT, "build", "mwr_mod.sh")

HEADER = r'''#!/system/bin/sh
# Mini World Royale — All-in-One Standalone Root Launcher & Injector
# Dat file nay vao /data/local/tmp/mwr_mod.sh roi chay bang quyen ROOT:
#   sh /data/local/tmp/mwr_mod.sh

if [ "$(id -u)" -ne 0 ]; then
    echo "[!] Can quyen ROOT! Hay chay script nay bang root (su / MT Manager / KernelSU)."
    exit 1
fi

WORK_DIR="/data/local/tmp/.mwr_work_$$"
cleanup_work() {
    rm -rf "$WORK_DIR" 2>/dev/null || true
}
trap cleanup_work EXIT INT TERM

mkdir -p "$WORK_DIR" || { echo "[!] Khong tao duoc $WORK_DIR"; exit 1; }

MARKER="__MWR_ARCHIVE_BELOW__"
LINE="$(grep -n "^${MARKER}$" "$0" | head -n 1 | cut -d: -f1)"
if [ -z "$LINE" ]; then
    echo "[!] Khong tim thay payload trong $0"
    exit 1
fi

echo "[*] Dang giai nen payload..."
tail -n +"$((LINE + 1))" "$0" | base64 -d | gzip -d | tar -xf - -C "$WORK_DIR"
if [ ! -f "$WORK_DIR/mwr_inject" ] || [ ! -f "$WORK_DIR/libmwr_imgui.so" ]; then
    echo "[!] Giai nen payload that bai."
    exit 1
fi
chmod 755 "$WORK_DIR/mwr_inject"

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
echo "[*] Package: $PKG"

PID="$(pidof "$PKG" | awk '{print $1}')"
if [ -n "$PID" ] && grep -q "libmwr_imgui" "/proc/$PID/maps" 2>/dev/null; then
    echo "[*] Da inject truoc do vao PID $PID — dang restart $PKG de nap ban moi sach..."
    am force-stop "$PKG" >/dev/null 2>&1 || kill -9 "$PID" >/dev/null 2>&1 || true
    sleep 1
    PID=""
fi
if [ -z "$PID" ]; then
    echo "[*] Dang mo game $PKG..."
    MAIN_ACT="$(cmd package resolve-activity --brief "$PKG" 2>/dev/null | tail -n 1)"
    if [ -n "$MAIN_ACT" ] && echo "$MAIN_ACT" | grep -q "/"; then
        am start -n "$MAIN_ACT" >/dev/null 2>&1 || monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    else
        monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    fi

    i=0
    while [ "$i" -lt 40 ]; do
        PID="$(pidof "$PKG" | awk '{print $1}')"
        [ -n "$PID" ] && break
        sleep 0.5
        i=$((i + 1))
    done
fi

if [ -z "$PID" ]; then
    echo "[!] Khong mo duoc game $PKG."
    exit 1
fi
echo "[*] PID: $PID — cho Unity/IL2CPP load..."

ready=0
i=0
while [ "$i" -lt 50 ]; do
    if [ ! -d "/proc/$PID" ]; then
        PID="$(pidof "$PKG" | awk '{print $1}')"
        [ -z "$PID" ] && break
    fi
    if grep -q "libil2cpp.so" "/proc/$PID/maps" 2>/dev/null && \
       grep -q "libunity.so" "/proc/$PID/maps" 2>/dev/null && \
       grep -q "libEGL.so" "/proc/$PID/maps" 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.5
    i=$((i + 1))
done

if [ "$ready" -ne 1 ]; then
    echo "[!] Timeout cho Unity/IL2CPP load trong PID $PID."
    exit 1
fi
sleep 1

CACHE_DIR="/data/data/$PKG/cache"
mkdir -p "$CACHE_DIR" 2>/dev/null || true

META="$(stat -c '%u:%g %C' "$CACHE_DIR" 2>/dev/null)"
OWNER="$(echo "$META" | awk '{print $1}')"
CTX="$(echo "$META" | awk '{print $2}')"
[ -z "$OWNER" ] && OWNER="1000:1000"
[ -z "$CTX" ] && CTX="u:object_r:app_data_file:s0"

TS="$(date +%s)"
DEST_SO="$CACHE_DIR/libmwr_imgui_${TS}.so"

rm -f "$CACHE_DIR"/libmwr_imgui*.so 2>/dev/null || true
cp -f "$WORK_DIR/libmwr_imgui.so" "$DEST_SO" || { echo "[!] Copy .so vao cache that bai."; exit 1; }
chown "$OWNER" "$DEST_SO" 2>/dev/null || true
chmod 555 "$DEST_SO"
chcon "$CTX" "$DEST_SO" 2>/dev/null || true

echo "[*] Staged: $DEST_SO ($OWNER | $CTX)"
"$WORK_DIR/mwr_inject" "$PID" "$DEST_SO"
RC=$?
if [ "$RC" -ne 0 ]; then
    echo "[!] Inject that bai (exit=$RC)."
    exit "$RC"
fi

echo "[+] Xong! Cham nut [MWR] trong game de mo menu."
exit 0
__MWR_ARCHIVE_BELOW__
'''


def main():
    for path in (INJECT_BIN, SO_BIN):
        if not os.path.exists(path):
            print(f"[!] Missing {path}", file=sys.stderr)
            sys.exit(1)

    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        tf.add(INJECT_BIN, arcname="mwr_inject")
        tf.add(SO_BIN, arcname="libmwr_imgui.so")

    gz_bytes = gzip.compress(tar_buf.getvalue(), compresslevel=9)
    b64_bytes = base64.encodebytes(gz_bytes)

    os.makedirs(os.path.dirname(OUT_SH), exist_ok=True)
    with open(OUT_SH, "wb") as f:
        f.write(HEADER.replace("\r\n", "\n").encode("utf-8"))
        f.write(b64_bytes)

    sz = os.path.getsize(OUT_SH)
    print(f"[+] Packed all-in-one script: {OUT_SH} ({sz} bytes)")


if __name__ == "__main__":
    main()
