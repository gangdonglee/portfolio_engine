#include "render/RootSignature.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    RootSignature::RootSignature(Device& device, const Desc& desc)
    {
        // 루트 파라미터 구성: 최대 7개 (b0 + b1 + t0 table + t1 + t2 + t3 table[normal] + t4 table[shadow]).
        D3D12_ROOT_PARAMETER  params[7]{};
        UINT                  paramCount = 0;

        // [0] b0 CBV root descriptor
        if (desc.cbvAtB0 != Desc::CbvB0::None)
        {
            params[paramCount].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[paramCount].Descriptor.ShaderRegister = 0;
            params[paramCount].Descriptor.RegisterSpace  = 0;
            params[paramCount].ShaderVisibility =
                (desc.cbvAtB0 == Desc::CbvB0::Vertex)
                    ? D3D12_SHADER_VISIBILITY_VERTEX
                    : D3D12_SHADER_VISIBILITY_ALL;
            ++paramCount;
        }

        // [1] b1 CBV root descriptor (VS 가시) — 본 팔레트
        if (desc.cbvB1Vertex)
        {
            params[paramCount].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[paramCount].Descriptor.ShaderRegister = 1;   // b1
            params[paramCount].Descriptor.RegisterSpace  = 0;
            params[paramCount].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
            ++paramCount;
        }

        // [2] t0 SRV descriptor table (PS visibility)
        // srvRange 는 D3D12SerializeRootSignature 호출 시점까지 살아 있어야 한다 (params 가
        // 주소 포인터로 참조). 본 함수 스코프 끝까지 유효하므로 if 블록 안에 두어도 안전.
        D3D12_DESCRIPTOR_RANGE srvRange{};
        if (desc.srvT0Pixel)
        {
            srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange.NumDescriptors     = 1;
            srvRange.BaseShaderRegister = 0;   // t0
            srvRange.RegisterSpace      = 0;
            srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
            params[paramCount].DescriptorTable.pDescriptorRanges   = &srvRange;
            params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;
        }

        // [3] t1 SRV root descriptor (PS 가시) — StructuredBuffer 가상주소 직접 바인딩.
        if (desc.srvT1Pixel)
        {
            params[paramCount].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[paramCount].Descriptor.ShaderRegister = 1;   // t1
            params[paramCount].Descriptor.RegisterSpace  = 0;
            params[paramCount].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;
        }

        // [4] t2 SRV root descriptor (PS 가시) — 두 번째 StructuredBuffer.
        if (desc.srvT2Pixel)
        {
            params[paramCount].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[paramCount].Descriptor.ShaderRegister = 2;   // t2
            params[paramCount].Descriptor.RegisterSpace  = 0;
            params[paramCount].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;
        }

        // [5] t3 SRV descriptor table (PS visibility) — normal map. srvRangeN 도 serialize 까지 유효.
        D3D12_DESCRIPTOR_RANGE srvRangeN{};
        if (desc.srvT3Pixel)
        {
            srvRangeN.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRangeN.NumDescriptors     = 1;
            srvRangeN.BaseShaderRegister = 3;   // t3
            srvRangeN.RegisterSpace      = 0;
            srvRangeN.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
            params[paramCount].DescriptorTable.pDescriptorRanges   = &srvRangeN;
            params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;
        }

        // [6] t4 SRV descriptor table (PS visibility) — 그림자맵. srvRangeS 도 serialize 까지 유효.
        D3D12_DESCRIPTOR_RANGE srvRangeS{};
        if (desc.srvT4Pixel)
        {
            srvRangeS.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRangeS.NumDescriptors     = 1;
            srvRangeS.BaseShaderRegister = 4;   // t4
            srvRangeS.RegisterSpace      = 0;
            srvRangeS.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
            params[paramCount].DescriptorTable.pDescriptorRanges   = &srvRangeS;
            params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;
        }

        // [post] 포스트프로세싱 프리셋 — b0 CBV(PS) + t0 table + t1 table. (다른 플래그와 배타)
        D3D12_DESCRIPTOR_RANGE ppT0{}, ppT1{};
        if (desc.postProcess)
        {
            params[paramCount].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[paramCount].Descriptor.ShaderRegister = 0;   // b0
            params[paramCount].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;

            ppT0.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ppT0.NumDescriptors     = 1; ppT0.BaseShaderRegister = 0;   // t0
            ppT0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
            params[paramCount].DescriptorTable.pDescriptorRanges   = &ppT0;
            params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;

            ppT1.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ppT1.NumDescriptors     = 1; ppT1.BaseShaderRegister = 1;   // t1
            ppT1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
            params[paramCount].DescriptorTable.pDescriptorRanges   = &ppT1;
            params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
            ++paramCount;
        }

        // Static samplers — s0 linear/wrap (albedo/normal), s1 comparison (그림자 PCF).
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        UINT samplerCount = 0;
        if (desc.postProcess)
        {
            samplers[samplerCount].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[samplerCount].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[samplerCount].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[samplerCount].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[samplerCount].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[samplerCount].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            samplers[samplerCount].MaxLOD           = D3D12_FLOAT32_MAX;
            samplers[samplerCount].ShaderRegister   = 0;   // s0
            samplers[samplerCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            ++samplerCount;
        }
        if (desc.srvT0Pixel)
        {
            samplers[samplerCount].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[samplerCount].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[samplerCount].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[samplerCount].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[samplerCount].MipLODBias       = 0.0f;
            samplers[samplerCount].MaxAnisotropy    = 1;
            samplers[samplerCount].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[samplerCount].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            samplers[samplerCount].MinLOD           = 0.0f;
            samplers[samplerCount].MaxLOD           = D3D12_FLOAT32_MAX;
            samplers[samplerCount].ShaderRegister   = 0;   // s0
            samplers[samplerCount].RegisterSpace    = 0;
            samplers[samplerCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            ++samplerCount;
        }
        if (desc.srvT4Pixel)
        {
            // comparison sampler — PCF. 경계 밖은 흰색(=1.0=lit) border 로 그림자 누수 방지.
            samplers[samplerCount].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            samplers[samplerCount].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            samplers[samplerCount].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            samplers[samplerCount].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            samplers[samplerCount].MipLODBias       = 0.0f;
            samplers[samplerCount].MaxAnisotropy    = 1;
            samplers[samplerCount].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            samplers[samplerCount].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            samplers[samplerCount].MinLOD           = 0.0f;
            samplers[samplerCount].MaxLOD           = D3D12_FLOAT32_MAX;
            samplers[samplerCount].ShaderRegister   = 1;   // s1
            samplers[samplerCount].RegisterSpace    = 0;
            samplers[samplerCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            ++samplerCount;
        }

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = paramCount;
        rsDesc.pParameters       = paramCount > 0 ? params : nullptr;
        rsDesc.NumStaticSamplers = samplerCount;
        rsDesc.pStaticSamplers   = samplerCount > 0 ? samplers : nullptr;
        // IA 단계의 input layout 사용 허용. 비어있는 RS 라도 IA 가 필요하면 이 플래그 필수.
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        const HRESULT serializeHr = ::D3D12SerializeRootSignature(
            &rsDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(serializeHr))
        {
            char buf[512];
            if (errors && errors->GetBufferSize() > 0)
            {
                const char* errMsg = static_cast<const char*>(errors->GetBufferPointer());
                std::snprintf(buf, sizeof(buf),
                              "D3D12SerializeRootSignature failed (hr=0x%08lX): %.*s",
                              static_cast<unsigned long>(serializeHr),
                              static_cast<int>(errors->GetBufferSize()),
                              errMsg);
            }
            else
            {
                std::snprintf(buf, sizeof(buf),
                              "D3D12SerializeRootSignature failed (hr=0x%08lX, no error blob)",
                              static_cast<unsigned long>(serializeHr));
            }
            throw std::runtime_error(buf);
        }

        ThrowIfFailed(
            device.Native()->CreateRootSignature(
                0,                              // node mask (단일 GPU)
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature");

        wchar_t buf[256];
        std::swprintf(buf, std::size(buf),
                      L"[render] RootSignature created (params=%u, samplers=%u, IA layout 허용)\n",
                      paramCount, samplerCount);
        engine::core::LogInfo(buf);
    }

    RootSignature::~RootSignature() = default;

    ID3D12RootSignature* RootSignature::Native() const noexcept
    {
        return m_rootSignature.Get();
    }
}
