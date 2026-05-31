#include "AssetBrowser.h"

#include "Panels.h"
#include "scene/Scene.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace editor
{
    namespace
    {
        bool IsMeshFile(const std::filesystem::path& p)
        {
            const std::string ext = p.extension().string();
            return ext == ".fbx" || ext == ".FBX" || ext == ".obj" || ext == ".OBJ";
        }

        bool IsAnimatorFile(const std::filesystem::path& p)
        {
            // *.animator.json — 단일 .json 은 Scene 일 수도 있어 제외.
            const std::string s = p.filename().string();
            static constexpr std::string_view kSuffix{ ".animator.json" };
            return s.size() >= kSuffix.size() &&
                   std::equal(kSuffix.begin(), kSuffix.end(), s.end() - kSuffix.size());
        }

        // 한 디렉토리만 (비재귀) 스캔 — 단순 구조 가정.
        void ScanDir(const std::filesystem::path&             dir,
                     std::vector<std::string>&                out,
                     bool                                  (*filter)(const std::filesystem::path&))
        {
            out.clear();
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec)) { return; }
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (ec) { break; }
                if (!entry.is_regular_file()) { continue; }
                if (!filter(entry.path()))    { continue; }
                // scene JSON 의 path 표기와 일관성 — 현재 작업 디렉토리 기준 상대 경로.
                std::filesystem::path rel = std::filesystem::relative(
                    entry.path(), std::filesystem::current_path(), ec);
                out.push_back((ec ? entry.path() : rel).generic_string());
            }
            std::sort(out.begin(), out.end());
        }

        void Rescan(AssetBrowserState& state)
        {
            ScanDir("Resources/FBX",    state.meshPaths,     IsMeshFile);
            ScanDir("assets/Animators", state.animatorPaths, IsAnimatorFile);
            state.scanned = true;
        }
    }

    bool DrawAssetBrowser(engine::scene::Scene&       scene,
                          editor::panels::Selection&  sel,
                          AssetBrowserState&          state)
    {
        if (!state.scanned) { Rescan(state); }

        bool changed = false;

        if (ImGui::SmallButton("Rescan")) { Rescan(state); }
        ImGui::SameLine();
        if (!state.brushMeshPath.empty())
        {
            ImGui::Text("Brush: %s", std::filesystem::path(state.brushMeshPath).filename().string().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) { state.brushMeshPath.clear(); }
            ImGui::SameLine();
            ImGui::TextDisabled("(Viewport LMB clic → place)");
        }
        else
        {
            ImGui::TextDisabled("(mesh: click → set brush, anim: dbl-click → bind to selected MeshInstance)");
        }
        ImGui::Separator();

        // === Meshes — 단일 클릭 → 브러시 설정 ===
        if (ImGui::TreeNodeEx("Meshes (Resources/FBX)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (state.meshPaths.empty())
            {
                ImGui::TextDisabled("(no scan results — cwd: %s)",
                                    std::filesystem::current_path().string().c_str());
            }
            for (const auto& path : state.meshPaths)
            {
                ImGui::PushID(path.c_str());
                const bool isBrush = (state.brushMeshPath == path);
                const std::string label = std::filesystem::path(path).filename().string();
                if (ImGui::Selectable(label.c_str(), isBrush))
                {
                    // 같은 항목 재클릭 → 브러시 해제 (토글).
                    state.brushMeshPath = isBrush ? std::string{} : path;
                }
                // Drag source — Inspector 의 meshAssetPath / animationClipPath 필드로 드롭.
                // payload 는 null-terminated UTF-8 string.
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload("ASSET_MESH_PATH",
                                              path.c_str(),
                                              path.size() + 1);
                    ImGui::Text("Mesh: %s", label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", path.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        ImGui::Separator();

        // === Animators — 더블 클릭으로 선택 MeshInstance 에 바인딩 ===
        if (ImGui::TreeNodeEx("Animators (assets/Animators)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool hasMeshSel = (sel.kind  == editor::panels::NodeKind::MeshInstance &&
                                     sel.index <  scene.meshes.size());
            if (!hasMeshSel)
            {
                ImGui::TextDisabled("(Hierarchy 에서 MeshInstance 선택 후 dbl-click)");
            }
            if (state.animatorPaths.empty())
            {
                ImGui::TextDisabled("(no scan results)");
            }
            for (const auto& path : state.animatorPaths)
            {
                ImGui::PushID(path.c_str());
                const std::string label = std::filesystem::path(path).filename().string();
                ImGui::Selectable(label.c_str());
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload("ASSET_ANIMATOR_PATH",
                                              path.c_str(),
                                              path.size() + 1);
                    ImGui::Text("Animator: %s", label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", path.c_str());
                }
                if (hasMeshSel && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    scene.meshes[sel.index].animatorControllerPath = path;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        return changed;
    }
}
