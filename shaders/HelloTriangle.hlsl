// HelloTriangle.hlsl
// Phong 조명(앰비언트 + 디퓨즈 + Blinn-Phong 스페큘러) 셰이딩.
// 정점 입력: POSITION + NORMAL + COLOR.
// 행렬은 row-major (DirectXMath 와 일치, CPU 측 transpose 불필요).

cbuffer FrameConstants : register(b0)
{
    row_major float4x4 mvp;
    row_major float4x4 world;
    float3 cameraPosWS;   float _pad0;
    float3 lightDirWS;    float _pad1;   // 라이트가 빛을 쏘는 방향(표면→라이트 와 반대)
    float3 lightColor;    float _pad2;
    float3 ambient;       float _pad3;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 color    : COLOR;
};

struct VSOutput
{
    float4 position   : SV_Position;
    float3 normalWS   : NORMAL;
    float3 positionWS : TEXCOORD0;  // world space 위치
    float3 color      : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    const float4 localPos = float4(input.position, 1.0);
    output.position   = mul(localPos, mvp);
    output.positionWS = mul(localPos, world).xyz;
    // uniform scale 가정 — 법선을 world 의 3x3 부분에 곱해도 됨.
    output.normalWS   = normalize(mul(input.normal, (float3x3)world));
    output.color      = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float3 N = normalize(input.normalWS);
    const float3 L = normalize(-lightDirWS);                       // 표면 → 라이트
    const float3 V = normalize(cameraPosWS - input.positionWS);    // 표면 → 카메라
    const float3 H = normalize(L + V);                              // 하프 벡터

    const float  NdotL  = saturate(dot(N, L));
    const float  NdotH  = saturate(dot(N, H));
    const float  shininess = 32.0;

    const float3 ambientLit = ambient    * input.color;
    const float3 diffuse    = NdotL      * lightColor * input.color;
    const float3 specular   = pow(NdotH, shininess) * lightColor * 0.5;

    return float4(ambientLit + diffuse + specular, 1.0);
}
