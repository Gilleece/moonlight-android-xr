<p align="center">
  <img src="moonlight-xr-logo-transparent.png" height="200" alt="moonlight-xr-logo"><br>
  <a href="https://ko-fi.com/moonlightxr">
    <img src="https://img.shields.io/badge/ko--fi-support-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white" height="35" alt="Ko-fi">
  </a>
  <br>
  <a href="https://ko-fi.com/moonlightxr">
    <strong>Support on Ko-fi</strong>
  </a>
</p>

# Moonlight XR

A fork of [Moonlight for Android](https://github.com/moonlight-stream/moonlight-android) that
runs as a native OpenXR application and shows the game stream in stereoscopic 3D on a headset.

The stereo is generated entirely on the headset. A normal mono stream arrives from the PC exactly
as stock Moonlight receives it, a depth model runs on the frame, and a depth image based rendering
shader synthesises a separate view for each eye. Nothing on the PC side changes: no ReShade, no
stereo injector, no side by side transport, no Sunshine modifications. The host does not know it
is feeding a VR client, so this works with any Moonlight compatible host and any game, including
ones no depth buffer injector can reach.

On a headset the app starts in VR with the 3D effect already on, because the stock defaults make
it look broken on a virtual screen. Every part of it is a setting, and turning VR mode off gives
you stock Moonlight behaviour.

## Hardware

Built and tested on Pico 4 Ultra and Quest 3, both Snapdragon XR2 Gen 2, from one APK.

Quest 2, Quest Pro and Pico 4 are the previous generation with much less GPU headroom. They are
untested, and the depth model may be too expensive for them at any resolution.

## How it works

    decoder -> SurfaceTexture (external OES texture)
            -> downscale to 256x256, read back
            -> MiDaS small on the GPU, on its own thread
            -> depth upsampled to quarter resolution, guided by the colour frame
            -> occlusion aware gather warp, one view per eye
            -> two OpenXR quad layers, one per eye

Depth inference costs about 22 ms, which is longer than a display frame, so it runs on a separate
thread at a configurable cadence rather than inline. The warp costs about 2.8 ms of GPU time per
frame out of the 11.1 ms budget at 90 Hz.

## What to expect

This is an honest 3D effect, not a native stereo renderer, and it has limits worth knowing before
you build it:

- **Separation is deliberately conservative.** The default of 0.5 percent of frame width was
  chosen by measurement. Higher values were tested blind and produced no more perceived depth
  while causing eye strain and worse edge artifacts.
- **Silhouettes against high contrast backgrounds show some smearing.** A mono frame does not
  contain the pixels a second eye needs behind a foreground object, so that region is stretched.
  It is most visible on hard edges such as a hillside against bright sky, and largely invisible
  in ordinary content.
- **Depth lags the picture by roughly 50 ms.** The depth map is re-snapped onto each frame's
  colour edges, so this shows up as depth values being slightly stale rather than as misaligned
  edges.

## Using the controllers

The controllers work as a mouse. Point at the screen and a laser appears, the
trigger is left click, the thumbstick scrolls. It wakes on deliberate movement
rather than on any nudge, and retires itself after five seconds of stillness.
The thumbstick click turns the whole thing off if you would rather not have it.

The screen itself can be moved and resized in place. Hover under it and a bar
appears to drag it around in 6DOF, hover any corner and a bracket appears to
resize it. Either grip or trigger holds a handle, since apps disagree about
which one should. Where you leave it is where it will be next time, and
recentring the headset puts it back to where a fresh install starts.

To the left of the move bar is a button that opens a grid of environments:
passthrough, an empty black room, and the bundled 360 photos. Passthrough is
in the grid as well as in the settings, so it can be switched mid stream.

## Settings

**VR Settings**:

| Setting | Default | Notes |
| --- | --- | --- |
| Stream in VR | on | Immersive OpenXR session instead of a flat panel |
| Head locked screen | off | Screen follows your view rather than staying in the world |
| Screen distance | 3.0 m | |
| Screen width | 3.0 m | 3 m wide at 3 m away is about 53 degrees |
| Passthrough mode | off | Show your room behind the screen. Costs performance, turn it back off if the stream suffers. Also reachable from the environment grid while streaming |
| Realtime 3D mode | V1.0 - MiDaS Based 3D | "Off" streams flat, the rest are test patterns |
| Stereo separation | 0.5 % | Of frame width. Above about 0.5 the picture is not any deeper, only harder on the eyes |
| Screen curvature | 0 | 0 is flat, higher wraps the screen around you |

**VR Debugging**, which you should not need:

| Setting | Default | Notes |
| --- | --- | --- |
| Depth model cadence | 3 frames | Run the depth model on every Nth video frame |
| Show depth map | off | Renders the depth map as grayscale instead of the video |
| Swap eyes | off | |

The stream defaults also change on a headset, because the stock ones look bad on a virtual screen:

| Setting | Default |
| --- | --- |
| Resolution | 2560x1440 |
| Frame rate | 90 |

1440p is the default because 4K costs decode latency and host bitrate for a gain that is easy to
miss. 4K is there in the list if you want it, and is worth trying. 720p is unusable on a virtual
screen this size and 1080p is merely acceptable. Bitrate follows the resolution and frame rate as
it does upstream, so changing either resets it.

"Show performance stats while streaming" works inside the VR session and adds warp GPU time,
depth inference time, depth age and skipped depth frames to the usual figures.

## Building

Requires Android Studio with the NDK, and the submodules:

    git submodule update --init --recursive

Debug build:

    ./gradlew assembleNonRootDebug

The APK lands in `app/build/outputs/apk/nonRoot/debug/`. Install it with `adb install -r`.

### Release APK

A headset will not install an unsigned APK, so the release build has to be signed. Create a
keystore once:

    keytool -genkeypair -v -keystore release.keystore -alias moonlightvr \
        -keyalg RSA -keysize 2048 -validity 10000

Then copy `keystore.properties.example` to `keystore.properties` and fill in the password. Both
that file and the keystore are gitignored. The build picks it up on its own:

    ./gradlew assembleNonRootRelease
    adb install -r app/build/outputs/apk/nonRoot/release/app-nonRoot-release.apk

Without `keystore.properties` the build still works, but it produces
`app-nonRoot-release-unsigned.apk` and you have to align and sign it yourself:

    zipalign -f 4 \
        app/build/outputs/apk/nonRoot/release/app-nonRoot-release-unsigned.apk \
        moonlight-vr-release.apk
    apksigner sign --ks release.keystore moonlight-vr-release.apk
    apksigner verify moonlight-vr-release.apk
    adb install -r moonlight-vr-release.apk

`zipalign` and `apksigner` are in `$ANDROID_HOME/build-tools/<version>/`. The release build uses
the `.unofficial` application ID suffix that upstream asks forks to keep, so it installs alongside
a debug build and pairs with your host separately. Worth knowing while developing: the two are
separate apps with separate settings, so a change tested on one is not on the other.

The APK is about 55 MB, most of which is the depth model and the LiteRT native libraries for four
ABIs. Only `arm64-v8a` is ever loaded on a headset; the other three are kept so the same build
still runs on phones.

## Code layout

The XR side lives in two places. On the Java side `XrRenderer.java` owns the threads, the
SurfaceTexture the decoder renders into, the frame loop and what gets saved between sessions;
`XrPanels.java` draws the picker, the settings sheets, the keyboard and the exit prompt, since
Java is the only place Android will lay out text; `MidasDepthSource.java` runs the depth model
on LiteRT. Everything OpenXR and GL is native, under `app/src/main/jni/xr-renderer/`, one
module per concern:

| File | What it holds |
| --- | --- |
| `xr_renderer.h` | The constants, the context struct and what each module exports |
| `xr_session.c` | Instance, session, the video swapchain, session state and the JNI lifecycle |
| `xr_gl.c` | GL setup, the depth upsample, offset search and warp passes, the GPU timer |
| `xr_depth.c`, `xr_depthmap.c` | The depth model staging and the CPU filtering of its output |
| `xr_input.c` | Actions and bindings, hands and gaze, and the per frame input pass |
| `xr_ui.c` | Where the furniture and the panels sit and what the ray is over |
| `xr_layers.c` | The composition layers of a frame, in draw order |
| `xr_assets.c` | The art swapchains and the uploads from Java that fill them |
| `xr_ambilight.c` | The frame colour sample, letterbox detection and the glow |
| `xr_room.c` | The 3d rooms |
| `xr_math.c` | Vectors, quaternions, the one euro filter and projection |
| `xr_shaders.c` | The GLSL |
| `xr_log.c`, `xr_debug.c` | The file log, the setprop tuning knobs and frame capture |

## Licences

GPLv3, as upstream. Added dependencies are all compatible: the Khronos OpenXR loader (Apache 2.0),
LiteRT and its GPU delegate (Apache 2.0), and the MiDaS v2.1 small depth model (MIT), converted to
TensorFlow Lite by `tools/convert_midas.py` and committed as an asset.

The 360 degree environments are from [Poly Haven](https://polyhaven.com), released under CC0 and
downsized to 4096x2048 for this app. Poly Haven is community funded and worth supporting.

The PSX Cinema environment is ["VR Cinema Environment"](https://skfb.ly/6VuIX) by fangzhangmnm,
licensed under [Creative Commons Attribution 4.0](http://creativecommons.org/licenses/by/4.0/).
Modified for this app: the mesh is baked to a flat format by `tools/bake_room.py` and the
embedded texture is shipped alongside it.

---

# Moonlight Android

[![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/232a8tadrrn8jv0k/branch/master?svg=true)](https://ci.appveyor.com/project/cgutman/moonlight-android/branch/master)
[![Translation Status](https://hosted.weblate.org/widgets/moonlight/-/moonlight-android/svg-badge.svg)](https://hosted.weblate.org/projects/moonlight/moonlight-android/)

[Moonlight for Android](https://moonlight-stream.org) is an open source client for NVIDIA GameStream and [Sunshine](https://github.com/LizardByte/Sunshine).

Moonlight for Android will allow you to stream your full collection of games from your Windows PC to your Android device,
whether in your own home or over the internet.

Moonlight also has a [PC client](https://github.com/moonlight-stream/moonlight-qt) and [iOS/tvOS client](https://github.com/moonlight-stream/moonlight-ios).

You can follow development on our [Discord server](https://moonlight-stream.org/discord) and help translate Moonlight into your language on [Weblate](https://hosted.weblate.org/projects/moonlight/moonlight-android/).

## Downloads
* [Google Play Store](https://play.google.com/store/apps/details?id=com.limelight)
* [Amazon App Store](https://www.amazon.com/gp/product/B00JK4MFN2)
* [F-Droid](https://f-droid.org/packages/com.limelight)
* [APK](https://github.com/moonlight-stream/moonlight-android/releases)

## Building
* Install Android Studio and the Android NDK
* Run ‘git submodule update --init --recursive’ from within moonlight-android/
* In moonlight-android/, create a file called ‘local.properties’. Add an ‘ndk.dir=’ property to the local.properties file and set it equal to your NDK directory.
* Build the APK using Android Studio or gradle

## Authors

* [Cameron Gutman](https://github.com/cgutman)  
* [Diego Waxemberg](https://github.com/dwaxemberg)  
* [Aaron Neyer](https://github.com/Aaronneyer)  
* [Andrew Hennessy](https://github.com/yetanothername)

Moonlight is the work of students at [Case Western](http://case.edu) and was
started as a project at [MHacks](http://mhacks.org).
