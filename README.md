# Rayman 2: The Great Escape — Apple Silicon Port

A native `arm64-apple-macos` static binary recompilation port of **Rayman 2: The Great Escape** (PC GOG v2) for Apple Silicon Macs — no Wine, no VM, and no runtime x86 emulation.

> **Legal Notice (Bring Your Own Game):**  
> This repository contains **zero copyrighted Ubisoft/GOG game binaries or assets**. To build and play, you must supply your own legally purchased copy of *Rayman 2: The Great Escape* (GOG Windows installer `.exe` + `.bin`, or an already-installed PC game directory).

## Features
- **Native ARM64 Metal Rendering**: Glide 2.x HLE (`src/hle/glide2x_metal.cpp`) backed by Metal via `SDL3_GPU`, with dual-TMU texture caching, widescreen 16:9 Hor+ FOV, and centered 4:3 HUD scaling (`src/hooks/widescreen_and_focus.cpp`).
- **Native CoreAudio Streaming**: `winmm` `waveOut` + `mixer` HLE (`src/hle/winmm_waveout.cpp`) over `SDL_AudioStream`.
- **macOS & Gamepad Controls**: DirectInput 5 COM HLE (`src/hooks/macos_input.cpp`) supporting `WASD` + Arrows, Mouse/Trackpad camera, and DualSense/Xbox/MFi controllers (`SDL_Gamepad`).
- **Standalone `.app` Bundle**: `packaging/build_app_bundle.sh` produces a self-contained, ad-hoc signed `build/Rayman 2.app`.

---

## 1-Click Setup (Recommended)
1. Make sure [Homebrew](https://brew.sh) is installed on your Mac.
2. Download or clone this repository:
   ```bash
   git clone https://github.com/EngineerDogIta/rayman2-apple-silicon.git
   ```
3. Double-click **`Install Rayman 2.command`** in Finder.
   - It automatically installs any missing Homebrew build tools (`cmake`, `ninja`, `llvm`, `sdl3`, `innoextract`, `mingw-w64`, `python3`).
   - Prompts you with a native macOS file picker to select your GOG `setup_rayman_2_-_the_great_escape_*.exe` installer (keep its `.bin` file in the same folder) or an existing game folder.
   - Builds `Rayman 2.app` and offers to copy it to `/Applications` and launch it.

---

## CLI Setup (Advanced)
```bash
git clone https://github.com/EngineerDogIta/rayman2-apple-silicon.git
cd rayman2-apple-silicon
./setup.sh --auto-deps /path/to/setup_rayman_2_-_the_great_escape_1.0_gog_v2_\(86383\).exe
open "build/Rayman 2.app"
```
To clean and rebuild from scratch after pulling updates:
```bash
./setup.sh --clean
```

---

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

## License
Port, HLE, hooks, and build tooling are licensed under the [MIT License](LICENSE). Original *Rayman 2: The Great Escape* binaries and assets remain copyright Ubisoft Entertainment.
