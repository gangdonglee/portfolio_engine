#pragma once

// unique_ptr 의 묵시적 소멸자 인스턴스화에 완전 타입 필요 — AnimatorController.h 포함.
#include "anim/AnimatorController.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace engine::scene  { struct Scene; }
namespace client         { class  SceneRuntime; }
namespace editor::panels { struct Selection; }

namespace editor
{
    // Animator 패널 상태 — 현재 선택된 MeshInstance 의 animator JSON 캐시 +
    // UI 가 마지막으로 푸시한 파라미터 값 (SceneRuntime 의 AnimatorRuntime 으로
    // 동기화). selection 의 animatorControllerPath 가 바뀌면 자동 재로드.
    //
    // dirty: 구조적 편집 (state/transition/parameter 의 *목록 또는 정의*) 발생 시 true.
    //        파라미터 *값* 만 바뀐 경우는 dirty 가 아니고 SceneRuntime 으로 곧장 푸시됨.
    //        Save 클릭 시 SaveJson + dirty=false + caller 에 rebuild 신호.
    struct AnimatorPanelState
    {
        std::string                                       loadedPath;
        std::unique_ptr<engine::anim::AnimatorController> controller;
        std::string                                       lastError;
        bool                                              dirty = false;

        // UI 가 사용자에게 보여주는 현재 값 (SetAnimatorFloat 등으로 SceneRuntime 에 푸시).
        // 첫 로드 시 controller.parameters 의 defaultValue 로 초기화.
        std::unordered_map<std::string, float> paramFloat;
        std::unordered_map<std::string, bool>  paramBool;
    };

    // Animator 패널 1프레임.
    //   - selection.kind == MeshInstance 이고 그 인스턴스에 animatorControllerPath 가 있으면
    //     해당 JSON 을 로드/표시.
    //   - 파라미터 *값* live-edit, current state / time, pause toggle.
    //   - 구조 편집 (add/remove/rename of params, states, transitions, conditions).
    //   - Save 버튼 → JSON 저장.
    //
    // 반환 true: Save 가 발생함 → 호출자가 SceneRuntime 재빌드해야 새 controller 적용.
    bool DrawAnimatorPanel(const engine::scene::Scene&        scene,
                           const editor::panels::Selection&   selection,
                           client::SceneRuntime*              sceneRuntime,
                           AnimatorPanelState&                state);

    // selection 의 animatorControllerPath 가 바뀌었으면 panel state 의 controller 를 reload.
    // Animator 패널이 collapsed/hidden 이어도 graph 패널이 같은 state 를 공유하므로 매 프레임
    // 호출 필요 — main.cpp 에서 ImGui::Begin 보다 *앞에* 1회 호출.
    void EnsureAnimatorLoaded(const engine::scene::Scene&        scene,
                              const editor::panels::Selection&   selection,
                              AnimatorPanelState&                state);
}
