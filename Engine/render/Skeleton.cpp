#include "render/Skeleton.h"

namespace engine::render
{
    int32 Skeleton::FindIndex(const std::wstring& boneName) const noexcept
    {
        for (size_t i = 0; i < m_bones.size(); ++i)
        {
            if (m_bones[i].name == boneName)
            {
                return static_cast<int32>(i);
            }
        }
        return -1;
    }
}
