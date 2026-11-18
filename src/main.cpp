// Native Apple Silicon entry point for Rayman 2: The Great Escape.
// Launches Rayman2.exe alongside its statically recompiled plugin DLLs:
//   - DLL\GliVd1vf.dll @ 0x10000000 (backed by Metal glide2x_metal.cpp)
//   - DLL\WAVPWBVR.dll @ 0x11000000 (backed by CoreAudio winmm_waveout.cpp)
//   - DLL\APMP5BVR.dll @ 0x12000000 (ADPCM decoder)

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "hle/glide2x_types.h"
#include "win32/process.h"

namespace {

bool HasRayman2Exe(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / "Rayman2.exe", ec);
}

std::string FindGameDir(const char* argv0) {
  if (const char* env = std::getenv("RAY2_GAME_DIR"); env && *env) {
    if (HasRayman2Exe(env)) return env;
  }

  if (argv0 && *argv0) {
    std::error_code ec;
    auto exe_dir = std::filesystem::weakly_canonical(argv0, ec).parent_path();
    auto bundle_res = exe_dir.parent_path() / "Resources" / "game";
    if (HasRayman2Exe(bundle_res)) return bundle_res.string();
    if (HasRayman2Exe(exe_dir / "game")) return (exe_dir / "game").string();
  }

  if (HasRayman2Exe("game")) return std::filesystem::absolute("game").string();
  return ".";
}

uint32_t EnvU32(const char* name, uint32_t def) {
  if (const char* s = std::getenv(name); s && *s) {
    long v = std::strtol(s, nullptr, 10);
    if (v > 0) return uint32_t(v);
  }
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "0");

  auto& disp = recomp::ray2::GetDisplayConfig();
  disp.host_width = EnvU32("RAY2_WIDTH", 1280);
  disp.host_height = EnvU32("RAY2_HEIGHT", 720);
  disp.widescreen = EnvU32("RAY2_WIDESCREEN", 1) != 0;

  std::string game_dir = FindGameDir(argc > 0 ? argv[0] : nullptr);
  std::string exe_path = (std::filesystem::path(game_dir) / "Rayman2.exe").string();
  std::string game_args;
  for (int i = 1; i < argc; i++) {
    if (!game_args.empty()) game_args += ' ';
    game_args += argv[i];
  }

  recomp::win32::GameConfig cfg{};
  cfg.id = "RAY2";
  cfg.exe_name = "Rayman2.exe";
  cfg.guest_dir = "C:\\Program Files\\Ubi Soft\\Rayman 2";
  cfg.fullscreen = recomp::win32::GameConfig::Fullscreen::Window;
  cfg.lifted_dlls = {
      {"DLL\\GliVd1vf.dll", 0x10000000u},
      {"DLL\\WAVPWBVR.dll", 0x11000000u},
      {"DLL\\APMP5BVR.dll", 0x12000000u},
  };

  return recomp::RunProcess(cfg, exe_path, game_dir, game_args);
}
