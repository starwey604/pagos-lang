#pragma once

#include "pagos/source/source_manager.h"
#include "pagos/stage/scalar_replay_execution.h"

namespace pagos::driver {
struct ScalarReplayOptions {
    std::string directory, function;
    std::vector<std::string> arguments;
    stage::ScalarReplayExecutionLimits limits;
    stage::ScalarReplayDetailRange range;
    std::size_t render_bytes{1024 * 1024};
    std::optional<std::uint32_t> element; // Explicit zero-based array index.
    bool specified{}, statistics{};
};
[[nodiscard]] std::expected<void, std::string>
parse_scalar_replay_option(std::string_view argument,
                           ScalarReplayOptions& options);
// 0 verified, 3 unavailable/limited (restart), 130 cancelled, 2 bad arguments,
// 1 operational failure. No Stage/cache writes or implicit recomputation.
[[nodiscard]] int run_scalar_replay(const syntax::Module& module,
                                    const sema::TypeTable& types,
                                    const cache::QueryContext& context,
                                    const source::SourceFile& source,
                                    const ScalarReplayOptions& options);
} // namespace pagos::driver
