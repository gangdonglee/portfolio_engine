#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

namespace engine::render
{
    // 본 트리 + 바인드 포즈 인버스(offsetMatrix).
    //
    // 본 한 개:
    //   - name: FBX 노드 이름 (디버그/검색).
    //   - parentIndex: 부모 본의 인덱스 (-1 = 루트).
    //   - offsetMatrix: bind-pose 의 inverse — 정점을 본의 로컬 공간으로 가져감.
    //
    // Animator 가 매 프레임 본 글로벌 변환을 계산하고 offsetMatrix 와 곱해 본 팔레트를 만듦.
    class Skeleton final
    {
    public:
        struct Bone
        {
            std::wstring         name;
            int32                parentIndex = -1;
            DirectX::XMFLOAT4X4  offsetMatrix;   // inverse bind pose (row-major)
        };

        Skeleton() = default;

        std::vector<Bone>& Bones() noexcept             { return m_bones; }
        const std::vector<Bone>& Bones() const noexcept { return m_bones; }
        size_t              BoneCount() const noexcept  { return m_bones.size(); }

        // 이름으로 본 인덱스 검색. 없으면 -1.
        int32 FindIndex(const std::wstring& boneName) const noexcept;

    private:
        std::vector<Bone> m_bones;
    };
}
