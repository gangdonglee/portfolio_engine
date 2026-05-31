#pragma once

#include "imgui.h"   // ImVec2

#include <string>
#include <unordered_map>

namespace engine::anim   { struct AnimatorController; }
namespace client         { class  SceneRuntime; }

namespace editor
{
    // Animator Graph 패널 상태 — 노드 위치 / 선택 / 자동 레이아웃 가드.
    // 위치 는 AnimatorController.name 기준으로 캐시 — 다른 animator 로딩 시 재레이아웃.
    struct AnimatorGraphState
    {
        std::unordered_map<std::string, ImVec2> nodePositions;
        ImVec2      anyStatePos{ 20.0f, 20.0f };
        int         selectedStateIdx       = -1;
        int         selectedTransitionIdx  = -1;
        std::string lastLayoutFor;   // 마지막 자동 레이아웃 controller.name
    };

    // Animator Graph 패널 1프레임. controller=nullptr 면 안내 텍스트만.
    //   - 노드: 상태 박스, 현재 활성 state 는 초록, default 는 황갈, 선택 은 노랑 테두리
    //   - 화살표: transition (Any State 는 동그라미 노드에서 출발)
    //   - 마우스 좌클릭으로 노드 선택, 드래그로 위치 이동
    void DrawAnimatorGraph(const engine::anim::AnimatorController* controller,
                           client::SceneRuntime*                   sceneRuntime,
                           AnimatorGraphState&                     graphState);
}
