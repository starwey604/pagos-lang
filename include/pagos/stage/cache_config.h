#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace pagos::stage {

enum class CacheMode { Off, ReadWrite, ReadOnly };

struct CacheConfig {
    CacheMode mode{CacheMode::Off};
    std::filesystem::path directory;
    bool measure_timings{};
};

// Physical work and storage outcomes, separate from semantic AnalysisStats.
struct CacheTelemetry {
    std::size_t lookups{};
    std::size_t hits{};
    std::size_t misses{};
    std::size_t writes{};
    std::size_t errors{};
    std::size_t rejected{};
    std::size_t budget_fallbacks{};
    std::size_t body_evaluations{};
    std::size_t bodies_skipped{};
    std::size_t residual_lookups{};
    std::size_t residual_hits{};
    std::size_t residual_writes{};
    std::size_t blocked_writes{};
    std::size_t pending_payload_bytes_peak{};
    // Inclusive physical phase times; never part of cache keys or budgets.
    std::uint64_t dependency_ns{};
    std::uint64_t query_ns{};
    std::uint64_t read_ns{};
    std::uint64_t decode_ns{};
    std::uint64_t encode_ns{};
    std::uint64_t publish_ns{};
    std::uint64_t resource_ns{};
    std::uint64_t restore_ns{};
    std::uint64_t evaluate_ns{};
};

} // namespace pagos::stage
