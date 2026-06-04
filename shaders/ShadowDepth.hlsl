// ShadowDepth.hlsl — 그림자맵 깊이 패스. 라이트 시점에서 스키닝된 정점의 깊이만 기록.
//   b0: lightMVP = world * lightViewProj (인스턴스별). b1: 본 팔레트 (메인 패스와 동일).
//   PS 없음 (깊이 전용). 입력 레이아웃은 메인 패스와 동일(kHelloTriangleInputLayout).

cbuffer ShadowConstants : register(b0)
{
    row_major float4x4 lightMVP;   // world * lightViewProj
};

#define MAX_BONES 256
cbuffer BonePalette : register(b1)
{
    column_major float4x4 bones[MAX_BONES];
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float2 uv          : TEXCOORD;
    float3 color       : COLOR;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

float4 VSMain(VSInput input) : SV_Position
{
    const float weightSum = input.boneWeights.x + input.boneWeights.y +
                            input.boneWeights.z + input.boneWeights.w;

    float3 localPos = input.position;
    if (weightSum > 0.0001)
    {
        float4 skinned = float4(0, 0, 0, 0);
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float w = input.boneWeights[i];
            if (w > 0.0)
            {
                skinned += w * mul(float4(input.position, 1.0), bones[input.boneIndices[i]]);
            }
        }
        localPos = skinned.xyz;
    }
    return mul(float4(localPos, 1.0), lightMVP);
}
