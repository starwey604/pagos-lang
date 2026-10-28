#pragma once

#include "pagos/stage/function_evidence_view.h"

namespace pagos::driver {
struct QueryTaskOptions;
struct RecursiveSummaryOptions {
    std::string name;
    std::optional<cache::Digest> key;
    std::string channel{"both"};
    std::optional<std::size_t> max_steps; // Per channel/invocation.
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
// Fresh: 0 completed/catalog, 3 budget exhausted (restart). Persistent: 0
// acknowledged/cancelled, 3 saved work or result delivery remains. 1 failure.
[[nodiscard]] int
run_recursive_summaries(const stage::FunctionEvidenceView& evidence,
                        std::string_view source, std::string_view mode,
                        std::size_t slice,
                        const RecursiveSummaryOptions& options, bool stage_ran,
                        bool statistics, const QueryTaskOptions& query);
} // namespace pagos::driver
