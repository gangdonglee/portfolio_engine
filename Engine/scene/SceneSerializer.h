#pragma once

#include "scene/Scene.h"

#include <string>
#include <string_view>

namespace engine::scene
{
    // Scene ↔ JSON 직렬화.
    //
    // 포맷 (사람 가독 + git diff 가능):
    //   {
    //     "name": "...",
    //     "ambient": [r, g, b],
    //     "cameraStart": { "position": [x,y,z], "target": [x,y,z], "fovYRad": <float> },
    //     "meshes":      [ { name, meshAssetPath, transform: { position:[..], rotation:[xyzw], scale:[..] } }, ... ],
    //     "dirLights":   [ { name, directionWS:[..], color:[..], intensity }, ... ],
    //     "pointLights": [ { name, positionWS:[..], color:[..], intensity, range }, ... ]
    //   }
    //
    // 라이트 개수는 자유 — 직렬화는 std::vector 크기 그대로 반영. 게임 런타임이 받아 GPU
    // StructuredBuffer 로 통째 업로드.
    //
    // 에러 정책: 파싱 실패 / 파일 미존재는 std::runtime_error throw. 호출자가 catch.
    void  SaveJson(const Scene& scene, std::string_view path);
    Scene LoadJson(std::string_view path);
}
