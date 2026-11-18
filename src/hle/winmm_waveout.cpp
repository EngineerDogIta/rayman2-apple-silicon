// WINMM waveOut + mixer HLE over SDL3 CoreAudio (SDL_AudioStream).
// Handles WAVPWBVR.dll audio streaming (CALLBACK_THREAD via PostThreadMessageHost)
// and Rayman2.exe waveOutGetDevCapsA + volume mixer controls.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/user32_window.h"
#include "x86/lifted.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kMMSYSERR_NOERROR = 0;
constexpr uint32_t kMMSYSERR_BADDEVICEID = 2;
constexpr uint32_t kMMSYSERR_INVALHANDLE = 5;
constexpr uint32_t kMMSYSERR_INVALPARAM = 11;

constexpr uint32_t kCALLBACK_TYPEMASK = 0x00070000u;
constexpr uint32_t kCALLBACK_WINDOW = 0x00010000u;
constexpr uint32_t kCALLBACK_THREAD = 0x00020000u;
constexpr uint32_t kWAVE_FORMAT_QUERY = 0x00000001u;

constexpr uint32_t kMM_WOM_OPEN = 0x3BBu;
constexpr uint32_t kMM_WOM_CLOSE = 0x3BCu;
constexpr uint32_t kMM_WOM_DONE = 0x3BDu;

constexpr uint32_t kWHDR_DONE = 0x00000001u;
constexpr uint32_t kWHDR_PREPARED = 0x00000002u;
constexpr uint32_t kWHDR_INQUEUE = 0x00000010u;

constexpr uint32_t kMIXERCONTROL_CONTROLTYPE_MUTE = 0x20010002u;
constexpr uint32_t kMIXERCONTROL_CONTROLTYPE_VOLUME = 0x50030001u;

struct QueuedHeader {
  uint32_t pwh = 0;
  uint64_t end_byte = 0;
};

struct WaveOutDevice {
  uint32_t handle = 0;
  uint32_t callback = 0;
  uint32_t instance = 0;
  uint32_t flags = 0;
  SDL_AudioStream* stream = nullptr;
  uint64_t total_queued_bytes = 0;
  std::deque<QueuedHeader> queue;
  bool closing = false;
};

std::mutex g_wave_mu;
std::condition_variable g_wave_cv;
std::map<uint32_t, std::shared_ptr<WaveOutDevice>> g_wave_devs;
uint32_t g_next_wave_handle = 0x574F0001u;
std::atomic<uint32_t> g_mixer_volume{0xFFFFu};
std::atomic<uint32_t> g_mixer_mute{0u};
std::thread g_wave_thread;
std::atomic<bool> g_wave_thread_started{false};

void EnsureAudioInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      WarnF("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
    }
  });
}

void ApplyMixerGain(SDL_AudioStream* s) {
  if (!s) return;
  float gain = g_mixer_mute.load() ? 0.0f : (float(g_mixer_volume.load()) / 65535.0f);
  SDL_SetAudioStreamGain(s, gain);
}

void NotifyWave(uint32_t flags, uint32_t cb, uint32_t msg, uint32_t hwo, uint32_t param2) {
  uint32_t type = flags & kCALLBACK_TYPEMASK;
  if ((type == kCALLBACK_THREAD || type == kCALLBACK_WINDOW) && cb) {
    PostThreadMessageHost(cb, msg, hwo, param2);
  }
}

void WaveMonitorLoop() {
  for (;;) {
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> done_events;
    {
      std::unique_lock<std::mutex> l(g_wave_mu);
      if (g_wave_devs.empty()) {
        g_wave_cv.wait_for(l, std::chrono::milliseconds(16));
      } else {
        for (auto& [h, dev] : g_wave_devs) {
          if (!dev->stream || dev->queue.empty()) continue;
          int queued = SDL_GetAudioStreamQueued(dev->stream);
          uint64_t remaining = queued > 0 ? uint64_t(queued) : 0;
          uint64_t played = dev->total_queued_bytes > remaining
                                ? dev->total_queued_bytes - remaining
                                : 0;
          while (!dev->queue.empty() && dev->queue.front().end_byte <= played) {
            QueuedHeader qh = dev->queue.front();
            dev->queue.pop_front();
            uint32_t fl = Rd32(qh.pwh + 16);
            fl = (fl & ~kWHDR_INQUEUE) | kWHDR_DONE;
            Wr32(qh.pwh + 16, fl);
            done_events.emplace_back(dev->flags, dev->callback, dev->handle, qh.pwh);
          }
        }
      }
    }
    for (auto& [fl, cb, hwo, pwh] : done_events) {
      NotifyWave(fl, cb, kMM_WOM_DONE, hwo, pwh);
    }
    SDL_Delay(2);
  }
}

void EnsureWaveThread() {
  if (!g_wave_thread_started.exchange(true)) {
    g_wave_thread = std::thread(WaveMonitorLoop);
    g_wave_thread.detach();
  }
}

void CopyPaddedStr(uint32_t dst, uint32_t max_chars, const char* s) {
  size_t n = std::min<size_t>(std::strlen(s), max_chars - 1);
  if (n) std::memcpy(GPtr(dst), s, n);
  Wr8(dst + uint32_t(n), 0);
}

// ---- waveOut* --------------------------------------------------------------

HLE("winmm.dll", waveOutGetDevCapsA, 3) {
  uint32_t id = Arg(c, 0), pwoc = Arg(c, 1), cb = Arg(c, 2);
  if (id != 0 && id != 0xFFFFFFFFu) return kMMSYSERR_BADDEVICEID;
  if (!pwoc || cb < 52) return kMMSYSERR_INVALPARAM;
  std::memset(GPtr(pwoc), 0, cb);
  Wr16(pwoc + 0, 1);                      // wMid
  Wr16(pwoc + 2, 2);                      // wPid
  Wr32(pwoc + 4, 0x0400);                 // vDriverVersion
  CopyPaddedStr(pwoc + 8, 32, "CoreAudio WaveOut");
  Wr32(pwoc + 40, 0x000FFFFFu);           // dwFormats (all standard PCM)
  Wr16(pwoc + 44, 2);                     // wChannels
  Wr32(pwoc + 48, 0x0000000Cu);           // dwSupport (VOLUME | LRVOLUME)
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", waveOutOpen, 6) {
  uint32_t phwo = Arg(c, 0), dev_id = Arg(c, 1), pwfx = Arg(c, 2);
  uint32_t dwCallback = Arg(c, 3), dwInstance = Arg(c, 4), fdwOpen = Arg(c, 5);
  if (!pwfx) return kMMSYSERR_INVALPARAM;
  uint16_t fmt_tag = Rd16(pwfx + 0);
  uint16_t channels = Rd16(pwfx + 2);
  uint32_t rate = Rd32(pwfx + 4);
  uint16_t bits = Rd16(pwfx + 14);
  if (fmt_tag != 1) {
    WarnF("waveOutOpen unsupported format tag %u\n", fmt_tag);
    return kMMSYSERR_INVALPARAM;
  }
  if (fdwOpen & kWAVE_FORMAT_QUERY) return kMMSYSERR_NOERROR;
  if (!phwo || (dev_id != 0 && dev_id != 0xFFFFFFFFu)) return kMMSYSERR_BADDEVICEID;

  EnsureAudioInit();
  EnsureWaveThread();

  SDL_AudioSpec spec{};
  spec.channels = std::clamp<int>(channels, 1, 2);
  spec.freq = rate ? int(rate) : 22050;
  spec.format = (bits == 8) ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;

  SDL_AudioStream* stream =
      SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream) {
    WarnF("SDL_OpenAudioDeviceStream failed (%dch %dHz %dbit): %s\n", spec.channels, spec.freq,
          bits, SDL_GetError());
  } else {
    ApplyMixerGain(stream);
    SDL_ResumeAudioStreamDevice(stream);
  }

  auto dev = std::make_shared<WaveOutDevice>();
  dev->callback = dwCallback;
  dev->instance = dwInstance;
  dev->flags = fdwOpen;
  dev->stream = stream;
  uint32_t h;
  {
    std::lock_guard<std::mutex> l(g_wave_mu);
    h = g_next_wave_handle++;
    dev->handle = h;
    g_wave_devs[h] = dev;
  }
  Wr32(phwo, h);
  g_wave_cv.notify_all();
  NotifyWave(fdwOpen, dwCallback, kMM_WOM_OPEN, h, 0);
  std::fprintf(stderr, "audio: waveOutOpen handle=%08X (%dHz %dch %dbit, flags=%08X, stream=%s)\n",
               h, spec.freq, spec.channels, bits, fdwOpen, stream ? "ok" : "null");
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", waveOutPrepareHeader, 3) {
  uint32_t pwh = Arg(c, 1);
  if (!pwh) return kMMSYSERR_INVALPARAM;
  uint32_t fl = Rd32(pwh + 16);
  Wr32(pwh + 16, (fl & ~kWHDR_DONE) | kWHDR_PREPARED);
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", waveOutWrite, 3) {
  uint32_t hwo = Arg(c, 0), pwh = Arg(c, 1);
  if (!pwh) return kMMSYSERR_INVALPARAM;
  uint32_t data = Rd32(pwh + 0);
  uint32_t len = Rd32(pwh + 4);
  uint32_t fl = Rd32(pwh + 16);

  std::shared_ptr<WaveOutDevice> dev;
  {
    std::lock_guard<std::mutex> l(g_wave_mu);
    auto it = g_wave_devs.find(hwo);
    if (it == g_wave_devs.end()) return kMMSYSERR_INVALHANDLE;
    dev = it->second;
    Wr32(pwh + 16, (fl & ~kWHDR_DONE) | kWHDR_INQUEUE);
    if (dev->stream && data && len) {
      SDL_PutAudioStreamData(dev->stream, GPtr(data), int(len));
      dev->total_queued_bytes += len;
    }
    dev->queue.push_back(QueuedHeader{pwh, dev->total_queued_bytes});
  }
  g_wave_cv.notify_all();
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", waveOutReset, 1) {
  uint32_t hwo = Arg(c, 0);
  std::vector<QueuedHeader> flushed;
  uint32_t fl = 0, cb = 0;
  {
    std::lock_guard<std::mutex> l(g_wave_mu);
    auto it = g_wave_devs.find(hwo);
    if (it == g_wave_devs.end()) return kMMSYSERR_INVALHANDLE;
    auto& dev = it->second;
    fl = dev->flags;
    cb = dev->callback;
    if (dev->stream) SDL_ClearAudioStream(dev->stream);
    while (!dev->queue.empty()) {
      flushed.push_back(dev->queue.front());
      dev->queue.pop_front();
    }
    dev->total_queued_bytes = 0;
  }
  for (const auto& qh : flushed) {
    uint32_t hfl = Rd32(qh.pwh + 16);
    Wr32(qh.pwh + 16, (hfl & ~kWHDR_INQUEUE) | kWHDR_DONE);
    NotifyWave(fl, cb, kMM_WOM_DONE, hwo, qh.pwh);
  }
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", waveOutClose, 1) {
  uint32_t hwo = Arg(c, 0);
  std::shared_ptr<WaveOutDevice> dev;
  {
    std::lock_guard<std::mutex> l(g_wave_mu);
    auto it = g_wave_devs.find(hwo);
    if (it == g_wave_devs.end()) return kMMSYSERR_INVALHANDLE;
    dev = it->second;
    g_wave_devs.erase(it);
  }
  if (dev->stream) {
    SDL_DestroyAudioStream(dev->stream);
    dev->stream = nullptr;
  }
  NotifyWave(dev->flags, dev->callback, kMM_WOM_CLOSE, hwo, 0);
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", waveOutGetErrorTextA, 3) {
  uint32_t err = Arg(c, 0), dst = Arg(c, 1), cch = Arg(c, 2);
  if (!dst || !cch) return kMMSYSERR_INVALPARAM;
  char msg[64];
  std::snprintf(msg, sizeof(msg), "MMRESULT %u", err);
  CopyPaddedStr(dst, cch, msg);
  return kMMSYSERR_NOERROR;
}

// ---- mixer* ----------------------------------------------------------------

HLE("winmm.dll", mixerGetNumDevs, 0) { return 1; }

HLE("winmm.dll", mixerOpen, 5) {
  uint32_t phmx = Arg(c, 0);
  if (phmx) Wr32(phmx, 0x4D580001u);
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", mixerClose, 1) { return kMMSYSERR_NOERROR; }

HLE("winmm.dll", mixerGetID, 3) {
  uint32_t pu = Arg(c, 1);
  if (pu) Wr32(pu, 0);
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", mixerGetDevCapsA, 3) {
  uint32_t caps = Arg(c, 1), cb = Arg(c, 2);
  if (!caps || cb < 48) return kMMSYSERR_INVALPARAM;
  std::memset(GPtr(caps), 0, cb);
  Wr16(caps + 0, 1);
  Wr16(caps + 2, 1);
  Wr32(caps + 4, 0x0400);
  CopyPaddedStr(caps + 8, 32, "CoreAudio Mixer");
  Wr32(caps + 40, 0);
  Wr32(caps + 44, 1);  // cDestinations
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", mixerGetLineInfoA, 3) {
  uint32_t pmxl = Arg(c, 1);
  if (!pmxl || Rd32(pmxl) < 168) return kMMSYSERR_INVALPARAM;
  uint32_t comp_type = Rd32(pmxl + 24);
  if (comp_type == 0) comp_type = 4;  // MIXERLINE_COMPONENTTYPE_DST_SPEAKERS
  Wr32(pmxl + 4, 0);                  // dwDestination
  Wr32(pmxl + 8, 0);                  // dwSource
  Wr32(pmxl + 12, 0x1001u);           // dwLineID
  Wr32(pmxl + 16, 1);                 // fdwLine = MIXERLINE_LINEF_ACTIVE
  Wr32(pmxl + 20, 0);
  Wr32(pmxl + 24, comp_type);
  Wr32(pmxl + 28, 2);                 // cChannels
  Wr32(pmxl + 32, 1);                 // cConnections
  Wr32(pmxl + 36, 2);                 // cControls (Volume + Mute)
  CopyPaddedStr(pmxl + 40, 16, "Wave");
  CopyPaddedStr(pmxl + 56, 64, "CoreAudio Wave Out");
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", mixerGetLineControlsA, 3) {
  uint32_t pmxlc = Arg(c, 1), fdw = Arg(c, 2);
  if (!pmxlc || Rd32(pmxlc) < 24) return kMMSYSERR_INVALPARAM;
  uint32_t cControls = Rd32(pmxlc + 12);
  uint32_t cbmxctrl = Rd32(pmxlc + 16);
  uint32_t pamxctrl = Rd32(pmxlc + 20);
  if (!pamxctrl || cbmxctrl < 148 || cControls == 0) return kMMSYSERR_INVALPARAM;

  uint32_t query = fdw & 0x0Fu;
  uint32_t req_type = Rd32(pmxlc + 8);
  auto write_ctrl = [&](uint32_t dst, uint32_t cid, uint32_t ctype, const char* name,
                        uint32_t mn, uint32_t mx) {
    std::memset(GPtr(dst), 0, cbmxctrl);
    Wr32(dst + 0, cbmxctrl);
    Wr32(dst + 4, cid);
    Wr32(dst + 8, ctype);
    Wr32(dst + 12, 0);  // fdwControl
    Wr32(dst + 16, 0);  // cMultipleItems
    CopyPaddedStr(dst + 20, 16, name);
    CopyPaddedStr(dst + 36, 64, name);
    Wr32(dst + 100, mn);
    Wr32(dst + 104, mx);
    Wr32(dst + 124, 256);  // cSteps
  };

  if (query == 2) {  // MIXER_GETLINECONTROLSF_ONEBYTYPE
    if (req_type == kMIXERCONTROL_CONTROLTYPE_MUTE)
      write_ctrl(pamxctrl, 2, kMIXERCONTROL_CONTROLTYPE_MUTE, "Mute", 0, 1);
    else
      write_ctrl(pamxctrl, 1, kMIXERCONTROL_CONTROLTYPE_VOLUME, "Volume", 0, 65535);
    return kMMSYSERR_NOERROR;
  }

  write_ctrl(pamxctrl, 1, kMIXERCONTROL_CONTROLTYPE_VOLUME, "Volume", 0, 65535);
  if (cControls >= 2)
    write_ctrl(pamxctrl + cbmxctrl, 2, kMIXERCONTROL_CONTROLTYPE_MUTE, "Mute", 0, 1);
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", mixerGetControlDetailsA, 3) {
  uint32_t pmxcd = Arg(c, 1);
  if (!pmxcd || Rd32(pmxcd) < 24) return kMMSYSERR_INVALPARAM;
  uint32_t cid = Rd32(pmxcd + 4);
  uint32_t cch = std::max<uint32_t>(1, Rd32(pmxcd + 8));
  uint32_t pa = Rd32(pmxcd + 20);
  if (!pa) return kMMSYSERR_INVALPARAM;
  uint32_t val = (cid == 2) ? g_mixer_mute.load() : g_mixer_volume.load();
  for (uint32_t i = 0; i < cch; i++) Wr32(pa + 4 * i, val);
  return kMMSYSERR_NOERROR;
}

HLE("winmm.dll", mixerSetControlDetails, 3) {
  uint32_t pmxcd = Arg(c, 1);
  if (!pmxcd || Rd32(pmxcd) < 24) return kMMSYSERR_INVALPARAM;
  uint32_t cid = Rd32(pmxcd + 4);
  uint32_t pa = Rd32(pmxcd + 20);
  if (!pa) return kMMSYSERR_INVALPARAM;
  uint32_t val = Rd32(pa);
  if (cid == 2)
    g_mixer_mute.store(val ? 1u : 0u);
  else
    g_mixer_volume.store(std::min<uint32_t>(val, 65535u));

  std::lock_guard<std::mutex> l(g_wave_mu);
  for (auto& [h, dev] : g_wave_devs) ApplyMixerGain(dev->stream);
  return kMMSYSERR_NOERROR;
}

}  // namespace
}  // namespace recomp::hle
