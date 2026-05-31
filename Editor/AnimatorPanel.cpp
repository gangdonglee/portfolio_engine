#include "AnimatorPanel.h"

#include "Panels.h"

#include "anim/AnimatorController.h"
#include "anim/AnimatorSerializer.h"
#include "scene/Scene.h"

#include "../Client/SceneRuntime.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <utility>

namespace editor
{
    namespace
    {
        using engine::anim::ParameterType;
        using engine::anim::ConditionOp;

        const char* ParameterTypeString(ParameterType t) noexcept
        {
            switch (t)
            {
                case ParameterType::Bool:    return "Bool";
                case ParameterType::Int:     return "Int";
                case ParameterType::Float:   return "Float";
                case ParameterType::Trigger: return "Trigger";
            }
            return "?";
        }
        const char* ConditionOpString(ConditionOp op) noexcept
        {
            switch (op)
            {
                case ConditionOp::IfTrue:    return "IfTrue";
                case ConditionOp::IfFalse:   return "IfFalse";
                case ConditionOp::Greater:   return "Greater";
                case ConditionOp::Less:      return "Less";
                case ConditionOp::Equals:    return "Equals";
                case ConditionOp::NotEquals: return "NotEquals";
            }
            return "?";
        }

        // std::string 을 InputText 에 바인딩 — Panels.cpp 와 동일 패턴.
        bool InputStdString(const char* label, std::string& str, std::size_t maxLen = 256)
        {
            char buf[1024];
            const std::size_t n = std::min(str.size(), sizeof(buf) - 1);
            std::memcpy(buf, str.data(), n);
            buf[n] = '\0';
            const bool changed = ImGui::InputText(label, buf, std::min(sizeof(buf), maxLen));
            if (changed) { str.assign(buf); }
            return changed;
        }

        // Drag-drop target — Asset Browser 의 메쉬 path payload 받음.
        bool AcceptPathDrop(std::string& dst)
        {
            bool changed = false;
            if (ImGui::BeginDragDropTarget())
            {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH_PATH");
                if (payload && payload->Data && payload->DataSize > 0)
                {
                    dst.assign(static_cast<const char*>(payload->Data));
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
            return changed;
        }

        // ParameterType combo.
        bool ParameterTypeCombo(const char* label, ParameterType& t)
        {
            int idx = static_cast<int>(t);
            const char* items[] = { "Bool", "Int", "Float", "Trigger" };
            if (ImGui::Combo(label, &idx, items, IM_ARRAYSIZE(items)))
            {
                t = static_cast<ParameterType>(idx);
                return true;
            }
            return false;
        }

        // ConditionOp combo.
        bool ConditionOpCombo(const char* label, ConditionOp& op)
        {
            int idx = static_cast<int>(op);
            const char* items[] = { "IfTrue", "IfFalse", "Greater", "Less", "Equals", "NotEquals" };
            if (ImGui::Combo(label, &idx, items, IM_ARRAYSIZE(items)))
            {
                op = static_cast<ConditionOp>(idx);
                return true;
            }
            return false;
        }

        // 문자열 리스트로 combo. selected 가 리스트에 없으면 "<none>" + 직접 입력 hint.
        bool StringChoiceCombo(const char* label,
                               std::string&                    selected,
                               const std::vector<std::string>& choices,
                               bool allowEmpty = true)
        {
            const std::string preview = selected.empty() ? std::string{"<none>"} : selected;
            bool changed = false;
            if (ImGui::BeginCombo(label, preview.c_str()))
            {
                if (allowEmpty)
                {
                    const bool isSel = selected.empty();
                    if (ImGui::Selectable("<none>", isSel))
                    {
                        if (!isSel) { selected.clear(); changed = true; }
                    }
                }
                for (const auto& c : choices)
                {
                    const bool isSel = (c == selected);
                    if (ImGui::Selectable(c.c_str(), isSel))
                    {
                        if (!isSel) { selected = c; changed = true; }
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        void InitParamValuesFromController(AnimatorPanelState& state)
        {
            state.paramFloat.clear();
            state.paramBool.clear();
            if (!state.controller) { return; }
            for (const auto& p : state.controller->parameters)
            {
                switch (p.type)
                {
                    case ParameterType::Float:
                        state.paramFloat[p.name] = p.defaultValue; break;
                    case ParameterType::Bool:
                        state.paramBool [p.name] = (p.defaultValue != 0.0f); break;
                    default: break;
                }
            }
        }

        void TryReloadIfChanged(AnimatorPanelState& state, const std::string& wantedPath)
        {
            if (state.loadedPath == wantedPath) { return; }
            state.loadedPath = wantedPath;
            state.controller.reset();
            state.lastError.clear();
            state.dirty = false;
            if (wantedPath.empty()) { return; }
            try
            {
                engine::anim::AnimatorController ctrl = engine::anim::LoadJson(wantedPath);
                state.controller = std::make_unique<engine::anim::AnimatorController>(std::move(ctrl));
                InitParamValuesFromController(state);
            }
            catch (const std::exception& e)
            {
                state.lastError = e.what();
            }
        }

        std::vector<std::string> CollectStateNames(const engine::anim::AnimatorController& ctrl)
        {
            std::vector<std::string> v;
            v.reserve(ctrl.states.size());
            for (const auto& s : ctrl.states) { v.push_back(s.name); }
            return v;
        }
        std::vector<std::string> CollectParamNames(const engine::anim::AnimatorController& ctrl)
        {
            std::vector<std::string> v;
            v.reserve(ctrl.parameters.size());
            for (const auto& p : ctrl.parameters) { v.push_back(p.name); }
            return v;
        }

        // ===== Parameters editor =====
        void DrawParametersEditor(engine::anim::AnimatorController& ctrl,
                                  AnimatorPanelState&               state,
                                  client::SceneRuntime*             sceneRuntime,
                                  bool                              runtimeActive)
        {
            for (size_t i = 0; i < ctrl.parameters.size(); ++i)
            {
                auto& p = ctrl.parameters[i];
                ImGui::PushID(static_cast<int>(i));

                // 이름 edit — rename 후 transition condition 의 참조는 동기화하지 않음 (수동).
                std::string oldName = p.name;
                if (InputStdString("name", p.name))
                {
                    state.dirty = true;
                    // local value 맵의 키도 이전 이름 기준 — 수동으로 정리 안 해도 무해.
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X##removeParam"))
                {
                    ctrl.parameters.erase(ctrl.parameters.begin() + static_cast<std::ptrdiff_t>(i));
                    state.dirty = true;
                    ImGui::PopID();
                    break;   // 다음 프레임 다시 그림 — 안전.
                }

                ParameterType prevType = p.type;
                if (ParameterTypeCombo("type", p.type))
                {
                    state.dirty = true;
                    if (prevType != p.type)
                    {
                        // 타입 바뀌면 default 의 의미가 달라질 수 있음 — 별도 reset 없음.
                    }
                }
                // default value — 타입별 위젯.
                switch (p.type)
                {
                    case ParameterType::Float:
                        if (ImGui::DragFloat("defaultValue", &p.defaultValue, 0.01f))
                        {
                            state.dirty = true;
                        }
                        break;
                    case ParameterType::Bool:
                    {
                        bool b = (p.defaultValue != 0.0f);
                        if (ImGui::Checkbox("defaultValue", &b))
                        {
                            p.defaultValue = b ? 1.0f : 0.0f;
                            state.dirty = true;
                        }
                        break;
                    }
                    case ParameterType::Int:
                    {
                        int v = static_cast<int>(p.defaultValue);
                        if (ImGui::DragInt("defaultValue", &v))
                        {
                            p.defaultValue = static_cast<float>(v);
                            state.dirty = true;
                        }
                        break;
                    }
                    case ParameterType::Trigger:
                        ImGui::TextDisabled("(Trigger — no default)");
                        break;
                }

                // === Live value editor (SceneRuntime 으로 푸시) ===
                ImGui::Separator();
                ImGui::TextDisabled("Live value:");
                switch (p.type)
                {
                    case ParameterType::Float:
                    {
                        float& v = state.paramFloat[p.name];
                        if (ImGui::DragFloat("value", &v, 0.01f, -10.0f, 10.0f))
                        {
                            if (runtimeActive) { sceneRuntime->SetAnimatorFloat(p.name, v); }
                        }
                        break;
                    }
                    case ParameterType::Bool:
                    {
                        bool& b = state.paramBool[p.name];
                        if (ImGui::Checkbox("value", &b))
                        {
                            if (runtimeActive) { sceneRuntime->SetAnimatorBool(p.name, b); }
                        }
                        break;
                    }
                    case ParameterType::Trigger:
                    {
                        // 강조된 주황색 버튼 + 파라미터명 직접 표기 — UX 가시성.
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.95f, 0.55f, 0.10f, 1.0f});
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.00f, 0.70f, 0.20f, 1.0f});
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.85f, 0.45f, 0.05f, 1.0f});
                        char btn[128];
                        std::snprintf(btn, sizeof(btn), "[ Fire: %s ]", p.name.c_str());
                        if (ImGui::Button(btn))
                        {
                            if (runtimeActive) { sceneRuntime->SetAnimatorTrigger(p.name); }
                        }
                        ImGui::PopStyleColor(3);
                        break;
                    }
                    default: break;
                }

                ImGui::Separator();
                ImGui::PopID();
            }

            if (ImGui::SmallButton("+ Add Parameter"))
            {
                engine::anim::AnimatorParameter np;
                np.name         = "NewParam" + std::to_string(ctrl.parameters.size());
                np.type         = ParameterType::Float;
                np.defaultValue = 0.0f;
                ctrl.parameters.push_back(std::move(np));
                state.dirty = true;
            }
        }

        // ===== State editor =====
        void DrawStateEditor(engine::anim::AnimatorState& s,
                             const std::vector<std::string>& paramNames,
                             AnimatorPanelState& state)
        {
            if (InputStdString("name", s.name))             { state.dirty = true; }
            if (InputStdString("motionClipPath", s.motionClipPath)) { state.dirty = true; }
            if (AcceptPathDrop(s.motionClipPath))           { state.dirty = true; }
            if (ImGui::Checkbox("loop",  &s.loop))          { state.dirty = true; }
            if (ImGui::DragFloat("speed", &s.speed, 0.01f, -10.0f, 10.0f)) { state.dirty = true; }
            if (ImGui::DragFloat("startNormTime", &s.startNormTime, 0.005f, 0.0f, 1.0f)) { state.dirty = true; }
            if (ImGui::DragFloat("endNormTime",   &s.endNormTime,   0.005f, 0.0f, 1.0f)) { state.dirty = true; }

            // Blend Tree
            ImGui::Separator();
            ImGui::Text("Blend Tree");
            if (StringChoiceCombo("blendParameter", s.blendParameter, paramNames, /*allowEmpty*/true))
            {
                state.dirty = true;
            }
            for (size_t bi = 0; bi < s.blendTree.size(); ++bi)
            {
                ImGui::PushID(static_cast<int>(bi));
                auto& e = s.blendTree[bi];
                if (ImGui::DragFloat("threshold", &e.threshold, 0.01f)) { state.dirty = true; }
                if (InputStdString("motionClipPath", e.motionClipPath)) { state.dirty = true; }
                if (AcceptPathDrop(e.motionClipPath))                   { state.dirty = true; }
                if (ImGui::SmallButton("Remove entry"))
                {
                    s.blendTree.erase(s.blendTree.begin() + static_cast<std::ptrdiff_t>(bi));
                    state.dirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Add Blend Entry"))
            {
                engine::anim::BlendTreeEntry ne;
                ne.threshold = 0.0f;
                s.blendTree.push_back(std::move(ne));
                state.dirty = true;
            }

            // Root motion — 토글 + 펼치면 필드.
            ImGui::Separator();
            if (ImGui::Checkbox("hasRootMotion", &s.hasRootMotion)) { state.dirty = true; }
            if (s.hasRootMotion && ImGui::TreeNode("rootMotion"))
            {
                if (ImGui::DragFloat("takeoffNormTime",  &s.rootMotion.takeoffNormTime,  0.005f, 0.0f, 1.0f)) { state.dirty = true; }
                if (ImGui::DragFloat("landingNormTime",  &s.rootMotion.landingNormTime,  0.005f, 0.0f, 1.0f)) { state.dirty = true; }
                if (ImGui::DragFloat("peakHeight",       &s.rootMotion.peakHeight,       0.5f, 0.0f, 500.0f)) { state.dirty = true; }
                if (ImGui::DragFloat("fadeWindow",       &s.rootMotion.fadeWindow,       0.005f, 0.0f, 0.5f)) { state.dirty = true; }
                if (InputStdString("extractRootMotionFromBone", s.rootMotion.extractRootMotionFromBone)) { state.dirty = true; }
                ImGui::TreePop();
            }
        }

        // ===== States section editor =====
        void DrawStatesEditor(engine::anim::AnimatorController& ctrl,
                              AnimatorPanelState&               state)
        {
            const auto paramNames = CollectParamNames(ctrl);

            // defaultStateName 선택 — 모든 state 이름 중에.
            const auto stateNames = CollectStateNames(ctrl);
            if (StringChoiceCombo("defaultStateName", ctrl.defaultStateName, stateNames, /*allowEmpty*/true))
            {
                state.dirty = true;
            }
            ImGui::Separator();

            for (size_t i = 0; i < ctrl.states.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = ctrl.states[i].name.empty()
                    ? ("State " + std::to_string(i))
                    : ctrl.states[i].name;
                if (ImGui::TreeNode(label.c_str()))
                {
                    if (ImGui::SmallButton("Remove State"))
                    {
                        ctrl.states.erase(ctrl.states.begin() + static_cast<std::ptrdiff_t>(i));
                        state.dirty = true;
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }
                    DrawStateEditor(ctrl.states[i], paramNames, state);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (ImGui::SmallButton("+ Add State"))
            {
                engine::anim::AnimatorState ns;
                ns.name = "NewState" + std::to_string(ctrl.states.size());
                ctrl.states.push_back(std::move(ns));
                state.dirty = true;
            }
        }

        // ===== Transitions editor =====
        void DrawTransitionsEditor(engine::anim::AnimatorController& ctrl,
                                   AnimatorPanelState&               state)
        {
            const auto stateNames = CollectStateNames(ctrl);
            const auto paramNames = CollectParamNames(ctrl);

            for (size_t i = 0; i < ctrl.transitions.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                auto& t = ctrl.transitions[i];
                const std::string from = t.fromStateName.empty() ? "[Any]" : t.fromStateName;
                const std::string label = from + " -> " + t.toStateName + "##" + std::to_string(i);
                if (ImGui::TreeNode(label.c_str()))
                {
                    if (ImGui::SmallButton("Remove Transition"))
                    {
                        ctrl.transitions.erase(ctrl.transitions.begin() + static_cast<std::ptrdiff_t>(i));
                        state.dirty = true;
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }
                    if (StringChoiceCombo("fromState (empty=AnyState)", t.fromStateName, stateNames, /*allowEmpty*/true))  { state.dirty = true; }
                    if (StringChoiceCombo("toState",                    t.toStateName,   stateNames, /*allowEmpty*/false)) { state.dirty = true; }
                    if (ImGui::DragFloat("crossfadeDuration", &t.crossfadeDuration, 0.01f, 0.0f, 2.0f)) { state.dirty = true; }
                    if (ImGui::Checkbox ("hasExitTime", &t.hasExitTime))             { state.dirty = true; }
                    if (t.hasExitTime &&
                        ImGui::DragFloat("exitTime",    &t.exitTime, 0.005f, 0.0f, 1.0f)) { state.dirty = true; }

                    ImGui::Separator();
                    ImGui::Text("Conditions (AND):");
                    for (size_t ci = 0; ci < t.conditions.size(); ++ci)
                    {
                        ImGui::PushID(static_cast<int>(ci));
                        auto& c = t.conditions[ci];
                        if (StringChoiceCombo("param", c.parameterName, paramNames, /*allowEmpty*/false))
                        {
                            state.dirty = true;
                        }
                        if (ConditionOpCombo("op", c.op)) { state.dirty = true; }
                        if (ImGui::DragFloat("value", &c.value, 0.01f)) { state.dirty = true; }
                        if (ImGui::SmallButton("Remove cond"))
                        {
                            t.conditions.erase(t.conditions.begin() + static_cast<std::ptrdiff_t>(ci));
                            state.dirty = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("+ Add Condition"))
                    {
                        engine::anim::TransitionCondition nc;
                        nc.op = ConditionOp::IfTrue;
                        nc.value = 0.0f;
                        t.conditions.push_back(std::move(nc));
                        state.dirty = true;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (ImGui::SmallButton("+ Add Transition"))
            {
                engine::anim::AnimatorTransition nt;
                nt.fromStateName = ctrl.states.empty() ? std::string{} : ctrl.states.front().name;
                nt.toStateName   = ctrl.states.size() > 1 ? ctrl.states[1].name :
                                   (ctrl.states.empty() ? std::string{} : ctrl.states.front().name);
                nt.crossfadeDuration = 0.2f;
                ctrl.transitions.push_back(std::move(nt));
                state.dirty = true;
            }
        }
    }

    void EnsureAnimatorLoaded(const engine::scene::Scene&       scene,
                              const editor::panels::Selection&  sel,
                              AnimatorPanelState&               state)
    {
        if (sel.kind != editor::panels::NodeKind::MeshInstance ||
            sel.index >= scene.meshes.size())
        {
            TryReloadIfChanged(state, std::string{});
            return;
        }
        const auto& inst = scene.meshes[sel.index];
        TryReloadIfChanged(state, inst.animatorControllerPath);
    }

    bool DrawAnimatorPanel(const engine::scene::Scene&       scene,
                           const editor::panels::Selection&  sel,
                           client::SceneRuntime*             sceneRuntime,
                           AnimatorPanelState&               state)
    {
        bool savedThisFrame = false;

        // === 선택 가드 ===
        if (sel.kind != editor::panels::NodeKind::MeshInstance ||
            sel.index >= scene.meshes.size())
        {
            ImGui::TextDisabled("(Hierarchy 에서 animator 바인딩된 MeshInstance 선택)");
            TryReloadIfChanged(state, std::string{});
            return false;
        }
        const auto& inst = scene.meshes[sel.index];
        if (inst.animatorControllerPath.empty())
        {
            ImGui::TextDisabled("(이 MeshInstance 에는 animator 가 없음)");
            ImGui::TextDisabled("(Asset Browser 의 .animator.json 을 Inspector 로 드래그하세요)");
            TryReloadIfChanged(state, std::string{});
            return false;
        }

        // === 경로 변경 감지 + 로드 ===
        TryReloadIfChanged(state, inst.animatorControllerPath);

        if (!state.controller)
        {
            ImGui::TextColored(ImVec4{1.0f, 0.5f, 0.5f, 1.0f},
                               "Animator 로드 실패: %s", state.lastError.c_str());
            return false;
        }
        auto& ctrl = *state.controller;

        // === 헤더 + Save 버튼 ===
        ImGui::Text("Animator: %s",
                    std::filesystem::path(state.loadedPath).filename().string().c_str());
        ImGui::TextDisabled("path: %s", state.loadedPath.c_str());

        ImGui::SameLine(0, 20);
        const bool canSave = state.dirty;
        if (!canSave) { ImGui::BeginDisabled(); }
        if (ImGui::Button(state.dirty ? "Save*" : "Save"))
        {
            try
            {
                engine::anim::SaveJson(ctrl, state.loadedPath);
                state.dirty = false;
                savedThisFrame = true;
            }
            catch (const std::exception& e)
            {
                state.lastError = std::string{"Save FAILED: "} + e.what();
            }
        }
        if (!canSave) { ImGui::EndDisabled(); }

        if (!state.lastError.empty())
        {
            ImGui::TextColored(ImVec4{1.0f, 0.5f, 0.5f, 1.0f}, "%s", state.lastError.c_str());
        }

        // controller 이름 (서식상 정보 — Save 시 JSON 의 name 필드).
        if (InputStdString("controller.name", ctrl.name)) { state.dirty = true; }
        ImGui::Separator();

        // === Runtime status ===
        const bool runtimeActive = (sceneRuntime != nullptr && sceneRuntime->HasAnimatorRuntime());
        if (runtimeActive)
        {
            const std::string curStateName = sceneRuntime->AnimatorCurrentStateName();
            const float       curStateTime = sceneRuntime->AnimatorCurrentStateTime();
            const float       curStateDur  = sceneRuntime->AnimatorStateDuration(curStateName);

            ImGui::Text("Current State: %s", curStateName.c_str());
            ImGui::Text("State Time:    %.3f / %.3f s", curStateTime, curStateDur);

            bool paused = sceneRuntime->AnimatorIsPaused();
            if (ImGui::Checkbox("Paused", &paused))
            {
                sceneRuntime->AnimatorSetPaused(paused);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Restart state"))
            {
                sceneRuntime->AnimatorSetCurrentStateTime(0.0f);
            }

            // === Frame-by-frame 스크럽 — Paused 시에만 유효 ===
            // 시간 슬라이더 + 이전/다음 프레임 step. Mixamo 표준 30 fps 가정.
            constexpr float kFps  = 30.0f;
            constexpr float kStep = 1.0f / kFps;
            if (paused)
            {
                ImGui::TextDisabled("Scrub (1/%g s 단위):", kFps);
                float t = curStateTime;
                const float maxDur = (curStateDur > 0.0f) ? curStateDur : kStep;
                if (ImGui::SliderFloat("time##scrub", &t, 0.0f, maxDur, "%.4f s"))
                {
                    if (t < 0.0f)     { t = 0.0f; }
                    if (t > maxDur)   { t = maxDur; }
                    sceneRuntime->AnimatorSetCurrentStateTime(t);
                }
                const int curFrame = static_cast<int>(curStateTime * kFps);
                const int maxFrame = static_cast<int>(maxDur      * kFps);
                ImGui::Text("Frame: %d / %d", curFrame, maxFrame);
                ImGui::SameLine();
                if (ImGui::SmallButton("<< -1 frame"))
                {
                    float nt = curStateTime - kStep;
                    if (nt < 0.0f) { nt = 0.0f; }
                    sceneRuntime->AnimatorSetCurrentStateTime(nt);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+1 frame >>"))
                {
                    float nt = curStateTime + kStep;
                    if (nt > maxDur) { nt = maxDur; }
                    sceneRuntime->AnimatorSetCurrentStateTime(nt);
                }
            }
            else
            {
                ImGui::TextDisabled("(Paused 체크 → 프레임 단위 스크럽 가능)");
            }
        }
        else
        {
            ImGui::TextDisabled("(SceneRuntime 의 AnimatorRuntime 비활성 — 아직 첫 프레임 전)");
        }
        ImGui::Separator();

        // === Parameters (edit + live values) ===
        if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // 점프 테스트 가이드 — Jump 트리거가 즉시 Locomotion 으로 복귀되어 안 보이는 흔한 함정 설명.
            ImGui::TextColored(ImVec4{0.7f, 0.9f, 1.0f, 1.0f}, "Tip: Trigger 발동");
            ImGui::TextWrapped("  - 'Fire: <name>' 주황색 버튼 클릭 → 트리거 1회 발동.");
            ImGui::TextWrapped("  - 'Jump' 테스트: 먼저 IsGrounded 의 *Live value* 체크 해제 → Fire: Jump → "
                               "Current State 가 Jump_Takeoff → Jump_Air 로 변하는지 확인. IsGrounded 가 "
                               "체크되어 있으면 Jump_Air → Locomotion 으로 즉시 복귀해서 점프 자세가 안 보임.");
            ImGui::Separator();
            DrawParametersEditor(ctrl, state, sceneRuntime, runtimeActive);
        }

        // === States ===
        if (ImGui::CollapsingHeader("States"))
        {
            DrawStatesEditor(ctrl, state);
        }

        // === Transitions ===
        if (ImGui::CollapsingHeader("Transitions"))
        {
            DrawTransitionsEditor(ctrl, state);
        }

        return savedThisFrame;
    }
}
