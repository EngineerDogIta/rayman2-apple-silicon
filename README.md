# Rayman 2: The Great Escape — Native Apple Silicon Port

A native `arm64-apple-macos` static binary recompilation port of **Rayman 2: The Great Escape** (PC GOG v2) for Apple Silicon Macs — no Wine, no VM, and no runtime x86 emulation.

> **Legal Notice (Bring Your Own Game):**  
> This repository contains **zero copyrighted Ubisoft/GOG game binaries or assets**. To build and play, you must supply your own legally purchased copy of *Rayman 2: The Great Escape* (GOG Windows installer `.exe` + `.bin`, or an already-installed PC game directory).

## Features
- **Native ARM64 Metal Rendering**: Glide 2.x HLE (`src/hle/glide2x_metal.cpp`) backed by Metal via `SDL3_GPU`, with dual-TMU texture caching, widescreen 16:9 Hor+ FOV, and centered 4:3 HUD scaling (`src/hooks/widescreen_and_focus.cpp`).
- **Native CoreAudio Streaming**: `winmm` `waveOut` + `mixer` HLE (`src/hle/winmm_waveout.cpp`) over `SDL_AudioStream`.
- **macOS & Gamepad Controls**: DirectInput 5 COM HLE (`src/hooks/macos_input.cpp`) supporting `WASD` + Arrows, Mouse/Trackpad camera, and DualSense/Xbox/MFi controllers (`SDL_Gamepad`).
- **Standalone `.app` Bundle**: `packaging/build_app_bundle.sh` produces a self-contained, ad-hoc signed `build/Rayman 2.app`.

## Prerequisites (Homebrew)
```bash
brew install cmake ninja llvm sdl3 innoextract mingw-w64 python3
```

## Quick Start (From GOG Installer)
1. Clone this repository:
   ```bash
   git clone <your-repo-url>
   cd rayman-mac-silicon-port
   ```
2. Place your GOG installer files (`setup_rayman_2_-_the_great_escape_1.0_gog_v2_*.exe` and `.bin`) anywhere convenient, then run:
   ```bash
   ./setup.sh /path/to/setup_rayman_2_-_the_great_escape_1.0_gog_v2_\(86383\).exe
   ```
   *(Or pass an already-extracted PC game directory containing `Rayman2.exe`.)*
3. Launch the app:
   ```bash
   open "build/Rayman 2.app"
   ```

## Default Controls
| Action | Keyboard / Mouse | Gamepad |
|---|---|---|
| Move | `W` `A` `S` `D` or Arrow Keys | Left Stick / D-Pad |
| Jump / Helicopter / Swim Up | `Space` | `A` / Cross |
| Shoot | `Left Click` / `F` / `K` | `X` / Square / `R2` |
| Strafe / Lock-On / Dive | `Right Click` / `Left Shift` / `L` | `L1` / `L2` |
| Camera Rotate | `Q` / `E` or Mouse | Right Stick |
| Camera Reset | `R` / `C` | `R1` / `R3` |
| First-Person Look | `V` | `Y` / Triangle |
| HUD Display | `Tab` / `J` | Back / Select |
| Confirm / Skip Cutscene | `Enter` / `Space` | `A` / Cross / Start |
| Quit | `Cmd+Q` / Window `[X]` / In-Game `EXIT` | — |
