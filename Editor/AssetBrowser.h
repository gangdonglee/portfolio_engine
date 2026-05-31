#pragma once

#include <string>
#include <vector>

namespace engine::scene
{
    struct Scene;
}

namespace editor::panels
{
    struct Selection;
}

namespace editor
{
    // Asset Browser 패널의 캐시 상태 + 현재 브러시.
    // 첫 호출 시 디렉토리 스캔. "Rescan" 버튼으로 재스캔.
    // brushMeshPath: 현재 선택된 배치 브러시 (단일 클릭으로 설정).
    //   비어 있으면 브러시 모드 OFF — Viewport LMB 클릭은 아무 일도 안 함.
    //   채워져 있으면 Viewport LMB 클릭 시 ground raycast 후 그 위치에 MeshInstance 배치.
    struct AssetBrowserState
    {
        std::vector<std::string> meshPaths;        // Resources/FBX/*.fbx, *.obj
        std::vector<std::string> animatorPaths;    // assets/Animators/*.animator.json
        bool                     scanned = false;

        std::string              brushMeshPath;    // 활성 배치 브러시 (메쉬). 빈 문자열 = OFF.
    };

    // Asset Browser 패널 1 프레임.
    //   - 메쉬 단일 클릭 → brushMeshPath 설정 (Viewport 클릭으로 배치).
    //   - 다시 같은 항목 클릭 또는 "Clear brush" → 브러시 해제.
    //   - 애니메이터 더블클릭 → MeshInstance 선택 시 animatorControllerPath 바인딩 (즉시 적용).
    //
    // 반환 true: animator path 가 즉시 변경됨 → 호출자 SceneRuntime 재빌드 필요.
    //   브러시 설정/해제 자체는 false (실제 배치는 main 의 Viewport 핸들러에서).
    bool DrawAssetBrowser(engine::scene::Scene&        scene,
                          editor::panels::Selection&   selection,
                          AssetBrowserState&           state);
}
