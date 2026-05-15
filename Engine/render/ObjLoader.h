#pragma once

#include <DirectXMath.h>
#include <memory>
#include <string>

namespace engine::render
{
    class Device;
    class Mesh;
}

// OBJ 파일 로더.
//
// 지원 형식:
//   - `v x y z` — 정점 위치
//   - `vn x y z` — 정점 normal
//   - `vt u v` — 텍스처 좌표 (현 단계 읽지만 무시)
//   - `f` 라인: `v//vn`, `v/vt/vn`, `v` (삼각형만, n-gon 미지원)
//   - 빈 줄 / `#` 주석 / 그 외 라인 무시
//
// 색상은 OBJ 표준에 없음 → defaultColor 로 모든 정점에 일괄 적용.
//
// 정점 dedup: (position 인덱스, normal 인덱스) 쌍을 키로 unique vertex 생성.
//
// 본 단계는 단순화 단순 파서 — 향후 텍스처 좌표(UV) 지원, MTL 파일, n-gon 삼각형화 추가.
namespace engine::render::obj_loader
{
    // OBJ 파일 절대 경로에서 Mesh 생성.
    std::unique_ptr<Mesh> LoadObj(
        Device&                  device,
        const wchar_t*           absolutePath,
        const DirectX::XMFLOAT3& defaultColor = { 1.0f, 1.0f, 1.0f });

    // 실행 파일(Client.exe) 옆 `assets/` 절대 경로 반환.
    // Client.vcxproj 의 PostBuildEvent 가 자산을 빌드 출력 폴더로 복사.
    std::wstring DefaultAssetsDir();
}
