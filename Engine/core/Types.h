#pragma once

#include <cstdint>

// 학습 자료의 short alias 패턴 채택 (CODE_STYLE.md §3-3).
// engine 의 하위 네임스페이스에서는 prefix 없이 사용 가능 (예: engine::render 안에서 `uint32 x;`).
// 외부에서는 `engine::uint32` 로 명시.
namespace engine
{
    using int8  = std::int8_t;
    using int16 = std::int16_t;
    using int32 = std::int32_t;
    using int64 = std::int64_t;
    using uint8  = std::uint8_t;
    using uint16 = std::uint16_t;
    using uint32 = std::uint32_t;
    using uint64 = std::uint64_t;
}
