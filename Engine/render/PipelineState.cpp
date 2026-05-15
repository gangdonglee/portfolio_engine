#include "render/PipelineState.h"

#include "core/HrCheck.h"

#include "render/Device.h"
#include "render/RootSignature.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {

        // HelloTriangle 정점 레이아웃: POSITION(float3) + COLOR(float3). 인터리브.
        constexpr D3D12_INPUT_ELEMENT_DESC kHelloTriangleInputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC DefaultRasterizer() noexcept
        {
            D3D12_RASTERIZER_DESC rs{};
            rs.FillMode              = D3D12_FILL_MODE_SOLID;
            rs.CullMode              = D3D12_CULL_MODE_BACK;
            rs.FrontCounterClockwise = FALSE;
            rs.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
            rs.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            rs.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            rs.DepthClipEnable       = TRUE;
            rs.MultisampleEnable     = FALSE;
            rs.AntialiasedLineEnable = FALSE;
            rs.ForcedSampleCount     = 0;
            rs.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            return rs;
        }

        D3D12_BLEND_DESC DefaultBlend() noexcept
        {
            D3D12_BLEND_DESC bs{};
            bs.AlphaToCoverageEnable  = FALSE;
            bs.IndependentBlendEnable = FALSE;
            constexpr D3D12_RENDER_TARGET_BLEND_DESC rtbDefault = {
                FALSE,                          // BlendEnable
                FALSE,                          // LogicOpEnable
                D3D12_BLEND_ONE,                // SrcBlend
                D3D12_BLEND_ZERO,                // DestBlend
                D3D12_BLEND_OP_ADD,              // BlendOp
                D3D12_BLEND_ONE,                 // SrcBlendAlpha
                D3D12_BLEND_ZERO,                // DestBlendAlpha
                D3D12_BLEND_OP_ADD,              // BlendOpAlpha
                D3D12_LOGIC_OP_NOOP,             // LogicOp
                D3D12_COLOR_WRITE_ENABLE_ALL,    // RenderTargetWriteMask
            };
            for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            {
                bs.RenderTarget[i] = rtbDefault;
            }
            return bs;
        }

        D3D12_DEPTH_STENCIL_DESC DepthStencilDisabled() noexcept
        {
            D3D12_DEPTH_STENCIL_DESC dss{};
            dss.DepthEnable      = FALSE;
            dss.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ZERO;
            dss.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
            dss.StencilEnable    = FALSE;
            dss.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
            dss.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
            const D3D12_DEPTH_STENCILOP_DESC noOp = {
                D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS
            };
            dss.FrontFace = noOp;
            dss.BackFace  = noOp;
            return dss;
        }
    }

    PipelineState::PipelineState(Device& device, const Desc& desc)
    {
        if (desc.vertexShader == nullptr || desc.pixelShader == nullptr || desc.rootSignature == nullptr)
        {
            throw std::runtime_error("PipelineState::Desc: vertexShader/pixelShader/rootSignature 가 nullptr");
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = desc.rootSignature->Native();

        psoDesc.VS.pShaderBytecode = desc.vertexShader->GetBufferPointer();
        psoDesc.VS.BytecodeLength  = desc.vertexShader->GetBufferSize();
        psoDesc.PS.pShaderBytecode = desc.pixelShader->GetBufferPointer();
        psoDesc.PS.BytecodeLength  = desc.pixelShader->GetBufferSize();

        psoDesc.InputLayout.pInputElementDescs = kHelloTriangleInputLayout;
        psoDesc.InputLayout.NumElements        = static_cast<UINT>(std::size(kHelloTriangleInputLayout));

        psoDesc.RasterizerState   = DefaultRasterizer();
        psoDesc.BlendState        = DefaultBlend();
        psoDesc.DepthStencilState = DepthStencilDisabled();
        psoDesc.DSVFormat         = DXGI_FORMAT_UNKNOWN;  // 깊이 버퍼 미사용

        psoDesc.SampleMask            = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets      = 1;
        psoDesc.RTVFormats[0]         = desc.rtvFormat;
        psoDesc.SampleDesc.Count      = 1;
        psoDesc.SampleDesc.Quality    = 0;
        psoDesc.NodeMask              = 0;

        ThrowIfFailed(
            device.Native()->CreateGraphicsPipelineState(
                &psoDesc,
                IID_PPV_ARGS(m_pso.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState");

        ::OutputDebugStringW(L"[render] PipelineState (Graphics, HelloTriangle layout) created\n");
    }

    PipelineState::~PipelineState() = default;

    ID3D12PipelineState* PipelineState::Native() const noexcept
    {
        return m_pso.Get();
    }
}
