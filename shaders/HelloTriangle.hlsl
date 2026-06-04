// HelloTriangle.hlsl
// Phong 조명(앰비언트 + 디퓨즈 + Blinn-Phong 스페큘러) + 알베도 텍스처 + 본 팔레트 스키닝
// + 다중 라이트 (Scene 의 가변 길이 dir/point 배열을 StructuredBuffer 로 받음).
// 정점 입력: POSITION + NORMAL + TEXCOORD + COLOR + BLENDINDICES + BLENDWEIGHT.
// 행렬은 row-major (DirectXMath 와 일치, CPU 측 transpose 정책: FBX 측 column-major 는 로딩 시 transpose).

cbuffer FrameConstants : register(b0)
{
    row_major float4x4 mvp;
    row_major float4x4 world;
    row_major float4x4 lightViewProj;          // 그림자 — 라이트 view-proj (방향광)
    float3 cameraPosWS;     float roughness;   // PBR — 표면 거칠기 0..1
    float3 ambient;         float metallic;    // PBR — 금속성 0..1
    uint   dirLightCount;
    uint   pointLightCount;
    uint   shadowEnabled;                      // 1=그림자 샘플, 0=항상 lit
    uint   normalFlipY;                        // 1=normal map Y(녹색) 반전 (OpenGL↔DirectX)
    float  normalStrength;                     // normal map 섭동 세기 (오브젝트별)
    uint   applyTonemap;                       // 1=셰이더에서 Reinhard(에디터/no-bloom), 0=선형HDR(게임)
    uint2  _pad;
};

// 가변 길이 라이트 — Scene 의 std::vector 가 그대로 GPU 로 올라옴. 캡 없음.
// CPU 측 stride 일치 필수 (DirectionalLightGpu / PointLightGpu 구조체).
struct DirectionalLightGpu
{
    float3 directionWS;   float _pad0;
    float3 color;         float intensity;
};
struct PointLightGpu
{
    float3 positionWS;    float _pad0;
    float3 color;         float intensity;
    float  range;         float3 _pad1;
};
StructuredBuffer<DirectionalLightGpu> g_dirLights   : register(t1);
StructuredBuffer<PointLightGpu>       g_pointLights : register(t2);

// 본 팔레트 — bone[i] = animatedGlobal[i] * inverseBindPose[i] (Animator 가 매 프레임 계산).
// 최대 256 본 — Dragon.fbx 는 182 본이라 128 로는 인덱스 OOB → 정점이 가비지 행렬과 곱해져 폭발.
// cbuffer 크기 256*64 = 16384 bytes (D3D12 cbuffer 64KB 한계 내).
#define MAX_BONES 256
cbuffer BonePalette : register(b1)
{
    column_major float4x4 bones[MAX_BONES];
};

Texture2D    g_albedo  : register(t0);
Texture2D    g_normal  : register(t3);   // tangent-space normal map (없으면 평탄 노멀 폴백)
Texture2D    g_shadowMap : register(t4); // 라이트 시점 깊이 (R32_FLOAT)
SamplerState g_sampler : register(s0);
SamplerComparisonState g_shadowSampler : register(s1);   // PCF comparison

// 그림자 인자 — 표면이 라이트 시점에서 가려졌으면 0, 보이면 1 (PCF 3x3 으로 0..1 부드럽게).
static const float kShadowMapSize = 2048.0;
float ShadowFactor(float3 posWS)
{
    if (shadowEnabled == 0) { return 1.0; }
    float4 lp = mul(float4(posWS, 1.0), lightViewProj);
    lp.xyz /= lp.w;
    float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;   // NDC → UV (y flip)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lp.z > 1.0) { return 1.0; }
    const float depth = lp.z;
    const float texel = 1.0 / kShadowMapSize;
    float sum = 0.0;
    [unroll] for (int x = -1; x <= 1; ++x)
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        sum += g_shadowMap.SampleCmpLevelZero(g_shadowSampler, uv + float2(x, y) * texel, depth);
    }
    return sum / 9.0;
}

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float2 uv          : TEXCOORD;
    float3 color       : COLOR;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct VSOutput
{
    float4 position   : SV_Position;
    float3 normalWS   : NORMAL;
    float2 uv         : TEXCOORD0;
    float3 positionWS : TEXCOORD1;
    float3 color      : COLOR;
};

VSOutput VSMain(VSInput input)
{
    // 스키닝 — weight 합 > epsilon 이면 본 팔레트 변환, 아니면 정점 그대로 통과.
    const float weightSum = input.boneWeights.x + input.boneWeights.y + input.boneWeights.z + input.boneWeights.w;

    float3 localPos    = input.position;
    float3 localNormal = input.normal;
    if (weightSum > 0.0001)
    {
        float4 skinnedPos    = float4(0, 0, 0, 0);
        float3 skinnedNormal = float3(0, 0, 0);
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float w = input.boneWeights[i];
            if (w > 0.0)
            {
                const uint b = input.boneIndices[i];
                skinnedPos    += w *  mul(float4(input.position, 1.0), bones[b]);
                skinnedNormal += w * (mul(float4(input.normal,   0.0), bones[b])).xyz;
            }
        }
        localPos    = skinnedPos.xyz;
        localNormal = skinnedNormal;
    }

    VSOutput output;
    const float4 localPos4 = float4(localPos, 1.0);
    output.position   = mul(localPos4, mvp);
    output.positionWS = mul(localPos4, world).xyz;
    output.normalWS   = normalize(mul(localNormal, (float3x3)world));
    output.uv         = input.uv;
    output.color      = input.color;
    return output;
}

// === PBR (Cook-Torrance, metallic/roughness workflow) ===
static const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz 법선 분포 — 거칠기에 따른 마이크로패싯 정렬.
float DistributionGGX(float NdotH, float rough)
{
    const float a  = rough * rough;
    const float a2 = a * a;
    const float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}
// Smith 기하 감쇠 (Schlick-GGX, direct light k).
float GeometrySchlickGGX(float NdotX, float rough)
{
    const float r = rough + 1.0;
    const float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}
float GeometrySmith(float NdotV, float NdotL, float rough)
{
    return GeometrySchlickGGX(NdotV, rough) * GeometrySchlickGGX(NdotL, rough);
}
// Fresnel-Schlick — 입사각에 따른 반사율.
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}
// roughness 인지 Fresnel — IBL ambient specular 용 (거칠면 grazing 반사 약화).
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float rough)
{
    const float3 Fr = max((1.0 - rough).xxx, F0);
    return F0 + (Fr - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// === 절차적 하늘 (IBL 환경) — dir 방향의 하늘색. 지평선→천정 그라데이션 + 태양. ===
//   스카이박스 배경과 표면 반사가 *같은 함수* 를 공유 → 반사가 배경과 일관됨.
float3 SkyColor(float3 dir, float3 sunDir)
{
    const float3 zenith  = float3(0.16, 0.31, 0.55);
    const float3 horizon = float3(0.64, 0.72, 0.82);
    const float3 ground  = float3(0.28, 0.26, 0.24);
    float3 sky = (dir.y >= 0.0)
        ? lerp(horizon, zenith, pow(saturate(dir.y), 0.45))
        : lerp(horizon, ground, saturate(-dir.y * 2.5));
    const float sd = saturate(dot(dir, sunDir));
    sky += float3(1.0, 0.92, 0.72) * pow(sd, 280.0) * 5.0;   // 태양 디스크
    sky += float3(1.0, 0.85, 0.65) * pow(sd, 8.0)   * 0.35;  // 주변 글로우
    return sky;
}

// 단일 광원의 Cook-Torrance 기여.
float3 PbrLight(float3 N, float3 V, float3 L, float3 radiance,
                float3 albedo, float metal, float rough)
{
    const float3 H     = normalize(V + L);
    const float  NdotV = saturate(dot(N, V)) + 1e-4;
    const float  NdotL = saturate(dot(N, L));
    const float  NdotH = saturate(dot(N, H));
    const float  HdotV = saturate(dot(H, V));

    const float3 F0  = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    const float  D   = DistributionGGX(NdotH, rough);
    const float  G   = GeometrySmith(NdotV, NdotL, rough);
    const float3 F   = FresnelSchlick(HdotV, F0);

    const float3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    const float3 kd   = (1.0 - F) * (1.0 - metal);   // 금속은 diffuse 없음
    return (kd * albedo / PI + spec) * radiance * NdotL;
}

// 정점 tangent 없이 *화면공간 미분(ddx/ddy)* 으로 cotangent frame 구성 (C. Schüler 기법).
//   p = world position, uv = 텍스처 좌표. 반환 행렬의 row 0/1/2 = T/B/N.
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    const float3 dp1 = ddx(p);
    const float3 dp2 = ddy(p);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, N);
    const float3 dp1perp = cross(N, dp1);
    const float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    const float  invmax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invmax, B * invmax, N);
}
// normal map 으로 N 섭동. map 이 평탄(0,0,1)이면 N 그대로 반환 → 폴백 안전.
//   xy 를 strength 배 — subtle 한 맵의 디테일을 가시화 (1=원본, 2~3=강조).
float3 PerturbNormal(float3 N, float3 p, float2 uv)
{
    float3 map = g_normal.Sample(g_sampler, uv).xyz * 2.0 - 1.0;
    if (normalFlipY != 0) { map.y = -map.y; }      // OpenGL↔DirectX 녹색 채널 규약
    map.xy *= normalStrength;                       // 오브젝트별 세기 (0=평탄 → 폴백)
    const float3x3 TBN = CotangentFrame(N, p, uv);
    return normalize(mul(map, TBN));
}

float4 PSMain(VSOutput input) : SV_Target
{
    float3 N = normalize(input.normalWS);
    N = PerturbNormal(N, input.positionWS, input.uv);   // normal map (없으면 무영향)
    const float3 V = normalize(cameraPosWS - input.positionWS);

    const float3 albedo = g_albedo.Sample(g_sampler, input.uv).rgb * input.color;
    const float  rough  = saturate(roughness);
    const float  metal  = saturate(metallic);

    // === IBL (절차적 하늘) — diffuse irradiance + specular reflection ===
    //   diffuse: 법선 방향 하늘색을 반구 irradiance 근사. specular: 반사 방향 하늘(거칠수록 N 쪽으로
    //   흐림 — mip 흉내) × roughness-Fresnel. 금속(metal=1)은 diffuse 0 → 환경 반사만(albedo tint).
    float3 sunDir = float3(0.0, 1.0, 0.0);
    if (dirLightCount > 0) { sunDir = normalize(-g_dirLights[0].directionWS); }
    const float  NdotVamb  = saturate(dot(N, V));
    const float3 F0amb     = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    const float3 Famb      = FresnelSchlickRoughness(NdotVamb, F0amb, rough);
    const float3 kdAmb     = (1.0 - Famb) * (1.0 - metal);
    const float3 irradiance = SkyColor(N, sunDir);
    const float3 reflDir    = normalize(lerp(reflect(-V, N), N, rough));
    const float3 prefiltered = SkyColor(reflDir, sunDir);
    float3 Lo = (kdAmb * irradiance * albedo) * 0.55     // diffuse 환경광
              + prefiltered * Famb                       // specular 환경 반사
              + ambient * albedo;                        // 씬 ambient floor (어두운 곳 바닥값)

    // 방향광 — directionWS 가 "빛이 향하는 방향" 이므로 표면→라이트 = -directionWS.
    //   방향광만 그림자맵 캐스팅 (태양). 그림자 인자를 직접광 기여에 곱함.
    const float shadow = ShadowFactor(input.positionWS);
    for (uint i = 0; i < dirLightCount; ++i)
    {
        const DirectionalLightGpu dl = g_dirLights[i];
        Lo += shadow * PbrLight(N, V, normalize(-dl.directionWS), dl.color * dl.intensity,
                                albedo, metal, rough);
    }

    // 점광 — range 외부 falloff 0. 거리 기반 smooth attenuation.
    for (uint j = 0; j < pointLightCount; ++j)
    {
        const PointLightGpu pl = g_pointLights[j];
        const float3 toLight = pl.positionWS - input.positionWS;
        const float  dist    = length(toLight);
        if (dist > pl.range || pl.range <= 0.0) { continue; }
        const float3 L     = toLight / max(dist, 1e-4);
        const float  k     = saturate(1.0 - dist / pl.range);
        Lo += PbrLight(N, V, L, pl.color * pl.intensity * (k * k), albedo, metal, rough);
    }

    // 노출 보정 — diffuse /PI 에너지보존으로 어두워진 만큼 끌어올림(라이트가 LDR 튜닝이라).
    //   게임: 선형 HDR 출력(톤맵은 bloom composite 패스). 에디터(no-bloom): 셰이더에서 Reinhard.
    Lo *= 3.0;
    if (applyTonemap != 0) { Lo = Lo / (Lo + 1.0); }
    return float4(Lo, 1.0);
}
