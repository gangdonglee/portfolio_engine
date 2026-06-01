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
    // **world coords** — 캔버스 안 가상 평면. 화면에 그릴 땐 ToScreen 변환 (viewPan + viewZoom).
    struct AnimatorGraphState
    {
        std::unordered_map<std::string, ImVec2> nodePositions;
        ImVec2      anyStatePos{ 20.0f, 20.0f };
        int         selectedStateIdx       = -1;
        int         selectedTransitionIdx  = -1;
        std::string lastLayoutFor;   // 마지막 자동 레이아웃 controller.name

        // View transform — 캔버스 zoom + pan.
        float       viewZoom = 1.0f;
        ImVec2      viewPan { 0.0f, 0.0f };

        // 진행 중 transition 생성 — Shift+LMB 드래그 시 set.
        //   draggingTransitionFromAny: Any State 에서 출발
        //   draggingTransitionFromIdx: state 인덱스 (-1 = 비활성)
        bool        draggingTransitionFromAny = false;
        int         draggingTransitionFromIdx = -1;

        // layout 영구 저장 — true 시 다음 Animator Save 에서 layout JSON 같이 기록.
        // node drag / anyState drag / zoom / pan / add state / delete state 시 set.
        bool        layoutDirty = false;
    };

    // <animatorPath>.animator.json  →  <animatorPath>.animator.layout.json
    //   nodePositions / anyStatePos / viewZoom / viewPan 직렬화. Editor-only metadata.
    //   animator.json 자체엔 영향 없음 — 깨끗하게 분리.
    bool LoadAnimatorGraphLayout(const std::string& animatorJsonPath, AnimatorGraphState& state);
    bool SaveAnimatorGraphLayout(const std::string& animatorJsonPath, const AnimatorGraphState& state);

    // Animator Graph 패널 1프레임. controller=nullptr 면 안내 텍스트만.
    //   - 노드: 상태 박스, 현재 활성 state 는 초록, default 는 황갈, 선택 은 노랑 테두리
    //   - 화살표: transition (Any State 는 동그라미 노드에서 출발), 좌클릭 → 선택
    //   - 마우스 좌클릭으로 노드 선택, 드래그로 위치 이동
    //   - 선택된 transition 은 캔버스 아래에 inline 편집기 (crossfade/exitTime/conditions)
    //
    // controller 가 non-const — Inline editor 가 직접 수정. 변경 시 outDirty 를 true 로 set.
    // outDirty 를 호출자가 AnimatorPanelState.dirty 에 OR-in 해서 Save 활성화.
    // animatorJsonPath: layout JSON 위치 파생용 (& 빈 문자열이면 layout I/O skip).
    void DrawAnimatorGraph(engine::anim::AnimatorController* controller,
                           const std::string&                animatorJsonPath,
                           client::SceneRuntime*             sceneRuntime,
                           AnimatorGraphState&               graphState,
                           bool&                             outDirty);
}
