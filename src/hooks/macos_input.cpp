// macOS Input Overhaul for Rayman 2:
// 1. Complete DINPUT.dll COM HLE (DirectInputCreateA, IDirectInputA, IDirectInputDeviceA)
//    backing keyboard, mouse, and SDL_Gamepad (DualSense / Xbox / MFi).
// 2. Replaces R2_fn_InputEnum (0x495170) to always return DIENUM_CONTINUE (fix.c).
// 3. Replaces INO_fn_wReadKeyboard (0x496590) and IPT_fn_vEngineReadInput (0x413890)
//    to inject simultaneous macOS-ergonomic controls WITHOUT changing native Arrow/Enter/Esc:
//      - W/A/S/D + Arrow keys + Left Analog Stick -> 360-deg movement & menus
//      - Space / Gamepad A(Cross) -> Jump / Helicopter / Swim Up
//      - Left-Click / F / K / Gamepad X(Square) / R2 -> Shoot (no Ctrl+Arrow Mission Control!)
//      - Right-Click / Left-Shift / L / Gamepad L2/L1 -> Strafe / Lock-on / Dive (replaces Num0/End)
//      - Q/E + Captured Mouse Delta + Right Stick X -> Camera Rotate Left/Right
//      - R / C / Gamepad R1/R3 -> Camera Reset
//      - V / Gamepad Y(Triangle) -> First-Person Look
//      - Tab / J / Gamepad Back -> HUD Display

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/ddraw_types.h"
#include "win32/hle.h"
#include "win32/input_state.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "x86/lifted.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kGUID_SysMouse = {0x6F1D2B60u, 0xD5A0u, 0x11CFu,
                                 {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_SysKeyboard = {0x6F1D2B61u, 0xD5A0u, 0x11CFu,
                                    {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_Gamepad0 = {0x52415932u, 0x0001u, 0x0000u,
                                 {0x4D, 0x41, 0x43, 0x50, 0x41, 0x44, 0x30, 0x31}};

constexpr uint32_t kDI_OK = 0;
constexpr uint32_t kDIERR_INVALIDPARAM = 0x80070057u;
constexpr uint32_t kDIERR_NOTACQUIRED = 0x8007000Cu;
constexpr uint32_t kDIENUM_STOP = 0;

constexpr uint32_t kDIDEVTYPE_MOUSE = 2;
constexpr uint32_t kDIDEVTYPE_KEYBOARD = 3;
constexpr uint32_t kDIDEVTYPE_JOYSTICK = 4;

// Native SDL3 Gamepad manager
struct PadManager {
  SDL_Gamepad* pad = nullptr;
  SDL_JoystickID id = 0;

  void EnsureInit() {
    static bool inited = false;
    if (!inited) {
      SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC);
      inited = true;
    }
    if (pad && !SDL_GamepadConnected(pad)) {
      SDL_CloseGamepad(pad);
      pad = nullptr;
      id = 0;
    }
    if (!pad) {
      int count = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&count);
      if (ids && count > 0) {
        pad = SDL_OpenGamepad(ids[0]);
        id = ids[0];
        if (pad) {
          std::fprintf(stderr, "input: Connected SDL_Gamepad: %s\n", SDL_GetGamepadName(pad));
        }
      }
      if (ids) SDL_free(ids);
    }
  }
};

PadManager& Pads() {
  static PadManager pm;
  return pm;
}

// DIK scancode table from SDL_Scancode
uint8_t DikFromSdlScancode(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02;
    case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06;
    case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08;
    case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10;
    case SDL_SCANCODE_W: return 0x11;
    case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_T: return 0x14;
    case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16;
    case SDL_SCANCODE_I: return 0x17;
    case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E;
    case SDL_SCANCODE_S: return 0x1F;
    case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22;
    case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24;
    case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C;
    case SDL_SCANCODE_X: return 0x2D;
    case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F;
    case SDL_SCANCODE_B: return 0x30;
    case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B;
    case SDL_SCANCODE_F2: return 0x3C;
    case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E;
    case SDL_SCANCODE_F5: return 0x3F;
    case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41;
    case SDL_SCANCODE_F8: return 0x42;
    case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C;
    case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_RALT: return 0xB8;
    case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8;
    case SDL_SCANCODE_PAGEUP: return 0xC9;
    case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD;
    case SDL_SCANCODE_END: return 0xCF;
    case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1;
    case SDL_SCANCODE_INSERT: return 0xD2;
    case SDL_SCANCODE_DELETE: return 0xD3;
    default: return 0;
  }
}

std::atomic<int32_t> g_raw_mouse_dx{0};
std::atomic<int32_t> g_raw_mouse_dy{0};
int32_t g_frame_mouse_dx = 0;

void MacRawInputSink(const RawInput& ev) {
  if (ev.kind == RawInput::MouseX) g_raw_mouse_dx.fetch_add(ev.value, std::memory_order_relaxed);
  if (ev.kind == RawInput::MouseY) g_raw_mouse_dy.fetch_add(ev.value, std::memory_order_relaxed);
}

void FillDikKeyboardState(uint8_t out[256]) {
  uint8_t raw[256]{};
  // 1) Convert SwARc g_key_down[vk] (hardware + TEST_INPUT) to raw DIK scan codes
  for (uint32_t vk = 1; vk < 256; vk++) {
    if (!g_key_down[vk]) continue;
    bool ext = false;
    uint8_t sc = ScanCodeForVk(uint8_t(vk), &ext);
    if (sc) {
      uint8_t dik = uint8_t(sc | (ext ? 0x80u : 0u));
      raw[dik] = 0x80u;
    }
  }
  // 2) Overlay live SDL3 keyboard state (reliable even on focus/Mac Fn-arrows)
  int nkeys = 0;
  const bool* sdl_keys = SDL_GetKeyboardState(&nkeys);
  if (sdl_keys) {
    for (int sc = 0; sc < nkeys; sc++) {
      if (!sdl_keys[sc]) continue;
      uint8_t dik = DikFromSdlScancode(SDL_Scancode(sc));
      if (dik) raw[dik] = 0x80u;
    }
  }

  std::memcpy(out, raw, 256);
  // Prevent physical macOS ergonomic keys from firing conflicting default bindings before remap
  out[0x10] = 0;  // Q
  out[0x11] = 0;  // W
  out[0x1E] = 0;  // A
  out[0x1F] = 0;  // S
  out[0x20] = 0;  // D
  out[0x21] = 0;  // F
  out[0x25] = 0;  // K
  out[0x26] = 0;  // L
  out[0x2A] = 0;  // LShift
  out[0x36] = 0;  // RShift
  out[0x39] = 0;  // Space

  SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
  bool lmb = (mb & SDL_BUTTON_LMASK) || (g_key_down[0x01] != 0);
  bool rmb = (mb & SDL_BUTTON_RMASK) || (g_key_down[0x02] != 0);
  int32_t mdx = g_frame_mouse_dx;

  // Map ergonomic macOS keys + mouse into Rayman 2's native Game.dsb DIK bindings so
  // IPT_fn_vReadInput (0x42E710) updates every derived menu/movement/action entry natively:
  if (raw[0x11]) out[0xC8] = 0x80u;                  // W -> DIK_UP
  if (raw[0x1F]) out[0xD0] = 0x80u;                  // S -> DIK_DOWN
  if (raw[0x1E]) out[0xCB] = 0x80u;                  // A -> DIK_LEFT
  if (raw[0x20]) out[0xCD] = 0x80u;                  // D -> DIK_RIGHT
  if (raw[0x39]) out[0x1E] = 0x80u;                  // Space -> DIK_A (Jump / Swim Up)
  if (raw[0x21] || raw[0x25] || lmb) out[0x39] = 0x80u;  // F/K/LMB -> DIK_SPACE (Shoot)
  if (raw[0x2A] || raw[0x36] || raw[0x26] || rmb) out[0x1D] = 0x80u;  // Shift/L/RMB -> DIK_LCONTROL (Strafe)
  if (raw[0x38] || raw[0x3A]) out[0x2A] = 0x80u;     // LAlt/Caps -> DIK_LSHIFT (Walk Slow)
  if (raw[0x10] || mdx < -4) out[0x10] = 0x80u;      // Q / Mouse Left -> 0x10 (ipt[17] Cam Left)
  if (raw[0x12] || mdx > 4) out[0x11] = 0x80u;       // E / Mouse Right -> 0x11 (ipt[16] Cam Right)
  if (raw[0x13] || raw[0x2E]) out[0xCF] = 0x80u;     // R / C -> DIK_END (ipt[19] Cam Cut/Reset)
  if (raw[0x2F]) out[0x52] = 0x80u;                  // V -> DIK_NUMPAD0 (ipt[18] Cam Look)
  if (raw[0x0F] || raw[0x24]) out[0x24] = 0x80u;     // Tab / J -> DIK_J (HUD Gauge)
  if (raw[0x0E]) out[0x1C] = 0x80u;                  // Backspace -> DIK_RETURN (Cutscene Skip)

  // Dynamically ensure Backspace / Space / Return also fire the configured Game.dsb DIK key(s)
  // for cutscene skipping and dialogue advancing.
  if (raw[0x0E] || raw[0x39] || raw[0x1C] ||
      g_key_down[0x08] || g_key_down[0x20] || g_key_down[0x0D]) {
    uint32_t n_entries = Rd32(0x00509E60u + 0x700u);
    uint32_t arr = Rd32(0x00509E60u + 0x704u);
    if (arr && n_entries > 0 && n_entries < 512) {
      for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t elem = arr + i * 24u;
        uint32_t name_ptr = Rd32(elem + 0x08);
        if (name_ptr < 0x10000u || name_ptr >= 0x80000000u) continue;
        char name[128]{};
        for (size_t j = 0; j + 1 < sizeof(name); j++) {
          char ch = char(Rd8(name_ptr + uint32_t(j)));
          if (!ch) break;
          name[j] = ch;
        }
        if (!std::strstr(name, "Couper") && !std::strstr(name, "Cinematique") &&
            !std::strstr(name, "Texte")) {
          continue;
        }
        uint32_t n_kw = Rd32(elem + 0x00);
        uint32_t kw = Rd32(elem + 0x04);
        if (!kw || n_kw < 2 || n_kw > 64) continue;
        for (uint32_t k = 0; k + 1 < n_kw; k++) {
          if (Rd16(kw + k * 8u) == 0xF006u) {
            uint32_t dik = Rd16(kw + (k + 1u) * 8u) & 0xFFu;
            if (dik > 0 && dik < 256) out[dik] = 0x80u;
          }
        }
      }
    }
  }
}

// ---- DINPUT.dll COM Objects ------------------------------------------------

struct DirectInputObj : com::Object {
  uint32_t version = 0x0500;
};

struct DirectInputDeviceObj : com::Object {
  enum class Kind { Keyboard, Mouse, Gamepad } kind = Kind::Keyboard;
  uint32_t hwnd = 0;
  uint32_t coop = 0;
  uint32_t buffer_size = 0;
  bool acquired = false;
  std::array<uint8_t, 256> prev_keys{};
};

uint32_t DirectInputVtable();
uint32_t DirectInputDeviceVtable();

void WriteGuid(uint32_t dst, const GUID& g) {
  Wr32(dst + 0, g.Data1);
  Wr16(dst + 4, g.Data2);
  Wr16(dst + 6, g.Data3);
  std::memcpy(GPtr(dst + 8), g.Data4, 8);
}

void WriteDeviceInstanceA(uint32_t dst, const GUID& inst, uint32_t devtype, const char* name) {
  uint32_t sz = Rd32(dst);
  if (sz < 292) sz = 580;
  std::memset(GPtr(dst), 0, sz);
  Wr32(dst + 0, sz);
  WriteGuid(dst + 4, inst);
  WriteGuid(dst + 20, inst);
  Wr32(dst + 36, devtype);
  size_t len = std::strlen(name);
  std::memcpy(GPtr(dst + 40), name, std::min<size_t>(len, 259));
  if (sz >= 560) std::memcpy(GPtr(dst + 300), name, std::min<size_t>(len, 259));
}

HLE("dinput.dll", DirectInputCreateA, 4) {
  uint32_t ver = Arg(c, 1), out = Arg(c, 2);
  if (!out) return kDIERR_INVALIDPARAM;
  SetRawInputSink(&MacRawInputSink);
  auto di = std::make_shared<DirectInputObj>();
  di->version = ver;
  Wr32(out, com::Publish(di, DirectInputVtable()));
  return kDI_OK;
}

HLE_NAMED("dinput.dll", "IDirectInputA::QueryInterface", DI_QI, 3) {
  com::AddRef(Arg(c, 0));
  Wr32(Arg(c, 2), Arg(c, 0));
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputA::AddRef", DI_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput.dll", "IDirectInputA::Release", DI_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("dinput.dll", "IDirectInputA::CreateDevice", DI_CreateDevice, 4) {
  uint32_t rguid = Arg(c, 1), out = Arg(c, 2);
  if (!rguid || !out) return kDIERR_INVALIDPARAM;
  auto dev = std::make_shared<DirectInputDeviceObj>();
  if (com::GuidEq(rguid, kGUID_SysMouse))
    dev->kind = DirectInputDeviceObj::Kind::Mouse;
  else if (com::GuidEq(rguid, kGUID_Gamepad0))
    dev->kind = DirectInputDeviceObj::Kind::Gamepad;
  else
    dev->kind = DirectInputDeviceObj::Kind::Keyboard;
  Wr32(out, com::Publish(dev, DirectInputDeviceVtable()));
  return kDI_OK;
}

HLE_NAMED("dinput.dll", "IDirectInputA::EnumDevices", DI_EnumDevices, 5) {
  uint32_t dev_type = Arg(c, 1), cb = Arg(c, 2), ctx = Arg(c, 3);
  if (!cb) return kDIERR_INVALIDPARAM;
  uint32_t inst = uint32_t(HeapAllocProcess(580, true));

  auto report = [&](const GUID& g, uint32_t t, const char* n) -> bool {
    if (dev_type != 0 && (dev_type & 0xFFu) != (t & 0xFFu)) return true;
    Wr32(inst, 580);
    WriteDeviceInstanceA(inst, g, t, n);
    return CallGuest(c, cb, {inst, ctx}) != kDIENUM_STOP;
  };

  bool cont = report(kGUID_SysKeyboard, kDIDEVTYPE_KEYBOARD | (4u << 8), "macOS Keyboard");
  if (cont) cont = report(kGUID_SysMouse, kDIDEVTYPE_MOUSE | (2u << 8), "macOS Mouse");
  Pads().EnsureInit();
  if (cont && Pads().pad) {
    report(kGUID_Gamepad0, kDIDEVTYPE_JOYSTICK | (4u << 8), SDL_GetGamepadName(Pads().pad));
  }
  HeapFreeProcess(inst);
  return kDI_OK;
}

HLE_NAMED("dinput.dll", "IDirectInputA::GetDeviceStatus", DI_GetDevStatus, 2) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputA::RunControlPanel", DI_RunCP, 3) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputA::Initialize", DI_Init, 3) { return kDI_OK; }

// ---- IDirectInputDeviceA ---------------------------------------------------

HLE_NAMED("dinput.dll", "IDirectInputDeviceA::QueryInterface", DID_QI, 3) {
  com::AddRef(Arg(c, 0));
  Wr32(Arg(c, 2), Arg(c, 0));
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::AddRef", DID_AddRef, 1) {
  return com::AddRef(Arg(c, 0));
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::Release", DID_Release, 1) {
  return com::Release(Arg(c, 0));
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::GetCapabilities", DID_GetCaps, 2) {
  auto* d = com::As<DirectInputDeviceObj>(Arg(c, 0));
  uint32_t p = Arg(c, 1);
  if (!d || !p) return kDIERR_INVALIDPARAM;
  uint32_t sz = Rd32(p);
  std::memset(GPtr(p), 0, sz);
  Wr32(p + 0, sz);
  Wr32(p + 4, 1);  // DIDC_ATTACHED
  Wr32(p + 8, d->kind == DirectInputDeviceObj::Kind::Keyboard
                  ? (kDIDEVTYPE_KEYBOARD | (4u << 8))
                  : (d->kind == DirectInputDeviceObj::Kind::Mouse ? (kDIDEVTYPE_MOUSE | (2u << 8))
                                                                  : (kDIDEVTYPE_JOYSTICK | (4u << 8))));
  Wr32(p + 12, d->kind == DirectInputDeviceObj::Kind::Keyboard ? 0 : 4);
  Wr32(p + 16, d->kind == DirectInputDeviceObj::Kind::Keyboard ? 128 : 16);
  Wr32(p + 20, d->kind == DirectInputDeviceObj::Kind::Gamepad ? 1 : 0);
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::EnumObjects", DID_EnumObj, 4) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::GetProperty", DID_GetProp, 3) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::SetProperty", DID_SetProp, 3) {
  if (auto* d = com::As<DirectInputDeviceObj>(Arg(c, 0))) {
    if (Arg(c, 1) == 1 && Arg(c, 2)) d->buffer_size = Rd32(Arg(c, 2) + 12);
  }
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::Acquire", DID_Acquire, 1) {
  if (auto* d = com::As<DirectInputDeviceObj>(Arg(c, 0))) d->acquired = true;
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::Unacquire", DID_Unacquire, 1) {
  if (auto* d = com::As<DirectInputDeviceObj>(Arg(c, 0))) d->acquired = false;
  return kDI_OK;
}

HLE_NAMED("dinput.dll", "IDirectInputDeviceA::GetDeviceState", DID_GetDevState, 3) {
  auto* d = com::As<DirectInputDeviceObj>(Arg(c, 0));
  uint32_t sz = Arg(c, 1), data = Arg(c, 2);
  if (!d || !data) return kDIERR_INVALIDPARAM;
  std::memset(GPtr(data), 0, sz);

  if (d->kind == DirectInputDeviceObj::Kind::Keyboard) {
    uint8_t kb[256];
    FillDikKeyboardState(kb);
    std::memcpy(GPtr(data), kb, std::min<uint32_t>(sz, 256));
    return kDI_OK;
  }
  if (d->kind == DirectInputDeviceObj::Kind::Mouse) {
    SDL_MouseButtonFlags btns = SDL_GetMouseState(nullptr, nullptr);
    int32_t rx = g_raw_mouse_dx.exchange(0, std::memory_order_relaxed);
    int32_t ry = g_raw_mouse_dy.exchange(0, std::memory_order_relaxed);
    if (sz >= 16) {
      Wr32(data + 0, uint32_t(rx));
      Wr32(data + 4, uint32_t(ry));
      Wr32(data + 8, 0);
      Wr8(data + 12, ((btns & SDL_BUTTON_LMASK) || g_key_down[0x01]) ? 0x80 : 0);
      Wr8(data + 13, ((btns & SDL_BUTTON_RMASK) || g_key_down[0x02]) ? 0x80 : 0);
      Wr8(data + 14, ((btns & SDL_BUTTON_MMASK) || g_key_down[0x04]) ? 0x80 : 0);
    }
    return kDI_OK;
  }
  // Gamepad (DIJOYSTATE: lX, lY, lZ, lRx, lRy, lRz, sliders[2], pov[4], rgbButtons[32])
  Pads().EnsureInit();
  if (Pads().pad && sz >= 80) {
    auto axis_u16 = [](int16_t v) -> int32_t {
      return int32_t(v) + 32768;  // 0..65535 (center 32768)
    };
    Wr32(data + 0, uint32_t(axis_u16(SDL_GetGamepadAxis(Pads().pad, SDL_GAMEPAD_AXIS_LEFTX))));
    Wr32(data + 4, uint32_t(axis_u16(SDL_GetGamepadAxis(Pads().pad, SDL_GAMEPAD_AXIS_LEFTY))));
    Wr32(data + 8, uint32_t(axis_u16(SDL_GetGamepadAxis(Pads().pad, SDL_GAMEPAD_AXIS_RIGHTX))));
    Wr32(data + 20, uint32_t(axis_u16(SDL_GetGamepadAxis(Pads().pad, SDL_GAMEPAD_AXIS_RIGHTY))));
    for (int i = 0; i < 4; i++) Wr32(data + 32 + 4 * i, 0xFFFFFFFFu);
    static const SDL_GamepadButton kPadMap[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    };
    for (int i = 0; i < 10; i++) {
      Wr8(data + 48 + i, SDL_GetGamepadButton(Pads().pad, kPadMap[i]) ? 0x80 : 0);
    }
  }
  return kDI_OK;
}

HLE_NAMED("dinput.dll", "IDirectInputDeviceA::GetDeviceData", DID_GetDevData, 5) {
  uint32_t pcount = Arg(c, 3);
  if (pcount) Wr32(pcount, 0);
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::SetDataFormat", DID_SetDataFmt, 2) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::SetEventNotification", DID_SetEvt, 2) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::SetCooperativeLevel", DID_SetCoop, 3) {
  if (auto* d = com::As<DirectInputDeviceObj>(Arg(c, 0))) {
    d->hwnd = Arg(c, 1);
    d->coop = Arg(c, 2);
  }
  return kDI_OK;
}
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::GetObjectInfo", DID_GetObjInfo, 4) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::GetDeviceInfo", DID_GetDevInfo, 2) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::RunControlPanel", DID_RunCP, 2) { return kDI_OK; }
HLE_NAMED("dinput.dll", "IDirectInputDeviceA::Initialize", DID_Init, 4) { return kDI_OK; }

uint32_t DirectInputVtable() {
  return com::Vtable("dinput.dll", "IDirectInputA",
                     {
                         "QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
                         "GetDeviceStatus", "RunControlPanel", "Initialize",
                     });
}

uint32_t DirectInputDeviceVtable() {
  return com::Vtable("dinput.dll", "IDirectInputDeviceA",
                     {
                         "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
                         "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState",
                         "GetDeviceData", "SetDataFormat", "SetEventNotification",
                         "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo",
                         "RunControlPanel", "Initialize",
                     });
}

// ---- Engine IPT / macOS + Gamepad Entry Overlays ---------------------------

// IPT_tdstEntryElement layout (24 bytes):
// +00: ulNumberOfKeyWordElement (u32)
// +04: d_stKeyWordElementArray (ptr)
// +08: p_szEntryName (ptr)
// +0C: lState (i32: >0 frame counter held, <0 released counter)
// +10: xAnalogicValue (f32)
// +14: bIsActivate (u32)
//
// IPT_g_stInputStructure = 0x509E60:
// +0700: ulNumberOfEntryElement (u32)
// +0704: d_stEntryElementArray (ptr)
constexpr uint32_t kIPT_InputStruct = 0x00509E60u;
constexpr uint32_t kIPT_FieldPadGlobalVecX = 0x004B9B68u;
constexpr uint32_t kIPT_FieldPadGlobalVecY = 0x004B9B6Cu;
constexpr uint32_t kIPT_FieldPadHorizontalAxis = 0x004B9B74u;
constexpr uint32_t kIPT_FieldPadVerticalAxis = 0x004B9B76u;
constexpr uint32_t kIPT_FieldPadAnalogForce = 0x004B9B78u;
constexpr uint32_t kIPT_FieldPadTrueAnalogForce = 0x004B9B7Cu;
constexpr uint32_t kIPT_FieldPadRotationAngle = 0x004B9B80u;
constexpr uint32_t kIPT_FieldPadSector = 0x004B9B84u;

void ReadGuestName(uint32_t ptr, char* out, size_t cap) {
  out[0] = '\0';
  if (ptr < 0x10000u || ptr >= 0x80000000u || cap == 0) return;
  size_t i = 0;
  for (; i + 1 < cap; i++) {
    char ch = char(Rd8(ptr + uint32_t(i)));
    if (!ch) break;
    out[i] = ch;
  }
  out[i] = '\0';
}

bool InjectDigitalEntry(uint32_t arr, uint32_t count, uint32_t entry_idx,
                        const std::array<int32_t, 256>& prev_lstate, uint32_t prev_count,
                        std::array<bool, 256>& overridden, bool active) {
  if (entry_idx >= count || !arr || !active) return false;
  uint32_t elem = arr + entry_idx * 24u;
  if (entry_idx < overridden.size()) overridden[entry_idx] = true;
  int32_t orig = int32_t(Rd32(elem + 0x0C));
  if (orig <= 0) {
    int32_t prev = (entry_idx < prev_count) ? prev_lstate[entry_idx] : 0;
    int32_t next_st = (prev > 0) ? (prev < 0x7FFFFFFF ? prev + 1 : 0x7FFFFFFF) : 1;
    Wr32(elem + 0x0C, uint32_t(next_st));
    uint32_t one = std::bit_cast<uint32_t>(100.0f);
    Wr32(elem + 0x10, one);
    Wr32(elem + 0x14, 1);
  }
  return true;
}

void InjectAnalogEntry(uint32_t arr, uint32_t count, uint32_t entry_idx,
                       const std::array<int32_t, 256>& prev_lstate, uint32_t prev_count,
                       std::array<bool, 256>& overridden, float val_pct) {
  if (entry_idx >= count || !arr || std::fabs(val_pct) < 1.0f) return;
  uint32_t elem = arr + entry_idx * 24u;
  if (entry_idx < overridden.size()) overridden[entry_idx] = true;
  Wr32(elem + 0x10, std::bit_cast<uint32_t>(val_pct));
  int32_t cur = int32_t(Rd32(elem + 0x0C));
  if (cur <= 0) {
    int32_t prev = (entry_idx < prev_count) ? prev_lstate[entry_idx] : 0;
    int32_t next_st = (prev > 0) ? (prev < 0x7FFFFFFF ? prev + 1 : 0x7FFFFFFF) : 1;
    Wr32(elem + 0x0C, uint32_t(next_st));
  }
  Wr32(elem + 0x14, 1);
}

void ApplyMacOsAndGamepadOverlay(Ctx& c, const std::array<int32_t, 256>& prev_lstate,
                                 uint32_t prev_count) {
  uint32_t n_entries = Rd32(kIPT_InputStruct + 0x700);
  uint32_t arr = Rd32(kIPT_InputStruct + 0x704);
  if (!arr || n_entries < 25) return;

  static bool s_dumped_ipt = (std::getenv("RAY2_DEBUG_IPT") == nullptr);
  if (!s_dumped_ipt) {
    s_dumped_ipt = true;
    uint32_t dump_n = std::min<uint32_t>(n_entries, 256);
    std::fprintf(stderr, "ipt: entry table n_entries=%u arr=0x%08x\n", n_entries, arr);
    for (uint32_t i = 0; i < dump_n; i++) {
      uint32_t elem = arr + i * 24u;
      char name[128];
      ReadGuestName(Rd32(elem + 0x08), name, sizeof(name));
      uint32_t n_kw = Rd32(elem + 0x00);
      uint32_t kw = Rd32(elem + 0x04);
      uint16_t kw0 = (kw && n_kw >= 1) ? Rd16(kw + 0) : 0;
      uint32_t kw1 = (kw && n_kw >= 2) ? Rd32(kw + 8) : 0;
      uint16_t kw2 = (kw && n_kw >= 3) ? Rd16(kw + 16) : 0;
      uint32_t kw3 = (kw && n_kw >= 4) ? Rd32(kw + 24) : 0;
      std::fprintf(stderr,
                   "ipt[%3u]: act=%u lSt=%d name=\"%s\" n_kw=%u [0]=0x%04x [1]=0x%08x [2]=0x%04x [3]=0x%08x\n",
                   i, Rd8(elem + 0x14), int32_t(Rd32(elem + 0x0C)), name, n_kw, kw0, kw1, kw2, kw3);
    }
  }

  int nkeys = 0;
  const bool* k = SDL_GetKeyboardState(&nkeys);
  auto key = [&](SDL_Scancode sc, uint8_t vk = 0) -> bool {
    return (k && sc < nkeys && k[sc]) || (vk != 0 && g_key_down[vk] != 0);
  };

  float mouse_dx = float(g_frame_mouse_dx);
  SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);

  Pads().EnsureInit();
  SDL_Gamepad* pad = Pads().pad;
  auto pad_btn = [&](SDL_GamepadButton b) -> bool { return pad && SDL_GetGamepadButton(pad, b); };
  auto pad_axis = [&](SDL_GamepadAxis a) -> float {
    if (!pad) return 0.0f;
    float v = float(SDL_GetGamepadAxis(pad, a)) / 32767.0f;
    return (std::fabs(v) < 0.16f) ? 0.0f : std::clamp(v, -1.0f, 1.0f);
  };

  // Analog & WASD composite directional axes (Up/Down Invert matches Rayman2 screen-forward)
  float lx = pad_axis(SDL_GAMEPAD_AXIS_LEFTX);
  float ly = pad_axis(SDL_GAMEPAD_AXIS_LEFTY);
  if (pad_btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) lx = -1.0f;
  if (pad_btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) lx = 1.0f;
  if (pad_btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) ly = -1.0f;
  if (pad_btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) ly = 1.0f;

  // When game is NOT suspended, inject ergonomic WASD movement + Mac bindings:
  bool move_up = key(SDL_SCANCODE_W, 'W') || (ly < -0.25f);
  bool move_down = key(SDL_SCANCODE_S, 'S') || (ly > 0.25f);
  bool move_left = key(SDL_SCANCODE_A, 'A') || (lx < -0.25f);
  bool move_right = key(SDL_SCANCODE_D, 'D') || (lx > 0.25f);

  bool jump = key(SDL_SCANCODE_SPACE, 0x20) || pad_btn(SDL_GAMEPAD_BUTTON_SOUTH);
  bool shoot = (mb & SDL_BUTTON_LMASK) || g_key_down[0x01] || key(SDL_SCANCODE_F, 'F') ||
               key(SDL_SCANCODE_K, 'K') || pad_btn(SDL_GAMEPAD_BUTTON_WEST) ||
               (pad_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.35f);
  bool strafe = (mb & SDL_BUTTON_RMASK) || g_key_down[0x02] || key(SDL_SCANCODE_LSHIFT, 0x10) ||
                key(SDL_SCANCODE_L, 'L') || pad_btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
                (pad_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.35f);
  bool dive = key(SDL_SCANCODE_LSHIFT, 0x10) || key(SDL_SCANCODE_Z, 'Z') ||
              pad_btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
  bool walk_slow = key(SDL_SCANCODE_LALT, 0x12) || key(SDL_SCANCODE_CAPSLOCK, 0x14) ||
                   (std::hypot(lx, ly) > 0.16f && std::hypot(lx, ly) < 0.55f);

  float rx = pad_axis(SDL_GAMEPAD_AXIS_RIGHTX);
  bool cam_right = key(SDL_SCANCODE_E, 'E') || (rx > 0.35f) || (mouse_dx > 4.0f);
  bool cam_left = key(SDL_SCANCODE_Q, 'Q') || (rx < -0.35f) || (mouse_dx < -4.0f);
  bool cam_look = key(SDL_SCANCODE_V, 'V') || pad_btn(SDL_GAMEPAD_BUTTON_NORTH);
  bool cam_reset = key(SDL_SCANCODE_R, 'R') || key(SDL_SCANCODE_C, 'C') ||
                   pad_btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ||
                   pad_btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
  bool hud = key(SDL_SCANCODE_TAB, 0x09) || key(SDL_SCANCODE_J, 'J') ||
             pad_btn(SDL_GAMEPAD_BUTTON_BACK);
  bool menu_start = pad_btn(SDL_GAMEPAD_BUTTON_START);
  bool menu_cancel = pad_btn(SDL_GAMEPAD_BUTTON_EAST);
  bool cutscene_skip = key(SDL_SCANCODE_SPACE, 0x20) || key(SDL_SCANCODE_RETURN, 0x0D) ||
                       key(SDL_SCANCODE_BACKSPACE, 0x08) || pad_btn(SDL_GAMEPAD_BUTTON_START) ||
                       pad_btn(SDL_GAMEPAD_BUTTON_SOUTH);

  std::array<bool, 256> overridden{};
  // Mark entries already activated by native DirectInput so composite pass doesn't overwrite analogs
  uint32_t count = std::min<uint32_t>(n_entries, 256);

  // Indices match IPT_tdeEntryActionName (0..24) verified in ACP_Ray2/IPT/IPT_Const.h:
  if (std::fabs(lx) > 0.16f)
    InjectAnalogEntry(arr, n_entries, 0 /* Pad_AxeX */, prev_lstate, prev_count, overridden, lx * 100.0f);
  if (std::fabs(ly) > 0.16f)
    InjectAnalogEntry(arr, n_entries, 1 /* Pad_AxeY */, prev_lstate, prev_count, overridden, ly * 100.0f);

  InjectDigitalEntry(arr, n_entries, 2 /* Clavier_Haut */, prev_lstate, prev_count, overridden, move_up);
  InjectDigitalEntry(arr, n_entries, 3 /* Clavier_Bas */, prev_lstate, prev_count, overridden, move_down);
  InjectDigitalEntry(arr, n_entries, 4 /* Clavier_Droite */, prev_lstate, prev_count, overridden, move_right);
  InjectDigitalEntry(arr, n_entries, 5 /* Clavier_Gauche */, prev_lstate, prev_count, overridden, move_left);
  InjectDigitalEntry(arr, n_entries, 6 /* Clavier_Ralentir */, prev_lstate, prev_count, overridden, walk_slow);
  InjectDigitalEntry(arr, n_entries, 7 /* Menu_Entrer */, prev_lstate, prev_count, overridden, menu_start);
  InjectDigitalEntry(arr, n_entries, 8 /* Menu_Annuler */, prev_lstate, prev_count, overridden, menu_cancel);
  InjectDigitalEntry(arr, n_entries, 9 /* Menu_Valider */, prev_lstate, prev_count, overridden, jump && menu_start);
  InjectDigitalEntry(arr, n_entries, 10 /* Sauter */, prev_lstate, prev_count, overridden, jump);
  InjectDigitalEntry(arr, n_entries, 11 /* Tirer */, prev_lstate, prev_count, overridden, shoot);
  InjectDigitalEntry(arr, n_entries, 13 /* Strafe */, prev_lstate, prev_count, overridden, strafe);
  InjectDigitalEntry(arr, n_entries, 14 /* Nage_Plonger */, prev_lstate, prev_count, overridden, dive);
  InjectDigitalEntry(arr, n_entries, 15 /* Nage_Remonter */, prev_lstate, prev_count, overridden, jump);
  InjectDigitalEntry(arr, n_entries, 16 /* Camera_TourneDroite */, prev_lstate, prev_count, overridden, cam_right);
  InjectDigitalEntry(arr, n_entries, 17 /* Camera_TourneGauche */, prev_lstate, prev_count, overridden, cam_left);
  InjectDigitalEntry(arr, n_entries, 18 /* Camera_Look */, prev_lstate, prev_count, overridden, cam_look);
  InjectDigitalEntry(arr, n_entries, 19 /* Camera_Cut */, prev_lstate, prev_count, overridden, cam_reset);
  InjectDigitalEntry(arr, n_entries, 20 /* CouperCinematique */, prev_lstate, prev_count, overridden, cutscene_skip);
  InjectDigitalEntry(arr, n_entries, 21 /* TexteSuivant */, prev_lstate, prev_count, overridden,
                     shoot || jump || cutscene_skip);
  InjectDigitalEntry(arr, n_entries, 24 /* Affiche_Jauge */, prev_lstate, prev_count, overridden, hud);

  // Re-evaluate all non-overridden composite/derived IPT_tdstEntryElements in ascending order
  // so entries 40..138 (MoveLeft/Right/Up/Down, Button_Jump, Button_Paf, Cutscene skip wrappers, etc.)
  // immediately reflect our injected base states and analog overrides in the same frame!
  bool any_valid = Rd32(0x00509E40u) != 0;
  for (uint32_t i = 0; i < count; i++) {
    if (overridden[i]) {
      any_valid = true;
      continue;
    }
    uint32_t elem = arr + i * 24u;
    if (Rd8(elem + 0x14) != 1u) continue;
    uint32_t n_kw = Rd32(elem + 0x00);
    uint32_t kw = Rd32(elem + 0x04);
    if (!kw || !n_kw) continue;
    int32_t prev = (i < prev_count) ? prev_lstate[i] : 0;
    Wr32(elem + 0x0C, uint32_t(prev));
    uint32_t valid = uint32_t(CallGuest(c, 0x0042EAC0u, {elem})) & 0xFFu;
    int32_t next_st;
    if (valid) {
      next_st = (prev > 0) ? (prev < 0x7FFFFFFF ? prev + 1 : 0x7FFFFFFF) : 1;
      any_valid = true;
    } else if (prev > 0) {
      next_st = -1;
    } else if (prev < 0) {
      next_st = (prev > int32_t(0x80000000u)) ? (prev - 1) : int32_t(0x80000000u);
    } else {
      next_st = 0;
    }
    Wr32(elem + 0x0C, uint32_t(next_st));
  }
  if (any_valid) Wr32(0x00509E40u, Rd32(0x00509E40u) | 1u);

  // If analog stick is actively deflected, also feed true analog vector into FieldPad globals!
  float mag = std::hypot(lx, ly);
  if (mag > 0.16f) {
    float norm_x = std::clamp(lx * 100.0f, -100.0f, 100.0f);
    float norm_y = std::clamp(ly * 100.0f, -100.0f, 100.0f);
    float clamped = std::min(1.0f, mag);
    Wr32(kIPT_FieldPadHorizontalAxis, std::bit_cast<uint32_t>(norm_x));
    Wr32(kIPT_FieldPadVerticalAxis, std::bit_cast<uint32_t>(norm_y));
    Wr32(kIPT_FieldPadAnalogForce, std::bit_cast<uint32_t>(clamped));
    Wr32(kIPT_FieldPadTrueAnalogForce, std::bit_cast<uint32_t>(clamped));
    (void)kIPT_FieldPadGlobalVecX;
    (void)kIPT_FieldPadGlobalVecY;
    (void)kIPT_FieldPadRotationAngle;
    (void)kIPT_FieldPadSector;
  }
}

}  // namespace
}  // namespace recomp::hle

namespace recomp::gen {

void f_0042EAC0(::recomp::Ctx& c);
void f_00495170_lifted(Ctx& c);
void f_00496590_lifted(Ctx& c);
void f_00413890_lifted(Ctx& c);

// R2_fn_InputEnum (0x495170): always return DIENUM_CONTINUE (1) so gamepads are never skipped
void f_00495170(Ctx& c) {
  f_00495170_lifted(c);
  c.eax = 1;  // DIENUM_CONTINUE
}

// INO_fn_wReadKeyboard (0x496590)
void f_00496590(Ctx& c) { f_00496590_lifted(c); }

// IPT_fn_vEngineReadInput (0x413890): run IPT_fn_vReadInput (0x42E710), apply our
// macOS + Gamepad overlay BEFORE 0x413860 consumes d_stEntryElementArray, then dispatch.
void f_00413890(Ctx& c) {
  (void)f_00413890_lifted;
  (void)&f_0042EAC0;
  recomp::hle::g_frame_mouse_dx =
      recomp::hle::g_raw_mouse_dx.exchange(0, std::memory_order_relaxed);
  (void)recomp::hle::g_raw_mouse_dy.exchange(0, std::memory_order_relaxed);
  if (Rd8(0x005003DBu) == 0) {
    std::array<int32_t, 256> prev_lstate{};
    uint32_t n_entries = Rd32(recomp::hle::kIPT_InputStruct + 0x700);
    uint32_t arr = Rd32(recomp::hle::kIPT_InputStruct + 0x704);
    uint32_t prev_count = (arr && n_entries > 0) ? std::min<uint32_t>(n_entries, 256) : 0u;
    for (uint32_t i = 0; i < prev_count; i++) {
      prev_lstate[i] = int32_t(Rd32(arr + i * 24u + 0x0C));
    }
    CallGuest(c, 0x0042E710u, {});
    recomp::hle::ApplyMacOsAndGamepadOverlay(c, prev_lstate, prev_count);
    uint32_t ok = uint32_t(CallGuest(c, 0x0042E850u, {Rd32(0x005002C0u)})) & 0xFFu;
    if (ok == 1u) {
      CallGuest(c, 0x00409E90u, {});
      CallGuest(c, 0x0041B110u, {});
      CallGuest(c, 0x00409F00u, {});
    }
  }
  c.esp += 4u;
}

}  // namespace recomp::gen
