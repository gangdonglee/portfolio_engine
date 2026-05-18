#include "render/ImageLoader.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>

#pragma comment(lib, "windowscodecs.lib")

namespace engine::render::image_loader
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        using engine::core::ThrowIfFailed;

        // 프로세스 1회 COM 초기화 — WIC 가 STA 컨텍스트 필요.
        // CoInitializeEx 는 idempotent (스레드 중복 호출은 S_FALSE 반환).
        void EnsureComInitialized()
        {
            // STA — UI 스레드 가정. 본 엔진은 단일 스레드 메인 루프.
            // S_FALSE 는 이미 초기화됨 — 정상.
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            {
                throw std::runtime_error("CoInitializeEx 실패 — WIC 사용 불가");
            }
        }
    }

    ImageData LoadImage(const wchar_t* absolutePath)
    {
        if (absolutePath == nullptr || *absolutePath == L'\0')
        {
            throw std::runtime_error("ImageLoader: 경로가 비어있음");
        }

        EnsureComInitialized();

        ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            ::CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.GetAddressOf())),
            "CoCreateInstance(WICImagingFactory)");

        ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromFilename(
                absolutePath,
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf()),
            "IWICImagingFactory::CreateDecoderFromFilename");

        ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(
            decoder->GetFrame(0, frame.GetAddressOf()),
            "IWICBitmapDecoder::GetFrame(0)");

        // 원본 포맷이 무엇이든 RGBA8 로 변환 — Texture 가 받는 표준 포맷.
        ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(
            factory->CreateFormatConverter(converter.GetAddressOf()),
            "IWICImagingFactory::CreateFormatConverter");

        ThrowIfFailed(
            converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeMedianCut),
            "IWICFormatConverter::Initialize(32bppRGBA)");

        UINT width  = 0;
        UINT height = 0;
        ThrowIfFailed(
            converter->GetSize(&width, &height),
            "IWICFormatConverter::GetSize");

        if (width == 0 || height == 0)
        {
            throw std::runtime_error("ImageLoader: 디코드 결과 크기가 0");
        }

        ImageData data;
        data.width  = width;
        data.height = height;
        const UINT rowPitch  = width * 4;            // RGBA8 4 bytes/pixel
        const UINT bufferSz  = rowPitch * height;
        data.pixels.resize(bufferSz);

        ThrowIfFailed(
            converter->CopyPixels(
                nullptr,                  // 전체 영역
                rowPitch,
                bufferSz,
                data.pixels.data()),
            "IWICFormatConverter::CopyPixels");

        wchar_t logLine[256];
        std::swprintf(logLine, std::size(logLine),
                      L"[render] Image loaded (RGBA8 %ux%u, %u bytes): %ls\n",
                      width, height, bufferSz, absolutePath);
        engine::core::LogInfo(logLine);

        return data;
    }
}
