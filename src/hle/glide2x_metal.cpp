// 3dfx glide2x.dll HLE backend over SwARc's Metal/SDL3_GPU Fixed-Function Device.
// Implements all 44 glide2x.dll imports required by GliVd1vf.dll:
// - 4 MB TMU texture cache decoding RGB565, ARGB1555, ARGB4444, AI88, P8, AP88, A8, I8
// - Chromakey-to-alpha transparency + NCCTABLE/PALETTE texture palette support
// - Perspective-correct D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 batched draws
// - W-buffer monotonic log-depth + fog + alpha test/blend + 16-bit LFB lock/unlock
// - 60.0 FPS FramePacer + PresentWithDevAids screenshot/frame-dump compatibility

#include "hle/glide2x_types.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/gfx/present.h"
#include "runtime/log.h"
#include "runtime/pacing.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "x86/lifted.h"
#include "x86/runtime.h"
#include "x86/watchdog.h"

namespace recomp::ray2 {
DisplayConfig& GetDisplayConfig() {
  static DisplayConfig cfg;
  return cfg;
}
}  // namespace recomp::ray2

namespace recomp::hle {
namespace {

using namespace recomp::ray2;

// ---- Glide 2.x constants & structs -----------------------------------------

constexpr uint32_t kTmuMemSize = 4u * 1024u * 1024u;  // 4 MB per TMU
constexpr uint32_t kSnapThreshold = 131072;
constexpr float kSnapOffset = 524288.0f;

// GrTexFormat_t
enum : uint32_t {
  GR_TEXFMT_8BIT = 0,
  GR_TEXFMT_RGB_332 = 0,
  GR_TEXFMT_YIQ_422 = 1,
  GR_TEXFMT_ALPHA_8 = 2,
  GR_TEXFMT_INTENSITY_8 = 3,
  GR_TEXFMT_ALPHA_INTENSITY_44 = 4,
  GR_TEXFMT_P_8 = 5,
  GR_TEXFMT_RSVD0 = 6,
  GR_TEXFMT_RSVD1 = 7,
  GR_TEXFMT_16BIT = 8,
  GR_TEXFMT_ARGB_8332 = 8,
  GR_TEXFMT_AYIQ_8422 = 9,
  GR_TEXFMT_RGB_565 = 10,
  GR_TEXFMT_ARGB_1555 = 11,
  GR_TEXFMT_ARGB_4444 = 12,
  GR_TEXFMT_ALPHA_INTENSITY_88 = 13,
  GR_TEXFMT_AP_88 = 14,
};

// GrTextureClampMode_t
enum : uint32_t {
  GR_TEXTURECLAMP_WRAP = 0,
  GR_TEXTURECLAMP_CLAMP = 1,
};

// GrTextureFilterMode_t
enum : uint32_t {
  GR_TEXTUREFILTER_POINT_SAMPLED = 0,
  GR_TEXTUREFILTER_BILINEAR = 1,
};

// GrDepthBufferMode_t
enum : uint32_t {
  GR_DEPTHBUFFER_DISABLE = 0,
  GR_DEPTHBUFFER_ZBUFFER = 1,
  GR_DEPTHBUFFER_WBUFFER = 2,
};

// GrColorFormat_t
enum : uint32_t {
  GR_COLORFORMAT_ARGB = 0,
  GR_COLORFORMAT_ABGR = 1,
  GR_COLORFORMAT_RGBA = 2,
  GR_COLORFORMAT_BGRA = 3,
};

// Packed vertex matching D3DFVF_XYZRHW (0x04) | D3DFVF_DIFFUSE (0x40) | D3DFVF_TEX1 (0x100)
struct GpuVertex {
  float x, y, z, rhw;
  uint32_t argb;
  float u, v;
};
static_assert(sizeof(GpuVertex) == 28);

struct CachedTex {
  gfx::TextureId id = 0;
  int w = 0, h = 0;
  uint32_t fmt = 0;
  uint32_t aspect = 3;
  uint32_t large_lod = 0;
  uint32_t chromakey = 0;
  bool chromakey_active = false;
  uint32_t palette_hash = 0;
  std::vector<uint8_t> raw_bytes;
};

struct TmuState {
  std::vector<uint8_t> ram;
  uint32_t bound_addr = UINT32_MAX;
  std::map<uint32_t, CachedTex> tex_cache;
  uint32_t combine_fn = 1;  // GR_TEXTURECOMBINE_DECAL default on TMU0
  uint32_t clamp_s = GR_TEXTURECLAMP_WRAP;
  uint32_t clamp_t = GR_TEXTURECLAMP_WRAP;
  uint32_t min_filter = GR_TEXTUREFILTER_BILINEAR;
  uint32_t mag_filter = GR_TEXTUREFILTER_BILINEAR;
  uint32_t mip_mode = 0;
  uint32_t lod_bias = 0;
  uint32_t palette[256] = {};
  uint32_t palette_hash = 0;
};

struct GlideState {
  bool initialized = false;
  bool window_open = false;
  bool in_scene = false;

  std::unique_ptr<gfx::Device> gfx;
  gfx::DepthId depth_id = 0;
  gfx::FfState ff = gfx::DefaultFfState();
  FramePacer pacer;

  SDL_Window* sdl_win = nullptr;
  uint32_t color_format = GR_COLORFORMAT_ARGB;
  uint32_t origin = 0;

  // Clip rect in Glide virtual pixels
  int32_t clip_minx = 0, clip_miny = 0, clip_maxx = 1024, clip_maxy = 768;

  // Two TMUs (GliVd1vf.dll alternates TMU0 and TMU1 across textures)
  TmuState tmu[2];
  uint32_t active_tmu = 0;
  bool tex_enabled = false;
  uint32_t color_combine_fn = 2;  // GR_COLORCOMBINE_ITRGB
  uint32_t alpha_combine_fn = 3;
  uint32_t alpha_combine_factor = 8;
  uint32_t alpha_combine_local = 0;
  uint32_t alpha_combine_other = 0;
  bool alpha_combine_invert = false;

  // Explicit game alpha test state kept separate from synthetic chromakey discard
  bool game_alpha_test_enable = false;
  uint32_t game_alpha_test_func = 8;  // D3DCMP_ALWAYS
  uint32_t game_alpha_test_ref = 0;

  // Chromakey / Fog / Depth
  bool chromakey_enable = false;
  uint32_t chromakey_value = 0;  // 0x00RRGGBB in ARGB order (bytes decoded via color_format)
  uint32_t fog_mode = 0;
  uint32_t fog_color = 0;
  uint8_t fog_table[64] = {};
  uint32_t depth_mode = GR_DEPTHBUFFER_WBUFFER;

  // LFB lock buffer (16-bit RGB565 in guest memory + GPU upload texture)
  gaddr lfb_guest_mem = 0;
  uint32_t lfb_size = 0;
  gfx::TextureId lfb_tex = 0;
  bool lfb_locked_write = false;

  // Triangle batch accumulator (flushed when FfState changes or buffer ends)
  gfx::FfState batch_state = gfx::DefaultFfState();
  bool has_batch_state = false;
  std::vector<GpuVertex> tri_batch;

  // Stats / optional trace
  uint32_t tris_drawn = 0;
  uint32_t frames = 0;
  bool trace_env = false;
};

GlideState g_glide;

// ---- Helpers ---------------------------------------------------------------

uint32_t GlidePackToArgb(uint32_t raw, uint32_t fmt) {
  switch (fmt) {
    case GR_COLORFORMAT_ARGB: return raw;
    case GR_COLORFORMAT_ABGR: {
      uint32_t a = (raw >> 24) & 0xFF, b = (raw >> 16) & 0xFF, g = (raw >> 8) & 0xFF,
               r = raw & 0xFF;
      return (a << 24) | (r << 16) | (g << 8) | b;
    }
    case GR_COLORFORMAT_RGBA: {
      uint32_t r = (raw >> 24) & 0xFF, g = (raw >> 16) & 0xFF, b = (raw >> 8) & 0xFF,
               a = raw & 0xFF;
      return (a << 24) | (r << 16) | (g << 8) | b;
    }
    case GR_COLORFORMAT_BGRA: {
      uint32_t b = (raw >> 24) & 0xFF, g = (raw >> 16) & 0xFF, r = (raw >> 8) & 0xFF,
               a = raw & 0xFF;
      return (a << 24) | (r << 16) | (g << 8) | b;
    }
    default: return raw;
  }
}

void GetResolutionDims(uint32_t res_id, uint32_t* w, uint32_t* h) {
  switch (res_id) {
    case 0: *w = 320; *h = 200; break;
    case 1: *w = 320; *h = 240; break;
    case 2: *w = 400; *h = 256; break;
    case 3: *w = 512; *h = 384; break;
    case 4: *w = 640; *h = 200; break;
    case 5: *w = 640; *h = 350; break;
    case 6: *w = 640; *h = 400; break;
    case 7: *w = 640; *h = 480; break;
    case 8: *w = 800; *h = 600; break;
    case 9: *w = 960; *h = 720; break;
    case 10: *w = 856; *h = 480; break;
    case 11: *w = 512; *h = 256; break;
    case 12: *w = 1024; *h = 768; break;
    case 13: *w = 1280; *h = 1024; break;
    case 14: *w = 1600; *h = 1200; break;
    default: *w = 1024; *h = 768; break;
  }
}

void TexDimsFromLodAspect(uint32_t lod, uint32_t aspect, int* w, int* h, float* umax,
                          float* vmax) {
  int max_dim = std::max(1, 256 >> std::min<uint32_t>(lod, 8));
  if (aspect <= 3) {
    int shift = int(3 - aspect);
    *w = max_dim;
    *h = std::max(1, max_dim >> shift);
    *umax = 256.0f;
    *vmax = float(256 >> shift);
  } else {
    int shift = int(aspect - 3);
    *w = std::max(1, max_dim >> shift);
    *h = max_dim;
    *umax = float(256 >> shift);
    *vmax = 256.0f;
  }
}

uint32_t BytesPerTexelX2(uint32_t fmt) {
  return (fmt >= GR_TEXFMT_16BIT) ? 2u : 1u;
}

uint32_t TexLevelBytes(int w, int h, uint32_t fmt) {
  return uint32_t(w) * uint32_t(h) * BytesPerTexelX2(fmt);
}

void DecodeTextureToArgb(const CachedTex& t, const TmuState& ts, std::vector<uint32_t>& out) {
  const size_t n = size_t(t.w) * size_t(t.h);
  out.resize(n);
  const uint8_t* src = t.raw_bytes.data();
  const bool ck = t.chromakey_active;
  const uint32_t ck_rgb = t.chromakey & 0x00FFFFFFu;
  // Convert RGB565 reference chromakey too in case caller used 565 or 888
  const uint32_t ck_r5 = ((ck_rgb >> 16) & 0xF8);
  const uint32_t ck_g6 = ((ck_rgb >> 8) & 0xFC);
  const uint32_t ck_b5 = (ck_rgb & 0xF8);

  for (size_t i = 0; i < n; i++) {
    uint32_t c = 0xFFFFFFFFu;
    switch (t.fmt) {
      case GR_TEXFMT_RGB_565: {
        uint16_t p = uint16_t(src[2 * i]) | (uint16_t(src[2 * i + 1]) << 8);
        uint32_t r = ((p >> 11) & 0x1F) * 255u / 31u;
        uint32_t g = ((p >> 5) & 0x3F) * 255u / 63u;
        uint32_t b = (p & 0x1F) * 255u / 31u;
        uint32_t a = 255u;
        if (ck && ((r & 0xF8) == ck_r5 && (g & 0xFC) == ck_g6 && (b & 0xF8) == ck_b5)) a = 0;
        c = (a << 24) | (r << 16) | (g << 8) | b;
        break;
      }
      case GR_TEXFMT_ARGB_1555: {
        uint16_t p = uint16_t(src[2 * i]) | (uint16_t(src[2 * i + 1]) << 8);
        uint32_t a = (p & 0x8000u) ? 255u : 0u;
        uint32_t r = ((p >> 10) & 0x1F) * 255u / 31u;
        uint32_t g = ((p >> 5) & 0x1F) * 255u / 31u;
        uint32_t b = (p & 0x1F) * 255u / 31u;
        if (ck && ((r & 0xF8) == ck_r5 && (g & 0xF8) == (ck_g6 & 0xF8) && (b & 0xF8) == ck_b5))
          a = 0;
        c = (a << 24) | (r << 16) | (g << 8) | b;
        break;
      }
      case GR_TEXFMT_ARGB_4444: {
        uint16_t p = uint16_t(src[2 * i]) | (uint16_t(src[2 * i + 1]) << 8);
        uint32_t a = ((p >> 12) & 0xF) * 17u;
        uint32_t r = ((p >> 8) & 0xF) * 17u;
        uint32_t g = ((p >> 4) & 0xF) * 17u;
        uint32_t b = (p & 0xF) * 17u;
        c = (a << 24) | (r << 16) | (g << 8) | b;
        break;
      }
      case GR_TEXFMT_ALPHA_8: {
        uint32_t a = src[i];
        c = (a << 24) | 0x00FFFFFFu;
        break;
      }
      case GR_TEXFMT_INTENSITY_8: {
        uint32_t y = src[i];
        c = (y << 24) | (y << 16) | (y << 8) | y;
        break;
      }
      case GR_TEXFMT_ALPHA_INTENSITY_44: {
        uint8_t p = src[i];
        uint32_t a = ((p >> 4) & 0xF) * 17u;
        uint32_t y = (p & 0xF) * 17u;
        c = (a << 24) | (y << 16) | (y << 8) | y;
        break;
      }
      case GR_TEXFMT_P_8: {
        uint32_t pal = ts.palette[src[i]];
        c = pal | 0xFF000000u;
        if (ck && ((c & 0x00FFFFFFu) == ck_rgb)) c &= 0x00FFFFFFu;
        break;
      }
      case GR_TEXFMT_ALPHA_INTENSITY_88: {
        uint32_t y = src[2 * i];
        uint32_t a = src[2 * i + 1];
        c = (a << 24) | (y << 16) | (y << 8) | y;
        break;
      }
      case GR_TEXFMT_AP_88: {
        uint32_t pal = ts.palette[src[2 * i]];
        uint32_t a = src[2 * i + 1];
        c = (a << 24) | (pal & 0x00FFFFFFu);
        if (ck && ((c & 0x00FFFFFFu) == ck_rgb)) c &= 0x00FFFFFFu;
        break;
      }
      default: {
        uint32_t y = src[i];
        c = 0xFF000000u | (y << 16) | (y << 8) | y;
        break;
      }
    }
    out[i] = c;
  }
}

void EnsureUploaded(CachedTex& t, const TmuState& ts) {
  if (t.id && t.chromakey_active == g_glide.chromakey_enable &&
      (!g_glide.chromakey_enable || t.chromakey == g_glide.chromakey_value) &&
      ((t.fmt != GR_TEXFMT_P_8 && t.fmt != GR_TEXFMT_AP_88) ||
       t.palette_hash == ts.palette_hash)) {
    return;
  }
  if (!g_glide.gfx || t.w <= 0 || t.h <= 0 || t.raw_bytes.empty()) return;
  if (!t.id) {
    t.id = g_glide.gfx->CreateTexture(t.w, t.h, 1, gfx::Format::A8R8G8B8, false);
  }
  t.chromakey_active = g_glide.chromakey_enable;
  t.chromakey = g_glide.chromakey_value;
  t.palette_hash = ts.palette_hash;
  std::vector<uint32_t> argb;
  DecodeTextureToArgb(t, ts, argb);
  g_glide.gfx->UpdateTexture(t.id, 0, gfx::Rect{0, 0, t.w, t.h}, argb.data(), uint32_t(t.w * 4));
}

void UpdateFfColorAlphaStages() {
  TmuState& ts = g_glide.tmu[g_glide.active_tmu];
  g_glide.ff.tss[0][13] = (ts.clamp_s == GR_TEXTURECLAMP_CLAMP) ? 3u : 1u;
  g_glide.ff.tss[0][14] = (ts.clamp_t == GR_TEXTURECLAMP_CLAMP) ? 3u : 1u;
  g_glide.ff.tss[0][17] = (ts.min_filter == GR_TEXTUREFILTER_POINT_SAMPLED) ? 1u : 2u;
  g_glide.ff.tss[0][16] = (ts.mag_filter == GR_TEXTUREFILTER_POINT_SAMPLED) ? 1u : 2u;
  g_glide.ff.tss[0][18] = (ts.mip_mode == 0) ? 0u : 3u;
  g_glide.ff.tss[0][19] = ts.lod_bias;

  // Texture stage 0 COLOROP/ALPHAOP combining per guColorCombineFunction & grAlphaCombine
  bool uses_tex_color =
      g_glide.color_combine_fn == 4 || g_glide.color_combine_fn == 5 ||
      g_glide.color_combine_fn == 6 || g_glide.color_combine_fn == 7 ||
      g_glide.color_combine_fn == 8 || g_glide.color_combine_fn == 9 ||
      g_glide.color_combine_fn == 10 || g_glide.color_combine_fn == 11 ||
      g_glide.color_combine_fn == 12 || g_glide.color_combine_fn == 13;
  bool uses_tex_alpha = (g_glide.alpha_combine_other == 1) &&
                        (g_glide.alpha_combine_fn >= 3);
  bool have_tex = (ts.bound_addr != UINT32_MAX && ts.combine_fn != 0);
  g_glide.tex_enabled = (uses_tex_color || uses_tex_alpha) && have_tex;

  if (!uses_tex_color || !have_tex) {
    g_glide.ff.tss[0][1] = 2;  // D3DTOP_SELECTARG1
    g_glide.ff.tss[0][2] = 0;  // D3DTA_DIFFUSE
  } else if (g_glide.color_combine_fn == 4 /* DECAL_TEXTURE */) {
    g_glide.ff.tss[0][1] = 2;  // D3DTOP_SELECTARG1
    g_glide.ff.tss[0][2] = 2;  // D3DTA_TEXTURE
  } else if (g_glide.color_combine_fn == 9 /* TEXTURE_TIMES_ALPHA */) {
    g_glide.ff.tss[0][1] = 4;             // D3DTOP_MODULATE
    g_glide.ff.tss[0][2] = 2;             // D3DTA_TEXTURE
    g_glide.ff.tss[0][3] = 0u | 0x20u;    // D3DTA_DIFFUSE | D3DTA_ALPHAREPLICATE
  } else if (g_glide.color_combine_fn == 11 /* TEXTURE_ADD_ITRGB */) {
    g_glide.ff.tss[0][1] = 7;  // D3DTOP_ADD
    g_glide.ff.tss[0][2] = 2;  // TEXTURE
    g_glide.ff.tss[0][3] = 0;  // DIFFUSE
  } else {
    g_glide.ff.tss[0][1] = 4;  // D3DTOP_MODULATE
    g_glide.ff.tss[0][2] = 2;  // TEXTURE
    g_glide.ff.tss[0][3] = 0;  // DIFFUSE
  }

  if (g_glide.alpha_combine_fn == 1 || g_glide.alpha_combine_fn == 2) {
    // GR_COMBINE_FUNCTION_LOCAL / LOCAL_ALPHA -> vertex diffuse alpha
    g_glide.ff.tss[0][4] = 2;  // D3DTOP_SELECTARG1
    g_glide.ff.tss[0][5] = 0;  // D3DTA_DIFFUSE
  } else if (!uses_tex_alpha || !have_tex) {
    g_glide.ff.tss[0][4] = 2;  // D3DTOP_SELECTARG1
    g_glide.ff.tss[0][5] = 0;  // D3DTA_DIFFUSE
  } else if (g_glide.alpha_combine_factor == 1 /* LOCAL */ ||
             g_glide.alpha_combine_factor == 3 /* LOCAL_ALPHA */ ||
             g_glide.alpha_combine_fn == 4 /* SCALE_OTHER_ADD_LOCAL */) {
    g_glide.ff.tss[0][4] = 4;  // D3DTOP_MODULATE
    g_glide.ff.tss[0][5] = 2;  // TEXTURE
    g_glide.ff.tss[0][6] = 0;  // DIFFUSE
  } else {
    g_glide.ff.tss[0][4] = 2;  // D3DTOP_SELECTARG1
    g_glide.ff.tss[0][5] = 2;  // D3DTA_TEXTURE
  }

  if (g_glide.tex_enabled) {
    auto it = ts.tex_cache.find(ts.bound_addr);
    if (it != ts.tex_cache.end()) {
      EnsureUploaded(it->second, ts);
      g_glide.ff.textures[0] = it->second.id;
    } else {
      g_glide.ff.textures[0] = 0;
    }
  } else {
    g_glide.ff.textures[0] = 0;
  }

  // Separate explicit game alpha-test from synthetic chromakey alpha discard.
  if (g_glide.game_alpha_test_enable) {
    g_glide.ff.rs[15] = 1;
    g_glide.ff.rs[25] = g_glide.game_alpha_test_func;
    g_glide.ff.rs[24] = g_glide.game_alpha_test_ref;
  } else if (g_glide.chromakey_enable && uses_tex_color && have_tex) {
    g_glide.ff.rs[15] = 1;    // ALPHATESTENABLE
    g_glide.ff.rs[24] = 0;    // ALPHAREF = 0 (strict > 0 discards key texels with alpha=0)
    g_glide.ff.rs[25] = 5;    // D3DCMP_GREATER
  } else {
    g_glide.ff.rs[15] = 0;
  }
}

void FlushBatch() {
  if (g_glide.tri_batch.empty() || !g_glide.gfx) return;
  if (!g_glide.in_scene) {
    g_glide.gfx->BeginScene();
    g_glide.in_scene = true;
  }
  constexpr uint32_t kGlideFvf = 0x004u | 0x040u | 0x100u;  // XYZRHW | DIFFUSE | TEX1
  gfx::FfDraw fd = gfx::FfInterleaved(4 /* D3DPT_TRIANGLELIST */, kGlideFvf,
                                      g_glide.tri_batch.data(), sizeof(GpuVertex));
  fd.vertex_count = uint32_t(g_glide.tri_batch.size());
  g_glide.gfx->DrawFf(g_glide.batch_state, fd);
  g_glide.tri_batch.clear();
  g_glide.has_batch_state = false;
}

void PrepareDrawState() {
  UpdateFfColorAlphaStages();
  if (g_glide.has_batch_state &&
      std::memcmp(&g_glide.batch_state, &g_glide.ff, sizeof(gfx::FfState)) != 0) {
    FlushBatch();
  }
  if (!g_glide.has_batch_state) {
    g_glide.batch_state = g_glide.ff;
    g_glide.has_batch_state = true;
  }
}

float GlideVertexDepth(float ooz, float oow) {
  if (g_glide.depth_mode == GR_DEPTHBUFFER_ZBUFFER) {
    float z = ooz / 65535.0f;
    return std::clamp(z, 0.0f, 1.0f);
  }
  // W-buffer monotonic log2 mapping across [0.01 .. 65536.0] -> [0.0 .. 1.0].
  float w = (oow > 1e-6f) ? (1.0f / oow) : 65536.0f;
  float lw = std::log2(std::clamp(w, 0.01f, 65536.0f));
  constexpr float kLogMin = -6.6438562f;   // log2(0.01)
  constexpr float kLogSpan = 22.6438562f;  // log2(65536) - log2(0.01)
  return std::clamp((lw - kLogMin) / kLogSpan, 0.0f, 1.0f);
}

void UnpackGrVertex(const uint8_t* p, uint32_t tmu_id, GpuVertex* out, float sx, float sy,
                    float umax, float vmax) {
  float x, y, r, g, b, ooz, a, oow, sow, tow;
  std::memcpy(&x, p + 0, 4);
  std::memcpy(&y, p + 4, 4);
  std::memcpy(&r, p + 12, 4);
  std::memcpy(&g, p + 16, 4);
  std::memcpy(&b, p + 20, 4);
  std::memcpy(&ooz, p + 24, 4);
  std::memcpy(&a, p + 28, 4);
  std::memcpy(&oow, p + 32, 4);
  const uint32_t uv_off = 36u + (tmu_id & 1u) * 12u;
  std::memcpy(&sow, p + uv_off, 4);
  std::memcpy(&tow, p + uv_off + 4u, 4);

  if (x > kSnapThreshold) x -= kSnapOffset;
  if (y > kSnapThreshold) y -= kSnapOffset;
  if (g_glide.origin == 1) y = float(GetDisplayConfig().glide_height) - y;

  out->x = x * sx - 0.5f;
  out->y = y * sy - 0.5f;
  out->z = GlideVertexDepth(ooz, oow);
  float safe_oow = (std::fabs(oow) > 1e-7f) ? oow : 1.0f;
  out->rhw = safe_oow;

  uint32_t ir = uint32_t(std::clamp(r, 0.0f, 255.0f));
  uint32_t ig = uint32_t(std::clamp(g, 0.0f, 255.0f));
  uint32_t ib = uint32_t(std::clamp(b, 0.0f, 255.0f));
  uint32_t ia = uint32_t(std::clamp(a, 0.0f, 255.0f));

  // Software per-vertex fog tint approximation when fog_mode != 0
  if (g_glide.fog_mode != 0) {
    float w = 1.0f / std::max(safe_oow, 1e-6f);
    // Quick log-approx table index matching Voodoo fog W table
    int idx = 0;
    uint32_t wi = uint32_t(std::max(1.0f, w));
    int msb = 0;
    for (uint32_t v = wi; v >>= 1;) msb++;
    idx = std::clamp(msb * 4, 0, 63);
    uint32_t fog_alpha = g_glide.fog_table[idx];
    if (fog_alpha > 0) {
      uint32_t fr = (g_glide.fog_color >> 16) & 0xFF;
      uint32_t fg = (g_glide.fog_color >> 8) & 0xFF;
      uint32_t fb = g_glide.fog_color & 0xFF;
      ir = (ir * (255u - fog_alpha) + fr * fog_alpha) / 255u;
      ig = (ig * (255u - fog_alpha) + fg * fog_alpha) / 255u;
      ib = (ib * (255u - fog_alpha) + fb * fog_alpha) / 255u;
    }
  }

  out->argb = (ia << 24) | (ir << 16) | (ig << 8) | ib;
  out->u = (sow / safe_oow) / umax;
  out->v = (tow / safe_oow) / vmax;
}

uint32_t MapGlideBlend(uint32_t gr, bool is_src) {
  switch (gr) {
    case 0: return 1;  // D3DBLEND_ZERO
    case 1: return 5;  // D3DBLEND_SRCALPHA
    case 2: return is_src ? 9 : 3;  // DESTCOLOR / SRCCOLOR
    case 3: return 7;  // DESTALPHA
    case 4: return 2;  // ONE
    case 5: return 6;  // INVSRCALPHA
    case 6: return is_src ? 10 : 4;  // INVDESTCOLOR / INVSRCCOLOR
    case 7: return 8;  // INVDESTALPHA
    case 0xF: return 11;  // SRCALPHASAT
    default: return is_src ? 2 : 1;
  }
}

void ApplyClipScissor() {
  const auto& cfg = GetDisplayConfig();
  float sx = float(cfg.host_width) / float(std::max<uint32_t>(1, cfg.glide_width));
  float sy = float(cfg.host_height) / float(std::max<uint32_t>(1, cfg.glide_height));
  g_glide.ff.scissor = {
      std::max<int32_t>(0, int32_t(std::floor(g_glide.clip_minx * sx))),
      std::max<int32_t>(0, int32_t(std::floor(g_glide.clip_miny * sy))),
      std::min<int32_t>(int32_t(cfg.host_width), int32_t(std::ceil(g_glide.clip_maxx * sx))),
      std::min<int32_t>(int32_t(cfg.host_height), int32_t(std::ceil(g_glide.clip_maxy * sy))),
  };
  bool full = (g_glide.clip_minx <= 0 && g_glide.clip_miny <= 0 &&
               g_glide.clip_maxx >= int32_t(cfg.glide_width) &&
               g_glide.clip_maxy >= int32_t(cfg.glide_height));
  g_glide.ff.rs[174] = full ? 0u : 1u;  // D3DRS_SCISSORTESTENABLE
}

void InitDefaultRenderState() {
  g_glide.ff = gfx::DefaultFfState();
  const auto& cfg = GetDisplayConfig();
  g_glide.ff.viewport = {0, 0, cfg.host_width, cfg.host_height, 0.0f, 1.0f};
  g_glide.ff.rs[22] = 1;    // D3DCULL_NONE
  g_glide.ff.rs[137] = 0;   // D3DRS_LIGHTING = FALSE (pre-lit XYZRHW vertices)
  g_glide.ff.rs[7] = 1;     // ZENABLE = TRUE
  g_glide.ff.rs[14] = 1;    // ZWRITEENABLE = TRUE
  g_glide.ff.rs[23] = 2;    // ZFUNC = D3DCMP_LESS (Glide GR_CMP_LESS + 1)
  g_glide.ff.rs[168] = 0xF; // COLORWRITEENABLE
  g_glide.ff.tss[0][13] = 1;  // WRAP
  g_glide.ff.tss[0][14] = 1;  // WRAP
  g_glide.ff.tss[0][16] = 2;  // LINEAR
  g_glide.ff.tss[0][17] = 2;  // LINEAR
  g_glide.ff.tss[0][18] = 0;  // MIPFILTER_NONE
  g_glide.has_batch_state = false;
  ApplyClipScissor();
}

// ---- 44 Glide 2.x HLEs -----------------------------------------------------

HLE_NAMED("glide2x.dll", "_grGlideInit@0", GrGlideInit, 0) {
  if (!g_glide.initialized) {
    g_glide.tmu[0].ram.assign(kTmuMemSize, 0);
    g_glide.tmu[1].ram.assign(kTmuMemSize, 0);
    const char* tr = std::getenv("RAY2_GLIDE_TRACE");
    g_glide.trace_env = (tr && tr[0] == '1');
    g_glide.initialized = true;
  }
  return 0;
}

template <typename Fn>
void RunOnHostMain(Fn&& fn) {
  if (SDL_IsMainThread()) {
    fn();
  } else {
    SDL_RunOnMainThread([](void* p) { (*static_cast<Fn*>(p))(); }, &fn, true);
  }
}

HLE_NAMED("glide2x.dll", "_grGlideShutdown@0", GrGlideShutdown, 0) {
  FlushBatch();
  if (g_glide.in_scene && g_glide.gfx) {
    g_glide.gfx->EndScene();
    g_glide.in_scene = false;
  }
  for (int t = 0; t < 2; t++) {
    for (auto& [addr, ct] : g_glide.tmu[t].tex_cache) {
      if (ct.id && g_glide.gfx) g_glide.gfx->DestroyTexture(ct.id);
    }
    g_glide.tmu[t].tex_cache.clear();
  }
  if (g_glide.lfb_tex && g_glide.gfx) {
    g_glide.gfx->DestroyTexture(g_glide.lfb_tex);
    g_glide.lfb_tex = 0;
  }
  if (g_glide.depth_id && g_glide.gfx) {
    g_glide.gfx->DestroyDepth(g_glide.depth_id);
    g_glide.depth_id = 0;
  }
  RunOnHostMain([&] { g_glide.gfx.reset(); });
  g_glide.window_open = false;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grSstQueryHardware@4", GrSstQueryHardware, 1) {
  uint32_t hw = Arg(c, 0);
  if (!hw) return 0;
  // GrHwConfiguration (max ~100 bytes; zero and fill 1 Voodoo2 w/ 4MB FB + 2 TMUs @ 4MB)
  std::memset(GPtr(hw), 0, 128);
  Wr32(hw + 0, 1);   // num_sst
  Wr32(hw + 4, 0);   // SSTs[0].type = GR_SSTTYPE_VOODOO
  Wr32(hw + 8, 4);   // fbRam = 4 MB
  Wr32(hw + 12, 2);  // fbiRev = 2
  Wr32(hw + 16, 2);  // nTexelfx = 2
  Wr32(hw + 20, 0);  // sliDetect = FALSE
  Wr32(hw + 24, 1);  // tmuConfig[0].tmuRev
  Wr32(hw + 28, 4);  // tmuConfig[0].tmuRam = 4 MB
  Wr32(hw + 32, 1);  // tmuConfig[1].tmuRev
  Wr32(hw + 36, 4);  // tmuConfig[1].tmuRam = 4 MB
  return 1;
}

HLE_NAMED("glide2x.dll", "_grSstSelect@4", GrSstSelect, 1) { return 0; }
HLE_NAMED("glide2x.dll", "_grSstIdle@0", GrSstIdle, 0) { return 0; }
HLE_NAMED("glide2x.dll", "_grSstControl@4", GrSstControl, 1) { return 1; }
HLE_NAMED("glide2x.dll", "_grSstVRetraceOn@0", GrSstVRetraceOn, 0) { return 1; }

HLE_NAMED("glide2x.dll", "_grSstResetPerfStats@0", GrSstResetPerfStats, 0) {
  g_glide.tris_drawn = 0;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grSstPerfStats@4", GrSstPerfStats, 1) {
  uint32_t st = Arg(c, 0);
  if (st) {
    std::memset(GPtr(st), 0, 20);
    Wr32(st + 4, g_glide.tris_drawn);
  }
  return 0;
}

HLE_NAMED("glide2x.dll", "_grSstWinOpen@28", GrSstWinOpen, 7) {
  uint32_t hwnd = Arg(c, 0);
  uint32_t res_id = Arg(c, 1);
  // Arg 2 = refresh, Arg 3 = colorFormat, Arg 4 = origin
  g_glide.color_format = Arg(c, 3);
  g_glide.origin = Arg(c, 4);

  auto& cfg = GetDisplayConfig();
  if (!hwnd) hwnd = cfg.main_hwnd;
  uint32_t gw = 1024, gh = 768;
  GetResolutionDims(res_id, &gw, &gh);
  cfg.glide_width = gw;
  cfg.glide_height = gh;
  cfg.main_hwnd = hwnd;

  SDL_Window* win = hwnd ? SdlWindowFor(hwnd) : nullptr;
  if (!win) {
    WarnF("grSstWinOpen: no SDL window for hwnd %08X\n", hwnd);
    return 0;
  }
  g_glide.sdl_win = win;
  RunOnHostMain([&] {
    SDL_SetWindowSize(win, int(cfg.host_width), int(cfg.host_height));
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(win);
    if (!g_glide.gfx) {
      g_glide.gfx = gfx::CreateChosenDevice(win);
      if (!g_glide.gfx) FatalF("grSstWinOpen: CreateChosenDevice (Metal) failed");
    }
    g_glide.gfx->ResetSwapChain(int(cfg.host_width), int(cfg.host_height), true);
  });

  if (g_glide.depth_id) g_glide.gfx->DestroyDepth(g_glide.depth_id);
  g_glide.depth_id =
      g_glide.gfx->CreateDepth(int(cfg.host_width), int(cfg.host_height), gfx::Format::D24X8);
  g_glide.gfx->SetRenderTarget(0, g_glide.depth_id);

  g_glide.clip_minx = 0;
  g_glide.clip_miny = 0;
  g_glide.clip_maxx = int32_t(gw);
  g_glide.clip_maxy = int32_t(gh);

  if (!g_glide.lfb_guest_mem) {
    g_glide.lfb_size = cfg.glide_width * cfg.glide_height * 2;
    g_glide.lfb_guest_mem = HeapAllocProcess(std::max<uint32_t>(1u, g_glide.lfb_size), true);
  }
  InitDefaultRenderState();
  g_glide.window_open = true;
  std::fprintf(stderr, "glide2x: grSstWinOpen glide=%ux%u -> Metal host=%ux%u (fmt=%u orig=%u)\n",
               gw, gh, cfg.host_width, cfg.host_height, g_glide.color_format, g_glide.origin);
  return 1;
}

HLE_NAMED("glide2x.dll", "_grSstWinClose@0", GrSstWinClose, 0) {
  FlushBatch();
  if (g_glide.in_scene && g_glide.gfx) {
    g_glide.gfx->EndScene();
    g_glide.in_scene = false;
  }
  g_glide.window_open = false;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grClipWindow@16", GrClipWindow, 4) {
  FlushBatch();
  g_glide.clip_minx = int32_t(Arg(c, 0));
  g_glide.clip_miny = int32_t(Arg(c, 1));
  g_glide.clip_maxx = int32_t(Arg(c, 2));
  g_glide.clip_maxy = int32_t(Arg(c, 3));
  ApplyClipScissor();
  return 0;
}

HLE_NAMED("glide2x.dll", "_grBufferClear@12", GrBufferClear, 3) {
  uint32_t raw_col = Arg(c, 0);
  uint32_t alpha = Arg(c, 1) & 0xFFu;
  uint32_t depth = Arg(c, 2);
  FlushBatch();
  if (!g_glide.gfx) return 0;
  const auto& cfg = GetDisplayConfig();
  uint32_t argb = (GlidePackToArgb(raw_col, g_glide.color_format) & 0x00FFFFFFu) | (alpha << 24);
  float z = std::clamp(float(depth) / 65535.0f, 0.0f, 1.0f);
  gfx::Rect r{0, 0, int32_t(cfg.host_width), int32_t(cfg.host_height)};
  uint32_t flags = (g_glide.depth_mode != GR_DEPTHBUFFER_DISABLE) ? 3u : 1u;  // TARGET | ZBUFFER
  g_glide.gfx->Clear(1, &r, flags, argb, z, 0);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grBufferSwap@4", GrBufferSwap, 1) {
  FlushBatch();
  if (!g_glide.gfx) return 0;
  if (g_glide.in_scene) {
    g_glide.gfx->EndScene();
    g_glide.in_scene = false;
  }
  const auto& cfg = GetDisplayConfig();
  gfx::PresentWithDevAids(*g_glide.gfx, int(cfg.host_width), int(cfg.host_height));
  ::recomp::WatchdogFrame();
  g_glide.frames++;
  g_glide.pacer.Wait(60.0);
  PumpHostEvents();
  return 0;
}

HLE_NAMED("glide2x.dll", "_grDrawTriangle@12", GrDrawTriangle, 3) {
  uint32_t pa = Arg(c, 0), pb = Arg(c, 1), pc = Arg(c, 2);
  if (!pa || !pb || !pc || !g_glide.gfx) return 0;
  PrepareDrawState();
  const auto& cfg = GetDisplayConfig();
  float sx = float(cfg.host_width) / float(std::max<uint32_t>(1, cfg.glide_width));
  float sy = float(cfg.host_height) / float(std::max<uint32_t>(1, cfg.glide_height));
  float umax = 256.0f, vmax = 256.0f;
  const uint32_t tid = g_glide.active_tmu & 1u;
  if (g_glide.tex_enabled) {
    const TmuState& ts = g_glide.tmu[tid];
    auto it = ts.tex_cache.find(ts.bound_addr);
    if (it != ts.tex_cache.end()) {
      int dummyw, dummyh;
      TexDimsFromLodAspect(it->second.large_lod, it->second.aspect, &dummyw, &dummyh, &umax, &vmax);
    }
  }
  GpuVertex v0, v1, v2;
  UnpackGrVertex(GPtr(pa), tid, &v0, sx, sy, umax, vmax);
  UnpackGrVertex(GPtr(pb), tid, &v1, sx, sy, umax, vmax);
  UnpackGrVertex(GPtr(pc), tid, &v2, sx, sy, umax, vmax);
  if (g_glide.trace_env && g_glide.frames == 149 && g_glide.tris_drawn < 35) {
    const TmuState& ts = g_glide.tmu[tid];
    auto it = ts.tex_cache.find(ts.bound_addr);
    std::fprintf(stderr,
                 "tri#%u tmu=%u addr=0x%x tex=%s fmt=%u %dx%d cfn=%u afn=%u fac=%u ck=%d(%06x) "
                 "v0=(%.1f,%.1f z=%.3f rhw=%.4f u=%.3f v=%.3f argb=%08x)\n",
                 g_glide.tris_drawn, tid, ts.bound_addr,
                 g_glide.tex_enabled ? "ON" : "OFF",
                 (it != ts.tex_cache.end()) ? it->second.fmt : 99u,
                 (it != ts.tex_cache.end()) ? it->second.w : 0,
                 (it != ts.tex_cache.end()) ? it->second.h : 0,
                 g_glide.color_combine_fn, g_glide.alpha_combine_fn, g_glide.alpha_combine_factor,
                 g_glide.chromakey_enable ? 1 : 0, g_glide.chromakey_value,
                 v0.x, v0.y, v0.z, v0.rhw, v0.u, v0.v, v0.argb);
  }
  g_glide.tri_batch.push_back(v0);
  g_glide.tri_batch.push_back(v1);
  g_glide.tri_batch.push_back(v2);
  g_glide.tris_drawn++;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grDrawLine@8", GrDrawLine, 2) {
  uint32_t pa = Arg(c, 0), pb = Arg(c, 1);
  if (!pa || !pb || !g_glide.gfx) return 0;
  PrepareDrawState();
  const auto& cfg = GetDisplayConfig();
  float sx = float(cfg.host_width) / float(std::max<uint32_t>(1, cfg.glide_width));
  float sy = float(cfg.host_height) / float(std::max<uint32_t>(1, cfg.glide_height));
  GpuVertex v0, v1;
  const uint32_t tid = g_glide.active_tmu & 1u;
  UnpackGrVertex(GPtr(pa), tid, &v0, sx, sy, 256.0f, 256.0f);
  UnpackGrVertex(GPtr(pb), tid, &v1, sx, sy, 256.0f, 256.0f);
  // Emit thin 1.5px screen quad as 2 triangles so D3DPT_TRIANGLELIST batch handles lines cleanly
  float dx = v1.x - v0.x, dy = v1.y - v0.y;
  float len = std::sqrt(dx * dx + dy * dy);
  float nx = len > 0.001f ? (-dy / len) * 0.75f : 0.75f;
  float ny = len > 0.001f ? (dx / len) * 0.75f : 0.0f;
  GpuVertex a = v0, b = v0, c1 = v1, d = v1;
  a.x -= nx; a.y -= ny;
  b.x += nx; b.y += ny;
  c1.x += nx; c1.y += ny;
  d.x -= nx; d.y -= ny;
  g_glide.tri_batch.insert(g_glide.tri_batch.end(), {a, b, c1, a, c1, d});
  return 0;
}

// ---- Render states, blending, depth, chromakey, fog ------------------------

HLE_NAMED("glide2x.dll", "_grAlphaBlendFunction@16", GrAlphaBlendFunction, 4) {
  FlushBatch();
  uint32_t rgb_sf = Arg(c, 0), rgb_df = Arg(c, 1);
  // Arg 2/3 are alpha_sf/alpha_df (keep single blend on color/alpha matching D3D7 default)
  bool enable = !(rgb_sf == 4 /* ONE */ && rgb_df == 0 /* ZERO */);
  g_glide.ff.rs[27] = enable ? 1u : 0u;  // ALPHABLENDENABLE
  g_glide.ff.rs[19] = MapGlideBlend(rgb_sf, true);
  g_glide.ff.rs[20] = MapGlideBlend(rgb_df, false);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grAlphaCombine@20", GrAlphaCombine, 5) {
  FlushBatch();
  g_glide.alpha_combine_fn = Arg(c, 0);
  g_glide.alpha_combine_factor = Arg(c, 1);
  g_glide.alpha_combine_local = Arg(c, 2);
  g_glide.alpha_combine_other = Arg(c, 3);
  g_glide.alpha_combine_invert = Arg(c, 4) != 0;
  return 0;
}

HLE_NAMED("glide2x.dll", "_guAlphaSource@4", GuAlphaSource, 1) {
  FlushBatch();
  uint32_t mode = Arg(c, 0);
  g_glide.alpha_combine_fn = 3;  // SCALE_OTHER
  g_glide.alpha_combine_factor = 8;
  g_glide.alpha_combine_other = (mode == 2 || mode == 3) ? 1u : 0u;
  return 0;
}

HLE_NAMED("glide2x.dll", "_guColorCombineFunction@4", GuColorCombineFunction, 1) {
  FlushBatch();
  g_glide.color_combine_fn = Arg(c, 0);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grAlphaTestFunction@4", GrAlphaTestFunction, 1) {
  FlushBatch();
  uint32_t fn = Arg(c, 0);
  if (fn == 7 /* GR_CMP_ALWAYS */) {
    g_glide.game_alpha_test_enable = false;
  } else {
    g_glide.game_alpha_test_enable = true;
    g_glide.game_alpha_test_func = std::clamp<uint32_t>(fn + 1, 1, 8);
  }
  return 0;
}

HLE_NAMED("glide2x.dll", "_grAlphaTestReferenceValue@4", GrAlphaTestReferenceValue, 1) {
  FlushBatch();
  g_glide.game_alpha_test_ref = Arg(c, 0) & 0xFFu;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grChromakeyMode@4", GrChromakeyMode, 1) {
  FlushBatch();
  g_glide.chromakey_enable = Arg(c, 0) != 0;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grChromakeyValue@4", GrChromakeyValue, 1) {
  FlushBatch();
  g_glide.chromakey_value = GlidePackToArgb(Arg(c, 0), g_glide.color_format);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grColorMask@8", GrColorMask, 2) {
  FlushBatch();
  uint32_t rgb = Arg(c, 0) ? 0x7u : 0u;
  uint32_t a = Arg(c, 1) ? 0x8u : 0u;
  g_glide.ff.rs[168] = rgb | a;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grDepthBufferMode@4", GrDepthBufferMode, 1) {
  FlushBatch();
  uint32_t m = Arg(c, 0);
  g_glide.depth_mode = m;
  g_glide.ff.rs[7] = (m == GR_DEPTHBUFFER_DISABLE) ? 0u : 1u;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grDepthBufferFunction@4", GrDepthBufferFunction, 1) {
  FlushBatch();
  uint32_t fn = Arg(c, 0);
  g_glide.ff.rs[23] = std::clamp<uint32_t>(fn + 1, 1, 8);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grDepthMask@4", GrDepthMask, 1) {
  FlushBatch();
  g_glide.ff.rs[14] = Arg(c, 0) ? 1u : 0u;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grFogMode@4", GrFogMode, 1) {
  FlushBatch();
  g_glide.fog_mode = Arg(c, 0);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grFogColorValue@4", GrFogColorValue, 1) {
  FlushBatch();
  g_glide.fog_color = GlidePackToArgb(Arg(c, 0), g_glide.color_format);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grFogTable@4", GrFogTable, 1) {
  FlushBatch();
  if (uint32_t p = Arg(c, 0)) std::memcpy(g_glide.fog_table, GPtr(p), 64);
  return 0;
}

HLE_NAMED("glide2x.dll", "_guFogTableIndexToW@4", GuFogTableIndexToW, 1) {
  int i = std::clamp<int>(int(Arg(c, 0)), 0, 63);
  float w = float(std::pow(2.0, 3.0 + double(i >> 2)) / (8.0 - double(i & 3)));
  uint32_t bits = std::bit_cast<uint32_t>(w);
  // Floating return value goes in x87 ST(0) on x86 cdecl/stdcall!
  x87::LoadF32(c.fpu, bits);
  return bits;
}

// ---- TMU textures & LFB ----------------------------------------------------

HLE_NAMED("glide2x.dll", "_grTexMinAddress@4", GrTexMinAddress, 1) { return 0; }
HLE_NAMED("glide2x.dll", "_grTexMaxAddress@4", GrTexMaxAddress, 1) { return kTmuMemSize; }

HLE_NAMED("glide2x.dll", "_grTexTextureMemRequired@8", GrTexTextureMemRequired, 2) {
  // args: evenOdd (ignored), GrTexInfo* info
  uint32_t pinfo = Arg(c, 1);
  if (!pinfo) return 0;
  uint32_t small_lod = Rd32(pinfo + 0);
  uint32_t large_lod = Rd32(pinfo + 4);
  uint32_t aspect = Rd32(pinfo + 8);
  uint32_t fmt = Rd32(pinfo + 12);
  uint32_t total = 0;
  for (uint32_t lod = large_lod; lod <= small_lod && lod <= 8; lod++) {
    int w, h;
    float du, dv;
    TexDimsFromLodAspect(lod, aspect, &w, &h, &du, &dv);
    total += TexLevelBytes(w, h, fmt);
  }
  return (total + 7u) & ~7u;
}

HLE_NAMED("glide2x.dll", "_grTexDownloadMipMap@16", GrTexDownloadMipMap, 4) {
  uint32_t tmu = Arg(c, 0) & 1u;
  uint32_t start = Arg(c, 1);
  // Arg 2 = evenOdd
  uint32_t pinfo = Arg(c, 3);
  if (!pinfo) return 0;

  uint32_t small_lod = Rd32(pinfo + 0);
  uint32_t large_lod = Rd32(pinfo + 4);
  uint32_t aspect = Rd32(pinfo + 8);
  uint32_t fmt = Rd32(pinfo + 12);
  uint32_t data_va = Rd32(pinfo + 16);
  if (!data_va) return 0;

  int w, h;
  float du, dv;
  TexDimsFromLodAspect(large_lod, aspect, &w, &h, &du, &dv);
  uint32_t level0_bytes = TexLevelBytes(w, h, fmt);

  auto& cache = g_glide.tmu[tmu].tex_cache;
  auto it = cache.find(start);
  if (it != cache.end() && (it->second.w != w || it->second.h != h || it->second.fmt != fmt)) {
    if (it->second.id && g_glide.gfx) g_glide.gfx->DestroyTexture(it->second.id);
    cache.erase(it);
    it = cache.end();
  }
  CachedTex& ct = cache[start];
  ct.w = w;
  ct.h = h;
  ct.fmt = fmt;
  ct.aspect = aspect;
  ct.large_lod = large_lod;
  ct.raw_bytes.resize(level0_bytes);
  std::memcpy(ct.raw_bytes.data(), GPtr(data_va), level0_bytes);
  // Invalidate GPU upload so EnsureUploaded refreshes
  ct.chromakey_active = !g_glide.chromakey_enable;

  (void)small_lod;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexDownloadTable@12", GrTexDownloadTable, 3) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  uint32_t type = Arg(c, 1);
  uint32_t data = Arg(c, 2);
  if (type == 2 /* GR_TEXTABLE_PALETTE */ && data) {
    std::memcpy(g_glide.tmu[tmu].palette, GPtr(data), sizeof(g_glide.tmu[tmu].palette));
    g_glide.tmu[tmu].palette_hash++;
  }
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexSource@16", GrTexSource, 4) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  uint32_t start = Arg(c, 1);
  g_glide.tmu[tmu].bound_addr = start;
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexCombineFunction@8", GrTexCombineFunction, 2) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  uint32_t fn = Arg(c, 1);
  g_glide.tmu[tmu].combine_fn = fn;
  if (fn == 1 /* GR_TEXTURECOMBINE_DECAL */) {
    g_glide.active_tmu = tmu;
  } else if (fn == 0 /* GR_TEXTURECOMBINE_ZERO */ && g_glide.tmu[tmu ^ 1u].combine_fn == 1) {
    g_glide.active_tmu = tmu ^ 1u;
  }
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexClampMode@12", GrTexClampMode, 3) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  g_glide.tmu[tmu].clamp_s = Arg(c, 1);
  g_glide.tmu[tmu].clamp_t = Arg(c, 2);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexFilterMode@12", GrTexFilterMode, 3) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  g_glide.tmu[tmu].min_filter = Arg(c, 1);
  g_glide.tmu[tmu].mag_filter = Arg(c, 2);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexMipMapMode@12", GrTexMipMapMode, 3) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  g_glide.tmu[tmu].mip_mode = Arg(c, 1);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grTexLodBiasValue@8", GrTexLodBiasValue, 2) {
  FlushBatch();
  uint32_t tmu = Arg(c, 0) & 1u;
  g_glide.tmu[tmu].lod_bias = Arg(c, 1);
  return 0;
}

HLE_NAMED("glide2x.dll", "_grLfbLock@24", GrLfbLock, 6) {
  uint32_t type = Arg(c, 0);
  // GrLfbInfo_t* at Arg(5)
  uint32_t info = Arg(c, 5);
  if (!info || !g_glide.lfb_guest_mem) return 0;
  FlushBatch();
  const auto& cfg = GetDisplayConfig();
  g_glide.lfb_locked_write = (type & 1u) != 0;
  Wr32(info + 4, uint32_t(g_glide.lfb_guest_mem));
  Wr32(info + 8, cfg.glide_width * 2);
  Wr32(info + 12, 0);  // GR_LFBWRITEMODE_565
  Wr32(info + 16, 0);  // UPPER_LEFT
  return 1;
}

HLE_NAMED("glide2x.dll", "_grLfbUnlock@8", GrLfbUnlock, 2) {
  if (!g_glide.lfb_locked_write || !g_glide.gfx || !g_glide.lfb_guest_mem) return 1;
  g_glide.lfb_locked_write = false;
  const auto& cfg = GetDisplayConfig();
  int gw = int(cfg.glide_width), gh = int(cfg.glide_height);
  if (!g_glide.lfb_tex) {
    g_glide.lfb_tex = g_glide.gfx->CreateTexture(gw, gh, 1, gfx::Format::A8R8G8B8, false);
  }
  const uint16_t* src = reinterpret_cast<const uint16_t*>(GPtr(g_glide.lfb_guest_mem));
  std::vector<uint32_t> argb(size_t(gw) * size_t(gh));
  for (size_t i = 0; i < argb.size(); i++) {
    uint16_t p = src[i];
    uint32_t r = ((p >> 11) & 0x1F) * 255u / 31u;
    uint32_t g = ((p >> 5) & 0x3F) * 255u / 63u;
    uint32_t b = (p & 0x1F) * 255u / 31u;
    argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
  }
  g_glide.gfx->UpdateTexture(g_glide.lfb_tex, 0, gfx::Rect{0, 0, gw, gh}, argb.data(),
                             uint32_t(gw * 4));

  if (!g_glide.in_scene) {
    g_glide.gfx->BeginScene();
    g_glide.in_scene = true;
  }
  gfx::FfState st = gfx::DefaultFfState();
  st.viewport = {0, 0, cfg.host_width, cfg.host_height, 0.0f, 1.0f};
  st.rs[7] = 0;     // ZENABLE = FALSE
  st.rs[14] = 0;    // ZWRITEENABLE = FALSE
  st.rs[22] = 1;    // CULL NONE
  st.rs[27] = 0;    // ALPHABLEND = FALSE
  st.rs[137] = 0;   // LIGHTING = FALSE
  st.rs[168] = 0xF;
  st.textures[0] = g_glide.lfb_tex;
  st.tss[0][1] = 2;  // SELECTARG1
  st.tss[0][2] = 2;  // TEXTURE
  st.tss[0][4] = 2;
  st.tss[0][5] = 2;

  float W = float(cfg.host_width), H = float(cfg.host_height);
  // Center 4:3 LFB loading screens / vignettes cleanly inside 16:9 widescreen host canvas!
  float target_w = std::min(W, H * (4.0f / 3.0f));
  float x0 = (W - target_w) * 0.5f - 0.5f;
  float x1 = (W + target_w) * 0.5f - 0.5f;
  float y0 = -0.5f, y1 = H - 0.5f;
  GpuVertex q[6] = {
      {x0, y0, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
      {x1, y0, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
      {x1, y1, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
      {x0, y0, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
      {x1, y1, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
      {x0, y1, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
  };
  gfx::FfDraw fd = gfx::FfInterleaved(4, 0x144u, q, sizeof(GpuVertex));
  fd.vertex_count = 6;
  g_glide.gfx->DrawFf(st, fd);
  return 1;
}

}  // namespace
}  // namespace recomp::hle
