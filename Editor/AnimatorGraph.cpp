#include "AnimatorGraph.h"

#include "anim/AnimatorController.h"

#include "../Client/SceneRuntime.h"

#include "imgui.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace editor
{
    namespace
    {
        constexpr float kNodeWidth      = 160.0f;
        constexpr float kNodeHeight     = 44.0f;
        constexpr float kAnyStateRadius = 30.0f;
        constexpr float kArrowSize      = 12.0f;

        ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }
        ImVec2 operator-(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x - b.x, a.y - b.y); }
        ImVec2 operator*(const ImVec2& a, float s)         { return ImVec2(a.x * s,   a.y * s);   }
        ImVec2 operator/(const ImVec2& a, float s)         { return ImVec2(a.x / s,   a.y / s);   }

        void AutoLayout(const engine::anim::AnimatorController& ctrl,
                        AnimatorGraphState&                     state)
        {
            constexpr int   kCols   = 3;
            constexpr float kColW   = 200.0f;
            constexpr float kRowH   = 80.0f;
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

        ImVec2 BoxEdge(ImVec2 nodeCenter, float w, float h, ImVec2 target)
        {
            const float dx = target.x - nodeCenter.x;
            const float dy = target.y - nodeCenter.y;
            if (std::abs(dx) < 0.001f && std::abs(dy) < 0.001f) { return nodeCenter; }
            const float hw = w * 0.5f;
            const float hh = h * 0.5f;
            const float tx = (std::abs(dx) > 0.001f) ? (hw / std::abs(dx)) : 1e9f;
            const float ty = (std::abs(dy) > 0.001f) ? (hh / std::abs(dy)) : 1e9f;
            const float t  = std::min(tx, ty);
            return ImVec2(nodeCenter.x + dx * t, nodeCenter.y + dy * t);
        }
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
            const float ux = dx / len, uy = dy / len;
            const float px = -uy,      py = ux;
            const ImVec2 base = ImVec2(to.x - ux * size, to.y - uy * size);
            const ImVec2 p1 = ImVec2(base.x + px * size * 0.45f, base.y + py * size * 0.45f);
            const ImVec2 p2 = ImVec2(base.x - px * size * 0.45f, base.y - py * size * 0.45f);
            dl->AddTriangleFilled(to, p1, p2, color);
        }

        float PointSegmentDist2(ImVec2 p, ImVec2 a, ImVec2 b) noexcept
        {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float len2 = dx * dx + dy * dy;
            if (len2 < 1e-4f)
            {
                const float ax = p.x - a.x, ay = p.y - a.y;
                return ax * ax + ay * ay;
            }
            float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
            t = std::clamp(t, 0.0f, 1.0f);
            const float cx = a.x + t * dx, cy = a.y + t * dy;
            const float ddx = p.x - cx, ddy = p.y - cy;
            return ddx * ddx + ddy * ddy;
        }

        bool InputStdString(const char* label, std::string& str)
        {
            char buf[512];
            const std::size_t n = std::min(str.size(), sizeof(buf) - 1);
            std::memcpy(buf, str.data(), n);
            buf[n] = '\0';
            const bool changed = ImGui::InputText(label, buf, sizeof(buf));
            if (changed) { str.assign(buf); }
            return changed;
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

        bool StringChoiceCombo(const char* label,
                               std::string& selected,
                               const std::vector<std::string>& choices,
                               bool allowEmpty)
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

        bool ConditionOpCombo(const char* label, engine::anim::ConditionOp& op)
        {
            int idx = static_cast<int>(op);
            const char* items[] = { "IfTrue", "IfFalse", "Greater", "Less", "Equals", "NotEquals" };
            if (ImGui::Combo(label, &idx, items, IM_ARRAYSIZE(items)))
            {
                op = static_cast<engine::anim::ConditionOp>(idx);
                return true;
            }
            return false;
        }
    }

    namespace
    {
        // "X.animator.json" → "X.animator.layout.json"
        // (단순 .json → .layout.json 치환; 그 외 확장자도 그대로 .layout.json append)
        std::string DeriveLayoutPath(const std::string& animatorJsonPath)
        {
            std::filesystem::path p{ animatorJsonPath };
            const std::string ext = p.extension().string();
            if (ext == ".json")
            {
                p.replace_extension("");
                p += ".layout.json";
                return p.string();
            }
            return animatorJsonPath + ".layout.json";
        }
    }

    bool LoadAnimatorGraphLayout(const std::string& animatorJsonPath, AnimatorGraphState& state)
    {
        if (animatorJsonPath.empty()) { return false; }
        const std::string layoutPath = DeriveLayoutPath(animatorJsonPath);
        std::ifstream f(layoutPath);
        if (!f.is_open()) { return false; }
        try
        {
            nlohmann::json j;
            f >> j;
            state.nodePositions.clear();
            if (j.contains("nodes") && j["nodes"].is_array())
            {
                for (const auto& n : j["nodes"])
                {
                    const std::string name = n.value("name", std::string{});
                    if (name.empty()) { continue; }
                    state.nodePositions[name] = ImVec2(
                        n.value("x", 0.0f), n.value("y", 0.0f));
                }
            }
            if (j.contains("anyStatePos"))
            {
                state.anyStatePos = ImVec2(
                    j["anyStatePos"].value("x", 20.0f),
                    j["anyStatePos"].value("y", 20.0f));
            }
            state.viewZoom = j.value("viewZoom", 1.0f);
            if (j.contains("viewPan"))
            {
                state.viewPan = ImVec2(
                    j["viewPan"].value("x", 0.0f),
                    j["viewPan"].value("y", 0.0f));
            }
            state.layoutDirty = false;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool SaveAnimatorGraphLayout(const std::string& animatorJsonPath, const AnimatorGraphState& state)
    {
        if (animatorJsonPath.empty()) { return false; }
        const std::string layoutPath = DeriveLayoutPath(animatorJsonPath);
        try
        {
            nlohmann::json j;
            auto& nodes = j["nodes"];
            nodes = nlohmann::json::array();
            for (const auto& [name, pos] : state.nodePositions)
            {
                nodes.push_back({ {"name", name}, {"x", pos.x}, {"y", pos.y} });
            }
            j["anyStatePos"] = { {"x", state.anyStatePos.x}, {"y", state.anyStatePos.y} };
            j["viewZoom"]    = state.viewZoom;
            j["viewPan"]     = { {"x", state.viewPan.x}, {"y", state.viewPan.y} };

            std::ofstream f(layoutPath);
            if (!f.is_open()) { return false; }
            f << j.dump(2);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void DrawAnimatorGraph(engine::anim::AnimatorController* ctrl,
                           const std::string&                animatorJsonPath,
                           client::SceneRuntime*             sceneRuntime,
                           AnimatorGraphState&               state,
                           bool&                             outDirty)
    {
        if (!ctrl)
        {
            ImGui::TextDisabled("(Hierarchy 에서 animator 바인딩된 MeshInstance 선택)");
            return;
        }

        if (state.lastLayoutFor != ctrl->name || state.nodePositions.empty())
        {
            // 1순위: layout JSON 로드 시도. 실패 시 그리드 auto layout.
            const bool loaded = LoadAnimatorGraphLayout(animatorJsonPath, state);
            if (!loaded)
            {
                AutoLayout(*ctrl, state);
            }
            state.lastLayoutFor = ctrl->name;
            state.layoutDirty   = false;
        }

        const ImVec2 canvasPos  = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 16.0f || canvasSize.y < 16.0f) { return; }
        const ImVec2 canvasEnd  = canvasPos + canvasSize;

        // === Coord transforms — world (graph plane) ↔ screen (window pixels) ===
        const float zoom = state.viewZoom;
        auto ToScreen = [&](ImVec2 world) -> ImVec2 {
            return ImVec2(canvasPos.x + state.viewPan.x + world.x * zoom,
                          canvasPos.y + state.viewPan.y + world.y * zoom);
        };
        auto ToWorld = [&](ImVec2 screen) -> ImVec2 {
            return ImVec2((screen.x - canvasPos.x - state.viewPan.x) / zoom,
                          (screen.y - canvasPos.y - state.viewPan.y) / zoom);
        };

        const float nodeW = kNodeWidth      * zoom;
        const float nodeH = kNodeHeight     * zoom;
        const float anyR  = kAnyStateRadius * zoom;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(canvasPos, canvasEnd, true);

        // 배경 + zoom-aware 그리드.
        dl->AddRectFilled(canvasPos, canvasEnd, IM_COL32(35, 35, 40, 255));
        dl->AddRect      (canvasPos, canvasEnd, IM_COL32(80, 80, 80, 255));
        const float gridStep = 40.0f * zoom;
        if (gridStep > 4.0f)
        {
            const float startX = std::fmod(state.viewPan.x, gridStep);
            const float startY = std::fmod(state.viewPan.y, gridStep);
            for (float x = startX < 0 ? startX + gridStep : startX; x < canvasSize.x; x += gridStep)
            {
                dl->AddLine(canvasPos + ImVec2(x, 0.0f),
                            canvasPos + ImVec2(x, canvasSize.y),
                            IM_COL32(50, 50, 55, 255));
            }
            for (float y = startY < 0 ? startY + gridStep : startY; y < canvasSize.y; y += gridStep)
            {
                dl->AddLine(canvasPos + ImVec2(0.0f, y),
                            canvasPos + ImVec2(canvasSize.x, y),
                            IM_COL32(50, 50, 55, 255));
            }
        }

        const std::string curStateName = (sceneRuntime && sceneRuntime->HasAnimatorRuntime())
            ? sceneRuntime->AnimatorCurrentStateName() : std::string{};

        // === Transition lines + hit-test 캐시 ===
        struct SegRec { int ti; ImVec2 from; ImVec2 to; };
        std::vector<SegRec> segCache;
        segCache.reserve(ctrl->transitions.size());
        for (size_t ti = 0; ti < ctrl->transitions.size(); ++ti)
        {
            const auto& t = ctrl->transitions[ti];
            const auto itTo = state.nodePositions.find(t.toStateName);
            if (itTo == state.nodePositions.end()) { continue; }
            const ImVec2 toCenter = ToScreen(itTo->second + ImVec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f));

            ImVec2 fromCenter;
            const bool fromIsAny = t.fromStateName.empty();
            if (fromIsAny)
            {
                fromCenter = ToScreen(state.anyStatePos + ImVec2(kAnyStateRadius, kAnyStateRadius));
            }
            else
            {
                const auto itFrom = state.nodePositions.find(t.fromStateName);
                if (itFrom == state.nodePositions.end()) { continue; }
                fromCenter = ToScreen(itFrom->second + ImVec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f));
            }
            const ImVec2 from = fromIsAny
                ? CircleEdge(fromCenter, anyR, toCenter)
                : BoxEdge   (fromCenter, nodeW, nodeH, toCenter);
            const ImVec2 to = BoxEdge(toCenter, nodeW, nodeH, fromCenter);

            const bool selected = (static_cast<int>(ti) == state.selectedTransitionIdx);
            const ImU32 col = selected ? IM_COL32(255, 220, 80, 255) : IM_COL32(180, 180, 180, 220);
            dl->AddLine(from, to, col, selected ? 3.0f : 2.0f);
            DrawArrowhead(dl, from, to, kArrowSize, col);
            segCache.push_back({ static_cast<int>(ti), from, to });
        }

        // === Any State 노드 ===
        bool dragStartedThisFrame = false;
        {
            const ImVec2 screenPos = ToScreen(state.anyStatePos);
            const ImVec2 sz(anyR * 2.0f, anyR * 2.0f);
            const ImVec2 center = screenPos + ImVec2(anyR, anyR);

            dl->AddCircleFilled(center, anyR, IM_COL32(140, 120, 50, 255));
            dl->AddCircle      (center, anyR, IM_COL32(255, 220, 80, 255), 0, 2.0f);
            const char*  label = "Any";
            const ImVec2 ts    = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                        IM_COL32_WHITE, label);

            ImGui::SetCursorScreenPos(screenPos);
            ImGui::InvisibleButton("##any_state", sz);

            if (ImGui::IsItemActivated() && ImGui::GetIO().KeyShift)
            {
                state.draggingTransitionFromAny = true;
                state.draggingTransitionFromIdx = -1;
                dragStartedThisFrame = true;
            }
            if (ImGui::IsItemActive() && !ImGui::GetIO().KeyShift
                && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            {
                state.anyStatePos = state.anyStatePos + ImGui::GetIO().MouseDelta / zoom;
                state.layoutDirty = true;
            }
        }

        // === State 노드 ===
        int hoveredStateIdx = -1;
        int rightClickedStateIdx = -1;
        bool stateListChanged = false;
        for (size_t i = 0; i < ctrl->states.size(); ++i)
        {
            const auto& s = ctrl->states[i];
            auto it = state.nodePositions.find(s.name);
            if (it == state.nodePositions.end()) { continue; }
            const ImVec2 screenPos = ToScreen(it->second);
            const ImVec2 sz(nodeW, nodeH);

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

            dl->AddRectFilled(screenPos, screenPos + sz, fillCol, 6.0f);
            dl->AddRect      (screenPos, screenPos + sz, borderCol, 6.0f, 0, isSelected ? 3.0f : 1.5f);

            const ImVec2 ts = ImGui::CalcTextSize(s.name.c_str());
            dl->AddText(ImVec2(screenPos.x + (sz.x - ts.x) * 0.5f,
                               screenPos.y + (sz.y - ts.y) * 0.5f),
                        IM_COL32_WHITE, s.name.c_str());

            ImGui::SetCursorScreenPos(screenPos);
            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##state_node", sz);

            if (ImGui::IsItemHovered()) { hoveredStateIdx = static_cast<int>(i); }

            // Left click selection (Shift 안 누름)
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyShift)
            {
                state.selectedStateIdx      = static_cast<int>(i);
                state.selectedTransitionIdx = -1;
            }
            // Shift+LMB activate → transition 생성 모드.
            if (ImGui::IsItemActivated() && ImGui::GetIO().KeyShift)
            {
                state.draggingTransitionFromAny = false;
                state.draggingTransitionFromIdx = static_cast<int>(i);
                dragStartedThisFrame = true;
            }
            // 일반 드래그 → 위치 이동 (Shift 안 눌렸을 때만).
            if (ImGui::IsItemActive() && !ImGui::GetIO().KeyShift
                && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            {
                it->second = it->second + ImGui::GetIO().MouseDelta / zoom;
                state.layoutDirty = true;
            }

            // 우클릭 → 노드 컨텍스트 메뉴.
            if (ImGui::BeginPopupContextItem("##node_ctx"))
            {
                rightClickedStateIdx = static_cast<int>(i);
                ImGui::Text("State: %s", s.name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Set as Default"))
                {
                    ctrl->defaultStateName = s.name;
                    outDirty = true;
                }
                if (ImGui::MenuItem("Delete State"))
                {
                    ctrl->states.erase(ctrl->states.begin() + static_cast<std::ptrdiff_t>(i));
                    state.nodePositions.erase(s.name);
                    state.selectedStateIdx = -1;
                    outDirty = true;
                    state.layoutDirty = true;
                    stateListChanged = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (stateListChanged) { break; }   // 인덱스 깨짐 — 다음 프레임에 다시 그림
        }

        // === Transition 생성 드래그 — 진행 중 임시 화살표 ===
        if (state.draggingTransitionFromIdx >= 0 || state.draggingTransitionFromAny)
        {
            ImVec2 startCenter{};
            bool   validStart   = false;
            if (state.draggingTransitionFromAny)
            {
                startCenter = ToScreen(state.anyStatePos + ImVec2(kAnyStateRadius, kAnyStateRadius));
                validStart  = true;
            }
            else if (state.draggingTransitionFromIdx >= 0
                  && state.draggingTransitionFromIdx < static_cast<int>(ctrl->states.size()))
            {
                const auto& srcName = ctrl->states[state.draggingTransitionFromIdx].name;
                auto it = state.nodePositions.find(srcName);
                if (it != state.nodePositions.end())
                {
                    startCenter = ToScreen(it->second + ImVec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f));
                    validStart  = true;
                }
            }
            if (validStart)
            {
                const ImVec2 cursor = ImGui::GetMousePos();
                dl->AddLine(startCenter, cursor, IM_COL32(255, 220, 80, 220), 2.5f);
                DrawArrowhead(dl, startCenter, cursor, kArrowSize, IM_COL32(255, 220, 80, 220));
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (validStart && hoveredStateIdx >= 0)
                {
                    engine::anim::AnimatorTransition nt;
                    nt.fromStateName     = state.draggingTransitionFromAny
                        ? std::string{}
                        : ctrl->states[state.draggingTransitionFromIdx].name;
                    nt.toStateName       = ctrl->states[hoveredStateIdx].name;
                    nt.crossfadeDuration = 0.2f;
                    ctrl->transitions.push_back(std::move(nt));
                    state.selectedTransitionIdx = static_cast<int>(ctrl->transitions.size()) - 1;
                    state.selectedStateIdx      = -1;
                    outDirty = true;
                }
                state.draggingTransitionFromAny = false;
                state.draggingTransitionFromIdx = -1;
            }
        }

        // === 화살표 hit-test — 드래그 생성 중 아닐 때만 ===
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered()
            && state.draggingTransitionFromIdx < 0
            && !state.draggingTransitionFromAny
            && !dragStartedThisFrame)
        {
            const ImVec2 mp = ImGui::GetMousePos();
            const bool inCanvas =
                mp.x >= canvasPos.x && mp.x <= canvasEnd.x &&
                mp.y >= canvasPos.y && mp.y <= canvasEnd.y;
            if (inCanvas)
            {
                constexpr float kHitDist2 = 36.0f;
                int   bestIdx  = -1;
                float bestDist = kHitDist2;
                for (const auto& seg : segCache)
                {
                    const float d2 = PointSegmentDist2(mp, seg.from, seg.to);
                    if (d2 < bestDist)
                    {
                        bestDist = d2;
                        bestIdx  = seg.ti;
                    }
                }
                if (bestIdx >= 0)
                {
                    state.selectedTransitionIdx = bestIdx;
                    state.selectedStateIdx      = -1;
                }
                else
                {
                    state.selectedTransitionIdx = -1;
                    state.selectedStateIdx      = -1;
                }
            }
        }

        // === Wheel zoom (마우스 커서 중심) + Middle button pan ===
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const ImVec2 mp           = ImGui::GetMousePos();
                const ImVec2 worldBefore  = ToWorld(mp);
                float        newZoom      = state.viewZoom * (wheel > 0 ? 1.1f : 1.0f / 1.1f);
                newZoom = std::clamp(newZoom, 0.3f, 3.0f);
                state.viewZoom = newZoom;
                // newZoom 으로 worldBefore 가 같은 스크린 mp 에 매핑되도록 pan 재계산.
                state.viewPan = ImVec2(mp.x - canvasPos.x - worldBefore.x * newZoom,
                                       mp.y - canvasPos.y - worldBefore.y * newZoom);
                state.layoutDirty = true;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f))
            {
                state.viewPan = state.viewPan + ImGui::GetIO().MouseDelta;
                state.layoutDirty = true;
            }
        }

        // === 빈 캔버스 우클릭 → 'Add State' 팝업 ===
        constexpr const char* kCanvasPopupId = "##canvas_ctx";
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
            && !ImGui::IsAnyItemHovered()
            && rightClickedStateIdx < 0)
        {
            const ImVec2 mp = ImGui::GetMousePos();
            if (mp.x >= canvasPos.x && mp.x <= canvasEnd.x &&
                mp.y >= canvasPos.y && mp.y <= canvasEnd.y)
            {
                ImGui::OpenPopup(kCanvasPopupId);
            }
        }
        if (ImGui::BeginPopup(kCanvasPopupId))
        {
            if (ImGui::MenuItem("Add State"))
            {
                engine::anim::AnimatorState ns;
                ns.name = "NewState" + std::to_string(ctrl->states.size());
                const ImVec2 mpWorld = ToWorld(ImGui::GetMousePosOnOpeningCurrentPopup());
                state.nodePositions[ns.name] = mpWorld;
                ctrl->states.push_back(std::move(ns));
                outDirty = true;
                state.layoutDirty = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset View"))
            {
                state.viewPan  = ImVec2(0.0f, 0.0f);
                state.viewZoom = 1.0f;
                state.layoutDirty = true;
            }
            ImGui::EndPopup();
        }

        dl->PopClipRect();

        // === 캔버스 끝 — 다음 위젯이 캔버스 아래로 가도록 cursor 진행.
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::Dummy(canvasSize);

        // === 선택된 transition 의 inline editor ===
        if (state.selectedTransitionIdx >= 0 &&
            state.selectedTransitionIdx < static_cast<int>(ctrl->transitions.size()))
        {
            auto& t = ctrl->transitions[state.selectedTransitionIdx];
            ImGui::Separator();
            const std::string from = t.fromStateName.empty() ? "[Any]" : t.fromStateName;
            ImGui::Text("Transition: %s -> %s", from.c_str(), t.toStateName.c_str());

            const auto stateNames = CollectStateNames(*ctrl);
            const auto paramNames = CollectParamNames(*ctrl);

            if (StringChoiceCombo("fromState (empty=AnyState)", t.fromStateName, stateNames, /*allowEmpty*/true))  { outDirty = true; }
            if (StringChoiceCombo("toState",                    t.toStateName,   stateNames, /*allowEmpty*/false)) { outDirty = true; }
            if (ImGui::DragFloat("crossfadeDuration", &t.crossfadeDuration, 0.01f, 0.0f, 2.0f))     { outDirty = true; }
            if (ImGui::Checkbox ("hasExitTime", &t.hasExitTime))                                    { outDirty = true; }
            if (t.hasExitTime &&
                ImGui::DragFloat("exitTime",    &t.exitTime, 0.005f, 0.0f, 1.0f))                   { outDirty = true; }

            ImGui::TextDisabled("Conditions (AND):");
            for (size_t ci = 0; ci < t.conditions.size(); ++ci)
            {
                ImGui::PushID(static_cast<int>(ci));
                auto& c = t.conditions[ci];
                if (StringChoiceCombo("param", c.parameterName, paramNames, /*allowEmpty*/false)) { outDirty = true; }
                if (ConditionOpCombo("op", c.op))                                                 { outDirty = true; }
                if (ImGui::DragFloat("value", &c.value, 0.01f))                                   { outDirty = true; }
                if (ImGui::SmallButton("Remove cond"))
                {
                    t.conditions.erase(t.conditions.begin() + static_cast<std::ptrdiff_t>(ci));
                    outDirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Add Condition"))
            {
                engine::anim::TransitionCondition nc;
                nc.op    = engine::anim::ConditionOp::IfTrue;
                nc.value = 0.0f;
                t.conditions.push_back(std::move(nc));
                outDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove Transition"))
            {
                ctrl->transitions.erase(ctrl->transitions.begin() +
                                        static_cast<std::ptrdiff_t>(state.selectedTransitionIdx));
                state.selectedTransitionIdx = -1;
                outDirty = true;
            }
        }
        else if (state.selectedStateIdx >= 0 &&
                 state.selectedStateIdx < static_cast<int>(ctrl->states.size()))
        {
            const auto& sel = ctrl->states[state.selectedStateIdx];
            ImGui::Separator();
            ImGui::TextDisabled("Selected state: %s  (clip: %s)",
                                sel.name.c_str(), sel.motionClipPath.c_str());
            ImGui::TextDisabled("(state 상세 편집은 Animator 패널 → States 섹션)");
        }
        else
        {
            ImGui::Separator();
            ImGui::TextDisabled("Tip:");
            ImGui::TextDisabled("  - 노드 좌클릭 = 선택  /  노드 좌드래그 = 이동");
            ImGui::TextDisabled("  - Shift+좌드래그 노드 → 다른 노드 = 새 transition 생성");
            ImGui::TextDisabled("  - 화살표 좌클릭 = transition 선택 + 아래 inline editor");
            ImGui::TextDisabled("  - 우클릭 (빈 영역) = Add State / Reset View");
            ImGui::TextDisabled("  - 우클릭 (노드) = Set as Default / Delete");
            ImGui::TextDisabled("  - 마우스 휠 = zoom (커서 중심)  /  중클릭 드래그 = pan");
            ImGui::Text("zoom=%.2f  pan=(%.0f,%.0f)  layout=%s",
                        state.viewZoom, state.viewPan.x, state.viewPan.y,
                        state.layoutDirty ? "DIRTY (Save 클릭 시 .layout.json 같이 저장)" : "saved");
        }

        // Layout 변경 시 Animator 패널 Save 활성화 — Save 시 layout JSON 도 같이 기록.
        if (state.layoutDirty) { outDirty = true; }

        // 자동 저장 — 사용자가 마우스 다 놓은 시점에 layout JSON 즉시 기록.
        // 드래그 도중에는 IO 부담 + 임시 위치 저장 회피 → 모든 마우스 버튼 release 일 때만.
        if (state.layoutDirty
            && !ImGui::IsMouseDown(ImGuiMouseButton_Left)
            && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)
            && !animatorJsonPath.empty())
        {
            if (SaveAnimatorGraphLayout(animatorJsonPath, state))
            {
                state.layoutDirty = false;
            }
        }
    }
}
