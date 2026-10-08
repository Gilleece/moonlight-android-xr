# Moonlight XR on the Steam Frame

[Moonlight XR](https://github.com/Gilleece/moonlight-android-xr) shows a Sunshine/Apollo game stream as a big stereoscopic 3D screen in a headset. The stock APK does nothing useful on Valve's Steam Frame, because the Frame runs Android apps through **Lepton**, Valve's Android container, and Lepton looks nothing like a Quest or a Pico to the app. This fork makes it work there; the `steam-frame` branch is the one to use.

Tested on a Steam Frame with SteamOS and Lepton 2.8.x, streaming from Sunshine over Wi-Fi, in October 2026. Not affiliated with Valve or the Moonlight projects.

## AI disclosure

The code changes, the scripts and this document were written by an AI: Anthropic's Claude, as Claude Fable 5.1 and Claude Opus 5.5, running through Claude Code on the Steam Frame itself. The human owner of this repository decided what to attempt, launched every build in the headset, and reported what worked and what did not; the AI read the logs and changed the code accordingly. Nothing here has been reviewed by the upstream Moonlight XR maintainer. Treat it as a community port, read the code before trusting it, and expect rough edges. The "How this was made" section at the end has more detail.

## Status

Confirmed working on the Frame, in the headset:

- Streaming in VR with the 3D depth effect, from the PC list, pairing and game selection in Moonlight's 2D window.
- The room environments (Home Theater, Grand Cinema, Synthwave) and the void, with their brightness and glow settings, and resizing the picture by its corners.
- The Steam Frame controllers in gamepad mode, laid out as an Xbox pad (table below), and the app's own "flip face buttons" setting on them.
- The pointer, the bar below the screen and its panels.

Does not work:

- **Passthrough.** The runtime says it supports the blend mode, the app switches to it, and the view stays opaque. Pick the void or a room instead.

Not tested: hand tracking, pointing by eye gaze, HDR.

## Credit

Everything that is the app is **Moonlight XR by Gilleece**, itself a fork of [Moonlight for Android](https://github.com/moonlight-stream/moonlight-android). The changes here only make it start, decode and take input on the Frame. The upstream request for Steam Frame support is [issue #38](https://github.com/Gilleece/moonlight-android-xr/issues/38).

## What the branch changes

1. **Headset detection** (`PreferenceConfiguration.isLepton()`): Lepton reports manufacturer `Valve`, model `Lepton`, with no VR system feature, so the app took its flat 2D path. Lepton now counts as a headset, which puts the stream on the immersive OpenXR activity (SteamVR is the OpenXR runtime inside Lepton).
2. **Software video decoding** (`MediaCodecHelper`): Lepton's Android image has only the AOSP software decoders (`OMX.google.*`), which Moonlight refuses by default. They are allowed on Lepton.
3. **No crash on decoder setup**: the app asks every decoder for Android 11 low-latency mode first. The AOSP software decoder rejects that, and on this Android image the failed configure then crashes the process in `ACodec::LoadedState::onShutdown`. The key is never offered to AOSP software decoders.
4. **Steam Frame controllers** (`xr_input.c`, `xr_gamepad.*`): bound through `XR_VALVE_frame_controller_interaction` and `/interaction_profiles/valve/frame_controller` instead of SteamVR's translation into Quest Touch buttons, and laid out as one Xbox pad in gamepad mode, d-pad and Back included.
5. **Face button swap** (`ControllerHandler.reportXrPad`): the app's "flip face buttons" setting applies to the VR gamepad too.
6. **No Android bars** (`UiHelper`): on Lepton the 2D activities hide the status and navigation bars, since they are only in the way on a flat panel in SteamVR.
7. **Depth behind the decoder** (`XrRenderer`): on Lepton the depth model's threads run at background priority, so the software video decoder gets the CPU first. A late depth map only smears the 3D; a late video frame is a hitch and a keyframe request.
8. **Rooms under SteamVR's swapchain limit** (`xr_session.c`, `xr_assets.c`): SteamVR allows a session 16 swapchains and the app made 16 before a room was picked, so the room's swapchain failed and the environments never appeared. On SteamVR the splash, toasts, report sheet, Ko-fi sheet, clock, hand hint and glow are left out, which makes room for the room and for the corner resize handles.

## Install

You need a Steam Frame with Lepton installed from Steam (Library → Tools), Sunshine or Apollo on the PC, and SteamVR running on the Frame.

1. Download `MoonlightXR-steam-frame-*.apk` from the Releases page, or build it (below).
2. On the Frame, in a terminal (SSH, or Konsole from desktop mode):

   ```sh
   git clone https://github.com/ajbeavers/moonlight-android-xr-steam-frame.git
   cd moonlight-android-xr-steam-frame
   steam-frame/install.sh ~/Downloads/MoonlightXR-steam-frame-v0.3.apk
   ```

   The installer copies the APK and a launch wrapper into `~/Games/MoonlightXR/`, then adds a **Moonlight XR** entry to your Steam library set to the Lepton compatibility tool. Steam must be running; the installer talks to it over its local debugging port, which is on by default on SteamOS.
3. Put the headset on and launch **Moonlight XR** from the library. The first launch takes about half a minute while Lepton installs the APK.
4. Moonlight's normal 2D window appears as a flat panel in SteamVR. Add your PC and pair it there, as on a phone, then pick a game. The stream opens as the immersive 3D screen.

To skip the PC list on every launch, put your PC's UUID in `~/Games/MoonlightXR/autostart.conf` (the installer leaves a commented example). If you already use Moonlight (the Qt desktop app) on the Frame, `steam-frame/seed-from-moonlight-qt.sh` copies its pairing into Moonlight XR so no pairing step is needed.

## Controls

In the stream, hold the left **View** button and the left grip for half a second to switch between the pointer and gamepad mode.

| Steam Frame | Gamepad |
|---|---|
| Right A, B, X, Y | A, B, X, Y |
| Left d-pad | D-pad |
| Left View | Back |
| Right Menu | Start |
| Bumpers (or grips) | LB, RB |
| Triggers, sticks, stick clicks | LT, RT, sticks, L3, R3 |

In pointer mode the trigger clicks, A/B (or d-pad down/right on the left) right- and middle-click, the stick scrolls and the grip moves the screen.

Steam Input and SteamVR's controller binding UI do not change this layout: the gamepad the PC sees is made by the app from the controllers' OpenXR inputs, not by Steam. To swap A/B and X/Y use the app's own "flip face buttons" setting (Settings → Input).

## Settings that matter on the Frame

Everything is decoded in software on the Frame's CPU, and the 3D depth model runs on the CPU too (the GPU path does not work under Lepton). These settings keep it smooth:

- **Video codec: H.264** (forced automatically; HEVC and AV1 are not available in software).
- **Bitrate: about 40 Mbps or less at 1440p.** At 100 Mbps the decoder falls behind, the app requests a keyframe every half second and the picture hitches on a beat.
- **Resolution: 1440p or 1080p.** Decoding 1080p takes about half the time of 1440p.
- **3D: Balanced or off.** The depth map lags the picture by a few hundred milliseconds on CPU; stronger separation makes anything that moves look smeared. Turn 3D off for fast games.
- **Depth rate: 20 or lower.** The CPU manages about 5 to 8 maps a second.

Open the settings from Moonlight's 2D window before starting a stream, or from the cog on the bar below the screen during one.

## Known limitations

- **Passthrough does not appear.** The runtime advertises the alpha blend mode and the app switches to it, but the view stays opaque. The void and the rooms work.
- **No picture glow, loading splash, pop-up notices, bug report sheet, clock or hand tracking hint.** They are what was given up to fit the rooms under SteamVR's 16-swapchain limit.
- **Everything is software decoded**, so the picture is more sensitive to bitrate and resolution than on a Quest or Pico, and the 3D depth map runs a few hundred milliseconds behind the picture. See the settings section.
- On the first launch Android's home screen shows for a few seconds while Lepton boots. The launch wrapper then disables that launcher inside the container, so later launches go from black straight to Moonlight. Lepton throws the container's data away after an APK update, or whenever a session ends within 30 seconds of starting, and the home screen then shows once more on the next launch.
- Refresh rate stays at 90 Hz; the runtime offers no other rate to the app.
- Hand tracking is untested.

## Building

`steam-frame/build.sh` downloads a self-contained toolchain into `~/.local/opt/android-build` and builds the checked-out branch (`steam-frame`), producing a signed APK in `steam-frame/release/`. It needs about 3 GB of disk, a network connection and no root. It runs on the Frame itself (arm64) or on an x86_64 Linux PC. The signing key is generated on first use in `steam-frame/signing/`; keep it so later builds install over earlier ones.

On arm64 Linux Google ships no NDK or `aapt2`, and the newest Android command line tools include an x86_64-only binary, so the script assembles the toolchain from these parts:

| Part | Source |
|---|---|
| JDK 21 | Temurin, `OpenJDK21U-jdk_aarch64_linux_hotspot` |
| Android command line tools | `commandlinetools-linux-13114758_latest.zip` (release 19.0, pure Java) |
| SDK packages | `platforms;android-37.0`, `build-tools;36.0.0`, `build-tools;37.0.0` via `sdkmanager` |
| NDK | HomuHomu833/android-ndk-custom release r30, `android-ndk-r30-aarch64-linux-gnu.tar.xz`, symlinked as `$ANDROID_HOME/ndk/30.0.16248370` |
| aapt2 | lzhiyong/android-sdk-tools release 35.0.2, `android-sdk-tools-static-aarch64.zip` |

By hand, with `JAVA_HOME` and `ANDROID_HOME` pointing at those and a `keystore.properties` in place (see `keystore.properties.example`):

```sh
git submodule update --init --recursive
./gradlew --no-daemon assembleNonRootRelease \
    -PmoonlightNdkVersion=30.0.16248370 \
    -Pandroid.aapt2FromMavenOverride=/path/to/android-sdk-tools/build-tools/aapt2 \
    -Pandroid.enableResourceOptimizations=false
```

`moonlightNdkVersion` overrides the NDK the build asks for (`app/build.gradle` reads it). The resource optimisation step must be off with that `aapt2`: its `optimize` command silently produces no output and the APK then has no manifest. A build takes a couple of minutes on the Frame.

On an x86_64 Linux PC nothing special is needed: the standard Android SDK with NDK `29.0.14206865` and `./gradlew assembleNonRootRelease`, as in the upstream README. The output runs on the Frame the same way.

## Runtime facts, for anyone digging further

- Lepton runs whatever `*.apk` sits in a Steam shortcut's folder; the shortcut's executable is the APK itself, its compatibility tool is `lepton`, and a file named `lepton-show-flatscreen` next to the APK makes the 2D window visible in SteamVR.
- SteamVR's runtime offers `XR_KHR_opengl_es_enable`, `XR_KHR_android_create_instance` and `XR_VALVE_frame_controller_interaction`, but not `XR_KHR_composition_layer_cylinder`, `XR_KHR_composition_layer_color_scale_bias` or `XR_FB_composition_layer_settings`, and it caps swapchains at 16 per session.
- The TensorFlow Lite GPU delegate fails under Lepton (no OpenCL; the GL path lacks an op), so depth runs on the CPU.
- Logs: `~/.local/share/Steam/logs/lepton-steamlaunch-<appid>.log` (the installer prints the appid), logcat dumps in `~/.local/share/Steam/logs/lepton-logcats/` when the app exits early, and `/tmp/moonlightxr-launch.log` for the launch wrapper. Lines starting `moonlight-xr:` and `com.limelight.LimeLog:` are the app's own.

## How this was made

I did not write the code by hand. The investigation, the patches, the scripts and this document were produced by Anthropic's Claude (Claude Fable 5.1 and Claude Opus 5.5, through Claude Code) working on my Steam Frame over SSH, with me launching builds in the headset and reporting back what I saw. The work went roughly like this: reading Lepton's scripts and the app's source to see why the stock APK did nothing, patching headset detection and the decoder path, finding the decoder-setup crash in a crash dump, building an arm64 toolchain on the Frame itself, binding the Frame controllers, and then several rounds of trying to fit the rooms under SteamVR's swapchain limit, one of which broke the view and was reverted before the one that worked. Every claim in the Status section comes from those tests on real hardware, not from reasoning about the code. Bug reports with the Lepton log attached are welcome.

## License

Moonlight XR and Moonlight for Android are GPL-3.0, and so is this fork. See `LICENSE.txt`.
