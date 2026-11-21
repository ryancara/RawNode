<p align="center">
  <img src="docs/logo.png" width="320" alt="RawNode application icon">
</p>

# RawNode

RawNode is a fork of [OFX Raw Host](https://github.com/aaronmurniadi/ofxrawhost)
with native, OFX and CTL processors sharing one document graph and evaluator.
Architecture Steps 1–7 and the targeted
post-Step-7 correction gate are complete. The switchable Graph/List editor now
includes the visual foundation (#79), contextual workspace (#80) and
connections/deletion (#82); placement/usability awaits an agreed PR 2C scope
before Step 8 masking/compositing. See [project context](docs/PROJECT_CONTEXT.md),
[architecture](docs/ARCHITECTURE.md), [coding guidance](docs/CODE_STYLE.md),
[roadmap](ROADMAP.md), [Graph UI validation](docs/GRAPH_VIEW_VALIDATION.md) and
[earlier manual validation](docs/STEP6_MANUAL_VALIDATION.md).

[![Latest RawNode release](https://img.shields.io/github/v/release/ryancara/RawNode)](https://github.com/ryancara/RawNode/releases)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)
![Built with](https://img.shields.io/badge/C%2B%2B17%20%2B%20Dear%20ImGui-lightgrey)

A minimal still-image [OpenFX](https://github.com/AcademySoftwareFoundation/openfx) **plugin host**.
Open a RAW (or PNG/JPEG/TIFF/EXR) photo, run it through one or more OFX filter plugins, preview the result,
and export to PNG or JPEG — no video NLE required.

Use it to try OFX effects that normally only run inside Resolve, Nuke, or similar hosts,
on still photos and a simple processing chain.

![Screenshot of the upstream OFX Raw Host application](docs/screenshot.png)

---

## Tested OFX plugins

| Plugin              | What it does                                                                                                                                                                                                    | Link                                                                                                                         |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| **spektrafilm-ofx** | Film simulation (film, print, scan, grain, halation, diffusion)                                                                                                                                                 | [chaert-s/spektrafilm-ofx](https://github.com/chaert-s/spektrafilm-ofx) / [spektrafilm.114c.de](https://spektrafilm.114c.de) |
| **ntsc-rs**         | Analog video / VHS-style effects                                                                                                                                                                                | [valadaptive/ntsc-rs](https://github.com/valadaptive/ntsc-rs)                                                                |
| **purzOS**          | A collection of 64 native OpenFX video plugins for DaVinci Resolve, Natron, and any other OFX host — retro/analog looks, pixelart, glitch, datamosh, CRT/VHS signal artifacts, colour grades and optical warps. | [purzbeats/purzos-ofx](https://github.com/purzbeats/purzos-ofx)                                                              |

Other OFX filter plugins should work 🤞 If you confirm one, a PR to this table is welcome.

---

## Settings and photo persistence

RawNode stores edits beside each editable photograph in `.rawnode.json` sidecars.
Application preferences and ImGui docking layout live in `preferences.json` and
`layout.ini` under `~/Library/Application Support/RawNode/` (macOS),
`$XDG_CONFIG_HOME/RawNode/` or `~/.config/RawNode/` (Linux), and
`%APPDATA%\RawNode\` (Windows). Settings load at startup and save
on normal shutdown, including sessions with no opened folder. Folder changes
keep the same preferences and docking layout. Photo colours stay with each grade;
photo controls never change future-photo defaults.

Old workspace JSON, per-folder layout and CWD ini files are ignored and left
untouched. See [settings locations and format](docs/workspace-format.md) and
[photo sidecars](SIDECAR.md). New application preferences can be added as optional
fields with defaults; no Settings UI or future preference features are added yet.

## Bundled plugins

### Crop

A bundled OFX plugin that simply crops the image. Placing it near the **beginning of the current
serial plugin chain** can improve efficiency because downstream plugins process fewer pixels; this
is guidance, not a permanent graph-placement restriction. It implements `getRegionOfDefinition`
to report the cropped output dimensions directly to the host.

**Parameters:**

- **Crop** — 0 = full image (identity), 100 = 2% of the original area.
- **Aspect** — crop window aspect ratio: *Original* (source ratio), *16:9*, *4:3*, *3:2*, *4:5*, *3:4*, *9:16*.
- **Offset X / Offset Y** — pan the crop window. At ±100 the window reaches the corresponding source edge. When the crop window fills the source in a dimension (e.g. a 4:5 aspect ratio on a wider source fills the height), the offset can slide the window **past** the source edge, and the out-of-bounds area is filled with **black pixels**.

---

## Download RawNode

Download the ZIP for your computer from [RawNode Releases](https://github.com/ryancara/RawNode/releases).
Each filename includes the release version.

| Platform | Archive |
| --- | --- |
| macOS 15 or later, Apple Silicon, JPEG XL ON (experimental) | `RawNode-macOS-arm64-JXL-ON-v<VERSION>.zip` |
| macOS 15 or later, Apple Silicon, JPEG XL OFF | `RawNode-macOS-arm64-JXL-OFF-v<VERSION>.zip` |
| macOS 15 or later, Intel, JPEG XL OFF | `RawNode-macOS-x86_64-JXL-OFF-v<VERSION>.zip` |
| Windows 10 version 1903 or later, x64, JPEG XL OFF | `RawNode-Windows-x64-JXL-OFF-v<VERSION>.zip` |

- **macOS:** Extract the ZIP and move `RawNode.app` to Applications.
  The app is ad-hoc signed. If Gatekeeper blocks the first launch, use
  **System Settings → Privacy & Security → Open Anyway**.
- **Windows:** Extract the entire ZIP and launch `RawNode.exe` from the
  extracted folder. Keep the DLLs, plugins and fonts alongside it. The app
  is unsigned and requires an OpenGL 3.3-capable graphics driver.

The Apple Silicon JPEG XL ON build is for testing integer JPEG XL DNG
decoding. See [JPEG XL DNG validation](docs/JPEG_XL_DNG.md) for its limits.

---

## Installing OFX plugins

RawNode loads plugins from the platform default OFX directory plus any paths in `OFX_PLUGIN_PATH`:

| Platform | Default path                                |
| -------- | ------------------------------------------- |
| macOS    | `/Library/OFX/Plugins`                      |
| Linux    | `/usr/OFX/Plugins`                          |
| Windows  | `C:\Program Files\Common Files\OFX\Plugins` |

Install plugins the way their authors recommend (installer, package, or copy `*.ofx.bundle`
into the directory above). For example on macOS:

```sh
sudo mkdir -p /Library/OFX/Plugins
sudo cp -R path/to/*.ofx.bundle /Library/OFX/Plugins/
```

Or point the host at a build directory without copying (apps launched from Finder
don't see shell variables, so start it from the terminal):

```sh
OFX_PLUGIN_PATH=/path/to/plugins /Applications/RawNode.app/Contents/MacOS/RawNode
```

Restart RawNode after installing plugins. If none are found, the status bar says so.

---

## Build

Requires CMake 3.16+, a C++17 compiler, JPEG and zlib development libraries,
[libtiff](https://libtiff.gitlab.io/libtiff/),
[Little CMS 2](https://www.littlecms.com/), and
[OpenEXR 3](https://openexr.com/) (with Imath) for the CTL backend.
GLFW, Dear ImGui and the [CTL](https://github.com/aces-aswf/CTL) reference
interpreter and pinned [LibRaw 0.22.3](https://www.libraw.org/) are fetched
automatically by CMake. No system LibRaw is used.

Integer JPEG XL DNG decoding is experimental and disabled by default.
See [JPEG XL DNG validation](docs/JPEG_XL_DNG.md) for the optional SDK build,
dependency pins, limitations and manual platform checks.

```sh
# macOS
brew install cmake jpeg-turbo libtiff little-cms2 openexr
export CMAKE_PREFIX_PATH="$(brew --prefix jpeg-turbo)${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

# Debian/Ubuntu
sudo apt install cmake pkg-config libglib2.0-dev libjpeg-dev libtiff-dev liblcms2-dev libopenexr-dev libgl1-mesa-dev xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols extra-cmake-modules

git clone --recursive https://github.com/ryancara/RawNode.git
cd RawNode
./build.sh
```

On macOS this produces `build/RawNode.app`. On Linux it produces `build/RawNode`.

```sh
# Enable native CPU instructions (AVX, F16C, etc.) for extra speed
OFX_NATIVE_ARCH=1 ./build.sh

# macOS self-test
build/RawNode.app/Contents/MacOS/RawNode --selftest

# Linux
build/RawNode --selftest
```

### macOS Apple Silicon development downloads

Successful macOS CI runs upload `RawNode-macOS-arm64-JXL-OFF` and
`RawNode-macOS-arm64-JXL-ON` under **Artifacts**. Download your chosen
artifact, extract the outer archive and then extract the enclosed ZIP to
obtain `RawNode.app`. These 14-day, ad-hoc-signed test builds require
**macOS 15 or later**. Gatekeeper may require **System Settings → Privacy &
Security → Open Anyway**. These builds are experimental.

JPEG XL ON contains the optional Adobe DNG SDK and is intended for testing
integer JPEG XL DNG support. Licence notices are bundled in the app Resources;
see [JPEG XL DNG validation](docs/JPEG_XL_DNG.md) for the remaining colour,
platform, security and release-readiness requirements.
### Windows x64

Use Windows 10 version 1903 or later, CMake 3.21 or later, Visual Studio 2022
with Desktop development with C++, and a bootstrapped vcpkg installation. Set
`VCPKG_INSTALLATION_ROOT` to the vcpkg directory. From a recursive checkout,
run the same Release build and full application self-test as Windows CI in
PowerShell.

```powershell
& "$env:VCPKG_INSTALLATION_ROOT/vcpkg.exe" install openexr:x64-windows tiff:x64-windows lcms:x64-windows libjpeg-turbo:x64-windows zlib:x64-windows --x-install-root="$pwd/native-deps/vcpkg"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_INSTALLED_DIR=$pwd/native-deps/vcpkg" `
  -DRAWNODE_ENABLE_JXL_DNG=OFF
cmake --build build --config Release --parallel 2
./build/Release/RawNode.exe --selftest
```

Launch `build/Release/RawNode.exe` on a desktop with an OpenGL 3.3-capable
graphics driver. The owner reports successful native Windows desktop validation
on 2026-11-20 using PR #120's portable x64 Release build; see the
[Windows validation record](docs/WINDOWS_VALIDATION.md). Windows JPEG XL support
remains outstanding. Windows Move to Trash in PR #122 has passed independent
review and owner-reported Windows desktop validation, including external
HDD/SSD. SD card testing remains unverified due to unavailable hardware, an
accepted non-blocking limitation; see the
[interactive contract and native evidence](docs/WINDOWS_TRASH_RESEARCH.md).

For manual validation without a compiler, download `RawNode-Windows-x64-Release`
from a successful Windows CI run's **Artifacts** section (GitHub sign-in is
required). Extract the artifact, then extract `RawNode-Windows-x64-Release.zip`
and launch `RawNode.exe` from its folder. Keep the bundled DLLs, `Plugins`
directory and icon font alongside it. These unsigned Release test builds are
retained for 14 days; they include a full self-test log from an isolated
extraction with the original checkout unavailable. This does not install the
application.

---

## Historical upstream release milestones

This table records upstream ofxrawhost milestones. Project/workspace persistence
here is historical and retired; current RawNode uses photo sidecars and global
settings. Current development milestones are in [ROADMAP.md](ROADMAP.md).

| Feature                                                             | Status         |
| ------------------------------------------------------------------- | -------------- |
| C++17 + Dear ImGui UI (cross-platform codebase)                     | Done (v0.2)    |
| Plugin chaining with reorder / bypass                               | Done (v0.3)    |
| Preview zoom / pan                                                  | Done (v0.3.2)  |
| TIFF open via libtiff (incl. half float)                            | Done (v0.3.3)  |
| Selectable ImGui themes (Photoshop default)                         | Done (v0.3.4)  |
| Performance: ICC caching, LTO, multithreaded resize, pooled buffers | Done (v0.3.5)  |
| Auto-detect input color space (ICC / RAW policy)                    | Done (v0.3.6)  |
| Workspace folder + filmstrip thumbnails                             | Done (v0.3.7)  |
| Project / source-sidecar JSON (editable per-source state)          | Done (v0.3.7)  |
| ImGui DockSpace layout + modular UI modules                         | Done (v0.3.7)  |
| Layered source layout (`imgio` / `ofx` / `persist` / `ui`)          | Done (v0.3.8)  |
| Correct JPEG/PNG passthrough preview color                          | Done (v0.3.8)  |
| Cached display buffer + Output tag recolor without re-render        | Done (v0.3.9)  |
| OFX multi-thread worker pool                                        | Done (v0.3.9)  |
| Open dialog / workspace filters (exclude sidecar JSON)              | Done (v0.3.9)  |
| OFX Metal GPU render support (spektrafilm diffuse/flow)             | Done (v0.3.10) |
| Selftest validation + performance instrumentation                   | Done (v0.3.10) |
| Fix OFX Metal render readback (blank spektrafilm output)            | Done (v0.3.11) |
| Click-to-step −/+ buttons on numeric parameter sliders              | Done (v0.3.11) |

---

## License

See [LICENSE](LICENSE). The OpenFX SDK in `third_party/openfx` (git submodule) is under its own license. Vendored headers under `third_party/stb`, `third_party/tinyexr`, `third_party/portable-file-dialogs`, and `third_party/fontawesome` keep their upstream licenses. UI themes in `src/ui/Themes.cpp` are from [ImThemes](https://github.com/Patitotective/ImThemes) (MIT).
