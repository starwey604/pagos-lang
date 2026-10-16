#pragma once

#include "pagos/stage/evidence_view.h"

namespace pagos::stage::provenance {

enum class EvidenceBuildMode { Memory, Spill };
enum class SpillFailurePolicy { KeepMemory, Unavailable };
struct EvidenceBuildConfig {
    EvidenceBuildMode mode{EvidenceBuildMode::Memory};
    // Explicit spill namespace parent, independent of semantic cache settings.
    std::filesystem::path directory;
    std::size_t block_bytes{64 * 1024}; // Logical node/edge bytes per segment.
    std::size_t resident_bytes{64 * 1024 * 1024}; // Active + resident + cache.
    std::size_t cache_bytes{1024 * 1024};
    std::size_t disk_bytes{256 * 1024 * 1024}; // CAS payloads + envelopes.
    SpillFailurePolicy failure_policy{SpillFailurePolicy::KeepMemory};
    cache::StoreOptions store;
    bool measure_memory{}; // Opt-in end-of-recording census, never a budget.
};
struct EvidenceBuildStats {
    std::size_t nodes{}, edges{}, summaries{};
    std::size_t sealed_blocks{}, written_blocks{}, fallback_blocks{};
    std::size_t payload_bytes_written{}, charged_disk_bytes{};
    std::size_t objects_read{}, payload_bytes_read{};
    std::size_t active_bytes{}, resident_bytes{}, cached_bytes{};
    // Includes externally pinned decoded blocks and active-node snapshots.
    std::size_t live_bytes{}, peak_live_bytes{}, peak_owned_bytes{};
    // Directory and summary vector capacities; excludes allocator/path costs.
    std::size_t directory_bytes{}, summary_bytes{};
    std::size_t released_active_bytes{};
};
struct EvidenceCleanupStats {
    std::size_t examined{}, reclaimed{}, active{}, skipped{}, errors{};
};
// Explicit maintenance only. Reclaim recognized task directories whose lifetime
// lock is free. Active views/builders and unknown/partial markers are skipped;
// unrelated files are untouched. No startup scan or implicit cache-dir cleanup.
[[nodiscard]] std::expected<EvidenceCleanupStats, EvidenceError>
reclaim_evidence_sessions(const std::filesystem::path& parent);

struct BuiltEvidence {
    std::shared_ptr<const EvidenceView> view;
    EvidenceBuildStats construction;
};

// Single-owner append builder. No public refs into mutable/evictable storage.
// Old blocks and active-node snapshots are accessed through owning leases.
// Forward summary references are reserved now and bound once before freeze.
// IO/capacity failures are auxiliary evidence errors, never Stage diagnostics.
class EvidenceBuilder {
  public:
    explicit EvidenceBuilder(EvidenceBuildConfig config = {});
    ~EvidenceBuilder();
    EvidenceBuilder(EvidenceBuilder&&) noexcept;
    EvidenceBuilder& operator=(EvidenceBuilder&&) noexcept;
    EvidenceBuilder(const EvidenceBuilder&) = delete;
    EvidenceBuilder& operator=(const EvidenceBuilder&) = delete;

    [[nodiscard]] std::expected<NodeId, EvidenceError>
    add(Rule rule, SiteId site, std::span<const Edge> edges = {},
        std::uint32_t payload = 0, SummaryId owner = {});
    [[nodiscard]] std::expected<SummaryId, EvidenceError> reserve_summary();
    [[nodiscard]] std::expected<void, EvidenceError>
    define_summary(SummaryId summary, NodeId root);
    [[nodiscard]] std::expected<NodeLease, EvidenceError> pin(NodeId id);
    [[nodiscard]] std::expected<void, EvidenceError> seal();
    [[nodiscard]] std::expected<BuiltEvidence, EvidenceError> freeze() &&;
    void evict();
    [[nodiscard]] EvidenceBuildStats stats() const;
    [[nodiscard]] const std::optional<EvidenceError>& failure() const;
    [[nodiscard]] const std::optional<EvidenceError>& last_spill_error() const;
    // Empty until a spill is attempted. Owned task directory only; RAII cleanup
    // waits for the builder/frozen view, while decoded leases survive cleanup.
    [[nodiscard]] std::filesystem::path temporary_directory() const;
    [[nodiscard]] bool consumed() const noexcept { return !impl_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    [[nodiscard]] Impl& state();
    [[nodiscard]] const Impl& state() const;
};
} // namespace pagos::stage::provenance
