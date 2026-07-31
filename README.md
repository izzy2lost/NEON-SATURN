# NEON SATURN

An Android port of [Ymir](https://github.com/StrikerX3/Ymir), StrikerX3's work-in-progress Sega Saturn emulator.

NEON SATURN keeps Ymir's emulation core as-is and replaces the desktop frontend with a native Android
one: a setup wizard, a cover-art game library, Saturn-style touch controls, and an in-game quick actions
menu. Everything under `libs/ymir-core` is upstream Ymir; the port lives in [`android/`](android/).

## Features

### Library

- Setup wizard for importing the IPL (BIOS) ROM and pointing the app at a games folder
- Games folder is read through the Storage Access Framework, so it can live anywhere on the device
- Three library views, cycled from the header: a list, NA box covers, and Japanese jewel cases
- Coverflow with perspective, reflections and snap-to-centre paging
- Cover art fetched and cached on demand, or downloaded in bulk from Settings
- Animated starfield backdrop
- Light / dark / follow-system theme, defaulting to dark

### In game

- Saturn-style touch control overlay: D-pad, analogue stick, A/B/C, X/Y/Z, L/R, Start
- Fully repositionable layout, stored separately for portrait and landscape
- Rewind and fast-forward buttons on the overlay
- Quick actions menu (back button or gamepad Select): resume, save state, load state, exit
- Physical gamepad and keyboard support via SDL, including a bundled controller database

### Emulation options

- Aspect ratio: 4:3, 16:9 or stretch
- Texture filtering: sharp (nearest) or smooth (bilinear)
- Upscaling filter: off, or 6x xBRZ
- Deinterlaced rendering of high-resolution modes
- Transparent mesh polygon rendering
- Rewind buffer (about five seconds at 60 fps), off by default
- Optional low-level CD block emulation when a CD block ROM is imported

Requires **Android 9 (API 28) or later** on a **64-bit ARM** device.


## Usage

Install the app. The setup wizard asks for two things:

1. **IPL BIOS** — import a Saturn BIOS ROM (`.bin` or `.rom`). Required.
2. **Games folder** — choose the folder holding your disc images. Required.

A **CD Block ROM** can also be imported. It is optional, and enables low-level CD block emulation.

Once the BIOS and games folder are set, the library replaces the wizard. Tap a game to stage and launch
it. The gear icon in the header opens Settings; the icon on the left cycles between list and cover views.

Supported disc formats: **MAME CHD, BIN+CUE, IMG+CCD, MDF+MDS and ISO**. Multi-file formats need their
companion track files present in the same folder — the launcher checks for them and refuses to start if
they're missing.

### Where files live

App data sits under `Android/data/com.izzy2lost.neonsaturn/files/neonsaturn/`:

| Directory | Contents |
| --- | --- |
| `ipl/` | Imported IPL (BIOS) ROMs |
| `cdb/` | Imported CD block ROMs |
| `disc/` | Games staged from the library before launch |
| `state/` | Persistent SMPC data (BIOS settings, clock) |
| `state/savestates/` | Save states, one directory per disc |
| `saves/` | Internal backup RAM |

Games are copied here from the games folder before launch so SDL and the emulator can open them as
ordinary files rather than through content URIs.


## Notes and limitations

**Upscaling doesn't raise the internal resolution.** Ymir's core has only a software renderer and
rasterises at Saturn-native resolution, so the upscaling filter is an edge-aware smoothing pass applied
to the finished frame. It sharpens 2D sprites and backgrounds noticeably; it cannot add 3D detail. Real
internal-resolution rendering would need a hardware renderer in the core, which does not exist yet.

**Rewind costs CPU and memory.** It saves emulator state every frame and keeps a few seconds of
LZ4-compressed deltas, so it is off by default and its on-screen button is hidden until enabled.

**Settings apply at launch.** Graphics and rewind options are read when a game starts, so changing them
takes effect the next time you launch a game.


## Compiling

Requires the Android SDK and a JDK, plus the toolchain Gradle pulls in:

| | Version |
| --- | --- |
| Android Gradle Plugin | 9.2.1 |
| Gradle | 9.4.1 |
| NDK | 30.0.15729638 |
| CMake | 3.30.3 |
| compileSdk / targetSdk | 36 |

AGP 9 has built-in Kotlin support, so no Kotlin Gradle plugin is declared.

Native dependencies come from vcpkg (checked in as a submodule) and the vendored libraries under
`vendor/`. Build from the `android/` directory:

```sh
cd android
./gradlew assembleDebug
```

The APK lands in `android/app/build/outputs/apk/debug/`. Only `arm64-v8a` is built.

> [!TIP]
> Release builds compile the core with ThinLTO, which is memory-hungry. On a machine with limited RAM,
> the parallel native build can exhaust memory — run it with reduced parallelism if that happens.

For building upstream Ymir for desktop platforms, see [COMPILING.md](COMPILING.md).


## Credits and licence

NEON SATURN is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).

- **[Ymir](https://github.com/StrikerX3/Ymir)** by StrikerX3 — the emulator core and everything this port
  is built on. If you like this, support the upstream author on
  [Patreon](https://www.patreon.com/StrikerX3) and join the
  [Discord](https://discord.gg/NN3A7n5dzn).
- **[Kronos](https://github.com/FCare/Kronos)** / **Yabause** — the 6x xBRZ upscaling shader in
  [`android/app/src/main/cpp/xbrz6x_shader.hpp`](android/app/src/main/cpp/xbrz6x_shader.hpp), used under
  GPL-2.0-or-later. The original copyright notice is kept in that file.
- **[SDL3](https://github.com/libsdl-org/SDL)** — windowing, audio, input and the Android activity.
