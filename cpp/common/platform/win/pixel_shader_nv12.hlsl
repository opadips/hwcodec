// NV12 -> BGRA for the D3D11 decode output. The colour matrix and range
// come from a constant buffer (see NativeDevice::SetNv12Color), so BT.601,
// BT.709 and BT.2020, limited or full range, all convert correctly.
//
// Regenerate pixel_shader_nv12.h after editing:
//   fxc /nologo /T ps_4_0_level_9_3 /E main /Vn g_PS_NV12 /Fh pixel_shader_nv12.h pixel_shader_nv12.hlsl

Texture2D<float> g_txY : register(t0);
Texture2D<float2> g_txUV : register(t1);
SamplerState g_Sam : register(s0);

cbuffer ColorCB : register(b0) {
  // x: luma offset, y: luma scale, z: chroma scale
  float4 g_Range;
  // x: R from Cr, y: G from Cb, z: G from Cr, w: B from Cb
  float4 g_Coeffs;
};

struct PS_INPUT {
  float4 Pos : SV_POSITION;
  float2 Tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_TARGET {
  float y = (g_txY.Sample(g_Sam, input.Tex) - g_Range.x) * g_Range.y;
  float2 c = (g_txUV.Sample(g_Sam, input.Tex) - 0.5) * g_Range.z;
  float3 rgb = float3(y + g_Coeffs.x * c.y,
                      y - g_Coeffs.y * c.x - g_Coeffs.z * c.y,
                      y + g_Coeffs.w * c.x);
  return float4(saturate(rgb), 1.0);
}
