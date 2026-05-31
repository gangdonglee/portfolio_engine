#include "AnimatorGraph.h"

#include "anim/AnimatorController.h"

#include "../Client/SceneRuntime.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace editor
{
    namespace
    {
        constexpr float kNodeWidth        = 160.0f;
        constexpr float kNodeHeight       = 44.0f;
        constexpr float kAnyStateRadius   = 30.0f;
        constexpr float kArrowSize        = 12.0f;

        ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }
        ImVec2 operator-(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x - b.x, a.y - b.y); }

        void AutoLayout(const engine::anim::AnimatorController& ctrl,
                        AnimatorGraphState&                     state)
        {
            // 3-col 그리드. Any State 노드 우측부터 state 들 배치.
            constexpr int   kCols  = 3;
            constexpr float kColW  = 200.0f;
            constexpr float kRowH  = 80.0f;
            constexpr float kStartX = 150.0f;
            constexpr float kStartY = 20.0f;
            for (size_t i = 0; i < ctrl.states.size(); ++i)
            {
                const int col = static_cast<int>(i) % kCols;
                const int row = static_cast<int>(i) / kCols;
                state.nodePositions[ctrl.states[i].name] = ImVec2(
                    kStartX + col * kColW,
                    kStartY + row * kRowH);
            }
        }

        // node 중심에서 target 방향으로 박스 가장자리 교점.
        ImVec2 BoxEdge(ImVec2 nodeCenter, float w, float h, ImVec2 target)
        {
            const float dx = target.x - nodeCenter.x;
            const float dy = target.y - nodeCenter.y;
            if (std::abs(dx) < 0.001f && std::abs(dy) < 0.001f) { return nodeCenter; }

            const float hw = w * 0.5f;
            const float hh = h * 0.5f;
            const float sx = (dx >= 0.0f) ? 1.0f : -1.0f;
            const float sy = (dy >= 0.0f) ? 1.0f : -1.0f;

            // 박스 좌/우 면 hit (dx 비율로 판정) vs 상/하 면.
            const float tx = (std::abs(dx) > 0.001f) ? (hw / std::abs(dx)) : 1e9f;
            const float ty = (std::abs(dy) > 0.001f) ? (hh / std::abs(dy)) : 1e9f;
            const float t  = std::min(tx, ty);
            return ImVec2(nodeCenter.x + dx * t, nodeCenter.y + dy * t);
        }

        // 원 가장자리 교점 (Any State 용).
        ImVec2 CircleEdge(ImVec2 center, float radius, ImVec2 target)
        {
            const float dx = target.x - center.x;
            const float dy = target.y - center.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 0.001f) { return center; }
            return ImVec2(center.x + dx / len * radius, center.y + dy / len * radius);
        }

        void DrawArrowhead(ImDrawList* dl, ImVec2 from, ImVec2 to, float size, ImU32 color)
        {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 0.001f) { return; }
            const float ux = dx / len;
            const float uy = dy / len;
            const float px = -uy;   // perp
            const float py =  ux;
            const ImVec2 base = ImVec2(to.x - ux * size, to.y - uy * size);
            const ImVec2 p1 = ImVec2(base.x + px * size * 0.45f, base.y + py * size * 0.45f);
            const ImVec2 p2 = ImVec2(base.x - px * size * 0.45f, base.y - py * size * 0.45f);
            dl->AddTriangleFilled(to, p1, p2, color);
        }

        int FindStateIdx(const engine::anim::AnimatorController& ctrl, const std::string& name)
        {
            for (size_t i = 0; i < ctrl.states.size(); ++i)
            {
                if (ctrl.states[i].name == name) { return static_cast<int>(i); }
            }
            return -1;
        }
    }

    void DrawAnimatorGraph(const engine::anim::AnimatorController* ctrl,
                           client::SceneRuntime*                   sceneRuntime,
                           AnimatorGraphState&                     state)
    {
        if (!ctrl)
        {
            ImGui::TextDisabled("(Hierarchy 에서 animator 바인딩된 MeshInstance 선택)");
            return;
        }

        // 자동 레이아웃 — 다른 controller 로 바뀌었거나 비어있으면.
        if (state.lastLayoutFor != ctrl->name || state.nodePositions.empty())
        {
            AutoLayout(*ctrl, state);
            state.lastLayoutFor = ctrl->name;
        }

        const ImVec2 canvasPos   = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize  = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 16.0f || canvasSize.y < 16.0f) { return; }
        const ImVec2 canvasEnd   = canvasPos + canvasSize;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 배경 + 그리드
        dl->AddRectFilled(canvasPos, canvasEnd, IM_COL32(35, 35, 40, 255));
        dl->AddRect      (canvasPos, canvasEnd, IM_COL32(80, 80, 80, 255));
        for (float x = 40.0f; x < canvasSize.x; x += 40.0f)
        {
            dl->AddLine(canvasPos + ImVec2(x, 0.0f),
                        canvasPos + ImVec2(x, canvasSize.y),
                        IM_COL32(50, 50, 55, 255));
        }
        for (float y = 40.0f; y < canvasSize.y; y += 40.0f)
        {
            dl->AddLine(canvasPos + ImVec2(0.0f, y),
                        canvasPos + ImVec2(canvasSize.x, y),
                        IM_COL32(50, 50, 55, 255));
        }

        // 활성 state — 노드 강조용.
        const std::string curStateName = (sceneRuntime && sceneRuntime->HasAnimatorRuntime())
            ? sceneRuntime->AnimatorCurrentStateName() : std::string{};

        // === Transition lines (노드 뒤) ===
        for (size_t ti = 0; ti < ctrl->transitions.size(); ++ti)
        {
            const auto& t = ctrl->transitions[ti];

            const auto itTo = state.nodePositions.find(t.toStateName);
            if (itTo == state.nodePositions.end()) { continue; }
            const ImVec2 toCenter = canvasPos + itTo->second + ImVec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f);

            ImVec2 fromCenter;
            bool   fromIsAny = t.fromStateName.empty();
            if (fromIsAny)
            {
                fromCenter = canvasPos + state.anyStatePos + ImVec2(kAnyStateRadius, kAnyStateRadius);
            }
            else
            {
                const auto itFrom = state.nodePositions.find(t.fromStateName);
                if (itFrom == state.nodePositions.end()) { continue; }
                fromCenter = canvasPos + itFrom->second + ImVec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f);
            }

            const ImVec2 from = fromIsAny
                ? CircleEdge(fromCenter, kAnyStateRadius, toCenter)
                : BoxEdge   (fromCenter, kNodeWidth, kNodeHeight, toCenter);
            const ImVec2 to   = BoxEdge(toCenter, kNodeWidth, kNodeHeight, fromCenter);

            const bool selected = (static_cast<int>(ti) == state.selectedTransitionIdx);
            const ImU32 col = selected
                ? IM_COL32(255, 220, 80, 255)
                : IM_COL32(180, 180, 180, 220);
            dl->AddLine(from, to, col, selected ? 3.0f : 2.0f);
            DrawArrowhead(dl, from, to, kArrowSize, col);
        }

        // === Any State 노드 (드래그 가능) ===
        {
            const ImVec2 pos = canvasPos + state.anyStatePos;
            const ImVec2 size(kAnyStateRadius * 2.0f, kAnyStateRadius * 2.0f);
            const ImVec2 center = pos + ImVec2(kAnyStateRadius, kAnyStateRadius);

            dl->AddCircleFilled(center, kAnyStateRadius, IM_COL32(140, 120, 50, 255));
            dl->AddCircle      (center, kAnyStateRadius, IM_COL32(255, 220, 80, 255), 0, 2.0f);
            const char*  label = "Any";
            const ImVec2 ts    = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                        IM_COL32_WHITE, label);

            ImGui::SetCursorScreenPos(pos);
            ImGui::InvisibleButton("##any_state", size);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            {
                state.anyStatePos = state.anyStatePos + ImGui::GetIO().MouseDelta;
            }
        }

        // === State 노드 (드래그 가능) ===
        for (size_t i = 0; i < ctrl->states.size(); ++i)
        {
            const auto& s = ctrl->states[i];
            auto it = state.nodePositions.find(s.name);
            if (it == state.nodePositions.end()) { continue; }
            const ImVec2 pos  = canvasPos + it->second;
            const ImVec2 size(kNodeWidth, kNodeHeight);

            const bool isCurrent  = !curStateName.empty() && (curStateName == s.name);
            const bool isDefault  = (ctrl->defaultStateName == s.name)
                                 || (ctrl->defaultStateName.empty() && i == 0);
            const bool isSelected = (static_cast<int>(i) == state.selectedStateIdx);

            const ImU32 fillCol = isCurrent ? IM_COL32(60, 160, 90, 255)
                                : isDefault ? IM_COL32(140, 100, 50, 255)
                                            : IM_COL32(55, 60, 80, 255);
            const ImU32 borderCol = isSelected
                ? IM_COL32(255, 220, 80, 255)
                : IM_COL32(200, 200, 200, 255);

            dl->AddRectFilled(pos, pos + size, fillCol, 6.0f);
            dl->AddRect      (pos, pos + size, borderCol, 6.0f, 0, isSelected ? 3.0f : 1.5f);

            const ImVec2 ts = ImGui::CalcTextSize(s.name.c_str());
            dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f,
                               pos.y + (size.y - ts.y) * 0.5f),
                        IM_COL32_WHITE, s.name.c_str());

            // Hit zone — InvisibleButton 으로 클릭/드래그.
            ImGui::SetCursorScreenPos(pos);
            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##state_node", size);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                state.selectedStateIdx      = static_cast<int>(i);
                state.selectedTransitionIdx = -1;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            {
                it->second = it->second + ImGui::GetIO().MouseDelta;
            }
            ImGui::PopID();
        }

        // === 캔버스 끝 — 다음 위젯이 캔버스 아래로 가도록 cursor 진행.
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::Dummy(canvasSize);

        // === 하단 상태 표시 (선택 정보) ===
        if (state.selectedStateIdx >= 0 &&
            state.selectedStateIdx < static_cast<int>(ctrl->states.size()))
        {
            const auto& sel = ctrl->states[state.selectedStateIdx];
            ImGui::TextDisabled("Selected state: %s  (clip: %s)",
                                sel.name.c_str(), sel.motionClipPath.c_str());
        }
    }
}
