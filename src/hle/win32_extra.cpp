// Extra Win32 HLE implementations needed by Rayman 2 & plugins:
// - KERNEL32: SetCurrentDirectoryA, GetWindowsDirectoryA, Get/WritePrivateProfileStringA,
//   OpenEventA, CompareStringA/W, FatalAppExitA
// - USER32: OffsetRect, ToAsciiEx, MapVirtualKeyExA, LoadKeyboardLayoutA, GetKeyboardLayoutNameA
// - SHELL32: ShellExecuteA
// - WINMM: timeGetDevCaps
// - DDRAW: DirectDrawCreate, IDirectDraw / IDirectDraw2, IDirectDrawSurface, IDirectDrawClipper

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/ddraw_types.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"
#include "x86/lifted.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

using namespace win32;

uint32_t CopyStrOut(uint32_t buf, uint32_t cap, const std::string& s) {
  if (!buf || cap == 0) return uint32_t(s.size());
  uint32_t n = std::min<uint32_t>(uint32_t(s.size()), cap - 1);
  if (n) std::memcpy(GPtr(buf), s.data(), n);
  Wr8(buf + n, 0);
  return n;
}

std::string Trim(std::string s) {
  auto ws = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
  size_t a = 0;
  while (a < s.size() && ws(uint8_t(s[a]))) a++;
  size_t b = s.size();
  while (b > a && ws(uint8_t(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

bool Equali(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  }
  return true;
}

bool FileExists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string ResolveIniHostPath(const std::string& guest_file, PathAccess access) {
  std::string primary = HostPath(guest_file, access);
  if (FileExists(primary)) return primary;
  // Rayman 2 asks for "C:\WINDOWS\UbiSoft\ubi.ini" or "UbiSoft\ubi.ini".
  // Check install/overlay "ubi.ini" or "UbiSoft\ubi.ini" fallback cleanly.
  std::string lower = Lower(guest_file);
  size_t sl = lower.find_last_of("\\/");
  std::string base = (sl == std::string::npos) ? lower : lower.substr(sl + 1);
  if (base == "ubi.ini" || base == "snd_cpa.ini" || base == "goginput.ini") {
    std::string local = HostPath(base, access);
    if (access != PathAccess::kRead || FileExists(local)) return local;
    std::string dll_local = HostPath("DLL\\" + base, access);
    if (FileExists(dll_local)) return dll_local;
  }
  return primary;
}

struct IniSection {
  std::string name;
  std::vector<std::string> lines;  // raw lines belonging to this section
};

struct IniDoc {
  std::vector<std::string> preamble;
  std::vector<IniSection> sections;
};

IniDoc ParseIniFile(const std::string& host_path) {
  IniDoc doc;
  std::ifstream in(host_path);
  if (!in.is_open()) return doc;
  std::string line;
  IniSection* cur = nullptr;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = Trim(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      doc.sections.push_back(IniSection{t.substr(1, t.size() - 2), {}});
      cur = &doc.sections.back();
    } else if (cur) {
      cur->lines.push_back(line);
    } else {
      doc.preamble.push_back(line);
    }
  }
  return doc;
}

void WriteIniFile(const std::string& host_path, const IniDoc& doc) {
  std::error_code ec;
  auto parent = std::filesystem::path(host_path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  std::ofstream out(host_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return;
  for (const auto& l : doc.preamble) out << l << "\r\n";
  for (const auto& sec : doc.sections) {
    out << "[" << sec.name << "]\r\n";
    for (const auto& l : sec.lines) out << l << "\r\n";
  }
}

uint32_t WriteNullTerminatedList(uint32_t buf, uint32_t cap, const std::vector<std::string>& items) {
  if (!buf || cap < 2) return 0;
  uint32_t pos = 0;
  for (const auto& it : items) {
    if (pos + 2 >= cap) break;
    uint32_t take = std::min<uint32_t>(uint32_t(it.size()), cap - 2 - pos);
    if (take) std::memcpy(GPtr(buf + pos), it.data(), take);
    pos += take;
    Wr8(buf + pos++, 0);
  }
  Wr8(buf + pos, 0);
  if (items.empty()) return 0;
  return pos;
}

// ---- KERNEL32 additions ----------------------------------------------------

HLE("kernel32.dll", SetCurrentDirectoryA, 1) {
  return SetGuestCurrentDir(GStr(Arg(c, 0))) ? 1 : 0;
}

HLE("kernel32.dll", GetWindowsDirectoryA, 2) {
  uint32_t buf = Arg(c, 0), sz = Arg(c, 1);
  return CopyStrOut(buf, sz, "C:\\WINDOWS");
}

HLE("kernel32.dll", GetPrivateProfileStringA, 6) {
  uint32_t p_app = Arg(c, 0), p_key = Arg(c, 1), p_def = Arg(c, 2);
  uint32_t p_out = Arg(c, 3), out_sz = Arg(c, 4), p_file = Arg(c, 5);
  std::string file = GStr(p_file);
  std::string def = p_def ? Trim(GStr(p_def)) : "";
  std::string hp = ResolveIniHostPath(file, PathAccess::kRead);
  IniDoc doc = ParseIniFile(hp);

  if (!p_app) {
    std::vector<std::string> names;
    for (const auto& s : doc.sections) names.push_back(s.name);
    return WriteNullTerminatedList(p_out, out_sz, names);
  }
  std::string app = GStr(p_app);
  const IniSection* sec = nullptr;
  for (const auto& s : doc.sections) {
    if (Equali(s.name, app)) {
      sec = &s;
      break;
    }
  }
  if (!p_key) {
    std::vector<std::string> keys;
    if (sec) {
      for (const auto& line : sec->lines) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq != std::string::npos) keys.push_back(Trim(t.substr(0, eq)));
      }
    }
    return WriteNullTerminatedList(p_out, out_sz, keys);
  }
  std::string key = GStr(p_key);
  if (Equali(app, "DLL_Sxd") && (Equali(key, "File0") || Equali(key, "Default"))) {
    return CopyStrOut(p_out, out_sz, "WAVPW");
  }
  if (Equali(app, "DLL_Adpcm") && (Equali(key, "File0") || Equali(key, "Default"))) {
    return CopyStrOut(p_out, out_sz, "APMP5");
  }
  if (sec) {
    for (const auto& line : sec->lines) {
      std::string t = Trim(line);
      if (t.empty() || t[0] == ';') continue;
      size_t eq = t.find('=');
      if (eq == std::string::npos) continue;
      if (Equali(Trim(t.substr(0, eq)), key)) {
        std::string val = Trim(t.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
          val = val.substr(1, val.size() - 2);
        return CopyStrOut(p_out, out_sz, val);
      }
    }
  }
  return CopyStrOut(p_out, out_sz, def);
}

HLE("kernel32.dll", WritePrivateProfileStringA, 4) {
  uint32_t p_app = Arg(c, 0), p_key = Arg(c, 1), p_val = Arg(c, 2), p_file = Arg(c, 3);
  if (!p_app || !p_file) return 0;
  std::string app = GStr(p_app), file = GStr(p_file);
  std::string read_hp = ResolveIniHostPath(file, PathAccess::kRead);
  std::string write_hp = ResolveIniHostPath(file, PathAccess::kWrite);
  IniDoc doc = ParseIniFile(FileExists(write_hp) ? write_hp : read_hp);

  auto sec_it = std::find_if(doc.sections.begin(), doc.sections.end(),
                             [&](const IniSection& s) { return Equali(s.name, app); });
  if (!p_key) {
    if (sec_it != doc.sections.end()) doc.sections.erase(sec_it);
    WriteIniFile(write_hp, doc);
    return 1;
  }
  std::string key = GStr(p_key);
  if (sec_it == doc.sections.end()) {
    if (!p_val) return 1;
    doc.sections.push_back(IniSection{app, {key + "=" + GStr(p_val)}});
    WriteIniFile(write_hp, doc);
    return 1;
  }
  for (auto it = sec_it->lines.begin(); it != sec_it->lines.end(); ++it) {
    std::string t = Trim(*it);
    size_t eq = t.find('=');
    if (eq != std::string::npos && Equali(Trim(t.substr(0, eq)), key)) {
      if (!p_val) {
        sec_it->lines.erase(it);
      } else {
        *it = key + "=" + GStr(p_val);
      }
      WriteIniFile(write_hp, doc);
      return 1;
    }
  }
  if (p_val) sec_it->lines.push_back(key + "=" + GStr(p_val));
  WriteIniFile(write_hp, doc);
  return 1;
}

// OpenEventA is implemented in SwARc kernel32_sync.cpp via OpenNamed(Event).

template <typename CharT>
uint32_t CompareStringsImpl(uint32_t flags, const CharT* s1, int32_t n1, const CharT* s2,
                            int32_t n2) {
  bool nocase = (flags & 0x1u) != 0;  // NORM_IGNORECASE
  size_t len1 = n1 < 0 ? 0 : size_t(n1);
  if (n1 < 0)
    while (s1[len1]) len1++;
  size_t len2 = n2 < 0 ? 0 : size_t(n2);
  if (n2 < 0)
    while (s2[len2]) len2++;
  size_t common = std::min(len1, len2);
  for (size_t i = 0; i < common; i++) {
    uint32_t a = uint32_t(s1[i]), b = uint32_t(s2[i]);
    if (nocase) {
      if (a < 128) a = uint32_t(std::tolower(int(a)));
      if (b < 128) b = uint32_t(std::tolower(int(b)));
    }
    if (a < b) return 1;  // CSTR_LESS_THAN
    if (a > b) return 3;  // CSTR_GREATER_THAN
  }
  if (len1 < len2) return 1;
  if (len1 > len2) return 3;
  return 2;  // CSTR_EQUAL
}

HLE("kernel32.dll", CompareStringA, 6) {
  uint32_t flags = Arg(c, 1);
  const auto* s1 = reinterpret_cast<const uint8_t*>(GPtr(Arg(c, 2)));
  int32_t n1 = int32_t(Arg(c, 3));
  const auto* s2 = reinterpret_cast<const uint8_t*>(GPtr(Arg(c, 4)));
  int32_t n2 = int32_t(Arg(c, 5));
  return CompareStringsImpl(flags, s1, n1, s2, n2);
}

HLE("kernel32.dll", CompareStringW, 6) {
  uint32_t flags = Arg(c, 1);
  const auto* s1 = reinterpret_cast<const uint16_t*>(GPtr(Arg(c, 2)));
  int32_t n1 = int32_t(Arg(c, 3));
  const auto* s2 = reinterpret_cast<const uint16_t*>(GPtr(Arg(c, 4)));
  int32_t n2 = int32_t(Arg(c, 5));
  return CompareStringsImpl(flags, s1, n1, s2, n2);
}

HLE("kernel32.dll", FatalAppExitA, 2) {
  std::string msg = Arg(c, 1) ? GStr(Arg(c, 1)) : "(null)";
  FatalF("FatalAppExitA(%u): %s", uint32_t(Arg(c, 0)), msg.c_str());
}

// ---- USER32 additions ------------------------------------------------------

HLE("user32.dll", OffsetRect, 3) {
  uint32_t rc = Arg(c, 0);
  int32_t dx = int32_t(Arg(c, 1)), dy = int32_t(Arg(c, 2));
  if (!rc) return 0;
  Wr32(rc + 0, uint32_t(int32_t(Rd32(rc + 0)) + dx));
  Wr32(rc + 4, uint32_t(int32_t(Rd32(rc + 4)) + dy));
  Wr32(rc + 8, uint32_t(int32_t(Rd32(rc + 8)) + dx));
  Wr32(rc + 12, uint32_t(int32_t(Rd32(rc + 12)) + dy));
  return 1;
}

uint32_t VkToAscii(uint32_t vk, bool shift) {
  if (vk >= 'A' && vk <= 'Z') return shift ? vk : (vk + 32);
  if (vk >= '0' && vk <= '9') {
    static const char* sh = ")!@#$%^&*(";
    return shift ? uint32_t(sh[vk - '0']) : vk;
  }
  switch (vk) {
    case 0x20: return ' ';
    case 0x08: return '\b';
    case 0x09: return '\t';
    case 0x0D: return '\r';
    case 0x1B: return 0x1B;
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    default: return 0;
  }
}

HLE("user32.dll", ToAsciiEx, 6) {
  uint32_t vk = Arg(c, 0), p_state = Arg(c, 2), p_char = Arg(c, 3);
  bool shift = p_state && ((Rd8(p_state + 0x10) & 0x80) != 0);
  uint32_t ch = VkToAscii(vk, shift);
  if (!ch) return 0;
  if (p_char) Wr16(p_char, uint16_t(ch));
  return 1;
}

HLE("user32.dll", MapVirtualKeyExA, 3) {
  uint32_t code = Arg(c, 0), map_type = Arg(c, 1);
  // Minimal US layout mapping: VK_LSHIFT/RSHIFT/letters/digits/arrows
  struct Pair {
    uint16_t vk, sc;
  };
  static const Pair kTable[] = {
      {0x1B, 0x01}, {'1', 0x02},  {'2', 0x03},  {'3', 0x04},  {'4', 0x05},  {'5', 0x06},
      {'6', 0x07},  {'7', 0x08},  {'8', 0x09},  {'9', 0x0A},  {'0', 0x0B},  {0xBD, 0x0C},
      {0xBB, 0x0D}, {0x08, 0x0E}, {0x09, 0x0F}, {'Q', 0x10},  {'W', 0x11},  {'E', 0x12},
      {'R', 0x13},  {'T', 0x14},  {'Y', 0x15},  {'U', 0x16},  {'I', 0x17},  {'O', 0x18},
      {'P', 0x19},  {0xDB, 0x1A}, {0xDD, 0x1B}, {0x0D, 0x1C}, {0x11, 0x1D}, {'A', 0x1E},
      {'S', 0x1F},  {'D', 0x20},  {'F', 0x21},  {'G', 0x22},  {'H', 0x23},  {'J', 0x24},
      {'K', 0x25},  {'L', 0x26},  {0xBA, 0x27}, {0xDE, 0x28}, {0xC0, 0x29}, {0x10, 0x2A},
      {0xDC, 0x2B}, {'Z', 0x2C},  {'X', 0x2D},  {'C', 0x2E},  {'V', 0x2F},  {'B', 0x30},
      {'N', 0x31},  {'M', 0x32},  {0xBC, 0x33}, {0xBE, 0x34}, {0xBF, 0x35}, {0x12, 0x38},
      {0x20, 0x39}, {0x14, 0x3A}, {0x26, 0x48}, {0x25, 0x4B}, {0x27, 0x4D}, {0x28, 0x50},
      {0x2D, 0x52}, {0x2E, 0x53}, {0x24, 0x47}, {0x23, 0x4F}, {0x21, 0x49}, {0x22, 0x51},
  };
  if (map_type == 0 || map_type == 4) {  // MAPVK_VK_TO_VSC
    for (const auto& p : kTable)
      if (p.vk == code) return p.sc;
    return 0;
  }
  if (map_type == 1 || map_type == 3) {  // MAPVK_VSC_TO_VK(_EX)
    for (const auto& p : kTable)
      if (p.sc == (code & 0x7F)) return p.vk;
    return 0;
  }
  if (map_type == 2) {  // MAPVK_VK_TO_CHAR
    return VkToAscii(code, true);
  }
  return 0;
}

HLE("user32.dll", LoadKeyboardLayoutA, 2) { return 0x04090409u; }

HLE("user32.dll", GetKeyboardLayoutNameA, 1) {
  if (uint32_t p = Arg(c, 0)) PutStr(p, "00000409");
  return 1;
}

// ---- SHELL32 & WINMM timer caps --------------------------------------------

HLE("shell32.dll", ShellExecuteA, 6) {
  RECOMP_TRACE("  ShellExecuteA(%s)\n", Arg(c, 2) ? GStr(Arg(c, 2)).c_str() : "");
  return 33;  // > 32 = success
}

HLE("winmm.dll", timeGetDevCaps, 2) {
  uint32_t ptc = Arg(c, 0), cb = Arg(c, 1);
  if (!ptc || cb < 8) return 96;  // TIMERR_NOCANDO
  Wr32(ptc + 0, 1);
  Wr32(ptc + 4, 1000000);
  return 0;
}

// ---- DDRAW.dll (DirectDraw 1 / 2 + Surface + Clipper for GliVd1vf) ---------

constexpr uint32_t kDD_OK = 0;
constexpr uint32_t kDDERR_INVALIDPARAMS = 0x80070057u;
constexpr uint32_t kDDENUMRET_OK = 1;
constexpr guest::GUID kIID_IDirectDraw2 = {0xB3A6F3E0u, 0x2B43u, 0x11CFu,
                                         {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};

struct DDrawObj : com::Object {
  uint32_t width = 1024, height = 768, bpp = 16;
  uint32_t hwnd = 0;
};

struct DDrawSurfaceObj : com::Object {
  uint32_t width = 1024, height = 768, bpp = 16, pitch = 2048;
  uint32_t caps = 0;
  uint32_t clipper = 0;
  gaddr pixels = 0;
  bool primary = false;
  ~DDrawSurfaceObj() override {
    if (pixels) HeapFreeProcess(pixels);
  }
};

struct DDrawClipperObj : com::Object {
  uint32_t hwnd = 0;
};

uint32_t DirectDrawVtable();
uint32_t DirectDraw2Vtable();
uint32_t DirectDrawSurfaceVtable();
uint32_t DirectDrawClipperVtable();

void WritePixelFormat16(uint32_t ddpf) {
  std::memset(GPtr(ddpf), 0, 32);
  Wr32(ddpf + 0x00, 32);
  Wr32(ddpf + 0x04, 0x40);  // DDPF_RGB
  Wr32(ddpf + 0x0C, 16);
  Wr32(ddpf + 0x10, 0xF800);
  Wr32(ddpf + 0x14, 0x07E0);
  Wr32(ddpf + 0x18, 0x001F);
}

void WriteSurfaceDesc1(uint32_t ddsd, const DDrawSurfaceObj* s, bool include_surface_ptr) {
  uint32_t sz = Rd32(ddsd);
  if (sz < 108) sz = 108;
  std::memset(GPtr(ddsd), 0, sz);
  Wr32(ddsd + 0x00, sz);
  uint32_t flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;  // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
  if (include_surface_ptr) flags |= 0x800;          // LPSURFACE
  Wr32(ddsd + 0x04, flags);
  Wr32(ddsd + 0x08, s->height);
  Wr32(ddsd + 0x0C, s->width);
  Wr32(ddsd + 0x10, s->pitch);
  if (include_surface_ptr) Wr32(ddsd + 0x24, uint32_t(s->pixels));
  WritePixelFormat16(ddsd + 0x48);
  Wr32(ddsd + 0x68, s->caps);
}

uint32_t DDrawCreateSurfaceImpl(Ctx& c, DDrawObj* dd, uint32_t ddsd, uint32_t out) {
  if (!ddsd || !out) return kDDERR_INVALIDPARAMS;
  uint32_t flags = Rd32(ddsd + 4);
  uint32_t caps = (flags & 1) ? Rd32(ddsd + 0x68) : 0;
  auto surf = std::make_shared<DDrawSurfaceObj>();
  surf->primary = (caps & 0x200u) != 0;  // DDSCAPS_PRIMARYSURFACE
  surf->caps = caps ? caps : 0x40u;      // DDSCAPS_OFFSCREENPLAIN
  surf->width = (!surf->primary && (flags & 4)) ? Rd32(ddsd + 0x0C) : dd->width;
  surf->height = (!surf->primary && (flags & 2)) ? Rd32(ddsd + 0x08) : dd->height;
  surf->bpp = 16;
  surf->pitch = surf->width * 2;
  surf->pixels = HeapAllocProcess(size_t(surf->pitch) * std::max<uint32_t>(1, surf->height), true);
  Wr32(out, com::Publish(surf, DirectDrawSurfaceVtable()));
  return kDD_OK;
}

uint32_t DDrawCreateClipperImpl(uint32_t out) {
  if (!out) return kDDERR_INVALIDPARAMS;
  Wr32(out, com::Publish(std::make_shared<DDrawClipperObj>(), DirectDrawClipperVtable()));
  return kDD_OK;
}

uint32_t DDrawEnumDisplayModesImpl(Ctx& c, uint32_t cb, uint32_t ctx) {
  struct Mode {
    uint32_t w, h, bpp;
  };
  static const Mode kModes[] = {
      {640, 480, 16}, {800, 600, 16}, {1024, 768, 16}, {1280, 720, 16},
      {1280, 800, 16}, {1600, 900, 16}, {1920, 1080, 16},
  };
  uint32_t ddsd = uint32_t(HeapAllocProcess(108, true));
  for (const auto& m : kModes) {
    DDrawSurfaceObj tmp;
    tmp.width = m.w;
    tmp.height = m.h;
    tmp.bpp = m.bpp;
    tmp.pitch = m.w * 2;
    Wr32(ddsd, 108);
    WriteSurfaceDesc1(ddsd, &tmp, false);
    if (CallGuest(c, cb, {ddsd, ctx}) != kDDENUMRET_OK) break;
  }
  HeapFreeProcess(ddsd);
  return kDD_OK;
}

HLE("ddraw.dll", DirectDrawCreate, 3) {
  uint32_t out = Arg(c, 1);
  if (!out) return kDDERR_INVALIDPARAMS;
  Wr32(out, com::Publish(std::make_shared<DDrawObj>(), DirectDrawVtable()));
  return kDD_OK;
}

// ---- IDirectDraw -----------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirectDraw::QueryInterface", DD1_QueryInterface, 3) {
  uint32_t self = Arg(c, 0), iid = Arg(c, 1), out = Arg(c, 2);
  auto* d = com::As<DDrawObj>(self);
  if (!d || !out) return com::kE_NOINTERFACE;
  if (com::GuidEq(iid, kIID_IDirectDraw2)) {
    auto copy = std::make_shared<DDrawObj>(*d);
    copy->guest = 0;
    copy->refs = 1;
    Wr32(out, com::Publish(copy, DirectDraw2Vtable()));
    return kDD_OK;
  }
  com::AddRef(self);
  Wr32(out, self);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::AddRef", DD1_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDraw::Release", DD1_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDraw::Compact", DD1_Compact, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::CreateClipper", DD1_CreateClipper, 4) {
  return DDrawCreateClipperImpl(Arg(c, 2));
}
HLE_NAMED("ddraw.dll", "IDirectDraw::CreatePalette", DD1_CreatePalette, 5) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::CreateSurface", DD1_CreateSurface, 4) {
  return DDrawCreateSurfaceImpl(c, com::As<DDrawObj>(Arg(c, 0)), Arg(c, 1), Arg(c, 2));
}
HLE_NAMED("ddraw.dll", "IDirectDraw::DuplicateSurface", DD1_DuplicateSurface, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::EnumDisplayModes", DD1_EnumDisplayModes, 5) {
  return DDrawEnumDisplayModesImpl(c, Arg(c, 4), Arg(c, 3));
}
HLE_NAMED("ddraw.dll", "IDirectDraw::EnumSurfaces", DD1_EnumSurfaces, 5) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::FlipToGDISurface", DD1_FlipToGDISurface, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::GetCaps", DD1_GetCaps, 3) {
  for (uint32_t p : {Arg(c, 1), Arg(c, 2)}) {
    if (p) {
      uint32_t sz = Rd32(p);
      if (sz >= 4) {
        std::memset(GPtr(p), 0, sz);
        Wr32(p, sz);
        if (sz >= 12) Wr32(p + 4, 0x00800000);  // DDCAPS_3D
      }
    }
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::GetDisplayMode", DD1_GetDisplayMode, 2) {
  auto* dd = com::As<DDrawObj>(Arg(c, 0));
  uint32_t ddsd = Arg(c, 1);
  if (!dd || !ddsd) return kDDERR_INVALIDPARAMS;
  DDrawSurfaceObj tmp;
  tmp.width = dd->width;
  tmp.height = dd->height;
  tmp.pitch = dd->width * 2;
  WriteSurfaceDesc1(ddsd, &tmp, false);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::GetFourCCCodes", DD1_GetFourCCCodes, 3) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::GetGDISurface", DD1_GetGDISurface, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::GetMonitorFrequency", DD1_GetMonitorFrequency, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 60);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::GetScanLine", DD1_GetScanLine, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::GetVerticalBlankStatus", DD1_GetVerticalBlankStatus, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 1);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::Initialize", DD1_Initialize, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::RestoreDisplayMode", DD1_RestoreDisplayMode, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw::SetCooperativeLevel", DD1_SetCooperativeLevel, 3) {
  if (auto* dd = com::As<DDrawObj>(Arg(c, 0))) dd->hwnd = Arg(c, 1);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::SetDisplayMode", DD1_SetDisplayMode, 4) {
  if (auto* dd = com::As<DDrawObj>(Arg(c, 0))) {
    dd->width = Arg(c, 1);
    dd->height = Arg(c, 2);
    dd->bpp = Arg(c, 3);
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw::WaitForVerticalBlank", DD1_WaitForVerticalBlank, 3) {
  return kDD_OK;
}

// ---- IDirectDraw2 (adds SetDisplayMode 6-arg + GetAvailableVidMem) ---------

HLE_NAMED("ddraw.dll", "IDirectDraw2::QueryInterface", DD2_QueryInterface, 3) {
  com::AddRef(Arg(c, 0));
  Wr32(Arg(c, 2), Arg(c, 0));
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::AddRef", DD2_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDraw2::Release", DD2_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDraw2::Compact", DD2_Compact, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::CreateClipper", DD2_CreateClipper, 4) {
  return DDrawCreateClipperImpl(Arg(c, 2));
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::CreatePalette", DD2_CreatePalette, 5) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::CreateSurface", DD2_CreateSurface, 4) {
  return DDrawCreateSurfaceImpl(c, com::As<DDrawObj>(Arg(c, 0)), Arg(c, 1), Arg(c, 2));
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::DuplicateSurface", DD2_DuplicateSurface, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::EnumDisplayModes", DD2_EnumDisplayModes, 5) {
  return DDrawEnumDisplayModesImpl(c, Arg(c, 4), Arg(c, 3));
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::EnumSurfaces", DD2_EnumSurfaces, 5) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::FlipToGDISurface", DD2_FlipToGDISurface, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetCaps", DD2_GetCaps, 3) { return hle_DD1_GetCaps(c); }
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetDisplayMode", DD2_GetDisplayMode, 2) {
  return hle_DD1_GetDisplayMode(c);
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetFourCCCodes", DD2_GetFourCCCodes, 3) {
  return hle_DD1_GetFourCCCodes(c);
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetGDISurface", DD2_GetGDISurface, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetMonitorFrequency", DD2_GetMonitorFrequency, 2) {
  return hle_DD1_GetMonitorFrequency(c);
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetScanLine", DD2_GetScanLine, 2) { return hle_DD1_GetScanLine(c); }
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetVerticalBlankStatus", DD2_GetVerticalBlankStatus, 2) {
  return hle_DD1_GetVerticalBlankStatus(c);
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::Initialize", DD2_Initialize, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::RestoreDisplayMode", DD2_RestoreDisplayMode, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw2::SetCooperativeLevel", DD2_SetCooperativeLevel, 3) {
  return hle_DD1_SetCooperativeLevel(c);
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::SetDisplayMode", DD2_SetDisplayMode, 6) {
  if (auto* dd = com::As<DDrawObj>(Arg(c, 0))) {
    dd->width = Arg(c, 1);
    dd->height = Arg(c, 2);
    dd->bpp = Arg(c, 3);
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::WaitForVerticalBlank", DD2_WaitForVerticalBlank, 3) {
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDraw2::GetAvailableVidMem", DD2_GetAvailableVidMem, 4) {
  if (Arg(c, 2)) Wr32(Arg(c, 2), 64u * 1024u * 1024u);
  if (Arg(c, 3)) Wr32(Arg(c, 3), 64u * 1024u * 1024u);
  return kDD_OK;
}

// ---- IDirectDrawSurface ----------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirectDrawSurface::QueryInterface", DDS1_QI, 3) {
  com::AddRef(Arg(c, 0));
  Wr32(Arg(c, 2), Arg(c, 0));
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::AddRef", DDS1_AddRef, 1) {
  return com::AddRef(Arg(c, 0));
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Release", DDS1_Release, 1) {
  return com::Release(Arg(c, 0));
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::AddAttachedSurface", DDS1_AddAtt, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::AddOverlayDirtyRect", DDS1_Overlay, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Blt", DDS1_Blt, 6) {
  auto* dst = com::As<DDrawSurfaceObj>(Arg(c, 0));
  auto* src = com::As<DDrawSurfaceObj>(Arg(c, 2));
  if (dst && src && dst->pixels && src->pixels) {
    uint32_t w = std::min(dst->width, src->width);
    uint32_t h = std::min(dst->height, src->height);
    for (uint32_t y = 0; y < h; y++)
      std::memcpy(GPtr(dst->pixels + y * dst->pitch), GPtr(src->pixels + y * src->pitch), w * 2);
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::BltBatch", DDS1_BltBatch, 4) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::BltFast", DDS1_BltFast, 6) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::DeleteAttachedSurface", DDS1_DelAtt, 3) {
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::EnumAttachedSurfaces", DDS1_EnumAtt, 3) {
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::EnumOverlayZOrders", DDS1_EnumOv, 4) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Flip", DDS1_Flip, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetAttachedSurface", DDS1_GetAtt, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetBltStatus", DDS1_GetBltSt, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetCaps", DDS1_GetCaps, 2) {
  if (auto* s = com::As<DDrawSurfaceObj>(Arg(c, 0))) {
    if (Arg(c, 1)) Wr32(Arg(c, 1), s->caps);
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetClipper", DDS1_GetClipper, 2) {
  if (auto* s = com::As<DDrawSurfaceObj>(Arg(c, 0))) {
    if (Arg(c, 1)) Wr32(Arg(c, 1), s->clipper);
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetColorKey", DDS1_GetCK, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetDC", DDS1_GetDC, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0xDC01u);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetFlipStatus", DDS1_GetFlipSt, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetOverlayPosition", DDS1_GetOvPos, 3) {
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetPalette", DDS1_GetPal, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetPixelFormat", DDS1_GetPF, 2) {
  if (Arg(c, 1)) WritePixelFormat16(Arg(c, 1));
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetSurfaceDesc", DDS1_GetDesc, 2) {
  auto* s = com::As<DDrawSurfaceObj>(Arg(c, 0));
  if (s && Arg(c, 1)) WriteSurfaceDesc1(Arg(c, 1), s, false);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Initialize", DDS1_Init, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::IsLost", DDS1_IsLost, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Lock", DDS1_Lock, 5) {
  auto* s = com::As<DDrawSurfaceObj>(Arg(c, 0));
  uint32_t ddsd = Arg(c, 2);
  if (!s || !ddsd) return kDDERR_INVALIDPARAMS;
  WriteSurfaceDesc1(ddsd, s, true);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::ReleaseDC", DDS1_RelDC, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Restore", DDS1_Restore, 1) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::SetClipper", DDS1_SetClipper, 2) {
  if (auto* s = com::As<DDrawSurfaceObj>(Arg(c, 0))) s->clipper = Arg(c, 1);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::SetColorKey", DDS1_SetCK, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::SetOverlayPosition", DDS1_SetOvPos, 3) {
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::SetPalette", DDS1_SetPal, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Unlock", DDS1_Unlock, 2) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::UpdateOverlay", DDS1_UpOv, 6) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::UpdateOverlayDisplay", DDS1_UpOvDisp, 2) {
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::UpdateOverlayZOrder", DDS1_UpOvZ, 3) { return kDD_OK; }

// ---- IDirectDrawClipper ----------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirectDrawClipper::QueryInterface", DDC_QI, 3) {
  com::AddRef(Arg(c, 0));
  Wr32(Arg(c, 2), Arg(c, 0));
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::AddRef", DDC_AddRef, 1) {
  return com::AddRef(Arg(c, 0));
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::Release", DDC_Release, 1) {
  return com::Release(Arg(c, 0));
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::GetClipList", DDC_GetClipList, 4) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::GetHWnd", DDC_GetHWnd, 2) {
  if (auto* cl = com::As<DDrawClipperObj>(Arg(c, 0))) {
    if (Arg(c, 1)) Wr32(Arg(c, 1), cl->hwnd);
  }
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::Initialize", DDC_Init, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::IsClipListChanged", DDC_IsChanged, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  return kDD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::SetClipList", DDC_SetClipList, 3) { return kDD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::SetHWnd", DDC_SetHWnd, 3) {
  if (auto* cl = com::As<DDrawClipperObj>(Arg(c, 0))) cl->hwnd = Arg(c, 2);
  return kDD_OK;
}

uint32_t DirectDrawVtable() {
  return com::Vtable("ddraw.dll", "IDirectDraw",
                     {
                         "QueryInterface",         "AddRef",
                         "Release",                "Compact",
                         "CreateClipper",          "CreatePalette",
                         "CreateSurface",          "DuplicateSurface",
                         "EnumDisplayModes",       "EnumSurfaces",
                         "FlipToGDISurface",       "GetCaps",
                         "GetDisplayMode",         "GetFourCCCodes",
                         "GetGDISurface",          "GetMonitorFrequency",
                         "GetScanLine",            "GetVerticalBlankStatus",
                         "Initialize",             "RestoreDisplayMode",
                         "SetCooperativeLevel",    "SetDisplayMode",
                         "WaitForVerticalBlank",
                     });
}

uint32_t DirectDraw2Vtable() {
  return com::Vtable("ddraw.dll", "IDirectDraw2",
                     {
                         "QueryInterface",         "AddRef",
                         "Release",                "Compact",
                         "CreateClipper",          "CreatePalette",
                         "CreateSurface",          "DuplicateSurface",
                         "EnumDisplayModes",       "EnumSurfaces",
                         "FlipToGDISurface",       "GetCaps",
                         "GetDisplayMode",         "GetFourCCCodes",
                         "GetGDISurface",          "GetMonitorFrequency",
                         "GetScanLine",            "GetVerticalBlankStatus",
                         "Initialize",             "RestoreDisplayMode",
                         "SetCooperativeLevel",    "SetDisplayMode",
                         "WaitForVerticalBlank",   "GetAvailableVidMem",
                     });
}

uint32_t DirectDrawSurfaceVtable() {
  return com::Vtable("ddraw.dll", "IDirectDrawSurface",
                     {
                         "QueryInterface",       "AddRef",
                         "Release",              "AddAttachedSurface",
                         "AddOverlayDirtyRect",  "Blt",
                         "BltBatch",             "BltFast",
                         "DeleteAttachedSurface","EnumAttachedSurfaces",
                         "EnumOverlayZOrders",   "Flip",
                         "GetAttachedSurface",   "GetBltStatus",
                         "GetCaps",              "GetClipper",
                         "GetColorKey",          "GetDC",
                         "GetFlipStatus",        "GetOverlayPosition",
                         "GetPalette",           "GetPixelFormat",
                         "GetSurfaceDesc",       "Initialize",
                         "IsLost",               "Lock",
                         "ReleaseDC",            "Restore",
                         "SetClipper",           "SetColorKey",
                         "SetOverlayPosition",   "SetPalette",
                         "Unlock",               "UpdateOverlay",
                         "UpdateOverlayDisplay", "UpdateOverlayZOrder",
                     });
}

uint32_t DirectDrawClipperVtable() {
  return com::Vtable("ddraw.dll", "IDirectDrawClipper",
                     {
                         "QueryInterface", "AddRef", "Release",
                         "GetClipList",    "GetHWnd", "Initialize",
                         "IsClipListChanged", "SetClipList", "SetHWnd",
                     });
}

}  // namespace
}  // namespace recomp::hle
