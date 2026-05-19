#include "render/DebugRenderer.h"

#include "core/HrCheck.h"
#include "core/Logger.h"
#include "render/ConstantBuffer.h"
#include "render/Device.h"
#include "render/ShaderCompiler.h"
#include "render/VertexBuffer.h"

#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {
        struct DebugVertex
        {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT3 color;
        };
        static_assert(sizeof(DebugVertex) == 24, "DebugVertex stride 깨짐");

        constexpr D3D12_INPUT_ELEMENT_DESC kDebugLineInputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // 좌표축 6 정점 — 3 라인. X=빨강, Y=초록, Z=파랑.
        std::array<DebugVertex, 6> BuildAxes(float len)
        {
            return {{
                { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.2f, 0.2f } },   // X 시작
                { {  len, 0.0f, 0.0f }, { 1.0f, 0.2f, 0.2f } },   // X 끝
                { { 0.0f, 0.0f, 0.0f }, { 0.2f, 1.0f, 0.2f } },   // Y 시작
                { { 0.0f,  len, 0.0f }, { 0.2f, 1.0f, 0.2f } },   // Y 끝
                { { 0.0f, 0.0f, 0.0f }, { 0.3f, 0.5f, 1.0f } },   // Z 시작
                { { 0.0f, 0.0f,  len }, { 0.3f, 0.5f, 1.0f } },   // Z 끝
            }};
        }
    }

    DebugRenderer::DebugRenderer(Device& device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
    {
        // === 1) Root Signature — b0 CBV (VS 가시), IA layout 허용. ===
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = 0;
        param.Descriptor.RegisterSpace  = 0;
        param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters   = &param;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> rsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> rsErrors;
        const HRESULT serializeHr = ::D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            rsBlob.GetAddressOf(), rsErrors.GetAddressOf());
        if (FAILED(serializeHr))
        {
            char buf[512];
            const char* err = (rsErrors && rsErrors->GetBufferSize() > 0)
                ? static_cast<const char*>(rsErrors->GetBufferPointer()) : "no error blob";
            std::snprintf(buf, sizeof(buf), "DebugRenderer: D3D12SerializeRootSignature failed: %s", err);
            throw std::runtime_error(buf);
        }
        ThrowIfFailed(
            device.Native()->CreateRootSignature(
                0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                IID_PPV_ARGS(m_rootSig.ReleaseAndGetAddressOf())),
            "DebugRenderer::CreateRootSignature");

        // === 2) 셰이더 컴파일. ===
        const std::wstring shaderDir = ShaderCompiler::DefaultShaderDir();
        const std::wstring shaderPath = shaderDir + L"DebugLines.hlsl";
        const auto vsBlob = ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "VSMain", ShaderCompiler::Stage::Vertex);
        const auto psBlob = ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "PSMain", ShaderCompiler::Stage::Pixel);

        // === 3) PSO — LineList topology + depth/blend OFF + back face cull OFF. ===
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        psoDesc.InputLayout.pInputElementDescs = kDebugLineInputLayout;
        psoDesc.InputLayout.NumElements        = static_cast<UINT>(std::size(kDebugLineInputLayout));

        // Rasterizer — 라인은 cull 의미 없음.
        psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias             = 0;
        psoDesc.RasterizerState.DepthBiasClamp        = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias  = 0.0f;
        psoDesc.RasterizerState.DepthClipEnable       = TRUE;
        psoDesc.RasterizerState.MultisampleEnable     = FALSE;
        psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;

        // Blend — 디폴트 (off).
        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        for (auto& rt : psoDesc.BlendState.RenderTarget)
        {
            rt.BlendEnable           = FALSE;
            rt.LogicOpEnable         = FALSE;
            rt.SrcBlend              = D3D12_BLEND_ONE;
            rt.DestBlend             = D3D12_BLEND_ZERO;
            rt.BlendOp               = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
            rt.DestBlendAlpha        = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            rt.LogicOp               = D3D12_LOGIC_OP_NOOP;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        // Depth — OFF (디버그 라인이 메시 뒤에 있어도 가시).
        psoDesc.DepthStencilState.DepthEnable   = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask             = UINT_MAX;
        psoDesc.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        psoDesc.NumRenderTargets       = 1;
        psoDesc.RTVFormats[0]          = rtvFormat;
        psoDesc.DSVFormat              = dsvFormat;   // depth-off 라도 bind 된 DSV format 일관 필요
        psoDesc.SampleDesc.Count       = 1;
        psoDesc.SampleDesc.Quality     = 0;

        ThrowIfFailed(
            device.Native()->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(m_pso.ReleaseAndGetAddressOf())),
            "DebugRenderer::CreateGraphicsPipelineState");

        // === 4) VertexBuffer + ConstantBuffer 슬롯. ===
        // 초기 길이 100 — 첫 DrawAxes 가 다른 길이면 재업로드 (m_currentAxisLength 추적).
        constexpr float kInitialLen = 100.0f;
        const auto initialVerts = BuildAxes(kInitialLen);
        m_axesVB = std::make_unique<VertexBuffer>(
            device,
            initialVerts.data(),
            static_cast<std::uint32_t>(initialVerts.size() * sizeof(DebugVertex)),
            static_cast<std::uint32_t>(sizeof(DebugVertex)));
        m_currentAxisLength = kInitialLen;

        for (engine::uint32 f = 0; f < kFrameCount; ++f)
        {
            m_cbs[f] = std::make_unique<ConstantBuffer>(
                device, static_cast<engine::uint32>(sizeof(DirectX::XMFLOAT4X4)));
        }

        engine::core::LogInfo(L"[debug] DebugRenderer ready (axes LineList, depth-off)\n");
    }

    DebugRenderer::~DebugRenderer() = default;

    void DebugRenderer::DrawAxes(ID3D12GraphicsCommandList*       list,
                                 engine::uint32                   frameIndex,
                                 const DirectX::XMMATRIX&         viewProj,
                                 float                            axisLength)
    {
        if (list == nullptr || frameIndex >= kFrameCount) { return; }

        // 축 길이 변경 — VertexBuffer *재할당* (UPLOAD heap 인스턴스 새로 — 비싸지만 디버그용).
        // 자주 바뀌면 UpdateRange 패턴으로 격상. 현재는 길이 상수 가정.
        if (axisLength != m_currentAxisLength)
        {
            const auto verts = BuildAxes(axisLength);
            // 기존 VB 의 Device 참조 필요 — 클래스가 Device& 보유 안 함. 단순화: 길이 변경 시
            // 경고 로그 + skip (호출자가 길이 일정 유지).
            engine::core::LogInfoA("[debug] DebugRenderer: axisLength 변경 무시 (ctor 길이 고정)\n");
        }

        // ConstantBuffer 업데이트.
        DirectX::XMFLOAT4X4 vpStored;
        DirectX::XMStoreFloat4x4(&vpStored, viewProj);
        m_cbs[frameIndex]->Update(&vpStored, static_cast<engine::uint32>(sizeof(vpStored)));

        // 명령 기록 — 호출 측이 이미 RTV/DSV/Viewport/Scissor 셋업.
        list->SetGraphicsRootSignature(m_rootSig.Get());
        list->SetPipelineState(m_pso.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

        m_axesVB->Bind(list, 0);
        list->SetGraphicsRootConstantBufferView(0, m_cbs[frameIndex]->GpuAddress());

        list->DrawInstanced(6, 1, 0, 0);   // 6 정점 = 3 라인
    }
}
