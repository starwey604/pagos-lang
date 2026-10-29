#pragma once

#include "pagos/cache/store.h"
#include "pagos/source/span.h"
#include "pagos/stage/provenance_graph.h"

namespace pagos::stage::provenance {

inline constexpr std::uint32_t evidence_block_version = 1;
enum class EvidenceErrorCode {
    InvalidReference,
    MissingBlock,
    CorruptBlock,
    IncompatibleVersion,
    LimitExceeded,
    IoFailure,
    OutOfMemory
};
struct EvidenceError {
    EvidenceErrorCode code;
    std::optional<cache::StoreError> storage;
    std::optional<cache::Error> codec;
};

struct EvidenceSite {
    source::Span span;
    std::string label;
};
// Loaded immutable segment. Optional site_indices has one entry per node,
// indexing sites; node.site itself may be a remapped global ID.
struct EvidenceBlock {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<EvidenceSite> sites;
    std::vector<std::uint32_t> site_indices;
    [[nodiscard]] std::size_t bytes() const;
};

// References/spans obtained from a lease remain valid while ANY copy of that
// lease lives, even after eviction or destruction of the view and store.
class NodeLease {
  public:
    [[nodiscard]] const Node& node() const { return *node_; }
    [[nodiscard]] std::span<const Edge> edges() const { return edges_; }
    [[nodiscard]] const EvidenceSite* site() const { return site_; }

  private:
    friend class EvidenceView;
    friend class EvidenceBuilder;
    NodeLease(std::shared_ptr<const void> owner, const Node* node,
              std::span<const Edge> edges, const EvidenceSite* site = nullptr)
        : owner_(std::move(owner)), node_(node), edges_(edges), site_(site) {}
    std::shared_ptr<const void> owner_;
    const Node* node_;
    std::span<const Edge> edges_;
    const EvidenceSite* site_;
};
struct EvidenceBlockRef {
    NodeId first;
    std::uint32_t nodes{};
    cache::ObjectRef object;
    bool disk_backed{true}; // False for resident segments in a mixed adapter.
};
struct EvidenceReadStats {
    std::size_t requests{};
    std::size_t cache_hits{};
    std::size_t
        objects_read{}; // Disk CAS reads; adapters count validated loads.
    std::size_t payload_bytes_read{}; // Excludes the CAS envelope (49 bytes).
    std::size_t
        cached_bytes{};       // Decoded buffers/heap labels owned by the cache.
    std::size_t live_bytes{}; // Includes evicted blocks retained by leases.
};

// The disk directory uses IDs local to ONE graph, never stable definition
// identities. Its caller must authenticate metadata/required summary closure.
// C4a's function Import/identity/root-manifest layer is separate. Construction
// checks the complete ID directory without opening any cold object. Each first
// read checks the whole referenced CAS object, then validates its block.
class EvidenceView {
  public:
    [[nodiscard]] static std::shared_ptr<const EvidenceView>
    memory(FrozenGraph graph);
    [[nodiscard]] static std::expected<std::shared_ptr<const EvidenceView>,
                                       EvidenceError>
    disk(cache::FileStore store, std::vector<EvidenceBlockRef> blocks,
         std::vector<SummaryDescription> summaries,
         std::size_t cache_bytes = 4UZ * 1024 * 1024,
         cache::Limits limits = {});
    using Loader =
        std::function<std::expected<std::shared_ptr<const EvidenceBlock>,
                                    EvidenceError>(std::size_t)>;
    // Trusted adapter boundary: loader returns fully validated/remapped blocks.
    // Directory ranges still undergo the same bounds/coverage validation.
    [[nodiscard]] static std::expected<std::shared_ptr<const EvidenceView>,
                                       EvidenceError>
    segmented(std::vector<EvidenceBlockRef> blocks,
              std::vector<SummaryDescription> summaries, Loader loader,
              std::size_t cache_bytes = 4UZ * 1024 * 1024,
              cache::Limits limits = {});
    [[nodiscard]] std::expected<NodeLease, EvidenceError> pin(NodeId id) const;
    [[nodiscard]] std::size_t node_count() const;
    [[nodiscard]] std::span<const SummaryDescription> summaries() const;
    [[nodiscard]] const SummaryDescription& summary(SummaryId id) const;
    [[nodiscard]] const FrozenGraph* memory_graph() const;
    [[nodiscard]] EvidenceReadStats read_stats() const;
    [[nodiscard]] std::size_t directory_bytes() const;
    // Drops cache ownership. Leases still own their blocks; a later request
    // may reuse a pinned block. Memory adapters retain the original graph.
    void evict() const;

  private:
    struct Impl;
    explicit EvidenceView(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

// Experimental physical block codec: contiguous graph-local IDs, local edge
// offsets and global graph-local targets/summary owners. No AST addresses or
// persisted query cursors. It does not export a reusable function unit.
[[nodiscard]] std::expected<cache::Bytes, cache::Error>
encode_evidence_block(const FrozenGraph& graph, NodeId first,
                      std::uint32_t nodes, cache::Limits limits = {});
[[nodiscard]] std::expected<cache::Bytes, cache::Error>
encode_evidence_block(const EvidenceBlock& block, NodeId first,
                      std::size_t summaries, cache::Limits limits = {});
[[nodiscard]] std::expected<std::shared_ptr<const EvidenceBlock>, cache::Error>
decode_evidence_block(std::span<const std::uint8_t> bytes,
                      const EvidenceBlockRef& reference, std::size_t summaries,
                      cache::Limits limits = {});

} // namespace pagos::stage::provenance
