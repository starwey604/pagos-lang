#pragma once

#include "pagos/stage/cache_identity.h"
#include "pagos/stage/provenance_analysis.h"
#include <map>
#include <tuple>

namespace pagos::stage {
inline constexpr std::uint32_t function_evidence_version = 1;
// Zero denotes module scope. Low IDs refer to this SCC's members (1-based);
// the high bit denotes an Import slot (also 1-based). No hash cycles.
using EvidenceScopeRef = std::uint32_t;
struct EvidenceImport {
    cache::ObjectRef summary;
    std::uint32_t member{}; // Zero-based index in the referenced SCC.
    cache::Digest key;
};
struct EvidenceDecision {
    provenance::NodeId node; // Member-local ID.
    hir::Stage result;
    StageRule rule;
    StageReason reason;
    std::optional<std::uint32_t> member;
    EvidenceScopeRef scope{};
    source::Span
        span; // Resolved from SourceAnchor, never persisted as offsets.
};
struct EvidenceRoot {
    std::string name;
    provenance::NodeId node;
    hir::Stage stage;
    bool requires_comptime{};
    EvidenceScopeRef scope{};
    source::Span span;
};
struct EvidenceMember {
    cache::QueryKey key;
    std::uint32_t nodes{};
    provenance::NodeId value;
    provenance::NodeId effects;
    // Third logical entry: explicitly recorded P0 decisions, not inferred
    // Stage.
    std::vector<EvidenceDecision> decisions;
    std::vector<EvidenceRoot> roots; // Module/call-site evidence only.
    bool recorded_complete{}; // Local Incomplete absence, not solver precision.
};
struct EvidenceGroup {
    cache::ObjectRef object;
    cache::ObjectRef
        detail; // Optional cold topology; failure is detail unavailability.
    std::vector<EvidenceImport> imports;
    std::vector<EvidenceMember> members;
};
struct EvidenceMemberDetail {
    std::vector<provenance::Node> nodes;
    std::vector<provenance::Edge> edges;
    std::vector<ProvenanceSite> sites;
    // Nodes/edges are member-local; Call payload and Parameter owner are
    // EvidenceScopeRef values. A query adapter must remap them before use.
};
struct EvidenceCatalogEntry {
    cache::ObjectRef summary;
    std::uint32_t member{};
    cache::Digest key;
};

// A validated immutable hot closure. Opening reads required summaries only.
// Missing/corrupt summaries reject the snapshot; cold detail failures do not
// invalidate its keys, ports or recorded decisions. Native disk queries/CLI
// are integrated separately in C4a3; this is not a Stage reuse hit API.
class FunctionEvidence {
  public:
    [[nodiscard]] static cache::QueryKey
    key_for(const CacheIdentity& identity, const cache::QueryContext& context);
    [[nodiscard]] static std::expected<cache::PublishResult, cache::StoreError>
    publish(const cache::FileStore& store, const ProvenanceAnalysis& analysis,
            const CacheIdentity& identity, const cache::QueryContext& context,
            cache::Limits limits = {});
    [[nodiscard]] static std::expected<FunctionEvidence, cache::StoreError>
    load(cache::FileStore store, CacheIdentity identity,
         const cache::QueryContext& context, cache::Limits limits = {});
    [[nodiscard]] std::span<const EvidenceCatalogEntry> catalog() const {
        return catalog_;
    }
    [[nodiscard]] std::span<const EvidenceGroup> groups() const {
        return groups_;
    }
    [[nodiscard]] const EvidenceMember&
    member(const EvidenceCatalogEntry& entry) const;
    [[nodiscard]] std::expected<std::vector<EvidenceMemberDetail>,
                                cache::StoreError>
    read_detail(const EvidenceGroup& group) const;
    [[nodiscard]] std::expected<source::Span, cache::Error>
    resolve_anchor(const SourceAnchor& anchor) const;
    [[nodiscard]] const cache::ObjectRef& root_object() const { return root_; }
    // Current anchor-to-offset binding, separate from the semantic root.
    [[nodiscard]] std::expected<cache::Digest, cache::Error>
    position_fingerprint(cache::Limits limits = {}) const;
    [[nodiscard]] std::size_t metadata_objects_read() const {
        return metadata_objects_;
    }
    [[nodiscard]] std::size_t metadata_bytes_read() const {
        return metadata_bytes_;
    }

  private:
    FunctionEvidence(cache::FileStore store, CacheIdentity identity,
                     cache::Limits limits);
    cache::FileStore store_;
    CacheIdentity identity_;
    cache::Limits limits_;
    cache::ObjectRef root_;
    std::map<std::tuple<unsigned, std::string, std::uint32_t>, std::size_t>
        anchors_;
    std::vector<EvidenceCatalogEntry> catalog_;
    std::vector<EvidenceGroup> groups_;
    std::size_t metadata_objects_{};
    std::size_t metadata_bytes_{};
};
} // namespace pagos::stage
