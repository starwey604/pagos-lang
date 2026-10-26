#pragma once

#include "pagos/stage/function_evidence_view.h"

namespace pagos::driver {
struct RecursiveSummaryOptions {
    std::string name;
    std::optional<cache::Digest> key;
    std::string channel{"both"};
    std::optional<std::size_t> max_steps; // Per channel, no saved continuation.
    bool specified{}, channel_set{}, steps_set{};
};
[[nodiscard]] bool recursive_evidence_mode(std::string_view mode);
[[nodiscard]] std::expected<void, std::string>
parse_recursive_summary_option(std::string_view argument,
                               RecursiveSummaryOptions& options);
[[nodiscard]] std::expected<void, std::string>
validate_recursive_summary_options(const RecursiveSummaryOptions& options,
                                   std::string_view mode, std::string_view root,
                                   bool query_options);
// 0: completed/catalog (possibly unresolved); 3: budget exhausted, restart
// required; 1: failure. No task-store writes or consumer acknowledgement.
[[nodiscard]] int run_recursive_summaries(
    const stage::FunctionEvidenceView& evidence, std::string_view source,
    std::string_view mode, std::size_t slice,
    const RecursiveSummaryOptions& options, bool stage_ran, bool statistics);
} // namespace pagos::driver
