#!/bin/sh
# ==============================================================================
# Aqara Magic Switch S1E - Turnkey Custom GUI Installer
# Repository: https://github.com/vvlang/AqaraS1E-Custom-GUI (fork of oopuuu/)
# ==============================================================================

set -euo pipefail

echo "========================================================"
echo "    Aqara S1E Standalone Custom GUI Installer"
echo "========================================================"

TARGET_DIR="/data/scripts"
mkdir -p "$TARGET_DIR"

# ---------------------------------------------------------------------------
# 1. Locate or prepare working curl
# ---------------------------------------------------------------------------
CURL_EXEC=""
if [ -x /tmp/curl ]; then
    CURL_EXEC="/tmp/curl"
elif [ -x /data/scripts/curl ]; then
    CURL_EXEC="/data/scripts/curl"
elif command -v curl >/dev/null 2>&1; then
    CURL_EXEC="curl"
else
    echo "[*] BusyBox wget does not support HTTPS. Downloading portable curl via HTTP..."
    wget -O /tmp/curl "http://master.dl.sourceforge.net/project/aqarahub/binutils/curl?viasf=1" 2>/dev/null || true
    chmod a+x /tmp/curl 2>/dev/null || true
    if [ -x /tmp/curl ]; then
        CURL_EXEC="/tmp/curl"
    fi
fi

if [ -z "$CURL_EXEC" ]; then
    echo "[-] Error: Failed to acquire HTTPS capable curl utility."
    echo "    Please run: wget -O /tmp/curl http://master.dl.sourceforge.net/project/aqarahub/binutils/curl?viasf=1 && chmod +x /tmp/curl"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Stop running services if any
# ---------------------------------------------------------------------------
echo "[1/6] Stopping existing processes..."
killall -9 aqgui gui_monitor.sh app_monitor.sh s1e_standalone_app ha_daemon httpd 2>/dev/null || true
sleep 1

# ---------------------------------------------------------------------------
# 3. Backup existing user configs (explicit file list, not glob-based)
# ---------------------------------------------------------------------------
BACKUP_DIR="/data/scripts_backup_$(date +%s)"
USER_CONFIGS="layout_p0.json sys_settings.json ha_config.json ha_cards.json lock_text.json notify_history.json energy_stats.json passwd .gesture_guide_seen"
HAS_CFG=0
for CFG in $USER_CONFIGS; do
    if [ -f "$TARGET_DIR/$CFG" ]; then
        HAS_CFG=1
        break
    fi
done

if [ "$HAS_CFG" -eq 1 ]; then
    echo "[2/6] Backing up existing user configurations to $BACKUP_DIR..."
    mkdir -p "$BACKUP_DIR"
    for CFG in $USER_CONFIGS; do
        if [ -f "$TARGET_DIR/$CFG" ]; then
            # -f (not -r) avoids accidentally copying directories of the same name.
            cp -f "$TARGET_DIR/$CFG" "$BACKUP_DIR/" 2>/dev/null || true
        fi
    done
fi

# ---------------------------------------------------------------------------
# 4. Download and verify release tarball via here-doc URL list
# ---------------------------------------------------------------------------
echo "[3/6] Downloading latest release bundle via $CURL_EXEC..."
LOCAL_TAR="/tmp/s1e_custom_gui_latest.tar.gz"
LOCAL_SHA="/tmp/s1e_custom_gui_latest.tar.gz.sha256"

# Mirror order: vvlang fork first (since vvlang forked oopuuu to maintain its own
# customized build), oopuuu upstream as fallback. Multi-protocol so any path works
# in restricted networks.
DOWNLOAD_OK=0
while read -r U; do
    [ -z "$U" ] && continue
    echo "[*] Trying download source: $U"
    rm -f "$LOCAL_TAR" "$LOCAL_SHA"
    if ! $CURL_EXEC -s -k -L --connect-timeout 10 -m 90 -o "$LOCAL_TAR" "$U"; then
        echo "[!] Download failed, trying next mirror."
        continue
    fi
    if [ ! -s "$LOCAL_TAR" ] || ! tar -tzf "$LOCAL_TAR" >/dev/null 2>&1; then
        echo "[!] Invalid tarball, trying next mirror."
        continue
    fi

    # Optional SHA256 verification: only if mirror ships a sidecar .sha256 file.
    SHA_OK=1
    SHA_URL="${U}.sha256"
    if $CURL_EXEC -s -k -L --connect-timeout 10 -m 30 -o "$LOCAL_SHA" "$SHA_URL" 2>/dev/null && [ -s "$LOCAL_SHA" ]; then
        EXPECTED=$(awk 'NF>=1 {print $1; exit}' "$LOCAL_SHA")
        ACTUAL=$(sha256sum "$LOCAL_TAR" | awk '{print $1}')
        if [ -n "$EXPECTED" ] && [ "$EXPECTED" = "$ACTUAL" ]; then
            echo "[+] SHA256 verified."
        else
            echo "[!] SHA256 mismatch (expected ${EXPECTED:-empty}, got $ACTUAL), trying next mirror."
            SHA_OK=0
        fi
    else
        echo "[*] sha256sum.txt not provided by this mirror, skipping verification."
    fi

    if [ "$SHA_OK" -eq 1 ]; then
        DOWNLOAD_OK=1
        echo "[+] Successfully downloaded and verified release archive."
        break
    fi
done <<'EOF'
https://ghfast.top/https://raw.githubusercontent.com/vvlang/AqaraS1E-Custom-GUI/main/release/s1e_custom_gui_latest.tar.gz
https://ghfast.top/https://raw.githubusercontent.com/oopuuu/AqaraS1E-Custom-GUI/main/release/s1e_custom_gui_latest.tar.gz
https://cdn.jsdelivr.net/gh/vvlang/AqaraS1E-Custom-GUI@latest/release/s1e_custom_gui_latest.tar.gz
https://cdn.jsdelivr.net/gh/oopuuu/AqaraS1E-Custom-GUI@latest/release/s1e_custom_gui_latest.tar.gz
https://raw.githubusercontent.com/vvlang/AqaraS1E-Custom-GUI/main/release/s1e_custom_gui_latest.tar.gz
https://raw.githubusercontent.com/oopuuu/AqaraS1E-Custom-GUI/main/release/s1e_custom_gui_latest.tar.gz
EOF

if [ "$DOWNLOAD_OK" -ne 1 ]; then
    echo "[-] Error: Failed to download valid release archive from any mirror!"
    rm -f "$LOCAL_TAR" "$LOCAL_SHA"
    exit 1
fi

# ---------------------------------------------------------------------------
# 5. Extract release tarball
# ---------------------------------------------------------------------------
echo "[4/6] Extracting assets to $TARGET_DIR..."
cd "$TARGET_DIR"
tar -zxvf "$LOCAL_TAR" -C "$TARGET_DIR"
rm -f "$LOCAL_TAR" "$LOCAL_SHA"

# Persist curl tool so subsequent OTA / scripted updates keep HTTPS capability.
if [ -f /tmp/curl ]; then
    cp -f /tmp/curl "$TARGET_DIR/curl" 2>/dev/null || true
    chmod +x "$TARGET_DIR/curl" 2>/dev/null || true
fi

# Restore user configuration (explicit per-file loop, no `/.*` glob which would
# match `.` and `..` and recursively copy the entire filesystem).
if [ -d "$BACKUP_DIR" ]; then
    echo "[*] Restoring user configurations..."
    for CFG in $USER_CONFIGS; do
        if [ -f "$BACKUP_DIR/$CFG" ]; then
            cp -f "$BACKUP_DIR/$CFG" "$TARGET_DIR/" 2>/dev/null || true
        fi
    done
    rm -rf "$BACKUP_DIR"
fi

# ---------------------------------------------------------------------------
# 6. Set executable permissions
# ---------------------------------------------------------------------------
chmod +x "$TARGET_DIR/s1e_standalone_app" \
         "$TARGET_DIR/ha_daemon" \
         "$TARGET_DIR/api.cgi" \
         "$TARGET_DIR/snap_fast" \
         "$TARGET_DIR/http_body_strip" \
         "$TARGET_DIR/safe_png_recv" \
         "$TARGET_DIR/fetch_https.sh" \
         "$TARGET_DIR/dump_incident.sh" \
         "$TARGET_DIR/post_init.sh" \
         "$TARGET_DIR/curl" 2>/dev/null || true
chmod +x "$TARGET_DIR"/*.sh 2>/dev/null || true

# ---------------------------------------------------------------------------
# 7. Zero-Reboot Hot Startup
# ---------------------------------------------------------------------------
echo "[5/6] Launching standalone GUI & web server (zero-reboot hot startup)..."

# 7.1 Setup Web Server — clean stale /tmp/www files first to avoid leftovers
#     from a previous install corrupting the new index.html / api.cgi.
mkdir -p /tmp/www/cgi-bin
rm -f /tmp/www/index.html /tmp/www/cgi-bin/api.cgi /tmp/snap_fast
cp -f "$TARGET_DIR/index.html" /tmp/www/index.html 2>/dev/null || true
cp -f "$TARGET_DIR/api.cgi" /tmp/www/cgi-bin/api.cgi 2>/dev/null || true
cp -f "$TARGET_DIR/snap_fast" /tmp/snap_fast 2>/dev/null || true
chmod +x /tmp/www/cgi-bin/api.cgi /tmp/snap_fast 2>/dev/null || true
killall httpd 2>/dev/null || true
sleep 1
httpd -p 8080 -h /tmp/www

# 7.2 Hardware Backlight Initialization
if [ ! -d /sys/class/pwm/pwmchip0/pwm0 ]; then
    echo 0 > /sys/class/pwm/pwmchip0/export 2>/dev/null || true
fi
echo 200000 > /sys/class/pwm/pwmchip0/pwm0/period 2>/dev/null || true
echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable 2>/dev/null || true
echo 200000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle 2>/dev/null || true

# 7.3 Disable stock GUI daemon restart — back up original setting first.
if [ -f /data/app/config/setting ] && [ ! -f /data/app/config/setting.bak ]; then
    cp -f /data/app/config/setting /data/app/config/setting.bak 2>/dev/null || true
fi
sed -i "s/option enable 'true'/option enable 'false'/" /data/app/config/setting 2>/dev/null || true
sed -i "s/option screenAlwaysOn 'false'/option screenAlwaysOn 'true'/" /data/app/config/setting 2>/dev/null || true

# 7.4 Launch core daemons (immune to SIGHUP via nohup)
killall -9 s1e_standalone_app ha_daemon 2>/dev/null || true
sleep 1
nohup "$TARGET_DIR/ha_daemon" >/tmp/ha_daemon.log 2>&1 &
nohup "$TARGET_DIR/s1e_standalone_app" >/tmp/app.log 2>&1 &

# 7.5 Launch post_init background supervisor
if ! ps | grep -v grep | grep -q "post_init.sh"; then
    nohup sh "$TARGET_DIR/post_init.sh" >/tmp/post_init_bg.log 2>&1 &
fi

# ---------------------------------------------------------------------------
# 8. Verify processes are actually responding (process-existence alone is not
#    enough — also probe the web server before declaring success).
# ---------------------------------------------------------------------------
echo "[6/6] Verifying..."

APP_OK=0
HTTP_OK=0
# Wait up to 5 seconds for both app and web server to settle.
for _i in 1 2 3 4 5; do
    sleep 1
    if ps | grep -v grep | grep -q "s1e_standalone_app"; then
        APP_OK=1
    fi
    # Best-effort HTTP probe (silently ignored if curl not available).
    if $CURL_EXEC -s -o /dev/null -w "%{http_code}" --connect-timeout 2 http://127.0.0.1:8080/ 2>/dev/null | grep -q "^2"; then
        HTTP_OK=1
    fi
    if [ "$APP_OK" -eq 1 ] && [ "$HTTP_OK" -eq 1 ]; then
        break
    fi
done

# Determine device IP — try Wi-Fi first, then wired fallback.
DEV_IP=""
for IFACE in wlan0 eth0; do
    DEV_IP=$(ip -4 addr show "$IFACE" 2>/dev/null | awk '/inet / {print $2; exit}' | cut -d/ -f1 || true)
    if [ -n "$DEV_IP" ]; then
        break
    fi
done
DEV_IP="${DEV_IP:-192.168.x.x}"

echo "========================================================"
if [ "$APP_OK" -eq 1 ]; then
    echo " [SUCCESS] Custom GUI installed and running!"
else
    echo " [WARNING] s1e_standalone_app did not start. Check /tmp/app.log"
fi
echo " -> Device Web Console: http://${DEV_IP}:8080/"
if [ "$HTTP_OK" -ne 1 ]; then
    echo " [WARNING] Web server on port 8080 did not respond. Try restarting manually."
fi
echo "========================================================"
