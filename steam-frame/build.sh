#!/bin/bash
# Builds this checkout of Moonlight XR with a self-contained toolchain (no root
# needed). Works on an arm64 Linux machine such as the Steam Frame itself, or on
# x86_64. Check out the branch you want first (steam-frame, or room-test).
# Usage: steam-frame/build.sh      output: steam-frame/release/MoonlightXR-steam-frame-<branch>.apk
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$(cd "$HERE/.." && pwd)
TOOLS=${MXR_TOOLS:-$HOME/.local/opt/android-build}
ARCH=$(uname -m)
mkdir -p "$TOOLS/dl" "$HERE/release"

fetch() { [ -s "$2" ] || { echo "downloading $(basename "$2")"; curl -L --retry 3 -o "$2" "$1"; }; }

# JDK 21 (Temurin)
if [ ! -x "$TOOLS/jdk/bin/java" ]; then
    case $ARCH in aarch64) A=aarch64;; x86_64) A=x64;; *) echo "unsupported host $ARCH"; exit 1;; esac
    fetch "https://api.adoptium.net/v3/binary/latest/21/ga/linux/$A/jdk/hotspot/normal/eclipse" "$TOOLS/dl/jdk21.tar.gz"
    mkdir -p "$TOOLS/jdk" && tar xzf "$TOOLS/dl/jdk21.tar.gz" -C "$TOOLS/jdk" --strip-components=1
fi
export JAVA_HOME=$TOOLS/jdk ANDROID_HOME=$TOOLS/sdk

# Android SDK command line tools, release 19 (the newer ones ship an x86_64-only binary)
if [ ! -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ]; then
    fetch https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip "$TOOLS/dl/cmdline-tools.zip"
    rm -rf "$ANDROID_HOME/cmdline-tools"; mkdir -p "$ANDROID_HOME/cmdline-tools"
    unzip -q "$TOOLS/dl/cmdline-tools.zip" -d "$TOOLS/cmdline-tmp" && mv "$TOOLS/cmdline-tmp/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest" && rm -rf "$TOOLS/cmdline-tmp"
fi
SDKM=$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager
yes | "$SDKM" --licenses >/dev/null 2>&1 || true
PKGS=("platforms;android-37.0" "build-tools;36.0.0" "build-tools;37.0.0")
GRADLE_EXTRA=()
if [ "$ARCH" = aarch64 ]; then
    # Google ships no arm64 Linux NDK or aapt2; these community builds fill in.
    NDK_VER=30.0.16248370
    if [ ! -d "$ANDROID_HOME/ndk/$NDK_VER" ]; then
        fetch https://github.com/HomuHomu833/android-ndk-custom/releases/download/r30/android-ndk-r30-aarch64-linux-gnu.tar.xz "$TOOLS/dl/ndk-r30-aarch64.tar.xz"
        mkdir -p "$TOOLS/ndk" && tar xJf "$TOOLS/dl/ndk-r30-aarch64.tar.xz" -C "$TOOLS/ndk"
        mkdir -p "$ANDROID_HOME/ndk" && ln -sfn "$TOOLS/ndk/android-ndk-r30" "$ANDROID_HOME/ndk/$NDK_VER"
    fi
    if [ ! -x "$TOOLS/tools/build-tools/aapt2" ]; then
        fetch https://github.com/lzhiyong/android-sdk-tools/releases/download/35.0.2/android-sdk-tools-static-aarch64.zip "$TOOLS/dl/sdk-tools-aarch64.zip"
        mkdir -p "$TOOLS/tools" && unzip -qo "$TOOLS/dl/sdk-tools-aarch64.zip" -d "$TOOLS/tools"
    fi
    # The old aapt2's optimize step silently drops the manifest, so it is skipped
    GRADLE_EXTRA=(-PmoonlightNdkVersion=$NDK_VER -Pandroid.aapt2FromMavenOverride="$TOOLS/tools/build-tools/aapt2" -Pandroid.enableResourceOptimizations=false)
else
    PKGS+=("ndk;29.0.14206865")
fi
"$SDKM" --install "${PKGS[@]}" >/dev/null

cd "$SRC"
git submodule update --init --recursive -q

# A local signing key, generated once
SIGN=$HERE/signing; mkdir -p "$SIGN"
if [ ! -f "$SIGN/local.jks" ]; then
    PW=$(head -c 24 /dev/urandom | base64 | tr -d '/+=' | head -c 24)
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$SIGN/local.jks" -storepass "$PW" -keypass "$PW" -alias moonlightxr -keyalg RSA -keysize 2048 -validity 10950 -dname "CN=Moonlight XR Steam Frame build" >/dev/null 2>&1
    printf '%s' "$PW" > "$SIGN/password"; chmod 600 "$SIGN/password" "$SIGN/local.jks"
fi
printf 'storeFile=%s\nstorePassword=%s\nkeyAlias=moonlightxr\nkeyPassword=%s\n' "$SIGN/local.jks" "$(cat "$SIGN/password")" "$(cat "$SIGN/password")" > keystore.properties
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > local.properties

./gradlew --no-daemon assembleNonRootRelease "${GRADLE_EXTRA[@]}"
BR=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo build)
OUT=$HERE/release/MoonlightXR-steam-frame-$BR.apk
cp app/build/outputs/apk/nonRoot/release/app-nonRoot-release.apk "$OUT"
echo "built $OUT"
