#pragma once

#include "pagos/stage/function_evidence.h"
#include "pagos/stage/provenance_query.h"

namespace pagos::stage {
struct FunctionSummaryEntry {
    provenance::SummaryId id; // Virtual graph-local ID; not a selector key.
    cache::Digest key;        // Full validated specialization QueryKey hash.
};
struct FunctionEvidenceRevision {
    cache::Digest closure, positions, source;
    bool operator==(const FunctionEvidenceRevision&) const = default;
};
// Owns the validated hot closure and a bounded, lease-backed SCC reader.
// IDs below belong to this handle's virtual graph, not persisted identities.
class FunctionEvidenceView {
  public:
    [[nodiscard]] static std::expected<FunctionEvidenceView,
                                       provenance::EvidenceError>
    open(FunctionEvidence evidence, std::size_t cache_bytes = 4 * 1024 * 1024);
    [[nodiscard]] std::shared_ptr<const provenance::EvidenceView> view() const {
        return view_;
    }
    [[nodiscard]] std::span<const ProvenanceRoot> roots() const {
        return roots_;
    }
    [[nodiscard]] std::span<const StageDecision> decisions() const {
        return decisions_;
    }
    [[nodiscard]] std::span<const ProvenanceScope> scopes() const {
        return scopes_;
    }
    [[nodiscard]] std::span<const FunctionSummaryEntry>
    summary_catalog() const {
        return summaries_;
    }
    // Metadata only. Pass the complete current source used for CacheIdentity;
    // this describes the view, not a forgeable checkpoint authorization.
    [[nodiscard]] std::expected<FunctionEvidenceRevision, cache::Error>
    revision(std::string_view source_text, cache::Limits limits = {}) const;
    [[nodiscard]] std::size_t metadata_objects_read() const {
        return closure_->metadata_objects_read();
    }
    [[nodiscard]] std::size_t metadata_bytes_read() const {
        return closure_->metadata_bytes_read();
    }
    // Pass the complete current source text used to build CacheIdentity.
    // Whitespace-only edits deliberately invalidate saved query tasks too.
    [[nodiscard]] std::expected<provenance::QueryCheckpointBinding,
                                cache::Error>
    checkpoint_binding(std::string_view source_text,
                       cache::Limits limits = {}) const;

  private:
    std::shared_ptr<const FunctionEvidence> closure_;
    std::shared_ptr<const provenance::EvidenceView> view_;
    std::vector<ProvenanceRoot> roots_;
    std::vector<StageDecision> decisions_;
    std::vector<ProvenanceScope> scopes_;
    std::vector<FunctionSummaryEntry> summaries_;
};
// Experimental text interface for fresh exports and independent disk reads.
// IDs never become user-visible comparison identities.
[[nodiscard]] bool
explain_function_evidence(const FunctionEvidenceView& evidence,
                          std::ostream& output, std::ostream& telemetry,
                          std::string_view selected_root = {},
                          std::string_view mode = "sources",
                          std::size_t slice = 4096, bool statistics = false);
} // namespace pagos::stage
