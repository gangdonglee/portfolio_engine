#pragma once

#include <cstddef>

namespace engine::scene
{
    struct Scene;
}

namespace editor::panels
{
    // 선택 트리의 한 노드 식별자.
    enum class NodeKind : int
    {
        None,
        SceneRoot,     // Scene 의 name/ambient/cameraStart 편집
        MeshInstance,  // scene.meshes[index]
        DirLight,      // scene.dirLights[index]
        PointLight,    // scene.pointLights[index]
    };

    struct Selection
    {
        NodeKind     kind  = NodeKind::None;
        std::size_t  index = 0;
    };

    // Hierarchy 패널 1프레임. 트리 표시 + 노드 클릭 → selection 갱신 + 추가/제거 버튼.
    // 반환값: scene 의 *구조* 가 변경됐으면 true (인스턴스/라이트 추가/제거).
    // 단순 필드 편집은 Inspector 의 책임 — 본 함수에선 false 가 정상.
    bool DrawHierarchy(engine::scene::Scene& scene, Selection& selection);

    // Inspector 패널 1프레임. selection 의 노드 타입별 위젯.
    // 반환값: 어떤 필드라도 변경됐으면 true (modified flag — Save 메뉴 활성화 신호 등).
    bool DrawInspector(engine::scene::Scene& scene, Selection& selection);
}
