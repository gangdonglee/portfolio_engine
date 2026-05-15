// HelloTriangle.hlsl
// 첫 삼각형용 VS + PS. 정점별 색상 패스스루.
// SM 5.0 (D3DCompileFromFile 호환). 향후 dxc 마이그레이션 시 SM 6.0+.

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
    output.position = float4(input.position, 1.0);
    output.color    = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
