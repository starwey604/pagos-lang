#pragma once

#include <cstddef>

namespace pagos::source {

struct Span {
    std::size_t begin{};
    std::size_t end{};

    [[nodiscard]] constexpr bool empty() const noexcept { return begin == end; }
};

[[nodiscard]] constexpr Span merge(Span first, Span second) noexcept {
    return {.begin = first.begin, .end = second.end};
}

} // namespace pagos::source
