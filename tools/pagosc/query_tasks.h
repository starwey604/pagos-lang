#pragma once

#include "pagos/stage/function_evidence_view.h"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace pagos::driver {
struct QueryTaskOptions {
    std::string directory, action{"start"}, storage{"memory"};
    std::optional<cache::Digest> id;
    std::optional<std::uint64_t> acknowledge;
    std::optional<std::size_t> max_slices{1};
    stage::provenance::QueryKind channel{stage::provenance::QueryKind::Value};
    stage::provenance::QueryPagingConfig paging;
    std::size_t disk_bytes{256UZ * 1024 * 1024};
    std::size_t record_limit{cache::Limits{}.elements};
    bool specified{}, read_only{}, slices_set{}, channel_set{}, paging_set{};
    [[nodiscard]] bool enabled() const { return !directory.empty(); }
};
[[nodiscard]] std::expected<void, std::string>
parse_query_task_option(std::string_view argument, QueryTaskOptions& options);
[[nodiscard]] std::expected<void, std::string>
validate_query_task_options(const QueryTaskOptions& options, bool evidence_read,
                            bool evidence_write, std::string_view root,
                            std::string_view mode);
// 0: completed/cancelled, 3: saved work/delivery remains, 1: failure.
[[nodiscard]] int run_query_task(const stage::FunctionEvidenceView& evidence,
                                 std::string_view source_text,
                                 std::string_view root, std::string_view mode,
                                 std::size_t slice,
                                 const QueryTaskOptions& options,
                                 bool stage_ran, bool statistics = false);
[[nodiscard]] int reclaim_query_tasks(int argc, char** argv);
} // namespace pagos::driver
