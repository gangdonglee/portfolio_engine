#include "core/HrCheck.h"

#include <cstdio>
#include <stdexcept>

namespace engine::core
{
    void ThrowIfFailed(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "%s failed: HRESULT=0x%08lX",
                          what,
                          static_cast<unsigned long>(hr));
            throw std::runtime_error(buf);
        }
    }
}
