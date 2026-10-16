#pragma once

#include "pagos/cache/store.h"
#include "pagos/stage/cache_identity.h"
#include "pagos/stage/provenance_analysis.h"

namespace pagos::stage {
inline constexpr std::uint32_t provenance_format_version = 1;
inline constexpr std::uint32_t provenance_record_model =
    1; // P0, not P1 history.
enum class EvidenceSection : std::uint32_t {
    Identity = 1,
    Root,
    Summary,
    Node,
    Edge,
    Decision,
    Site,
    String
};
struct EvidenceSectionInfo {
    EvidenceSection kind;
    bool required;
    std::uint64_t offset;
    std::uint64_t size;
};
struct SnapshotArtifacts {
    cache::QueryKey key;
    cache::Bytes graph;
    cache::Bytes positions;
    cache::Digest semantic;
};
// Owns immutable bytes and the validated section directory. First opening
// validates the whole checksum. Restoration currently decodes the full graph;
// borrowed section spans live as long as this handle or its copies.
class ProvenanceSnapshot {
  public:
    [[nodiscard]] static std::expected<SnapshotArtifacts, cache::Error>
    encode(const ProvenanceAnalysis& analysis, const CacheIdentity& identity,
           const cache::QueryContext& context, const source::SourceFile& source,
           cache::Limits limits = {});
    [[nodiscard]] static std::expected<ProvenanceSnapshot, cache::Error>
    open(cache::Bytes bytes, cache::Limits limits = {});
    [[nodiscard]] std::span<const EvidenceSectionInfo> sections() const;
    [[nodiscard]] std::span<const std::uint8_t>
    section(EvidenceSection kind) const;
    [[nodiscard]] const cache::QueryKey& key() const { return key_; }
    [[nodiscard]] std::expected<std::shared_ptr<const ProvenanceAnalysis>,
                                cache::Error>
    restore(const CacheIdentity& identity,
            const cache::QueryContext& context) const;
    [[nodiscard]] static cache::QueryKey
    key_for(const CacheIdentity& identity, const cache::QueryContext& context);
    [[nodiscard]] static std::expected<void, cache::Error>
    validate_positions(std::span<const std::uint8_t> bytes,
                       const cache::Digest& semantic,
                       cache::Limits limits = {});

  private:
    std::shared_ptr<const cache::Bytes> bytes_;
    std::vector<EvidenceSectionInfo> sections_;
    cache::QueryKey key_;
    cache::Limits limits_;
};
[[nodiscard]] std::expected<cache::PublishResult, cache::StoreError>
publish_snapshot(const cache::FileStore& store,
                 const SnapshotArtifacts& artifacts);
[[nodiscard]] std::expected<std::shared_ptr<const ProvenanceAnalysis>,
                            cache::StoreError>
load_snapshot(const cache::FileStore& store, const CacheIdentity& identity,
              const cache::QueryContext& context);
} // namespace pagos::stage
