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
        // 루트 파라미터 구성. cbvAtB0 가 None 외면 b0 CBV root descriptor 1개.
        D3D12_ROOT_PARAMETER params[1]{};
        UINT paramCount = 0;
        if (desc.cbvAtB0 != Desc::CbvB0::None)
        {
            params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].Descriptor.ShaderRegister = 0;
            params[0].Descriptor.RegisterSpace  = 0;
            params[0].ShaderVisibility =
                (desc.cbvAtB0 == Desc::CbvB0::Vertex)
                    ? D3D12_SHADER_VISIBILITY_VERTEX
                    : D3D12_SHADER_VISIBILITY_ALL;
            paramCount = 1;
        }

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = paramCount;
        rsDesc.pParameters       = paramCount > 0 ? params : nullptr;
        rsDesc.NumStaticSamplers = 0;
        rsDesc.pStaticSamplers   = nullptr;
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

        switch (desc.cbvAtB0)
        {
            case Desc::CbvB0::None:
                engine::core::LogInfo(L"[render] RootSignature created (empty, IA layout 허용)\n");
                break;
            case Desc::CbvB0::Vertex:
                engine::core::LogInfo(L"[render] RootSignature created (1 CBV @ b0 vertex visible)\n");
                break;
            case Desc::CbvB0::All:
                engine::core::LogInfo(L"[render] RootSignature created (1 CBV @ b0 all-visible)\n");
                break;
        }
    }

    RootSignature::~RootSignature() = default;

    ID3D12RootSignature* RootSignature::Native() const noexcept
    {
        return m_rootSignature.Get();
    }
}
