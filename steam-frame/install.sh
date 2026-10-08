#!/bin/bash
# Installs Moonlight XR for the Steam Frame: copies the APK and launch wrapper to
# ~/Games/MoonlightXR and adds a "Moonlight XR" Steam library entry that runs
# through the Lepton compatibility tool. Steam must be running.
# Usage: steam-frame/install.sh [path/to/MoonlightXR-steam-frame.apk]
# Without an argument it looks in steam-frame/release/ and then at the Gradle output.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APK=${1:-$(ls "$HERE"/release/MoonlightXR-steam-frame*.apk 2>/dev/null | grep -v room-test | head -1)}
[ -f "$APK" ] || APK=$HERE/../app/build/outputs/apk/nonRoot/release/app-nonRoot-release.apk
[ -f "$APK" ] || { echo "usage: $0 path/to/MoonlightXR-steam-frame.apk (download one from the Releases page or run steam-frame/build.sh)"; exit 1; }
DEST=$HOME/Games/MoonlightXR
LEPTON=$HOME/.local/share/Steam/steamapps/common/Lepton
[ -x "$LEPTON/lepton" ] || echo "warning: Lepton not found at $LEPTON; install it from Steam (Library > Tools) before launching"
curl -s --max-time 3 127.0.0.1:8080/json >/dev/null || { echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default; elsewhere start Steam with -cef-enable-debugging)."; exit 1; }

mkdir -p "$DEST/app"
cp "$APK" "$DEST/app/MoonlightXR-frame.apk"
touch "$DEST/app/lepton-show-flatscreen"      # show Moonlight's 2D window in SteamVR
cp "$HERE/moonlightxr-launch.sh" "$DEST/moonlightxr-launch.sh"; chmod +x "$DEST/moonlightxr-launch.sh"
[ -f "$DEST/autostart.conf" ] || cp "$HERE/autostart.conf.example" "$DEST/autostart.conf"
mkdir -p "$DEST/tools"; cp "$HERE/steam-cef.py" "$DEST/tools/steam-cef.py"; chmod +x "$DEST/tools/steam-cef.py"

NAME="Moonlight XR"
EXISTING=$(python3 "$DEST/tools/steam-cef.py" "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2>/dev/null || echo 0)
if [ "$EXISTING" != "0" ] && [ -n "$EXISTING" ]; then
    APPID=$EXISTING
    echo "Steam already has a '$NAME' shortcut (appid $APPID), updating it"
    python3 "$DEST/tools/steam-cef.py" "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$DEST/app/MoonlightXR-frame.apk\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$DEST/app\"'); return 1; })()" >/dev/null
else
    APPID=$(python3 "$DEST/tools/steam-cef.py" "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$DEST/app/MoonlightXR-frame.apk', '$DEST/app', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
fi
python3 "$DEST/tools/steam-cef.py" "(() => { SteamClient.Apps.SetShortcutLaunchOptions($APPID, '$DEST/moonlightxr-launch.sh %command%'); SteamClient.Apps.SpecifyCompatTool($APPID, 'lepton'); return 1; })()" >/dev/null

cat <<MSG

Installed. Steam library entry "$NAME" (appid $APPID) runs $DEST/app/MoonlightXR-frame.apk through Lepton.
Launch it from the library with SteamVR running. Pair your PC in Moonlight's window, then pick a game.
Optional: set PC_UUID in $DEST/autostart.conf to open that PC's game list right away.
Lepton log: ~/.local/share/Steam/logs/lepton-steamlaunch-$APPID.log
MSG
