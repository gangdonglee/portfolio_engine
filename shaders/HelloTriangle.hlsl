// HelloTriangle.hlsl
// Phong 조명(앰비언트 + 디퓨즈 + Blinn-Phong 스페큘러) + 알베도 텍스처 + 본 팔레트 스키닝.
// 정점 입력: POSITION + NORMAL + TEXCOORD + COLOR + BLENDINDICES + BLENDWEIGHT.
// 행렬은 row-major (DirectXMath 와 일치, CPU 측 transpose 정책: FBX 측 column-major 는 로딩 시 transpose).

cbuffer FrameConstants : register(b0)
{
    row_major float4x4 mvp;
    row_major float4x4 world;
    float3 cameraPosWS;   float _pad0;
    float3 lightDirWS;    float _pad1;   // 라이트가 빛을 쏘는 방향(표면→라이트 와 반대)
    float3 lightColor;    float _pad2;
    float3 ambient;       float _pad3;
};

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
    const float3 L = normalize(-lightDirWS);
    const float3 V = normalize(cameraPosWS - input.positionWS);
    const float3 H = normalize(L + V);

    const float  NdotL  = saturate(dot(N, L));
    const float  NdotH  = saturate(dot(N, H));
    const float  shininess = 32.0;

    const float3 albedo = g_albedo.Sample(g_sampler, input.uv).rgb * input.color;

    const float3 ambientLit = ambient    * albedo;
    const float3 diffuse    = NdotL      * lightColor * albedo;
    const float3 specular   = pow(NdotH, shininess) * lightColor * 0.5;

    return float4(ambientLit + diffuse + specular, 1.0);
}
