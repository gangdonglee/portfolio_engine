#include "render/RootSignature.h"

#include "core/HrCheck.h"

#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {
    }

    RootSignature::RootSignature(Device& device)
    {
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 0;
        desc.pParameters       = nullptr;
        desc.NumStaticSamplers = 0;
        desc.pStaticSamplers   = nullptr;
        // IA 단계의 input layout 사용 허용. 비어있는 RS 라도 IA 가 필요하면 이 플래그 필수.
        desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        const HRESULT serializeHr = ::D3D12SerializeRootSignature(
            &desc,
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

        ::OutputDebugStringW(L"[render] RootSignature created (empty, IA layout 허용)\n");
    }

    RootSignature::~RootSignature() = default;

    ID3D12RootSignature* RootSignature::Native() const noexcept
    {
        return m_rootSignature.Get();
    }
}
