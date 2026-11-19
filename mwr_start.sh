#!/system/bin/sh
# Mini World Royale — Standalone Root Launcher + Injector
# Chạy trực tiếp trên điện thoại với quyền root:
#   su -c "sh /data/local/tmp/mwr_start.sh"

TMP_DIR="/data/local/tmp"
SRC_SO="$TMP_DIR/libmwr_imgui.so"
INJECT_BIN="$TMP_DIR/mwr_inject"

if [ "$(id -u)" -ne 0 ]; then
    echo "[!] Can quyen ROOT! Hay chay script nay bang root (su / MT Manager / Termux su)."
    exit 1
fi

if [ ! -f "$SRC_SO" ]; then
    echo "[!] Thieu $SRC_SO"
    exit 1
fi

if [ ! -f "$INJECT_BIN" ]; then
    echo "[!] Thieu $INJECT_BIN"
    exit 1
fi
chmod 755 "$INJECT_BIN" 2>/dev/null || true

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
echo "[*] PID: $PID — cho libil2cpp.so + libunity.so + libEGL.so load..."

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
if [ ! -d "$CACHE_DIR" ]; then
    mkdir -p "$CACHE_DIR"
fi

META="$(stat -c '%u:%g %C' "$CACHE_DIR" 2>/dev/null)"
OWNER="$(echo "$META" | awk '{print $1}')"
CTX="$(echo "$META" | awk '{print $2}')"
[ -z "$OWNER" ] && OWNER="1000:1000"
[ -z "$CTX" ] && CTX="u:object_r:app_data_file:s0"

TS="$(date +%s)"
DEST_SO="$CACHE_DIR/libmwr_imgui_${TS}.so"

rm -f "$CACHE_DIR"/libmwr_imgui*.so 2>/dev/null || true
cp -f "$SRC_SO" "$DEST_SO" || { echo "[!] Copy .so vao cache that bai."; exit 1; }
chown "$OWNER" "$DEST_SO" 2>/dev/null || true
chmod 555 "$DEST_SO"
chcon "$CTX" "$DEST_SO" 2>/dev/null || true

echo "[*] Staged: $DEST_SO ($OWNER | $CTX)"
"$INJECT_BIN" "$PID" "$DEST_SO"
RC=$?
if [ "$RC" -ne 0 ]; then
    echo "[!] Inject that bai (exit=$RC)."
    exit "$RC"
fi

echo "[+] Xong! Cham nut [MWR] trong game de mo menu."
