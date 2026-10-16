#pragma once

#include <cstddef>

namespace pagos::stage {

// Capacity census, not RSS. Includes vector capacity, map value objects and
// bucket arrays, plus owned heap payloads. Excludes map links, control blocks,
// allocator overhead, codec temporaries and the census's own traversal index.
struct ProvenanceMemoryStats {
    std::size_t sites_bytes{}, roots_bytes{}, decisions_bytes{}, scopes_bytes{};
    std::size_t specialization_bytes{}, identity_bytes{};
    std::size_t view_directory_bytes{}, graph_bytes{}, intern_bytes{};
    bool recorder_measured{};
    std::size_t recorder_maps_bytes{}, recorder_frames_bytes{};
    std::size_t definition_identity_bytes{};
    std::size_t memo_entries{}, expired_memo_entries{}, parameter_entries{};
    std::size_t memo_peak_entries{}, pruned_memo_entries{};
};
struct StageWorkingMemory {
    std::size_t tables_bytes{}, arguments_bytes{}, cache_bytes{}, entries{};
};
struct StageMemoryStats {
    bool measured{};
    // Sampled immediately before/after completion cleanup, not a peak census.
    StageWorkingMemory before_release, after_release;
};

} // namespace pagos::stage
