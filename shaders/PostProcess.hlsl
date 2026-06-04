// PostProcess.hlsl — bloom. 풀스크린 패스들: BrightPass → Blur(H/V) → Composite(+tonemap).
//   메인/스카이박스 패스가 *노출만 적용한 선형 HDR* 을 HDR RT 에 출력 → 여기서 bloom + tonemap.

cbuffer PostConstants : register(b0)
{
    float2 texelSize;        // 1/소스폭, 1/소스높이
    float2 blurDir;          // (1,0)=수평, (0,1)=수직
    float  threshold;        // bright-pass 휘도 임계
    float  bloomIntensity;   // composite bloom 가중
    float2 _pad;
};

Texture2D    g_src   : register(t0);   // HDR scene 또는 blur 소스
Texture2D    g_bloom : register(t1);   // composite 의 bloom 텍스처
SamplerState g_samp  : register(s0);   // linear clamp

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut FullscreenVS(uint id : SV_VertexID)
{
    const float2 uv = float2((id << 1) & 2, id & 2);   // (0,0)(2,0)(0,2)
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

// 밝은 부분만 추출 (soft knee 없이 단순 threshold). 휘도 기준.
float4 BrightPassPS(VSOut i) : SV_Target
{
    const float3 c   = g_src.Sample(g_samp, i.uv).rgb;
    const float  lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float  k   = max(lum - threshold, 0.0) / max(lum, 1e-4);
    return float4(c * k, 1.0);
}

// 분리형 가우시안 (9-tap). blurDir 로 수평/수직.
static const float kW[5] = { 0.227027, 0.194594, 0.121622, 0.054054, 0.016216 };
float4 BlurPS(VSOut i) : SV_Target
{
    const float2 step = blurDir * texelSize;
    float3 c = g_src.Sample(g_samp, i.uv).rgb * kW[0];
    [unroll] for (int k = 1; k < 5; ++k)
    {
        c += g_src.Sample(g_samp, i.uv + step * k).rgb * kW[k];
        c += g_src.Sample(g_samp, i.uv - step * k).rgb * kW[k];
    }
    return float4(c, 1.0);
}

// HDR scene(t0) + bloom(t1) 합성 → Reinhard tonemap → LDR 백버퍼.
float4 CompositePS(VSOut i) : SV_Target
{
    const float3 hdr   = g_src.Sample(g_samp, i.uv).rgb;
    const float3 bloom = g_bloom.Sample(g_samp, i.uv).rgb;
    float3 c = hdr + bloom * bloomIntensity;
    c = c / (c + 1.0);   // Reinhard tonemap (노출은 메인 패스에서 이미 적용)
    return float4(c, 1.0);
}
