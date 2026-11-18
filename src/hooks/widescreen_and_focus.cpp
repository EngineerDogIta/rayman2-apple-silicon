// Widescreen (16:9 / 16:10 Hor+ FOV & HUD scaling) + Cmd+Tab focus-loss freeze fix
// Replaces exact verified Retail Rayman 2 hooks in manifests/rayman2.manifest:
//   - 0x4016E0 R2_fn_bAppliOnDeActivate -> return 0 (never freeze on Cmd+Tab)
//   - 0x401790 R2_fn_bAppliOnActivate   -> return 0
//   - 0x422AB0 GLI_xAdjustCameraToViewport2 -> Hor+ FOV equation from Ray2Fix/GliFixVf
//   - 0x429670 GLI_vDraw2DSpriteWithPercent -> 4:3 HUD aspect centering inside widescreen
//   - 0x422940 GLI_vGet3DVertexFromScreenPos -> handles 0x475C93 (HUD Code4 Pos) & 0x4711FB (proc 409)
//   - 0x446590 POS_fn_vSetTranslationVector  -> handles 0x475CD8 (HUD Code4 Size) & SPO 409/410
//   - 0x4710C0 R2_fn_p_stSPOSuperimpoed      -> fixes superimposed 3D HUD scale on exit

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "hle/glide2x_types.h"
#include "x86/lifted.h"
#include "x86/runtime.h"

namespace recomp::gen {

void f_004016E0_lifted(Ctx& c);
void f_00401790_lifted(Ctx& c);
void f_00422AB0_lifted(Ctx& c);
void f_00422940_lifted(Ctx& c);
void f_00446590_lifted(Ctx& c);
void f_00429670_lifted(Ctx& c);
void f_004710C0_lifted(Ctx& c);

namespace {

float ReadF32(uint32_t va) { return std::bit_cast<float>(Rd32(va)); }
void WriteF32(uint32_t va, float f) { Wr32(va, std::bit_cast<uint32_t>(f)); }

float ActualAspectRatio() {
  const auto& cfg = recomp::ray2::GetDisplayConfig();
  return float(cfg.host_height) / float(std::max<uint32_t>(1u, cfg.host_width));
}

float WidescreenHudScale() { return ActualAspectRatio() / 0.75f; }

thread_local uint32_t tl_active_spo_matrix = 0;

}  // namespace

// BOOL R2_fn_bAppliOnDeActivate(BOOL bIsFullScreen)
void f_004016E0(Ctx& c) {
  (void)f_004016E0_lifted;
  c.eax = 0;
  c.esp += 4;
}

// BOOL R2_fn_bAppliOnActivate(void)
void f_00401790(Ctx& c) {
  (void)f_00401790_lifted;
  c.eax = 0;
  c.esp += 4;
}

// void GLI_xAdjustCameraToViewport2(GLD_tdstDeviceAttributes* dev,
//                                   GLD_tdstViewportAttributes* vpt,
//                                   GLI_tdstCamera* cam)
void f_00422AB0(Ctx& c) {
  const auto& cfg = recomp::ray2::GetDisplayConfig();
  if (cfg.widescreen) {
    uint32_t p_stDev = Rd32(c.esp + 4);
    uint32_t p_stCam = Rd32(c.esp + 12);
    if (p_stDev && p_stCam) {
      // p_stDev: +0x08 xPixelDimensionX, +0x0C xPixelDimensionY
      float pix_w = float(cfg.host_width) * ReadF32(p_stDev + 0x08);
      float pix_h = float(cfg.host_height) * ReadF32(p_stDev + 0x0C);
      float vpt_ratio = (pix_w > 0.0f && pix_h > 0.0f) ? (pix_h / pix_w) : ActualAspectRatio();
      float used_ratio = (vpt_ratio != 0.0f) ? vpt_ratio : ReadF32(p_stCam + 0xD8);
      if (used_ratio > 0.0f) {
        // Hor+ FOV formula matching Ray2Fix/GliFixVf/fix.c:125
        float alpha_y = ReadF32(p_stCam + 0x68);
        double new_fov = 2.0 * std::atan((1.0 / double(used_ratio)) / (1.0 / 0.75) *
                                         std::tan((double(alpha_y) / 0.75) / 2.0));
        WriteF32(p_stCam + 0x64, float(new_fov));
      }
    }
  }
  f_00422AB0_lifted(c);
}

// void GLI_vDraw2DSpriteWithPercent(vpt, XMin, YMin, XMax, YMax, hMaterial)
void f_00429670(Ctx& c) {
  const auto& cfg = recomp::ray2::GetDisplayConfig();
  if (cfg.widescreen) {
    float xmin = ReadF32(c.esp + 8);
    float xmax = ReadF32(c.esp + 16);
    if (xmin != 0.0f || xmax != 100.0f) {
      float ratio = WidescreenHudScale();
      float center_offset_pct = (100.0f - 100.0f * ratio) * 0.5f;
      WriteF32(c.esp + 8, xmin * ratio + center_offset_pct);
      WriteF32(c.esp + 16, xmax * ratio + center_offset_pct);
    }
  }
  f_00429670_lifted(c);
}

// void GLI_vGet3DVertexFromScreenPos(p_stVpt, p_stPos3D, xRatioX, xRatioY, xSize)
void f_00422940(Ctx& c) {
  const auto& cfg = recomp::ray2::GetDisplayConfig();
  if (cfg.widescreen) {
    uint32_t ret = Rd32(c.esp);
    // 0x475C93 = FIX_Code4Patch_Pos inside 0x475680; 0x4711FB = proc 409 inside 0x4710C0
    if (ret == 0x00475C93u || ret == 0x004711FBu) {
      float x = ReadF32(c.esp + 12);
      float ratio = WidescreenHudScale();
      WriteF32(c.esp + 12, x * ratio + (1.0f - ratio) * 0.5f);
    }
  }
  f_00422940_lifted(c);
}

// void POS_fn_vSetTranslationVector(p_stMatrix, p_stTrs)
void f_00446590(Ctx& c) {
  uint32_t ret = Rd32(c.esp);
  uint32_t mat = Rd32(c.esp + 4);
  const auto& cfg = recomp::ray2::GetDisplayConfig();
  bool is_code4 = cfg.widescreen && (ret == 0x00475CD8u);
  if (cfg.widescreen && (ret == 0x00471209u || ret == 0x004712E4u)) {
    tl_active_spo_matrix = mat;
  }

  f_00446590_lifted(c);

  if (is_code4 && mat) {
    float ratio = WidescreenHudScale();
    float s = (float(cfg.glide_height) / 480.0f) * ratio;
    WriteF32(mat + 0x34, s);
    WriteF32(mat + 0x54, s);
  }
}

// AI_tdstNodeInterpret* R2_fn_p_stSPOSuperimpoed(p_SuperObjPerso, p_stTree)
void f_004710C0(Ctx& c) {
  tl_active_spo_matrix = 0;
  f_004710C0_lifted(c);
  const auto& cfg = recomp::ray2::GetDisplayConfig();
  if (cfg.widescreen && tl_active_spo_matrix) {
    float ratio = WidescreenHudScale();
    float s = (float(cfg.glide_height) / 480.0f) * ratio;
    WriteF32(tl_active_spo_matrix + 0x34, s);
    WriteF32(tl_active_spo_matrix + 0x54, s);
    tl_active_spo_matrix = 0;
  }
}

}  // namespace recomp::gen
