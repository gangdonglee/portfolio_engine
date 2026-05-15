// HelloTriangle.hlsl — Phase 2 후 회전 큐브용.
// b0 의 cbuffer 에서 MVP 행렬을 받아 정점 좌표를 변환. PS 는 정점 색을 그대로 출력.
//
// row_major 명시로 CPU 측 transpose 불필요 (DirectXMath 의 XMMATRIX 자체가 row-major).

cbuffer FrameConstants : register(b0)
{
    row_major float4x4 mvp;
};

struct VSInput
{
    float3 position : POSITION;
    float3 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), mvp);
    output.color    = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
