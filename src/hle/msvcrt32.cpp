// VC6 MSVCRT.dll HLE for 32-bit Win32 guests (Rayman 2 / GliVd1vf).
// Covers all 70 MSVCRT.dll symbols imported by Rayman2.exe and GliVd1vf.dll.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"
#include "x86/lifted.h"
#include "x86/runtime.h"
#include "x86/x87.h"

namespace recomp::hle {
namespace {

using namespace win32;

uint32_t StackArg(const Ctx& c, int i) { return Rd32(c.esp + 4 + 4 * i); }
uint64_t StackArg64(const Ctx& c, int i) {
  return uint64_t(StackArg(c, i + 1)) << 32 | StackArg(c, i);
}

// ---- Data exports (_acmdln, _adjust_fdiv) -----------------------------------

gaddr g_acmdln_slot = 0;
gaddr g_fmode_va = 0;
gaddr g_commode_va = 0;
gaddr g_tm_va = 0;
gaddr g_strtok_cur = 0;

void InitAcmdln(gaddr slot) {
  g_acmdln_slot = slot;
  PutStr(slot + 8, "Rayman2.exe");
  Wr32(slot, uint32_t(slot + 8));
}

void InitAdjustFdiv(gaddr slot) { Wr32(slot, 0); }

static DataExport g_msvcrt_acmdln{"msvcrt.dll", "_acmdln", 256, InitAcmdln};
static DataExport g_msvcrt_fdiv{"msvcrt.dll", "_adjust_fdiv", 4, InitAdjustFdiv};

// ---- Process globals for startup & CRT -------------------------------------

std::vector<std::string> SplitArgs(const std::string& cmd) {
  std::vector<std::string> out;
  size_t i = 0, n = cmd.size();
  while (i < n) {
    while (i < n && (cmd[i] == ' ' || cmd[i] == '\t')) i++;
    if (i >= n) break;
    std::string tok;
    bool in_q = false;
    while (i < n) {
      char ch = cmd[i];
      if (!in_q && (ch == ' ' || ch == '\t')) break;
      if (ch == '"') {
        in_q = !in_q;
      } else {
        tok.push_back(ch);
      }
      i++;
    }
    out.push_back(std::move(tok));
  }
  if (out.empty()) out.push_back("Rayman2.exe");
  return out;
}

// ---- FILE streams & POSIX fds ----------------------------------------------

struct GuestStream {
  FILE* f = nullptr;
  int fd = -1;
};

std::recursive_mutex g_crt_mu;
std::map<uint32_t, GuestStream> g_files;  // keyed by guest FILE* (32 bytes each)
std::map<int, int> g_fds;                 // guest fd -> host fd
int g_next_fd = 3;

// Allocate a 32-byte MSVC6 FILE struct with _cnt=0 so getc/putc enter _filbuf/_flsbuf.
uint32_t AllocGuestFile(FILE* fp) {
  uint32_t gf = uint32_t(HeapAllocProcess(32, true));
  int fd = fileno(fp);
  Wr32(gf + 16, uint32_t(fd));  // _file
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  g_files[gf] = GuestStream{fp, fd};
  return gf;
}

FILE* HostFile(uint32_t gf) {
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  auto it = g_files.find(gf);
  return it != g_files.end() ? it->second.f : nullptr;
}

void EnsureFileParentExists(const std::string& host_path) {
  std::error_code ec;
  auto parent = std::filesystem::path(host_path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
}

PathAccess AccessFromFopenMode(const char* mode) {
  bool plus = std::strchr(mode, '+') != nullptr;
  if (mode[0] == 'r') return plus ? PathAccess::kUpdate : PathAccess::kRead;
  if (mode[0] == 'a') return PathAccess::kUpdate;
  return PathAccess::kWrite;
}

// ---- Wildcard matching for _findfirst/_findnext ------------------------------

bool MatchWildcard(const char* pat, const char* str) {
  // Case-insensitive DOS/Win32 glob (* and ?). *.* matches everything on Windows.
  if (strcasecmp(pat, "*.*") == 0) return true;
  while (*pat) {
    if (*pat == '*') {
      while (*pat == '*') pat++;
      if (!*pat) return true;
      for (; *str; str++) {
        if (MatchWildcard(pat, str)) return true;
      }
      return false;
    } else if (*pat == '?') {
      if (!*str) return false;
      pat++;
      str++;
    } else {
      if (std::tolower(uint8_t(*pat)) != std::tolower(uint8_t(*str))) return false;
      pat++;
      str++;
    }
  }
  return *str == '\0';
}

struct FindSearch {
  struct Entry {
    std::string name;
    uint32_t attrib;
    uint32_t size;
    uint32_t atime, mtime, ctime;
  };
  std::vector<Entry> items;
  size_t index = 0;
};

std::map<int32_t, FindSearch> g_finds;
int32_t g_next_find = 1;

void WriteFindData32(uint32_t dst, const FindSearch::Entry& e) {
  std::memset(GPtr(dst), 0, 0x118);
  Wr32(dst + 0x00, e.attrib);
  Wr32(dst + 0x04, e.ctime);
  Wr32(dst + 0x08, e.atime);
  Wr32(dst + 0x0C, e.mtime);
  Wr32(dst + 0x10, e.size);
  PutStr(dst + 0x14, e.name.substr(0, 259));
}

// ---- printf / sscanf helpers for 32-bit guest stack args -------------------

std::string FormatGuestPrintf(const char* fmt, const Ctx& c, int arg_idx) {
  std::string out;
  int idx = arg_idx;
  for (const char* p = fmt; *p; p++) {
    if (*p != '%') {
      out.push_back(*p);
      continue;
    }
    const char* spec_start = p++;
    if (*p == '%') {
      out.push_back('%');
      continue;
    }
    std::string sub(1, '%');
    while (*p && std::strchr("-+ #0", *p)) sub.push_back(*p++);
    if (*p == '*') {
      int w = int(StackArg(c, idx++));
      sub += std::to_string(w);
      p++;
    } else {
      while (*p && std::isdigit(uint8_t(*p))) sub.push_back(*p++);
    }
    if (*p == '.') {
      sub.push_back(*p++);
      if (*p == '*') {
        int pr = int(StackArg(c, idx++));
        sub += std::to_string(pr);
        p++;
      } else {
        while (*p && std::isdigit(uint8_t(*p))) sub.push_back(*p++);
      }
    }
    bool is_long64 = false;
    if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
      sub += "ll";
      is_long64 = true;
      p += 3;
    } else if (*p == 'l' && p[1] == 'l') {
      sub += "ll";
      is_long64 = true;
      p += 2;
    } else if (*p == 'h' || *p == 'l' || *p == 'L') {
      if (*p != 'l') sub.push_back(*p);
      p++;
    }
    char conv = *p;
    if (!conv) break;
    sub.push_back(conv);

    char buf[1024];
    switch (conv) {
      case 'd':
      case 'i': {
        if (is_long64) {
          int64_t v = int64_t(StackArg64(c, idx));
          idx += 2;
          std::snprintf(buf, sizeof(buf), sub.c_str(), v);
        } else {
          int32_t v = int32_t(StackArg(c, idx++));
          std::snprintf(buf, sizeof(buf), sub.c_str(), v);
        }
        out += buf;
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        if (is_long64) {
          uint64_t v = StackArg64(c, idx);
          idx += 2;
          std::snprintf(buf, sizeof(buf), sub.c_str(), v);
        } else {
          uint32_t v = StackArg(c, idx++);
          std::snprintf(buf, sizeof(buf), sub.c_str(), v);
        }
        out += buf;
        break;
      }
      case 'p': {
        uint32_t v = StackArg(c, idx++);
        std::snprintf(buf, sizeof(buf), "%08X", v);
        out += buf;
        break;
      }
      case 'c': {
        int ch = int(StackArg(c, idx++)) & 0xFF;
        std::snprintf(buf, sizeof(buf), sub.c_str(), ch);
        out += buf;
        break;
      }
      case 's': {
        uint32_t va = StackArg(c, idx++);
        const char* s = va ? reinterpret_cast<const char*>(GPtr(va)) : "(null)";
        std::snprintf(buf, sizeof(buf), sub.c_str(), s);
        out += buf;
        break;
      }
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G': {
        uint64_t raw = StackArg64(c, idx);
        idx += 2;
        double d = std::bit_cast<double>(raw);
        std::snprintf(buf, sizeof(buf), sub.c_str(), d);
        out += buf;
        break;
      }
      case 'n': {
        uint32_t ptr = StackArg(c, idx++);
        if (ptr) Wr32(ptr, uint32_t(out.size()));
        break;
      }
      default:
        out.append(spec_start, size_t(p - spec_start + 1));
        break;
    }
  }
  return out;
}

int RunGuestSscanf(const char* src_str, const char* fmt, const Ctx& c, int arg_idx) {
  const char* s = src_str;
  const char* p = fmt;
  int idx = arg_idx;
  int assigned = 0;

  while (*p) {
    if (std::isspace(uint8_t(*p))) {
      while (std::isspace(uint8_t(*p))) p++;
      while (std::isspace(uint8_t(*s))) s++;
      continue;
    }
    if (*p != '%') {
      if (*s != *p) break;
      s++;
      p++;
      continue;
    }
    p++;
    if (*p == '%') {
      if (*s != '%') break;
      s++;
      p++;
      continue;
    }
    bool suppress = false;
    if (*p == '*') {
      suppress = true;
      p++;
    }
    int width = 0;
    while (std::isdigit(uint8_t(*p))) {
      width = width * 10 + (*p - '0');
      p++;
    }
    char len_mod = 0;
    if (*p == 'h' || *p == 'l' || *p == 'L') len_mod = *p++;
    char conv = *p++;
    if (!conv) break;

    if (conv == 'n') {
      if (!suppress) {
        uint32_t out_ptr = StackArg(c, idx++);
        if (out_ptr) Wr32(out_ptr, uint32_t(s - src_str));
      }
      continue;
    }

    if (conv != 'c' && conv != '[') {
      while (std::isspace(uint8_t(*s))) s++;
    }
    if (!*s && conv != 'c') break;

    if (conv == 'c') {
      int n = width > 0 ? width : 1;
      for (int k = 0; k < n; k++) {
        if (!*s) return assigned;
        if (!suppress) {
          uint32_t dst = StackArg(c, idx);
          Wr8(dst + k, uint8_t(*s));
        }
        s++;
      }
      if (!suppress) {
        idx++;
        assigned++;
      }
    } else if (conv == 's') {
      std::string val;
      int n = width > 0 ? width : INT32_MAX;
      while (*s && !std::isspace(uint8_t(*s)) && int(val.size()) < n) val.push_back(*s++);
      if (val.empty()) break;
      if (!suppress) {
        uint32_t dst = StackArg(c, idx++);
        PutStr(dst, val);
        assigned++;
      }
    } else if (conv == '[') {
      bool neg = false;
      if (*p == '^') {
        neg = true;
        p++;
      }
      bool allowed[256] = {};
      if (*p == ']') {
        allowed[uint8_t(']')] = true;
        p++;
      }
      uint8_t prev = 0;
      while (*p && *p != ']') {
        if (*p == '-' && prev && p[1] && p[1] != ']') {
          uint8_t hi = uint8_t(p[1]);
          for (uint8_t ch = std::min(prev, hi); ch <= std::max(prev, hi); ch++) allowed[ch] = true;
          p += 2;
          prev = 0;
        } else {
          prev = uint8_t(*p++);
          allowed[prev] = true;
        }
      }
      if (*p == ']') p++;
      std::string val;
      int n = width > 0 ? width : INT32_MAX;
      while (*s && int(val.size()) < n) {
        bool match = allowed[uint8_t(*s)] != neg;
        if (!match) break;
        val.push_back(*s++);
      }
      if (val.empty()) break;
      if (!suppress) {
        uint32_t dst = StackArg(c, idx++);
        PutStr(dst, val);
        assigned++;
      }
    } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' ||
               conv == 'o') {
      std::string tok;
      int n = width > 0 ? width : 64;
      const char* cur = s;
      if (int(tok.size()) < n && (*cur == '+' || *cur == '-')) tok.push_back(*cur++);
      int base = (conv == 'x' || conv == 'X') ? 16
                 : (conv == 'o')              ? 8
                 : (conv == 'i')              ? 0
                                              : 10;
      if (base == 16 && cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X') && int(tok.size()) + 2 <= n) {
        tok.push_back(*cur++);
        tok.push_back(*cur++);
      } else if (base == 0 && cur[0] == '0') {
        tok.push_back(*cur++);
        if ((cur[0] == 'x' || cur[0] == 'X') && int(tok.size()) < n) tok.push_back(*cur++);
      }
      while (*cur && int(tok.size()) < n) {
        unsigned char uch = uint8_t(*cur);
        bool ok = false;
        if (base == 10 || base == 0)
          ok = std::isdigit(uch) != 0;
        if (base == 16 || (base == 0 && tok.size() >= 2 && (tok[1] == 'x' || tok[1] == 'X')))
          ok = std::isxdigit(uch) != 0;
        if (base == 8) ok = (uch >= '0' && uch <= '7');
        if (!ok) break;
        tok.push_back(*cur++);
      }
      if (tok.empty() || tok == "+" || tok == "-" || tok == "0x" || tok == "0X") break;
      char* endp = nullptr;
      int64_t sv = (conv == 'd' || conv == 'i') ? std::strtoll(tok.c_str(), &endp, base)
                                                : int64_t(std::strtoull(tok.c_str(), &endp, base));
      if (endp == tok.c_str()) break;
      s += (endp - tok.c_str());
      if (!suppress) {
        uint32_t dst = StackArg(c, idx++);
        if (len_mod == 'h')
          Wr16(dst, uint16_t(sv));
        else
          Wr32(dst, uint32_t(sv));
        assigned++;
      }
    } else if (conv == 'f' || conv == 'g' || conv == 'e' || conv == 'F' || conv == 'G' ||
               conv == 'E') {
      char* endp = nullptr;
      std::string slice = width > 0 ? std::string(s, strnlen(s, size_t(width))) : std::string(s);
      double dv = std::strtod(slice.c_str(), &endp);
      if (endp == slice.c_str()) break;
      s += (endp - slice.c_str());
      if (!suppress) {
        uint32_t dst = StackArg(c, idx++);
        if (len_mod == 'l')
          Wr64(dst, std::bit_cast<uint64_t>(dv));
        else
          Wr32(dst, std::bit_cast<uint32_t>(float(dv)));
        assigned++;
      }
    } else {
      break;
    }
  }
  return assigned;
}

uint64_t PopRetF64(Ctx& c) {
  uint64_t v = x87::StoreF64(c.fpu);
  x87::Pop(c.fpu);
  return v;
}

// ---- Startup, globals, exceptions ------------------------------------------

HLE("msvcrt.dll", __set_app_type, kCdecl) {
  if (g_acmdln_slot && g_win32 && g_win32->cmdline_a) {
    Wr32(g_acmdln_slot, uint32_t(g_win32->cmdline_a));
  }
  return 0;
}

HLE("msvcrt.dll", __p__fmode, kCdecl) {
  if (!g_fmode_va) {
    g_fmode_va = HeapAllocProcess(4, true);
    Wr32(g_fmode_va, 0x4000);  // _O_TEXT
  }
  return g_fmode_va;
}

HLE("msvcrt.dll", __p__commode, kCdecl) {
  if (!g_commode_va) g_commode_va = HeapAllocProcess(4, true);
  return g_commode_va;
}

HLE("msvcrt.dll", __setusermatherr, kCdecl) { return 0; }

HLE("msvcrt.dll", __getmainargs, kCdecl) {
  if (g_acmdln_slot && g_win32 && g_win32->cmdline_a) {
    Wr32(g_acmdln_slot, uint32_t(g_win32->cmdline_a));
  }
  uint32_t p_argc = StackArg(c, 0);
  uint32_t p_argv = StackArg(c, 1);
  uint32_t p_envp = StackArg(c, 2);
  std::string cmd = (g_win32 && g_win32->cmdline_a) ? GStr(g_win32->cmdline_a) : "Rayman2.exe";
  auto args = SplitArgs(cmd);
  uint32_t argc = uint32_t(args.size());
  uint32_t argv = uint32_t(HeapAllocProcess(4 * (argc + 1), true));
  for (uint32_t i = 0; i < argc; i++) {
    uint32_t s = uint32_t(HeapAllocProcess(args[i].size() + 1, false));
    PutStr(s, args[i]);
    Wr32(argv + 4 * i, s);
  }
  uint32_t envp = uint32_t(HeapAllocProcess(4, true));
  if (p_argc) Wr32(p_argc, argc);
  if (p_argv) Wr32(p_argv, argv);
  if (p_envp) Wr32(p_envp, envp);
  return 0;
}

HLE("msvcrt.dll", _initterm, kCdecl) {
  uint32_t pfbegin = StackArg(c, 0);
  uint32_t pfend = StackArg(c, 1);
  for (uint32_t p = pfbegin; p < pfend; p += 4) {
    uint32_t fn = Rd32(p);
    if (fn) CallGuest(c, fn, {});
  }
  return 0;
}

HLE("msvcrt.dll", _controlfp, kCdecl) {
  uint32_t newval = StackArg(c, 0);
  uint32_t mask = StackArg(c, 1);
  uint16_t old_cw = x87::ControlWord(c.fpu);
  if (mask) {
    uint16_t updated = uint16_t((old_cw & ~mask) | (newval & mask));
    x87::SetControlWord(c.fpu, updated);
  }
  return old_cw;
}

HLE("msvcrt.dll", _XcptFilter, kCdecl) { return 0; }

HLE("msvcrt.dll", _except_handler3, kCdecl) {
  uint32_t rec = StackArg(c, 0);
  uint32_t reg = StackArg(c, 1);
  uint32_t ctx = StackArg(c, 2);
  uint32_t flags = Rd32(rec + 4);
  uint32_t scopetable = Rd32(reg + 8);
  int32_t trylevel = int32_t(Rd32(reg + 12));
  uint32_t orig_ebp = reg + 16;

  // Unwinding path: run any __finally funclets (filter == 0) cleanly.
  if (flags & 0x6) {
    uint32_t saved_ebp = c.ebp;
    c.ebp = orig_ebp;
    for (int32_t lvl = trylevel; lvl != -1; lvl = int32_t(Rd32(scopetable + lvl * 12))) {
      uint32_t filter = Rd32(scopetable + lvl * 12 + 4);
      uint32_t handler = Rd32(scopetable + lvl * 12 + 8);
      if (filter == 0 && handler != 0) {
        Wr32(reg + 12, Rd32(scopetable + lvl * 12));
        CallGuest(c, handler, {});
      }
    }
    c.ebp = saved_ebp;
    return 1;  // ExceptionContinueSearch
  }

  // Exception search pass: evaluate filters.
  uint32_t ep = uint32_t(c.esp - 8);
  Wr32(ep, rec);
  Wr32(ep + 4, ctx);
  Wr32(reg - 4, ep);
  uint32_t saved_ebp = c.ebp;
  c.ebp = orig_ebp;
  for (int32_t lvl = trylevel; lvl != -1; lvl = int32_t(Rd32(scopetable + lvl * 12))) {
    uint32_t filter = Rd32(scopetable + lvl * 12 + 4);
    uint32_t handler = Rd32(scopetable + lvl * 12 + 8);
    if (!filter) continue;
    int32_t res = int32_t(CallGuest(c, filter, {}));
    if (res < 0) {
      c.ebp = saved_ebp;
      return 0;  // ExceptionContinueExecution
    }
    if (res > 0) {
      FatalF("_except_handler3 matched __except handler %08X for exception %08X at %08X",
             handler, Rd32(rec), Rd32(rec + 0x0C));
    }
  }
  c.ebp = saved_ebp;
  return 1;  // ExceptionContinueSearch
}

[[noreturn]] void DoExit(uint32_t code) {
  std::fflush(stdout);
  std::fprintf(stderr, "msvcrt!exit(%u)\n", code);
  std::fflush(stderr);
  std::_Exit(int(code));
}

HLE("msvcrt.dll", exit, kCdecl) { DoExit(StackArg(c, 0)); }
HLE("msvcrt.dll", _exit, kCdecl) { DoExit(StackArg(c, 0)); }

// ---- Heap & Memory ---------------------------------------------------------

HLE("msvcrt.dll", malloc, kCdecl) {
  size_t sz = std::max<size_t>(1, StackArg(c, 0));
  return HeapAllocProcess(sz, false);
}

HLE("msvcrt.dll", calloc, kCdecl) {
  size_t cnt = StackArg(c, 0);
  size_t sz = StackArg(c, 1);
  size_t total = std::max<size_t>(1, cnt * sz);
  return HeapAllocProcess(total, true);
}

HLE("msvcrt.dll", free, kCdecl) {
  uint32_t ptr = StackArg(c, 0);
  if (ptr) HeapFreeProcess(ptr);
  return 0;
}

HLE("msvcrt.dll", memmove, kCdecl) {
  uint32_t dst = StackArg(c, 0);
  uint32_t src = StackArg(c, 1);
  uint32_t n = StackArg(c, 2);
  if (n) std::memmove(GPtr(dst), GPtr(src), n);
  return dst;
}

// ---- Math & x87 CRT intrinsics ---------------------------------------------

HLE("msvcrt.dll", _ftol, kCdecl) {
  int64_t r = x87::StoreI64Trunc(c.fpu);
  x87::Pop(c.fpu);
  c.edx = uint32_t(uint64_t(r) >> 32);
  return uint32_t(uint64_t(r));
}

HLE("msvcrt.dll", _CIpow, kCdecl) {
  x87::Xch(c.fpu, 1);
  double x = std::bit_cast<double>(PopRetF64(c));
  double y = std::bit_cast<double>(PopRetF64(c));
  double r = std::pow(x, y);
  x87::LoadF64(c.fpu, std::bit_cast<uint64_t>(r));
  return c.eax;
}

HLE("msvcrt.dll", _CIacos, kCdecl) {
  double x = std::bit_cast<double>(PopRetF64(c));
  double r = std::acos(x);
  x87::LoadF64(c.fpu, std::bit_cast<uint64_t>(r));
  return c.eax;
}

HLE("msvcrt.dll", floor, kCdecl) {
  double x = std::bit_cast<double>(StackArg64(c, 0));
  double r = std::floor(x);
  x87::LoadF64(c.fpu, std::bit_cast<uint64_t>(r));
  return c.eax;
}

HLE("msvcrt.dll", modf, kCdecl) {
  double x = std::bit_cast<double>(StackArg64(c, 0));
  uint32_t iptr = StackArg(c, 2);
  double ip = 0.0;
  double fp = std::modf(x, &ip);
  if (iptr) Wr64(iptr, std::bit_cast<uint64_t>(ip));
  x87::LoadF64(c.fpu, std::bit_cast<uint64_t>(fp));
  return c.eax;
}

// ---- Strings, chars, conversions, rand, time, qsort ------------------------

HLE("msvcrt.dll", atoi, kCdecl) { return uint32_t(std::atoi(GStr(StackArg(c, 0)).c_str())); }
HLE("msvcrt.dll", atol, kCdecl) { return uint32_t(std::atol(GStr(StackArg(c, 0)).c_str())); }
HLE("msvcrt.dll", tolower, kCdecl) { return uint32_t(std::tolower(uint8_t(StackArg(c, 0)))); }
HLE("msvcrt.dll", toupper, kCdecl) { return uint32_t(std::toupper(uint8_t(StackArg(c, 0)))); }

HLE("msvcrt.dll", _itoa, kCdecl) {
  int32_t val = int32_t(StackArg(c, 0));
  uint32_t buf = StackArg(c, 1);
  int radix = int(StackArg(c, 2));
  if (radix < 2 || radix > 36) return buf;
  std::string out;
  uint32_t u = (radix == 10 && val < 0) ? uint32_t(-int64_t(val)) : uint32_t(val);
  do {
    uint32_t d = u % uint32_t(radix);
    out.push_back(d < 10 ? char('0' + d) : char('a' + d - 10));
    u /= uint32_t(radix);
  } while (u);
  if (radix == 10 && val < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  PutStr(buf, out);
  return buf;
}

HLE("msvcrt.dll", strchr, kCdecl) {
  uint32_t s = StackArg(c, 0);
  int ch = int(uint8_t(StackArg(c, 1)));
  const char* p = reinterpret_cast<const char*>(GPtr(s));
  const char* hit = std::strchr(p, ch);
  return hit ? s + uint32_t(hit - p) : 0;
}

HLE("msvcrt.dll", strrchr, kCdecl) {
  uint32_t s = StackArg(c, 0);
  int ch = int(uint8_t(StackArg(c, 1)));
  const char* p = reinterpret_cast<const char*>(GPtr(s));
  const char* hit = std::strrchr(p, ch);
  return hit ? s + uint32_t(hit - p) : 0;
}

HLE("msvcrt.dll", strstr, kCdecl) {
  uint32_t s1 = StackArg(c, 0);
  uint32_t s2 = StackArg(c, 1);
  const char* p1 = reinterpret_cast<const char*>(GPtr(s1));
  const char* p2 = reinterpret_cast<const char*>(GPtr(s2));
  const char* hit = std::strstr(p1, p2);
  return hit ? s1 + uint32_t(hit - p1) : 0;
}

HLE("msvcrt.dll", strncmp, kCdecl) {
  const char* s1 = reinterpret_cast<const char*>(GPtr(StackArg(c, 0)));
  const char* s2 = reinterpret_cast<const char*>(GPtr(StackArg(c, 1)));
  int r = std::strncmp(s1, s2, StackArg(c, 2));
  return uint32_t((r > 0) - (r < 0));
}

HLE("msvcrt.dll", _stricmp, kCdecl) {
  const unsigned char* s1 = GPtr(StackArg(c, 0));
  const unsigned char* s2 = GPtr(StackArg(c, 1));
  for (;;) {
    int a = std::tolower(*s1++), b = std::tolower(*s2++);
    if (a != b) return uint32_t(a - b);
    if (!a) return 0;
  }
}

HLE("msvcrt.dll", _strnicmp, kCdecl) {
  const unsigned char* s1 = GPtr(StackArg(c, 0));
  const unsigned char* s2 = GPtr(StackArg(c, 1));
  uint32_t n = StackArg(c, 2);
  for (uint32_t i = 0; i < n; i++) {
    int a = std::tolower(*s1++), b = std::tolower(*s2++);
    if (a != b) return uint32_t(a - b);
    if (!a) return 0;
  }
  return 0;
}

HLE("msvcrt.dll", strncpy, kCdecl) {
  uint32_t dst = StackArg(c, 0);
  uint32_t src = StackArg(c, 1);
  uint32_t n = StackArg(c, 2);
  std::strncpy(reinterpret_cast<char*>(GPtr(dst)),
               reinterpret_cast<const char*>(GPtr(src)), n);
  return dst;
}

HLE("msvcrt.dll", strncat, kCdecl) {
  uint32_t dst = StackArg(c, 0);
  uint32_t src = StackArg(c, 1);
  uint32_t n = StackArg(c, 2);
  std::strncat(reinterpret_cast<char*>(GPtr(dst)),
               reinterpret_cast<const char*>(GPtr(src)), n);
  return dst;
}

HLE("msvcrt.dll", _strupr, kCdecl) {
  uint32_t s = StackArg(c, 0);
  for (uint8_t* p = GPtr(s); *p; p++) *p = uint8_t(std::toupper(*p));
  return s;
}

HLE("msvcrt.dll", strtok, kCdecl) {
  uint32_t str = StackArg(c, 0);
  const char* delim = reinterpret_cast<const char*>(GPtr(StackArg(c, 1)));
  gaddr cur = str ? str : g_strtok_cur;
  if (!cur) return 0;
  char* p = reinterpret_cast<char*>(GPtr(cur));
  while (*p && std::strchr(delim, *p)) p++;
  if (!*p) {
    g_strtok_cur = 0;
    return 0;
  }
  uint32_t tok = uint32_t(p - reinterpret_cast<char*>(g_guest_base));
  while (*p && !std::strchr(delim, *p)) p++;
  if (*p) {
    *p++ = '\0';
    g_strtok_cur = uint32_t(p - reinterpret_cast<char*>(g_guest_base));
  } else {
    g_strtok_cur = 0;
  }
  return tok;
}

void QuickSortGuest(Ctx& c, uint32_t base, int32_t lo, int32_t hi, uint32_t width,
                    uint32_t cmp, std::vector<uint8_t>& tmp) {
  if (lo >= hi) return;
  int32_t i = lo, j = hi;
  std::vector<uint8_t> pivot(width);
  std::memcpy(pivot.data(), GPtr(base + uint32_t((lo + hi) / 2) * width), width);
  uint32_t piv_guest = uint32_t(HeapAllocProcess(width, false));
  std::memcpy(GPtr(piv_guest), pivot.data(), width);
  while (i <= j) {
    while (int32_t(CallGuest(c, cmp, {base + uint32_t(i) * width, piv_guest})) < 0) i++;
    while (int32_t(CallGuest(c, cmp, {base + uint32_t(j) * width, piv_guest})) > 0) j--;
    if (i <= j) {
      if (i != j) {
        uint8_t* a = GPtr(base + uint32_t(i) * width);
        uint8_t* b = GPtr(base + uint32_t(j) * width);
        std::memcpy(tmp.data(), a, width);
        std::memcpy(a, b, width);
        std::memcpy(b, tmp.data(), width);
      }
      i++;
      j--;
    }
  }
  HeapFreeProcess(piv_guest);
  if (lo < j) QuickSortGuest(c, base, lo, j, width, cmp, tmp);
  if (i < hi) QuickSortGuest(c, base, i, hi, width, cmp, tmp);
}

HLE("msvcrt.dll", qsort, kCdecl) {
  uint32_t base = StackArg(c, 0);
  uint32_t num = StackArg(c, 1);
  uint32_t width = StackArg(c, 2);
  uint32_t cmp = StackArg(c, 3);
  if (num > 1 && width > 0) {
    std::vector<uint8_t> tmp(width);
    QuickSortGuest(c, base, 0, int32_t(num - 1), width, cmp, tmp);
  }
  return 0;
}

static uint32_t g_msvcrt_seed = 1;
HLE("msvcrt.dll", srand, kCdecl) {
  g_msvcrt_seed = StackArg(c, 0);
  return 0;
}
HLE("msvcrt.dll", rand, kCdecl) {
  g_msvcrt_seed = g_msvcrt_seed * 214013u + 2531011u;
  return (g_msvcrt_seed >> 16) & 0x7FFFu;
}

HLE("msvcrt.dll", time, kCdecl) {
  uint32_t ptr = StackArg(c, 0);
  uint32_t now = uint32_t(std::time(nullptr));
  if (ptr) Wr32(ptr, now);
  return now;
}

HLE("msvcrt.dll", localtime, kCdecl) {
  uint32_t tptr = StackArg(c, 0);
  time_t tv = tptr ? time_t(int32_t(Rd32(tptr))) : std::time(nullptr);
  std::tm tm_val{};
  localtime_r(&tv, &tm_val);
  if (!g_tm_va) g_tm_va = HeapAllocProcess(36, true);
  Wr32(g_tm_va + 0, uint32_t(tm_val.tm_sec));
  Wr32(g_tm_va + 4, uint32_t(tm_val.tm_min));
  Wr32(g_tm_va + 8, uint32_t(tm_val.tm_hour));
  Wr32(g_tm_va + 12, uint32_t(tm_val.tm_mday));
  Wr32(g_tm_va + 16, uint32_t(tm_val.tm_mon));
  Wr32(g_tm_va + 20, uint32_t(tm_val.tm_year));
  Wr32(g_tm_va + 24, uint32_t(tm_val.tm_wday));
  Wr32(g_tm_va + 28, uint32_t(tm_val.tm_yday));
  Wr32(g_tm_va + 32, uint32_t(tm_val.tm_isdst));
  return g_tm_va;
}

HLE("msvcrt.dll", difftime, kCdecl) {
  int32_t t1 = int32_t(StackArg(c, 0));
  int32_t t0 = int32_t(StackArg(c, 1));
  double r = std::difftime(time_t(t1), time_t(t0));
  x87::LoadF64(c.fpu, std::bit_cast<uint64_t>(r));
  return c.eax;
}

// ---- Filesystem, _findfirst, _stat, stdio ----------------------------------

HLE("msvcrt.dll", _access, kCdecl) {
  std::string gp = GStr(StackArg(c, 0));
  int mode = int(StackArg(c, 1));
  std::string hp = HostPath(gp, PathAccess::kRead);
  int am = F_OK;
  if (mode & 2) am |= W_OK;
  if (mode & 4) am |= R_OK;
  return uint32_t(access(hp.c_str(), am));
}

HLE("msvcrt.dll", _chdir, kCdecl) {
  return SetGuestCurrentDir(GStr(StackArg(c, 0))) ? 0u : uint32_t(-1);
}

HLE("msvcrt.dll", _getcwd, kCdecl) {
  uint32_t buf = StackArg(c, 0);
  int maxlen = int(StackArg(c, 1));
  std::string cwd = GuestCurrentDir();
  if (!buf) buf = uint32_t(HeapAllocProcess(std::max<size_t>(cwd.size() + 1, size_t(maxlen)), false));
  else if (maxlen <= int(cwd.size())) return 0;
  PutStr(buf, cwd);
  return buf;
}

HLE("msvcrt.dll", _chmod, kCdecl) {
  std::string hp = HostPath(GStr(StackArg(c, 0)), PathAccess::kUpdate);
  return uint32_t(chmod(hp.c_str(), 0644));
}

HLE("msvcrt.dll", _mkdir, kCdecl) {
  std::string hp = HostPath(GStr(StackArg(c, 0)), PathAccess::kWrite);
  std::error_code ec;
  std::filesystem::create_directories(hp, ec);
  return ec ? uint32_t(-1) : 0u;
}

HLE("msvcrt.dll", _rmdir, kCdecl) {
  std::string hp = HostPath(GStr(StackArg(c, 0)), PathAccess::kWrite);
  return uint32_t(rmdir(hp.c_str()));
}

HLE("msvcrt.dll", _unlink, kCdecl) {
  std::string hp = HostPath(GStr(StackArg(c, 0)), PathAccess::kWrite);
  return uint32_t(unlink(hp.c_str()));
}

HLE("msvcrt.dll", remove, kCdecl) {
  std::string hp = HostPath(GStr(StackArg(c, 0)), PathAccess::kWrite);
  return uint32_t(std::remove(hp.c_str()));
}

HLE("msvcrt.dll", _stat, kCdecl) {
  std::string hp = HostPath(GStr(StackArg(c, 0)), PathAccess::kRead);
  uint32_t dst = StackArg(c, 1);
  struct stat st{};
  if (stat(hp.c_str(), &st) != 0) return uint32_t(-1);
  std::memset(GPtr(dst), 0, 36);
  Wr32(dst + 0x00, 2);  // drive C
  Wr16(dst + 0x04, uint16_t(st.st_ino));
  Wr16(dst + 0x06, S_ISDIR(st.st_mode) ? (0x4000 | 0777) : (0x8000 | 0666));
  Wr16(dst + 0x08, uint16_t(st.st_nlink));
  Wr32(dst + 0x10, 2);
  Wr32(dst + 0x14, uint32_t(st.st_size));
  Wr32(dst + 0x18, uint32_t(st.st_atime));
  Wr32(dst + 0x1C, uint32_t(st.st_mtime));
  Wr32(dst + 0x20, uint32_t(st.st_ctime));
  return 0;
}

HLE("msvcrt.dll", _open, kCdecl) {
  std::string gp = GStr(StackArg(c, 0));
  uint32_t oflag = StackArg(c, 1);
  PathAccess acc = PathAccess::kRead;
  if ((oflag & 3) == 1 || (oflag & 0x0100)) acc = PathAccess::kWrite;
  else if ((oflag & 3) == 2 || (oflag & 0x0008)) acc = PathAccess::kUpdate;
  std::string hp = HostPath(gp, acc);
  if (acc != PathAccess::kRead) EnsureFileParentExists(hp);
  int host_flags = O_RDONLY;
  if ((oflag & 3) == 1) host_flags = O_WRONLY;
  else if ((oflag & 3) == 2) host_flags = O_RDWR;
  if (oflag & 0x0100) host_flags |= O_CREAT;
  if (oflag & 0x0200) host_flags |= O_TRUNC;
  if (oflag & 0x0008) host_flags |= O_APPEND;
  int hfd = open(hp.c_str(), host_flags, 0644);
  if (hfd < 0) return uint32_t(-1);
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  int gfd = g_next_fd++;
  g_fds[gfd] = hfd;
  return uint32_t(gfd);
}

HLE("msvcrt.dll", _close, kCdecl) {
  int gfd = int(StackArg(c, 0));
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  auto it = g_fds.find(gfd);
  if (it == g_fds.end()) return uint32_t(-1);
  int r = close(it->second);
  g_fds.erase(it);
  return uint32_t(r);
}

HLE("msvcrt.dll", _filelength, kCdecl) {
  int gfd = int(StackArg(c, 0));
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  auto it = g_fds.find(gfd);
  if (it == g_fds.end()) return uint32_t(-1);
  struct stat st{};
  if (fstat(it->second, &st) != 0) return uint32_t(-1);
  return uint32_t(st.st_size);
}

HLE("msvcrt.dll", _findfirst, kCdecl) {
  std::string gspec = GStr(StackArg(c, 0));
  uint32_t dst = StackArg(c, 1);
  std::string dir = ".", pat = gspec;
  size_t s = gspec.find_last_of("\\/");
  if (s != std::string::npos) {
    dir = gspec.substr(0, s);
    if (dir.size() == 2 && dir[1] == ':') dir += '\\';
    pat = gspec.substr(s + 1);
  }
  FindSearch fs;
  std::map<std::string, FindSearch::Entry> merged;
  for (const auto& hdir : HostDirs(dir)) {
    DIR* d = opendir(hdir.c_str());
    if (!d) continue;
    while (struct dirent* de = readdir(d)) {
      std::string name = de->d_name;
      if (!MatchWildcard(pat.c_str(), name.c_str())) continue;
      std::string key = win32::Lower(name);
      if (merged.count(key)) continue;
      struct stat st{};
      std::string full = hdir + "/" + name;
      if (stat(full.c_str(), &st) != 0) continue;
      FindSearch::Entry e;
      e.name = name;
      e.attrib = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
      e.size = S_ISDIR(st.st_mode) ? 0u : uint32_t(st.st_size);
      e.atime = uint32_t(st.st_atime);
      e.mtime = uint32_t(st.st_mtime);
      e.ctime = uint32_t(st.st_ctime);
      merged[key] = std::move(e);
    }
    closedir(d);
  }
  if (merged.empty()) return uint32_t(-1);
  for (auto& [k, e] : merged) fs.items.push_back(std::move(e));
  WriteFindData32(dst, fs.items[0]);
  fs.index = 1;
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  int32_t h = g_next_find++;
  g_finds[h] = std::move(fs);
  return uint32_t(h);
}

HLE("msvcrt.dll", _findnext, kCdecl) {
  int32_t h = int32_t(StackArg(c, 0));
  uint32_t dst = StackArg(c, 1);
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  auto it = g_finds.find(h);
  if (it == g_finds.end() || it->second.index >= it->second.items.size()) return uint32_t(-1);
  WriteFindData32(dst, it->second.items[it->second.index++]);
  return 0;
}

HLE("msvcrt.dll", _findclose, kCdecl) {
  int32_t h = int32_t(StackArg(c, 0));
  std::lock_guard<std::recursive_mutex> l(g_crt_mu);
  return g_finds.erase(h) ? 0u : uint32_t(-1);
}

HLE("msvcrt.dll", fopen, kCdecl) {
  std::string gp = GStr(StackArg(c, 0));
  std::string mode = GStr(StackArg(c, 1));
  PathAccess acc = AccessFromFopenMode(mode.c_str());
  std::string hp = HostPath(gp, acc);
  if (acc != PathAccess::kRead) EnsureFileParentExists(hp);
  FILE* fp = std::fopen(hp.c_str(), mode.c_str());
  if (!fp) return 0;
  return AllocGuestFile(fp);
}

HLE("msvcrt.dll", fclose, kCdecl) {
  uint32_t gf = StackArg(c, 0);
  FILE* fp = HostFile(gf);
  if (!fp) return uint32_t(EOF);
  int r = std::fclose(fp);
  {
    std::lock_guard<std::recursive_mutex> l(g_crt_mu);
    g_files.erase(gf);
  }
  HeapFreeProcess(gf);
  return uint32_t(r);
}

HLE("msvcrt.dll", fread, kCdecl) {
  uint32_t buf = StackArg(c, 0);
  size_t sz = StackArg(c, 1);
  size_t cnt = StackArg(c, 2);
  FILE* fp = HostFile(StackArg(c, 3));
  if (!fp || !sz || !cnt) return 0;
  return uint32_t(std::fread(GPtr(buf), sz, cnt, fp));
}

HLE("msvcrt.dll", fwrite, kCdecl) {
  uint32_t buf = StackArg(c, 0);
  size_t sz = StackArg(c, 1);
  size_t cnt = StackArg(c, 2);
  FILE* fp = HostFile(StackArg(c, 3));
  if (!fp || !sz || !cnt) return 0;
  return uint32_t(std::fwrite(GPtr(buf), sz, cnt, fp));
}

HLE("msvcrt.dll", fseek, kCdecl) {
  FILE* fp = HostFile(StackArg(c, 0));
  if (!fp) return uint32_t(-1);
  int32_t off = int32_t(StackArg(c, 1));
  int origin = int(StackArg(c, 2));
  int whence = origin == 0 ? SEEK_SET : (origin == 1 ? SEEK_CUR : SEEK_END);
  return uint32_t(std::fseek(fp, off, whence));
}

HLE("msvcrt.dll", ftell, kCdecl) {
  FILE* fp = HostFile(StackArg(c, 0));
  if (!fp) return uint32_t(-1);
  return uint32_t(std::ftell(fp));
}

HLE("msvcrt.dll", rewind, kCdecl) {
  if (FILE* fp = HostFile(StackArg(c, 0))) std::rewind(fp);
  return 0;
}

HLE("msvcrt.dll", _filbuf, kCdecl) {
  uint32_t gf = StackArg(c, 0);
  FILE* fp = HostFile(gf);
  if (!fp) return uint32_t(EOF);
  Wr32(gf + 4, 0);
  return uint32_t(std::fgetc(fp));
}

HLE("msvcrt.dll", _flsbuf, kCdecl) {
  int ch = int(StackArg(c, 0));
  uint32_t gf = StackArg(c, 1);
  FILE* fp = HostFile(gf);
  if (!fp) return uint32_t(EOF);
  Wr32(gf + 4, 0);
  return uint32_t(std::fputc(ch, fp));
}

HLE("msvcrt.dll", fprintf, kCdecl) {
  FILE* fp = HostFile(StackArg(c, 0));
  std::string fmt = GStr(StackArg(c, 1));
  std::string text = FormatGuestPrintf(fmt.c_str(), c, 2);
  if (!fp) return uint32_t(text.size());
  size_t w = std::fwrite(text.data(), 1, text.size(), fp);
  return uint32_t(w);
}

HLE("msvcrt.dll", sprintf, kCdecl) {
  uint32_t dst = StackArg(c, 0);
  std::string fmt = GStr(StackArg(c, 1));
  std::string text = FormatGuestPrintf(fmt.c_str(), c, 2);
  PutStr(dst, text);
  return uint32_t(text.size());
}

HLE("msvcrt.dll", sscanf, kCdecl) {
  std::string src = GStr(StackArg(c, 0));
  std::string fmt = GStr(StackArg(c, 1));
  return uint32_t(RunGuestSscanf(src.c_str(), fmt.c_str(), c, 2));
}

}  // namespace
}  // namespace recomp::hle
