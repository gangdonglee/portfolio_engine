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

    // Inspector 1프레임 결과.
    //   changed     — 어떤 필드라도 변경됨 (modified flag / Save 활성화).
    //   needRebuild — *path* 필드 변경 (mesh/animator/controller) 또는 drag-drop 으로
    //                 자산 교체 발생. 호출자가 SceneRuntime 재빌드 트리거 필요.
    struct InspectorResult
    {
        bool changed     = false;
        bool needRebuild = false;
    };

    // Inspector 패널 1프레임. selection 의 노드 타입별 위젯.
    InspectorResult DrawInspector(engine::scene::Scene& scene, Selection& selection);
}
