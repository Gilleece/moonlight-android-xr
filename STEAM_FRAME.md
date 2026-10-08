# Moonlight XR on the Steam Frame

This branch makes Moonlight XR run on Valve's Steam Frame, where Android apps run inside **Lepton**, Valve's Android container, with SteamVR as the OpenXR runtime. Prebuilt APKs, a Steam library installer and the launch wrapper live in the companion repository: https://github.com/ajbeavers/moonlight-xr-steam-frame. This file is about building this branch yourself, on the Frame or on a Linux PC.

## What the branch changes

- `PreferenceConfiguration.isLepton()`: Lepton reports manufacturer `Valve`, model `Lepton`, and declares no VR system feature. It now counts as a headset, so the immersive activity is used.
- `MediaCodecHelper`: Lepton's Android image only has the AOSP software decoders (`OMX.google.*`), so the software decoder block is lifted on Lepton, and the Android 11 low latency key is never offered to AOSP software decoders. They reject it, and on this image the failed configure crashes the process in `ACodec::LoadedState::onShutdown`.
- `xr_input.c`, `xr_gamepad.*`, `xr_shared.h`: the Steam Frame controllers are bound through `XR_VALVE_frame_controller_interaction` and `/interaction_profiles/valve/frame_controller` instead of SteamVR's translation to Touch controllers. Gamepad mode lays them out as an Xbox pad: right A/B/X/Y, right menu = Start, left d-pad, left view = Back, bumpers or grips = LB/RB. The packet gains the d-pad and Back bits.
- `ControllerHandler.reportXrPad`: the flip face buttons setting applies to the VR gamepad.

The `room-test` branch adds one experimental commit: SteamVR allows a session 16 swapchains and this app makes 16 before a room is picked, so on SteamVR it leaves out the splash, toasts, report sheet, Ko-fi sheet, clock, hand hint, glow, outline and corner handles to make room for the room's swapchain.

## Building on the Frame (arm64 Linux, no root)

Google ships no Linux arm64 NDK or `aapt2`, and the newest Android command line tools include an x86_64-only binary, so the toolchain is assembled from these parts. All of it unpacks under one directory and needs about 3 GB.

| Part | Source |
|---|---|
| JDK 21 | Temurin, `OpenJDK21U-jdk_aarch64_linux_hotspot` |
| Android command line tools | `commandlinetools-linux-13114758_latest.zip` (release 19.0, pure Java) |
| SDK packages | `platforms;android-37.0`, `build-tools;36.0.0`, `build-tools;37.0.0` via `sdkmanager` |
| NDK | HomuHomu833/android-ndk-custom release r30, `android-ndk-r30-aarch64-linux-gnu.tar.xz`, symlinked as `$ANDROID_HOME/ndk/30.0.16248370` |
| aapt2 | lzhiyong/android-sdk-tools release 35.0.2, `android-sdk-tools-static-aarch64.zip` |

Then, with `JAVA_HOME` and `ANDROID_HOME` pointing at those and a `keystore.properties` in place (see `keystore.properties.example`):

```sh
git submodule update --init --recursive
./gradlew --no-daemon assembleNonRootRelease \
    -PmoonlightNdkVersion=30.0.16248370 \
    -Pandroid.aapt2FromMavenOverride=/path/to/android-sdk-tools/build-tools/aapt2 \
    -Pandroid.enableResourceOptimizations=false
```

`moonlightNdkVersion` overrides the NDK the build asks for (`app/build.gradle` reads it). The resource optimisation step must be off with that `aapt2`: its `optimize` command silently produces no output, and the APK then has no manifest. The build takes a couple of minutes on the Frame. `scripts/build.sh` in the companion repository does all of the above unattended.

## Building on an x86_64 Linux PC

Nothing special: the standard Android SDK with NDK `29.0.14206865` (what `app/build.gradle` asks for) and `./gradlew assembleNonRootRelease`, as in the upstream README. The output runs on the Frame the same way.

## Installing on the Frame

Lepton runs whatever `*.apk` sits in a Steam shortcut's folder. The shortcut's executable is the APK itself, its compatibility tool is `lepton`, and a file named `lepton-show-flatscreen` next to the APK makes Moonlight's 2D window visible in SteamVR, which is where pairing and game selection happen. The companion repository's `scripts/install.sh` sets this up through Steam's local debugging port, and its launch wrapper can open a PC's game list as soon as the app is up.

## Things to know

- H.264 only, decoded in software on the CPU. Keep the bitrate around 40 Mbps or less at 1440p; above that the decoder falls behind and the app requests a keyframe every half second.
- The depth model for 3D runs on the CPU (the TensorFlow Lite GPU delegate fails under Lepton: no OpenCL, and the GL path lacks an op), at roughly 5 to 8 maps a second.
- SteamVR's runtime offers `XR_KHR_opengl_es_enable`, `XR_KHR_android_create_instance` and `XR_VALVE_frame_controller_interaction`, but not `XR_KHR_composition_layer_cylinder`, `XR_KHR_composition_layer_color_scale_bias` or `XR_FB_composition_layer_settings`, and it caps swapchains at 16 per session.
- Lepton's logs: `~/.local/share/Steam/logs/lepton-steamlaunch-<appid>.log`, with logcat dumps in `~/.local/share/Steam/logs/lepton-logcats/` when the app exits early.

The work on this branch was done with Anthropic's Claude (Claude Fable 5.1 and Claude Opus 5.5 through Claude Code) and tested on a Steam Frame; see the companion repository's README.
