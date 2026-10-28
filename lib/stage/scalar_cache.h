#pragma once

#include "pagos/cache/store.h"
#include "pagos/hir/hir.h"
#include "pagos/stage/cache_config.h"
#include "pagos/stage/cache_identity.h"
#include "pagos/stage/generator_replay_record.h"
#include "pagos/stage/replay_descriptor.h"

#include <chrono>

namespace pagos::stage {

// C2a scalar trees and C2b single scalar-array constructions. No ordinary
// calls, control flow, local contracts, or memory operations are admitted.
struct ScalarQuery {
    cache::QueryKey key;
    SourceAnchor result_anchor;
    std::optional<std::size_t> result_parameter;
    std::vector<SourceAnchor> fuel_sites;
    std::vector<source::Span> fuel_spans;
    // A generator repeats the suffix, including its iteration checkpoint.
    std::size_t repeat_begin{};
    std::size_t repetitions{1};
    std::size_t body_fuel{};
    sema::Type result_type;
    std::optional<std::size_t> construction_after;
    cache::Bytes recipe;
    bool residual{};
    std::optional<ReplayDescriptor> replay;
    std::optional<GeneratorReplayAdmission> generator;
    [[nodiscard]] source::Span fuel_span(std::size_t index) const;
};

// Validate the real evaluator's ordered events without allocating one record
// per iteration. Publication requires the entire template to have matched.
struct ResourceCapture {
    const ScalarQuery* query{};
    std::size_t fuel{};
    std::size_t constructions{};
    bool valid{true};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> generator_range;
    void bounds(std::uint32_t first, std::uint32_t last, source::Span span);
    void consume(source::Span span);
    void construct(std::size_t count, const sema::Type& type,
                   source::Span span);
    [[nodiscard]] bool complete() const;
};

class CacheTimer {
  public:
    CacheTimer(std::uint64_t& destination, bool enabled)
        : destination_(destination), enabled_(enabled) {
        if (enabled_)
            start_ = std::chrono::steady_clock::now();
    }
    ~CacheTimer() { stop(); }
    void stop() {
        if (enabled_)
            destination_ += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start_)
                    .count());
        enabled_ = false;
    }

  private:
    std::uint64_t& destination_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_;
};

struct ScalarCachedResult {
    cache::TypedValue value;
    source::Span span;
    std::size_t body_fuel{};
};

class ScalarCache {
  public:
    ScalarCache(const syntax::Module& module, const sema::TypeTable& types,
                const cache::QueryContext& context, const CacheConfig& config,
                CacheTelemetry& telemetry, bool defer_publication = false);
    void commit(bool permitted);
    [[nodiscard]] std::size_t memory_bytes() const;
    [[nodiscard]] bool has_pending() const { return !pending_.empty(); }
    [[nodiscard]] std::optional<ScalarQuery>
    query(const syntax::Function& function,
          const std::vector<hir::ExprPtr>& arguments, bool residual = false);
    [[nodiscard]] std::optional<ScalarCachedResult>
    load(const ScalarQuery& query, const std::vector<hir::ExprPtr>& arguments);
    void publish(const ScalarQuery& query, const hir::ExprPtr& result,
                 const ResourceCapture& capture, std::size_t entry_depth,
                 std::size_t specializations);

    [[nodiscard]] std::optional<hir::ExprPtr>
    load_residual(const ScalarQuery& query, const syntax::Function& function,
                  const std::vector<hir::ExprPtr>& bindings);
    void publish_residual(const ScalarQuery& query,
                          const syntax::Function& function,
                          const hir::ExprPtr& body,
                          const std::vector<hir::ExprPtr>& bindings,
                          const ResourceCapture& capture);

  private:
    bool admit(const syntax::Expr& expression, const syntax::Function& function,
               const DefinitionIdentity& definition, ScalarQuery& query,
               std::string_view generator_variable = {});
    std::optional<SourceAnchor> anchor(const DefinitionIdentity& definition,
                                       source::Span span) const;

    const syntax::Module& module_;
    const sema::TypeTable& types_;
    cache::QueryContext context_;
    CacheMode mode_;
    CacheEvidence evidence_;
    bool timings_;
    CacheTelemetry& telemetry_;
    cache::FileStore store_;
    std::optional<CacheIdentity> identity_;
    struct Pending {
        cache::QueryKey key;
        cache::Digest semantic;
        std::vector<cache::ArtifactInput> artifacts;
        bool residual{};
    };
    void publish_or_defer(const cache::QueryKey& key,
                          const cache::Digest& semantic,
                          std::span<const cache::ArtifactInput> artifacts,
                          bool residual);
    bool defer_publication_{};
    std::vector<Pending> pending_;
    std::size_t pending_bytes_{};
};

} // namespace pagos::stage
