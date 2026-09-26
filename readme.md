Light Host Reforge
---

A fork of [LightHost](https://github.com/opencma/LightHost) with the following changes:

- Ported to JUCE8
- Added VST2 and VST3 plug-in hosting
- Added support for Waves plugins (tested with Waves V15)
- Added plug-in scanning for standard and custom folders, with isolated helpers for crash-prone plug-ins
- Added effect-chain presets and saves the current rack when the app closes
- Added a main window for plug-in management, presets, and audio/MIDI settings
- Added plugin window toolbar
- Added Loopback audio device type, capable of capturing desktop audio into the effect chain (Windows only, no output support)
- Added MIDI input support for hosted instrument plugins
- Added timed helper-process checks for Windows MIDI responsiveness and plug-in scanning
- Added a default BPM option for plug-ins that need a continuously advancing host transport
- Support for resizing plugin windows, partial HiDPI support
- Support for keeping plugin windows on top
- Added plug-in bypass and latency status
- Added fade-in/fade-out transition when audio chain changes
- Added plug-in scan failure and helper stack-trace logs on Windows
- Changed to CMake build system
- Added release builds for Windows x64, macOS Apple Silicon, and macOS Intel

Notes:

- VST2 hosting is opt-in for local builds when a VST2 SDK is available; VST3 hosting is enabled on Windows and macOS
- AU hosting is enabled on macOS
- Windows loopback capture remains Windows-only
- If the Windows MIDI check times out, MIDI device restoration is skipped for that run
- Windows plug-in scan failures and timed-out helper thread stacks are logged to `%APPDATA%\Light Host\PluginScanFailures.log` and `PluginScanStackTraces.log`
- macOS release artifacts are currently unsigned and unnotarized

## Install lhc

Install the latest released `lhc` binary with a single command.

macOS:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/TheLazyCat00/LightHostReforge/master/install.sh)"
```

Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/TheLazyCat00/LightHostReforge/master/install.ps1 | iex
```

The installer downloads the matching asset from the latest GitHub release and installs
`lhc` together with its debugger symbols. On macOS it installs to
`/usr/local/bin` by default; on Windows it installs to
`%LOCALAPPDATA%\LightHostReforge\bin` and adds that directory to the user `PATH`.
Set `LHC_INSTALL_DIR` to override the destination. Re-run the same command to update.

## Build

VST2 hosting is disabled by default because JUCE requires separately supplied VST2
SDK headers. To enable it for a local build, use a VST2 SDK that you are authorized
to use and set `LIGHTHOST_ENABLE_VST2_HOST` plus `LIGHTHOST_VST2_SDK_PATH` when
configuring CMake:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=path\to\vcpkg.cmake `
  -DLIGHTHOST_ENABLE_VST2_HOST=ON `
  -DLIGHTHOST_VST2_SDK_PATH="C:\path\to\VST2_SDK"
```

The SDK is not included in this repository. Steinberg no longer accepts new VST2
license agreements, so builds that include VST2 support should only be distributed
if you already have the necessary rights.

Windows (using VS2022 + vcpkg):

```bash
vcpkg install juce asiosdk
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=path\to\vcpkg.cmake ..
MSBuild .\LightHostReforge.sln /p:Configuration=Release
```

macOS (using CMake + vcpkg):

```bash
vcpkg install juce
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Development environment

The canonical Unix development environment is defined by `devbox.json`.
On Linux, macOS, or Windows through WSL:

```bash
devbox install
devbox run configure
devbox run build
```

Devbox pins CMake, Ninja, pkg-config, and JUCE for reproducible local development
and Apple Silicon macOS CI builds. Native Windows CI remains on MSVC + vcpkg
because Devbox is not a native Windows build environment and the Windows host
needs the native ASIO/MSVC toolchain. The macOS Intel CI job also remains on
vcpkg because current Devbox/Nix upstream support for x86_64-darwin is broken.
The source supports both the JUCE 8.0.7 API used by vcpkg and JUCE 8.0.9+
used by the Devbox environment.

### Debug / CLI host

The build also produces **Light Host CLI**, a console-subsystem variant intended for
Binary Ninja, x64dbg, WinDbg, LLDB, and other reverse-engineering/debugging workflows.
It still runs the full JUCE message loop and can display the plugin's real editor.

`--debug` switches the host to a synthetic stereo device. The graph is prepared with
a normal sample rate and block size, but **no real-time audio callback thread is
started**. You can stop at a breakpoint for as long as necessary without underrunning
an ASIO/CoreAudio device or freezing a DAW.

Example:

```powershell
& ".\lhc.exe" --debug --plugin "C:\Program Files\Common Files\VST3\Example.vst3" --sample-rate 48000 --block-size 512
```

If a shell contains several plugin types, select one explicitly:

```powershell
& ".\lhc.exe" --debug --plugin "C:\Program Files\Common Files\VST3\WaveShell1-VST3 15.0_x64.vst3" --plugin-name "Clarity Vx"
```

Useful options:

- `--plugin <path>` loads a plugin directly and opens its editor; repeat it to load a chain.
- `--plugin-name <name>` selects a sub-plugin from the most recent shell path.
- `--append` keeps the persisted chain and appends CLI plugins instead of starting isolated.
- `--no-editor` skips automatically opening plugin GUIs.
- `--sample-rate <hz>` and `--block-size <samples>` configure the synthetic debug device.
- `--process-blocks <count>` manually processes silent blocks once at startup.
- `--exit-after-process` exits after the requested batch, which is useful for scripted debugger runs.
- `--help` prints the complete command-line reference.

Debug mode uses a separate settings file and permits multiple instances, so it can run
alongside the normal tray host without replacing its saved chain or audio-device setup.
The tray menu also exposes **Process 1 silent block** and **Process 100 silent blocks**
while debug mode is active.

`lhc` is intentionally built without optimisation, inlining, LTO, dead-code
stripping, or identical-code folding. Frame pointers and full debugger symbols are kept.
Windows builds emit and package a full PDB; macOS builds emit and package a dSYM. Set
`-DLIGHTHOST_CLI_KEEP_SYMBOLS=OFF` if you want an optimised CLI binary instead.

Pushing a tag matching `v*` runs the release workflow and publishes packaged Windows and macOS builds.

### Screenshot

![Light Host Reforge](Resources/LightHostReforge.png)

---

# Light Host

---

A simple VST/AU host for OS X, Windows, and Linux that sits in the menu/task bar.

### Features

See [#1](https://github.com/rolandoislas/LightHost/issues/1)

### Screenshot

![Light Host 1.2](http://i.imgur.com/UF9SWfC.jpg)
