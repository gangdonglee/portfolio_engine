// DebugLines.hlsl
// 디버그 라인 (좌표축 등) — vertex color passthrough.
// 입력: position(float3) + color(float3). row-major view-projection 매트릭스.

cbuffer DebugFrame : register(b0)
{
    row_major float4x4 viewProj;
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
    VSOutput o;
    o.position = mul(float4(input.position, 1.0), viewProj);
    o.color    = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
