#pragma once
// Glide 2.x ABI definitions + shared widescreen/display state between
// glide2x_metal.cpp, win32_extra.cpp, and widescreen_and_focus.cpp.

#include <cstdint>

namespace recomp::ray2 {

// Desired host presentation resolution (configured in main.cpp from RAY2_WIDTH/HEIGHT
// env vars or 1280x720 16:9 widescreen default) vs internal Glide resolution.
struct DisplayConfig {
  uint32_t host_width = 1280;
  uint32_t host_height = 720;
  uint32_t glide_width = 1024;
  uint32_t glide_height = 768;
  uint32_t main_hwnd = 0;
  bool widescreen = true;
};

DisplayConfig& GetDisplayConfig();

}  // namespace recomp::ray2
