// HelloTriangle.hlsl
// Phong 조명(앰비언트 + 디퓨즈 + Blinn-Phong 스페큘러) + 알베도 텍스처 + 본 팔레트 스키닝
// + 다중 라이트 (Scene 의 가변 길이 dir/point 배열을 StructuredBuffer 로 받음).
// 정점 입력: POSITION + NORMAL + TEXCOORD + COLOR + BLENDINDICES + BLENDWEIGHT.
// 행렬은 row-major (DirectXMath 와 일치, CPU 측 transpose 정책: FBX 측 column-major 는 로딩 시 transpose).

cbuffer FrameConstants : register(b0)
{
    row_major float4x4 mvp;
    row_major float4x4 world;
    float3 cameraPosWS;     float _pad0;
    float3 ambient;         float _pad1;
    uint   dirLightCount;
    uint   pointLightCount;
    uint2  _pad2;
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
SamplerState g_sampler : register(s0);

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

float4 PSMain(VSOutput input) : SV_Target
{
    const float3 N = normalize(input.normalWS);
    const float3 V = normalize(cameraPosWS - input.positionWS);
    const float  shininess = 32.0;

    const float3 albedo = g_albedo.Sample(g_sampler, input.uv).rgb * input.color;

    float3 lit = ambient * albedo;

    // 방향광 — directionWS 가 "빛이 향하는 방향" 이므로 표면→라이트 = -directionWS.
    for (uint i = 0; i < dirLightCount; ++i)
    {
        const DirectionalLightGpu dl = g_dirLights[i];
        const float3 L      = normalize(-dl.directionWS);
        const float3 H      = normalize(L + V);
        const float  NdotL  = saturate(dot(N, L));
        const float  NdotH  = saturate(dot(N, H));
        const float3 lightC = dl.color * dl.intensity;
        lit += NdotL * lightC * albedo;
        lit += pow(NdotH, shininess) * lightC * 0.5;
    }

    // 점광 — range 외부는 falloff 0. 거리 기반 smooth attenuation (inverse-square 보다 안정).
    for (uint j = 0; j < pointLightCount; ++j)
    {
        const PointLightGpu pl = g_pointLights[j];
        const float3 toLight = pl.positionWS - input.positionWS;
        const float  dist    = length(toLight);
        if (dist > pl.range || pl.range <= 0.0) { continue; }
        const float3 L      = toLight / max(dist, 1e-4);
        const float3 H      = normalize(L + V);
        const float  NdotL  = saturate(dot(N, L));
        const float  NdotH  = saturate(dot(N, H));
        const float  k      = saturate(1.0 - dist / pl.range);
        const float  atten  = k * k;
        const float3 lightC = pl.color * pl.intensity * atten;
        lit += NdotL * lightC * albedo;
        lit += pow(NdotH, shininess) * lightC * 0.5;
    }

    return float4(lit, 1.0);
}
