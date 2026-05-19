#include "Panels.h"

#include "scene/Scene.h"

#include "imgui.h"

#include <DirectXMath.h>

#include <cmath>
#include <string>

namespace editor::panels
{
    namespace
    {
        // 노드 선택 헬퍼 — Selectable + selection 비교.
        void SelectableNode(const char* label, NodeKind kind, std::size_t index, Selection& sel)
        {
            const bool isSelected = (sel.kind == kind) && (sel.index == index);
            if (ImGui::Selectable(label, isSelected))
            {
                sel.kind  = kind;
                sel.index = index;
            }
        }

        // 쿼터니언 정규화 — Inspector 의 rotation 편집 후 적용.
        void NormalizeQuaternion(DirectX::XMFLOAT4& q)
        {
            using namespace DirectX;
            const XMVECTOR v = XMVector4Normalize(XMVectorSet(q.x, q.y, q.z, q.w));
            XMStoreFloat4(&q, v);
        }

        // float3 정규화 (라이트 방향).
        void NormalizeFloat3(DirectX::XMFLOAT3& v)
        {
            using namespace DirectX;
            const XMVECTOR n = XMVector3Normalize(XMVectorSet(v.x, v.y, v.z, 0.0f));
            XMStoreFloat3(&v, n);
        }

        // std::string 을 InputText 에 바인딩 — ImGui 의 char-buffer 패턴 우회.
        bool InputStdString(const char* label, std::string& str)
        {
            // 256 chars 임시 버퍼. 이름은 짧음 — 256 충분.
            char buf[256];
            const std::size_t n = (str.size() < sizeof(buf) - 1) ? str.size() : sizeof(buf) - 1;
            std::memcpy(buf, str.data(), n);
            buf[n] = '\0';
            const bool changed = ImGui::InputText(label, buf, sizeof(buf));
            if (changed) { str.assign(buf); }
            return changed;
        }
    }

    bool DrawHierarchy(engine::scene::Scene& scene, Selection& sel)
    {
        bool structureChanged = false;

        ImGui::TextDisabled("Scene: %s", scene.name.c_str());
        ImGui::Separator();

        SelectableNode("[ Scene Root ]", NodeKind::SceneRoot, 0, sel);
        ImGui::Separator();

        // === Mesh Instances ===
        if (ImGui::TreeNodeEx("Meshes", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (std::size_t i = 0; i < scene.meshes.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = scene.meshes[i].name.empty()
                    ? ("MeshInstance " + std::to_string(i))
                    : scene.meshes[i].name;
                SelectableNode(label.c_str(), NodeKind::MeshInstance, i, sel);
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Add MeshInstance"))
            {
                engine::scene::MeshInstance m;
                m.name = "NewMesh";
                m.meshAssetPath = "Resources/FBX/Dragon.fbx";
                scene.meshes.push_back(std::move(m));
                sel.kind  = NodeKind::MeshInstance;
                sel.index = scene.meshes.size() - 1;
                structureChanged = true;
            }
            if (sel.kind == NodeKind::MeshInstance && sel.index < scene.meshes.size())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("- Remove selected"))
                {
                    scene.meshes.erase(scene.meshes.begin() + static_cast<std::ptrdiff_t>(sel.index));
                    sel.kind  = NodeKind::None;
                    structureChanged = true;
                }
            }
            ImGui::TreePop();
        }

        // === Directional Lights ===
        if (ImGui::TreeNodeEx("Directional Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (std::size_t i = 0; i < scene.dirLights.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = scene.dirLights[i].name.empty()
                    ? ("DirLight " + std::to_string(i))
                    : scene.dirLights[i].name;
                SelectableNode(label.c_str(), NodeKind::DirLight, i, sel);
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Add DirLight"))
            {
                engine::scene::DirectionalLight d;
                d.name = "NewDirLight";
                d.directionWS = { 0.0f, -1.0f, 0.0f };
                d.color       = { 1.0f,  1.0f, 1.0f };
                d.intensity   = 1.0f;
                scene.dirLights.push_back(std::move(d));
                sel.kind  = NodeKind::DirLight;
                sel.index = scene.dirLights.size() - 1;
                structureChanged = true;
            }
            if (sel.kind == NodeKind::DirLight && sel.index < scene.dirLights.size())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("- Remove selected"))
                {
                    scene.dirLights.erase(scene.dirLights.begin() + static_cast<std::ptrdiff_t>(sel.index));
                    sel.kind = NodeKind::None;
                    structureChanged = true;
                }
            }
            ImGui::TreePop();
        }

        // === Point Lights ===
        if (ImGui::TreeNodeEx("Point Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (std::size_t i = 0; i < scene.pointLights.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = scene.pointLights[i].name.empty()
                    ? ("PointLight " + std::to_string(i))
                    : scene.pointLights[i].name;
                SelectableNode(label.c_str(), NodeKind::PointLight, i, sel);
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Add PointLight"))
            {
                engine::scene::PointLight p;
                p.name = "NewPointLight";
                p.positionWS = { 0.0f, 50.0f, 0.0f };
                p.color      = { 1.0f, 1.0f, 1.0f };
                p.intensity  = 1.0f;
                p.range      = 100.0f;
                scene.pointLights.push_back(std::move(p));
                sel.kind  = NodeKind::PointLight;
                sel.index = scene.pointLights.size() - 1;
                structureChanged = true;
            }
            if (sel.kind == NodeKind::PointLight && sel.index < scene.pointLights.size())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("- Remove selected"))
                {
                    scene.pointLights.erase(scene.pointLights.begin() + static_cast<std::ptrdiff_t>(sel.index));
                    sel.kind = NodeKind::None;
                    structureChanged = true;
                }
            }
            ImGui::TreePop();
        }

        return structureChanged;
    }

    bool DrawInspector(engine::scene::Scene& scene, Selection& sel)
    {
        bool changed = false;

        switch (sel.kind)
        {
            case NodeKind::None:
            {
                ImGui::TextDisabled("(노드를 선택하세요)");
                break;
            }
            case NodeKind::SceneRoot:
            {
                ImGui::Text("Scene");
                ImGui::Separator();
                changed |= InputStdString("name", scene.name);
                changed |= ImGui::ColorEdit3("ambient", &scene.ambient.x);
                ImGui::Separator();
                ImGui::Text("Camera Start");
                changed |= ImGui::DragFloat3("camera.position", &scene.cameraStart.position.x, 1.0f);
                changed |= ImGui::DragFloat3("camera.target",   &scene.cameraStart.target.x,   1.0f);
                changed |= ImGui::DragFloat ("camera.fovYRad",  &scene.cameraStart.fovYRad, 0.01f, 0.1f, 3.0f);
                break;
            }
            case NodeKind::MeshInstance:
            {
                if (sel.index >= scene.meshes.size()) { ImGui::TextDisabled("(잘못된 인덱스)"); break; }
                auto& m = scene.meshes[sel.index];
                ImGui::Text("MeshInstance [%zu]", sel.index);
                ImGui::Separator();
                changed |= InputStdString("name",              m.name);
                changed |= InputStdString("meshAssetPath",     m.meshAssetPath);
                changed |= InputStdString("animationClipPath", m.animationClipPath);
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##anim"))
                {
                    m.animationClipPath.clear();
                    changed = true;
                }
                if (!m.animationClipPath.empty())
                {
                    ImGui::TextDisabled("(Client F0 = T-pose, 1..4 = clip select)");
                }
                changed |= InputStdString("animatorControllerPath", m.animatorControllerPath);
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##animator"))
                {
                    m.animatorControllerPath.clear();
                    changed = true;
                }
                if (!m.animatorControllerPath.empty())
                {
                    ImGui::TextDisabled("(M0: 로드 + 로그. M1+ 에서 런타임 평가.)");
                }
                ImGui::Separator();
                ImGui::Text("Transform");
                changed |= ImGui::DragFloat3("position", &m.transform.position.x, 1.0f);
                changed |= ImGui::DragFloat3("scale",    &m.transform.scale.x,    0.01f, 0.01f, 100.0f);
                if (ImGui::DragFloat4("rotation (quat)", &m.transform.rotation.x, 0.01f, -1.0f, 1.0f))
                {
                    NormalizeQuaternion(m.transform.rotation);
                    changed = true;
                }
                break;
            }
            case NodeKind::DirLight:
            {
                if (sel.index >= scene.dirLights.size()) { ImGui::TextDisabled("(잘못된 인덱스)"); break; }
                auto& d = scene.dirLights[sel.index];
                ImGui::Text("DirectionalLight [%zu]", sel.index);
                ImGui::Separator();
                changed |= InputStdString("name", d.name);
                if (ImGui::DragFloat3("directionWS", &d.directionWS.x, 0.01f, -1.0f, 1.0f))
                {
                    NormalizeFloat3(d.directionWS);
                    changed = true;
                }
                changed |= ImGui::ColorEdit3("color",     &d.color.x);
                changed |= ImGui::DragFloat ("intensity", &d.intensity, 0.05f, 0.0f, 20.0f);
                break;
            }
            case NodeKind::PointLight:
            {
                if (sel.index >= scene.pointLights.size()) { ImGui::TextDisabled("(잘못된 인덱스)"); break; }
                auto& p = scene.pointLights[sel.index];
                ImGui::Text("PointLight [%zu]", sel.index);
                ImGui::Separator();
                changed |= InputStdString("name", p.name);
                changed |= ImGui::DragFloat3("positionWS", &p.positionWS.x, 1.0f);
                changed |= ImGui::ColorEdit3("color",      &p.color.x);
                changed |= ImGui::DragFloat ("intensity",  &p.intensity, 0.05f, 0.0f, 20.0f);
                changed |= ImGui::DragFloat ("range",      &p.range,     1.0f,  0.1f, 10000.0f);
                break;
            }
        }

        return changed;
    }
}
