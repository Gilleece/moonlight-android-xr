#!/bin/bash
# Steam launch wrapper for the Moonlight XR shortcut (Lepton compatibility tool).
# Steam launch options: /home/<you>/Games/MoonlightXR/moonlightxr-launch.sh %command%
#
# Lepton boots Android and starts Moonlight's 2D launcher. Once that is up this
# optionally opens a PC's game list, or a stream, through the app's own shortcut
# trampoline, so no clicking around in the 2D window is needed. See autostart.conf.
ADB=/usr/lib/android-sdk/platform-tools/adb
SAVED_LD_PRELOAD=$LD_PRELOAD
unset LD_PRELOAD   # Steam's overlay preload only spams errors for adb
PKG=com.gilleece.moonlightxr
HERE=$(dirname "$(readlink -f "$0")")
LOG=/tmp/moonlightxr-launch.log
PC_UUID=; APP_ID=; APP_NAME=
[ -f "$HERE/autostart.conf" ] && . "$HERE/autostart.conf"
# Per-shortcut overrides from Steam launch options, e.g.
#   MOONLIGHT_XR_APP_ID=1234 /home/<you>/Games/MoonlightXR/moonlightxr-launch.sh %command%
PC_UUID=${MOONLIGHT_XR_PC_UUID:-$PC_UUID}
APP_ID=${MOONLIGHT_XR_APP_ID:-$APP_ID}
APP_NAME=${MOONLIGHT_XR_APP_NAME:-$APP_NAME}

# Optional: pairing imported by seed-from-moonlight-qt.sh, copied into the app's
# data on the first launch of a shortcut. Lepton mounts
# $STEAM_COMPAT_DATA_PATH/internal/<package> as the app's data directory.
if [ -n "$STEAM_COMPAT_DATA_PATH" ] && [ -f "$HERE/seed/client.crt" ]; then
    D=$STEAM_COMPAT_DATA_PATH/internal/$PKG
    if [ ! -f "$D/files/client.crt" ]; then
        mkdir -p "$D/files" "$D/databases"
        cp "$HERE/seed/client.crt" "$HERE/seed/client.key" "$HERE/seed/uniqueid" "$D/files/"
        [ -f "$D/databases/computers4.db" ] || cp "$HERE/seed/computers4.db" "$D/databases/"
        chmod 600 "$D/files/"*
        echo "$(date +%T) seeded pairing data into $D" >> "$LOG"
    fi
fi

(
    echo "$(date +%T) waiting for $PKG"
    for _ in $(seq 1 480); do
        sleep 0.5
        timeout 5 "$ADB" connect localhost:5555 >/dev/null 2>&1
        if timeout 5 "$ADB" -s localhost:5555 shell dumpsys window 2>/dev/null \
            | grep -q "mCurrentFocus=.*${PKG}/"; then
            echo "$(date +%T) app focused"
            if [ -n "$PC_UUID" ]; then
                sleep 1
                ARGS=(--es UUID "$PC_UUID")
                [ -n "$APP_ID" ] && ARGS+=(--es AppId "$APP_ID")
                [ -n "$APP_NAME" ] && ARGS+=(--es AppName "$APP_NAME")
                timeout 10 "$ADB" -s localhost:5555 shell am start -n "$PKG/com.limelight.ShortcutTrampoline" "${ARGS[@]}"
                echo "$(date +%T) trampoline started: ${ARGS[*]}"
            fi
            exit 0
        fi
    done
    echo "$(date +%T) app never came up"
) >"$LOG" 2>&1 &

[ -n "$SAVED_LD_PRELOAD" ] && export LD_PRELOAD=$SAVED_LD_PRELOAD
exec "$@"
