#pragma once

#include "pagos/cache/store.h"
#include "pagos/stage/replay_descriptor.h"

namespace pagos::stage {
inline constexpr std::uint32_t scalar_replay_evidence_version = 1;
enum class ScalarReplayErrorCode {
    MissingDescriptor,
    UnsupportedSource,
    InvalidDescriptor,
    StorageFailure
};
struct ScalarReplayError {
    ScalarReplayErrorCode code;
    std::optional<ReplayError> descriptor;
    std::optional<cache::StoreError> storage;
};
struct ScalarReplayRecord {
    ReplayDescriptor descriptor;
    cache::TypedValue value;
    cache::ObjectRef object;
};
// A distinct versioned capability index, never a relabelled C2 value hit.
[[nodiscard]] cache::QueryKey scalar_replay_key(cache::QueryKey specialization);
// Checked AST/types are needed only for admission and source correspondence;
// the returned record owns no AST/Analyzer state. No Stage or recipe execution,
// no writes, and no fallback synthesized from a folded value.
[[nodiscard]] std::expected<ScalarReplayRecord, ScalarReplayError>
load_scalar_replay(const cache::FileStore& store, const syntax::Module& module,
                   const sema::TypeTable& types,
                   const cache::QueryKey& specialization,
                   cache::Limits limits = {});
} // namespace pagos::stage
