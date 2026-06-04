// Skybox.hlsl — 풀스크린 삼각형으로 절차적 하늘 배경. 기하 뒤(빈 픽셀=far)만 채움.
//   뷰 레이를 invViewProj 로 복원해 SkyColor 샘플. HelloTriangle.hlsl 의 SkyColor 와 동일 식
//   (표면 반사와 배경이 일관되게).

cbuffer SkyConstants : register(b0)
{
    row_major float4x4 invViewProj;   // inverse(view*proj)
    float3 cameraPosWS;  float _p0;
    float3 sunDirWS;     float _p1;    // 태양을 향하는 방향(정규화)
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;   // clip-space xy [-1,1]
};

VSOut VSMain(uint id : SV_VertexID)
{
    // 풀스크린 삼각형: id 0→(0,0) 1→(2,0) 2→(0,2) → NDC (-1,-1)(3,-1)(-1,3).
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);   // z=1 (far) → 깊이 test LESS_EQUAL 로 빈 픽셀만 통과
    return o;
}

float3 SkyColor(float3 dir, float3 sunDir)
{
    const float3 zenith  = float3(0.16, 0.31, 0.55);
    const float3 horizon = float3(0.64, 0.72, 0.82);
    const float3 ground  = float3(0.28, 0.26, 0.24);
    float3 sky = (dir.y >= 0.0)
        ? lerp(horizon, zenith, pow(saturate(dir.y), 0.45))
        : lerp(horizon, ground, saturate(-dir.y * 2.5));
    const float sd = saturate(dot(dir, sunDir));
    sky += float3(1.0, 0.92, 0.72) * pow(sd, 280.0) * 5.0;
    sky += float3(1.0, 0.85, 0.65) * pow(sd, 8.0)   * 0.35;
    return sky;
}

float4 PSMain(VSOut input) : SV_Target
{
    // 화면 픽셀의 far-plane 점을 world 로 복원 → 카메라에서의 레이 방향.
    float4 farWorld = mul(float4(input.ndc, 1.0, 1.0), invViewProj);
    farWorld /= farWorld.w;
    const float3 rayDir = normalize(farWorld.xyz - cameraPosWS);

    float3 sky = SkyColor(rayDir, sunDirWS);
    // 메인 패스와 동일 노출(×3). 톤맵은 composite 로 이동 — 선형 HDR 출력.
    sky *= 3.0;
    return float4(sky, 1.0);
}
