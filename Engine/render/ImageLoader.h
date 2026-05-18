#pragma once

#include "core/Types.h"

#include <vector>

namespace engine::render
{
    // WIC (Windows Imaging Component) 기반 RGBA8 이미지 디코더.
    // Windows SDK 내장 — 외부 라이브러리 0. JPG/PNG/BMP/TIFF/DDS 등 다양한 포맷 지원.
    //
    // 결과는 항상 GUID_WICPixelFormat32bppRGBA 로 정규화 — Texture 가 받는 RGBA8 row-major.
    //
    // 본 단계는 단일 mip + 단일 frame 만 추출. 큐브맵 / 애니메이션 GIF / mipmap 은 향후 단계.
    struct ImageData
    {
        std::vector<uint8> pixels;   // RGBA8 row-major (width * height * 4 바이트)
        uint32             width  = 0;
        uint32             height = 0;
    };

    namespace image_loader
    {
        // 절대 경로의 이미지를 RGBA8 로 디코드.
        // 실패 시 throw std::runtime_error. WIC COM 초기화는 함수 내부에서 처리 (CoInitialize 가 호출자 책임 아님).
        ImageData LoadImage(const wchar_t* absolutePath);
    }
}
