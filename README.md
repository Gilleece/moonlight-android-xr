> **Steam Frame:** this fork runs on Valve's Steam Frame through Lepton. Everything about it, from install to building, is in [STEAM_FRAME.md](STEAM_FRAME.md); prebuilt APKs are on the Releases page.

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

Built and tested on Pico 4 Ultra and Quest 3, both Snapdragon XR2 Gen 2, from one APK. On these
the ZipDepth model runs on the GPU, and the older MiDaS model is offered as an alternative.

Quest 2, Quest Pro and Pico 4 are the previous generation, XR2 Gen 1, with much less GPU headroom.
There ZipDepth runs on the CPU from a smaller int8 copy, MiDaS is not offered, and a first run
starts on a lighter profile: 1440p at 72 fps, 12 depth maps a second and the rooms at low
resolution. A Quest 2 held 90 and 120 Hz with the 3D on in testing; the Quest Pro and Pico 4 are
untested.

The app also declares what Android XR needs to run it, but it has not been tried on a Galaxy XR.

## How it works

    decoder -> SurfaceTexture (external OES texture)
            -> downscale to the model's input and read back, on a stage thread
            -> ZipDepth on its own thread: 512x288 on the GPU, 256x256 on the CPU on XR2 Gen 1
            -> averaged over time per texel, started afresh at a scene cut
            -> depth upsampled to quarter resolution, guided by the colour frame
            -> occlusion aware gather warp, one view per eye, then the picture controls
            -> two OpenXR layers, one per eye, flat or curved

ZipDepth takes about 15 ms a map on a Quest 3 at idle and 16 ms under a stream (MiDaS about 24),
which is longer than a display frame, so the model runs beside the frame loop rather than inline,
at a set number of maps a second whatever the stream's frame rate: 20 by default. The warp costs
about 1.3 ms of GPU time a frame at 1080p and 4.7 ms at 4K on a Quest 3.

The display follows the stream. The headset is asked for the stream's own frame rate, or failing
that the lowest whole multiple of it that it offers (120 Hz for a 60 fps stream), or the nearest
rate above. If the headset falls behind with the 3D on, a governor halves the depth rate first and
only then steps the display down, never below 72 Hz, and raises the depth rate again once things
settle.

The three rooms are baked meshes with ASTC compressed textures, loaded when picked and drawn for
each eye behind the picture, which lights them.

## What to expect

This is an honest 3D effect, not a native stereo renderer, and it has limits worth knowing before
you build it:

- **Separation is deliberately conservative.** Each model starts on its own separation, 0.6
  percent of frame width for ZipDepth and 0.5 for MiDaS, chosen by blind testing. Higher values
  were tested blind and produced no more perceived depth while causing eye strain and worse edge
  artifacts. The Strong preset is there if you want more anyway.
- **Silhouettes against high contrast backgrounds show some smearing.** A mono frame does not
  contain the pixels a second eye needs behind a foreground object, so that region is stretched.
  It is most visible on hard edges such as a hillside against bright sky, and largely invisible
  in ordinary content.
- **Depth lags the picture by roughly 50 ms.** The depth map is re-snapped onto each frame's
  colour edges, so this shows up as depth values being slightly stale rather than as misaligned
  edges.

## In the headset

### Pointing

The controllers work as a mouse. Point at the screen and a laser appears, the trigger is left
click, A or X right click, and the thumbstick scrolls. It wakes on deliberate movement rather than
on any nudge, and retires itself after five seconds of stillness unless pointer sleep is off.
The thumbstick click turns pointing off for a game that needs the mouse itself. For lightgun games
the ray button on the bar hides the beam and keeps the dot where it lands, and "Show the
controllers" in the settings draws a simple controller in each hand.

Put the controllers down and your hands point, with a pinch to click. Three quick pinches of thumb
and index finger on either hand lock the hands out, so a stray pinch does not click, and the same
triple pinch unlocks them; the first two pinches of a lock still reach the PC as clicks. On a
headset with eye tracking you can point by looking and click with a pinch.

### The bar

Hover under the picture and a row of buttons appears either side of the move bar. From the left:

| Button | What it does |
| --- | --- |
| Head aim | Head aim on or off for the session. Only there while the screen is head locked outside a room |
| Gamepad | Switches the controllers between the pointer and gamepad mode |
| Exit | Ends the stream, after a prompt |
| Environment | Opens the environment picker |
| Move bar | Drag it to move the screen around in 6DOF |
| Cog | Opens the settings panel |
| Keyboard | Opens a keyboard that types into the PC |
| Ray | Hides or shows the controller ray for the session |
| 3D | Switches between 2D and 3D for the session. Not there when the 3D mode is off |

Hover any corner of the screen and a bracket appears to resize it. Either grip or trigger holds a
handle, since apps disagree about which one should. A fresh install puts a 3 m wide screen 3 m
away, about 53 degrees, and where you leave it is where it will be next time. Recentring the
headset turns the screen to face you and keeps its distance, size, curve and head lock.

### The settings panel

The cog opens a panel with five tabs. Everything on it applies at once and is saved, apart from
the rows that do what a bar button does, which last for the session.

| Tab | Rows |
| --- | --- |
| Screen | Distance, height, tilt, rotate, curve and size; head aim's sensitivity and dead zone, live while the screen is head locked; Reset |
| Display | Sharpen (Quality), supersample (Off), stats, head locked, head aim, controllers (pointer or gamepad), pointer sleep, ray, controller model, click sound, glow, screen light and the glow level |
| 3D | Comfort, Balanced and Strong presets, the depth and convergence tracks, 3D on or off for the session, Reset |
| Picture | Brightness, contrast, gamma and saturation, each marked where the picture is as streamed; Reset |
| About | The version and commit, where the log is, a Ko-fi sheet with a QR code to scan with your phone, and Report a problem |
| Room | Takes the Screen tab's place while a room is up: the room's brightness, glow, light level and picture size, kept per room, and the screen light that throws the picture's light over it |

The depth model itself is chosen in the 2D settings, since it is settled when a session starts.

### Environments

The button to the left of the move bar opens the picker: passthrough, an empty black void, and
three 3D rooms that hang the screen for you, the Home Theater, the Grand Cinema and Synthwave. In
the Home Theater and Synthwave the picture's corners resize it, from a quarter to all of the room's
screen; the Grand Cinema keeps its screen whole. Passthrough is in the picker as well as in the
settings, so it can be switched mid stream, and the "Environment" setting picks what a session
starts in.

### Gamepad mode and head aim

Gamepad mode turns the two controllers into one Xbox controller on the PC: the sticks, A, B, X and
Y, the triggers, the grips as bumpers, the stick clicks and the left menu button as Start, with the
PC's rumble on the controllers. There is no Back, Guide or d-pad. Every session starts with the
controllers as the pointer; the gamepad button, the Display tab or a shortcut on the controllers
switches them, by default holding the left menu button and the left grip. In gamepad mode the
controllers cannot reach the panels, but hands and eyes still can.

Head aim is for first person games. With the screen locked to your head, turning your head moves
the mouse the way a game's look control does, and the controller pointer still nudges it on top.
Tune its sensitivity together with the game's own until the view turns as far as your head does.

### The keyboard

The keyboard's Fn key opens a sheet with Esc, F1 to F12, Tab, Insert, Delete, Home, End, Page Up,
Page Down, the arrows, and Ctrl, Alt and Win, which stay held until the next key.

### Taking the headset off

Take the headset off for a moment and the stream waits up to a minute with the sound muted, then
carries on when it goes back on.

## Settings

Whatever can change live sits on the panel in the headset. The 2D settings keep what has to be set
before a session starts, and a few that are in both places.

**VR Settings**:

| Setting | Default | Notes |
| --- | --- | --- |
| Stream in VR | on | Immersive OpenXR session instead of a flat panel, and the only mode with the 3D effect |
| Controllers as a mouse | on | |
| Pause the pointer when the controller is still | on | Off keeps the pointer up, so the thumbstick scrolls with it off the screen |
| Show the controller ray | on | Off for lightgun games. The dot stays where the beam would land |
| Show the controllers | off | A simple controller drawn in each hand. Not in passthrough, nor while your hands are tracked |
| Head aims the mouse | off | Only while the screen is head locked, outside a room |
| Head aim sensitivity | 8 px per degree | |
| Head aim dead zone | 2 degrees per second | Slower turns send nothing, so tracking jitter does not move the mouse |
| Gamepad mode shortcut | Hold the left menu button and left grip | Or press both thumbsticks, or squeeze both triggers and both grips, though those controls then reach the game a moment late |
| Click when a panel is pressed | on | |
| Look to point | on | Headsets with eye tracking |
| Hand tracking | on | |
| Passthrough mode | off | Show your room behind the screen. Costs performance, turn it back off if the stream suffers |
| Environment | Black void, or passthrough with Passthrough mode on | What surrounds the screen when a session starts. Picking one in the headset changes this too |
| Realtime 3D mode | V2.0 - ZipDepth Based 3D (Recommended) | V1.0 is the older MiDaS model, not offered on Quest 2, Quest Pro and Pico 4. "Off" streams flat. Debug builds add test patterns |
| Environment Res | Standard | How sharply the rooms render. Low on Quest 2, Quest Pro and Pico 4. Ultra is experimental |
| Keep the stream for a minute when the headset is taken off | on | Off ends the stream as soon as the headset comes off |
| Picture brightness | 0 | -50 to 50 |
| Picture contrast | 100 % | 50 to 150 |
| Picture gamma | 1.00 | 0.50 to 2.00. Above 1.00 lifts the shadows, which usually suits a dark game better than brightness |
| Picture saturation | 100 % | 0 to 200 |

**VR Debugging**, which you should not need:

| Setting | Default | Notes |
| --- | --- | --- |
| Depth updates per second | 20 | 12 on Quest 2, Quest Pro and Pico 4. Fewer is lighter on the headset, but the depth lags further behind |
| Show depth map | off | Renders the depth map as grayscale instead of the video |
| Swap eyes | off | |
| Save a log file | Warnings and errors | Or off, or verbose. See [Reporting a problem](#reporting-a-problem) |

Elsewhere in the settings:

| Setting | Default | Notes |
| --- | --- | --- |
| Audio: Virtual surround for headsets | off | Places each 5.1 or 7.1 speaker around you in stereo sound, held to the screen as you turn your head. Needs 5.1 or 7.1 chosen above it, and a host and game that output surround |
| Host: Open Desktop automatically | off | A tap on a PC starts its app named Desktop without showing the app list, if the host has one |
| UI: Language | Default | See [Languages](#languages) |
| About: Check for updates | on | Once a day, a notice on the PC list when there is a newer release. See below |
| About | | The version and commit, the GPLv3 licence, and Report a problem |

Check for updates is the one thing this fork adds that reaches anything besides your PC and a report
you choose to send. At most once a day, when the PC list opens, the app asks GitHub's API for this
repository's latest release. The request carries nothing but the app's version, though GitHub sees
the address it comes from as it would for any web page. If that release is newer, a notice above the
PC list offers View, which opens its page in your browser, and Dismiss, which hides it until a newer
one. Nothing is ever downloaded or installed. To turn it off, untick Check for updates under About
at the end of the settings.

The stream defaults also change on a headset, because the stock ones look bad on a virtual screen:

| Setting | Default |
| --- | --- |
| Resolution | 2560x1440 |
| Frame rate | 90, or 72 on Quest 2, Quest Pro and Pico 4 |

1440p is the default because 4K costs decode latency and host bitrate for a gain that is easy to
miss. 4K is there in the list if you want it, and is worth trying. 720p is unusable on a virtual
screen this size and 1080p is merely acceptable. Bitrate follows the resolution and frame rate as
it does upstream, so changing either resets it. The frame rate list also offers every rate the
headset's display can run, and a 1440p stream at 120 fps has held 120 Hz with the 3D on, on a
Quest 3.

"Show performance stats while streaming" works inside the VR session and adds the display rate,
warp GPU time, the depth model with its inference time, rate, age and skipped maps, and audio
underruns to the usual figures, with the clock and battery on its first line.

## Reporting a problem

Use Report a problem under About at the end of the settings, or on the About tab of the panel
inside a session. Write what happened, and your email if you want a reply, then press Send. The
report carries your note, the version and the commit it was built from, the headset, your settings
and environment, why VR last failed to start if it did, and the newest 6 MB of the two log files.
The log can include your PC's name and its address on your network.

Send posts the report to the collector the build was made with, which works from a headset with no
email app, and nothing is kept on the headset. Only when that fails is the report saved beside the
log in `Download/MoonlightXR`, with both logs whole, and the screen says why and shows the path:
"Could not send", "The collector is busy" when it is at its limits, or "This build has no
collector" for a build made without one (see [Report collector](#report-collector)). Only the
newest five saved reports are kept.

The log itself is `Download/MoonlightXR/moonlight.log`, which the headset's file manager can open
and which can be copied off over USB without adb. "Save a log file" sets how much goes in it, and
a change takes effect when the app restarts. At 5 MB it rolls over into `moonlight.previous.log`,
so the two together never take more than 10 MB. If Download cannot be written to, after a
reinstall for instance, the log goes to the app's own folder and the setting shows where. Look a
log over before attaching it to a public GitHub issue.

## Building

Requires Android Studio with the NDK, and the submodules:

    git submodule update --init --recursive

Debug build:

    ./gradlew assembleNonRootDebug

The APK lands in `app/build/outputs/apk/nonRoot/debug/`. Install it with `adb install -r`.

### Tests

The parts of the renderer with no GL, OpenXR or Android in them, the maths, the depth map
filtering, the rate choice, the input rules, the layout, the room parsers and the surround's
convolution among them, build and run on a desktop with any C compiler, and the Java side has plain
unit tests for the settings, the depth presets, the report, the update check and the virtual
surround:

    make -C app/src/test/cpp test
    ./gradlew testNonRootDebugUnitTest

The workflow in `.github/workflows/build.yml` runs both and `tools/check_strings.py`, which checks
every translation's keys and placeholders, then lint and a debug and a release build, on every
push. A tag starting with `v` also publishes the release APK, signed when the repository has
`KEYSTORE_BASE64`, `KEYSTORE_PASSWORD` and `KEY_ALIAS` secrets and unsigned otherwise.

### Report collector

With `reportUrl` and `reportToken` set in `keystore.properties` (or `moonlightReportUrl` and
`moonlightReportToken` passed with `-P`), Send posts the report to that https URL as JSON, with the
token as a bearer token. `tools/report-worker` is a Cloudflare Worker that takes exactly that and
emails it on as an attachment; its README has the fields, the limits and the answers. Both values
are baked into the APK, which is why they belong in the ignored file rather than in
`gradle.properties`. A build without them still has Send, which then saves the report on the
headset and says the build has no collector.

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

`zipalign` and `apksigner` are in `$ANDROID_HOME/build-tools/<version>/`. The release build installs
as `com.gilleece.moonlightxr` and a debug build as `com.gilleece.moonlightxr.debug`, so the two
install side by side and pair with your host separately. Worth knowing while developing: they are
separate apps with separate settings, so a change tested on one is not on the other.

Debug builds also read the `debug.moonlight.*` system properties, so the warp, pointer and glow
tuning can be changed over `adb shell setprop` mid session, and `setprop debug.moonlight.capture 1`
dumps a frame's warp inputs for `tools/warp_lab.py`. Release builds leave all of that out.

The APK is about 98 MB. Most of it is the depth models (ZipDepth for the GPU, its int8 copy for the
CPU, and MiDaS, which alone is about a third of the APK), the three rooms with their textures at
two sizes, the surround's HRTF set and the LiteRT native libraries for four ABIs. Only `arm64-v8a`
is ever loaded on a headset; the other three are kept so the same build still runs on phones.

## Code layout

The XR side lives in two places. On the Java side `XrRenderer.java` owns the threads, the
SurfaceTexture the decoder renders into, the frame loop and what gets saved between sessions;
`XrPanels.java` draws the picker, the settings panel, the keyboard and the prompts and sheets,
since Java is the only place Android will lay out text; `MidasDepthSource.java` runs both depth
models on LiteRT, on the GPU or the CPU as the headset allows; `XrPad.java` decides how gamepad
mode's pad arrives on the host; `utils/BugReport.java` puts a report together for both report
screens; and the virtual surround lives in `binding/audio/`, with its convolution in a small native
library of its own under `app/src/main/jni/xr-audio/`. Everything OpenXR and GL is native, under
`app/src/main/jni/xr-renderer/`, one module per concern:

| File | What it holds |
| --- | --- |
| `xr_shared.h` | Every value Java and the native side agree on; the build generates `XrShared.java` from it |
| `xr_renderer.h` | The native-only constants, the context struct and what each module exports |
| `xr_session.c` | Instance, session, the video swapchain, session state and the JNI lifecycle |
| `xr_display.c`, `xr_rate.c` | The refresh rate and performance levels asked of the runtime, and the rate choice, frame budget and depth rate governor behind them |
| `xr_gl.c` | GL setup, the depth upsample, offset search and warp passes, the GPU timer |
| `xr_depth.c` | The depth model's staging across the frame loop, the stage thread and the depth thread |
| `xr_depthmap.c` | The CPU side of the depth map: its range, the low pass, the averages over time and the scene cut detector |
| `xr_input.c` | Actions and bindings, hands and gaze, and the per frame input pass |
| `xr_gate.c`, `xr_pinch.c` | Whether a controller, a hand or the eyes may point, the pinch and the triple pinch hand lock |
| `xr_headaim.c` | Head turns into mouse motion |
| `xr_gamepad.c` | Gamepad mode: the controllers as one Xbox pad, and its shortcut |
| `xr_keys.c` | The keyboard's sheets and its held Ctrl, Alt and Win |
| `xr_ui.c` | Where the furniture and the panels sit and what the ray is over |
| `xr_layout.c` | The bar's row under the picture, the stand in screen the panels hang from in a room, the hit tests and the panel's tracks |
| `xr_notice.c` | The splash, the panel fades and the toast queue |
| `xr_layers.c` | The composition layers of a frame, in draw order |
| `xr_assets.c` | The art swapchains and the uploads from Java that fill them |
| `xr_ambilight.c`, `xr_glow.c` | The frame colour sample, letterbox detection and the glow, flat or round a curved screen |
| `xr_grade.c` | The picture's brightness, contrast, gamma and saturation |
| `xr_room.c` | The 3D rooms and the table of seats, screens and lighting behind them |
| `xr_roommesh.c`, `xr_atlas.c` | The parsers for a baked room's mesh and its ASTC texture |
| `xr_controller.c`, `xr_model.c` | When the ray and the controller model show, and the model drawn at each grip |
| `xr_math.c` | Vectors, quaternions, the one euro filter and projection |
| `xr_shaders.c` | The GLSL |
| `xr_log.c`, `xr_debug.c` | The file log, the setprop tuning knobs and frame capture |

## Languages

The app is complete in nineteen languages besides English, in the headset and in the settings:
Czech, Danish, Dutch, Finnish, French, German, Italian, Japanese, Korean, Norwegian Bokmål, Polish,
Portuguese (Brazil), Russian, Simplified Chinese, Spanish, Swedish, Traditional Chinese, Turkish
and Ukrainian. Where upstream Moonlight already had a translation its strings are kept. French and
Chinese were started by [@moi952](https://github.com/moi952) and
[@TianQuanDiWen](https://github.com/TianQuanDiWen), and the rest of the VR side was translated in
house, so most of it has not been read by a native speaker yet. Corrections from native speakers
are welcome as pull requests. The language list also offers upstream's other translations, where
the VR parts stay in English.

## Licences

GPLv3, as upstream. Added dependencies are all compatible: the Khronos OpenXR loader (Apache 2.0),
LiteRT and its GPU delegate (Apache 2.0), and the MiDaS v2.1 small depth model (MIT), converted to
TensorFlow Lite by `tools/convert_midas.py` and committed as an asset.

The default depth model is [ZipDepth](https://github.com/fabiotosi92/ZipDepth) by Fabio Tosi (MIT),
converted to TensorFlow Lite by `tools/convert_zipdepth.py` (fp16, 512x288) and
`tools/quantize_depth.py` (the int8 copy the XR2 Gen 1 headsets run) and committed as assets. Its
licence ships in the APK as `assets/licenses/zipdepth_LICENSE.txt`. Two ideas in the depth path
came from [Nightfall](https://github.com/tB0nE/nightfall) (GPLv3), another Moonlight client with
realtime 3D: exporting ZipDepth's learned upsampling head for the GPU delegate, and smoothing the
depth map over real time rather than per frame. Both are implemented independently here.

Virtual surround uses the KEMAR head related transfer function measurements by Bill Gardner and
Keith Martin, MIT Media Lab, 1994 ("HRTF Measurements of a KEMAR Dummy-Head Microphone", MIT Media
Lab Perceptual Computing Technical Report #280). The data "is provided free with no restrictions on
use, provided the authors are cited when the data is used in any research or commercial
application". The 37 elevation 0 files of the compact set ship unchanged in `assets/hrtf/kemar/`,
with those terms in the `LICENSE.txt` beside them.

The Home Theater, Grand Cinema and Synthwave environments are the fork author's own models,
Copyright (c) 2026 Sean Gilleece / Woodford XR, licensed under
[Creative Commons Attribution 4.0](http://creativecommons.org/licenses/by/4.0/) with attribution to
Sean Gilleece / Woodford XR. They were first made for Depthray, Woodford XR's VR video player, and
are borrowed from it here. Their meshes are baked by `tools/bake_room.py` and their textures
compressed to ASTC by `tools/atlas_astc.py`, which runs ARM's
[astc-encoder](https://github.com/ARM-software/astc-encoder) (Apache 2.0) at build time only; the
commands are in each room's `NOTE.txt` under `tools/rooms/`.

The controller model drawn when "Show the controllers" is on is this repository's own work,
Copyright (c) 2026 Sean Gilleece / Woodford XR, licensed under
[Creative Commons Attribution 4.0](http://creativecommons.org/licenses/by/4.0/) with attribution to
Sean Gilleece / Woodford XR. It is made in code by `tools/make_controller.py` and baked by
`tools/bake_room.py`, as `tools/models/controller/NOTE.txt` describes.

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
