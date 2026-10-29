#include "scalar_cache.h"
#include "generator_replay_record.h"
#include "memory_accounting.h"
#include "pagos/stage/provenance_snapshot.h"
#include "residual_template.h"
#include "scalar_replay.h"

#include "../cache/wire.h"

#include <algorithm>
#include <limits>
#include <new>

namespace pagos::stage {
namespace {

bool scalar(const sema::Type& type) {
    return type.kind == sema::TypeKind::Integer ||
           type.kind == sema::TypeKind::Bool;
}

// This protocol records checkpoints from the evaluator, not a cost estimated
// from the value. Exact equality with the admitted tree also validates every
// structural location before any semantic state is changed on a hit.
std::expected<cache::Bytes, cache::Error>
recipe(const ScalarQuery& query, std::size_t entry_depth = 1,
       std::size_t specializations = 1) {
    const auto anchor = encode_source_anchor(query.result_anchor);
    if (!anchor)
        return std::unexpected(anchor.error());
    cache::wire::Writer writer({});
    cache::wire::header(writer, cache::ObjectKind::ResourceRecipe);
    writer.u32(cache::budget_protocol_version);
    writer.count(entry_depth);
    writer.count(specializations);
    writer.count(anchor->size());
    writer.raw(*anchor);
    writer.boolean(query.result_parameter.has_value());
    if (query.result_parameter)
        writer.count(*query.result_parameter);
    writer.count(query.fuel_sites.size());
    for (const auto& site : query.fuel_sites)
        writer.u32(site.position);
    if (query.result_type.is_array()) {
        // Array extension: a single reservation at this Fuel position, then
        // an ordered repeated suffix. Scalar C2a recipes retain their bytes.
        writer.u8(1);
        writer.count(*query.construction_after);
        writer.count(query.result_type.length);
        writer.count(query.result_type.element_type() == sema::TypeKind::Bool
                         ? 1
                         : query.result_type.integer_type.width / 8);
        writer.count(query.repeat_begin);
        writer.count(query.repetitions);
    }
    if (query.residual) {
        writer.u8(2);
        writer.count(1);
    }
    return writer.finish();
}

std::expected<cache::Digest, cache::Error>
semantic_result(std::span<const std::uint8_t> value,
                std::span<const std::uint8_t> recipe) {
    cache::wire::Writer writer({});
    writer.count(value.size());
    writer.raw(value);
    writer.count(recipe.size());
    writer.raw(recipe);
    const auto bytes = writer.finish();
    if (!bytes)
        return std::unexpected(bytes.error());
    return cache::fingerprint("pagos.pure-scalar-result.v1", *bytes);
}

} // namespace

source::Span ScalarQuery::fuel_span(std::size_t index) const {
    if (index >= repeat_begin)
        index = repeat_begin +
                (index - repeat_begin) % (fuel_spans.size() - repeat_begin);
    return fuel_spans[index];
}

void ResourceCapture::consume(source::Span span) {
    if (!query)
        return;
    if (fuel >= query->body_fuel) {
        valid = false;
        return;
    }
    const auto expected = query->fuel_span(fuel++);
    valid &= expected.begin == span.begin && expected.end == span.end;
    // A construction must occur between the bound/prefix and element steps.
    if (query->construction_after && fuel > *query->construction_after &&
        constructions != 1)
        valid = false;
}

void ResourceCapture::construct(std::size_t count, const sema::Type& type,
                                source::Span span) {
    if (!query)
        return;
    ++constructions;
    valid &= query->construction_after == fuel && constructions == 1 &&
             type == query->result_type && count == type.length &&
             span.begin == query->fuel_spans.front().begin &&
             span.end == query->fuel_spans.front().end;
}

bool ResourceCapture::complete() const {
    return query && valid && fuel == query->body_fuel &&
           constructions == (query->construction_after ? 1U : 0U) &&
           (!query->generator || generator_range.has_value());
}

void ResourceCapture::bounds(std::uint32_t first, std::uint32_t last,
                             source::Span span) {
    if (!query || !query->generator)
        return;
    valid &= !generator_range && last > first &&
             last - first == query->result_type.length &&
             query->construction_after == fuel &&
             span.begin == query->fuel_spans.front().begin &&
             span.end == query->fuel_spans.front().end;
    generator_range = {first, last};
}

ScalarCache::ScalarCache(const syntax::Module& module,
                         const sema::TypeTable& types,
                         const cache::QueryContext& context,
                         const CacheConfig& config, CacheTelemetry& telemetry,
                         bool defer_publication)
    : module_(module), types_(types), context_(context), mode_(config.mode),
      evidence_(config.evidence), timings_(config.measure_timings),
      telemetry_(telemetry), store_(config.directory),
      defer_publication_(defer_publication) {
    CacheTimer timer(telemetry_.dependency_ns, timings_);
    auto identity = build_cache_identity(module, types, context);
    if (identity)
        identity_ = std::move(*identity);
    else
        ++telemetry_.errors;
}

std::optional<SourceAnchor>
ScalarCache::anchor(const DefinitionIdentity& definition,
                    source::Span span) const {
    for (const auto& site : identity_->sites)
        if (site.anchor.definition == definition.id &&
            site.span.begin == span.begin && site.span.end == span.end)
            return site.anchor;
    return std::nullopt;
}

bool ScalarCache::admit(const syntax::Expr& expression,
                        const syntax::Function& function,
                        const DefinitionIdentity& definition,
                        ScalarQuery& query,
                        std::string_view generator_variable) {
    const auto type = types_.expressions.find(&expression);
    if (type == types_.expressions.end() || !scalar(type->second))
        return false;
    const auto site = anchor(definition, expression.span);
    if (!site)
        return false;
    query.fuel_sites.push_back(*site);
    query.fuel_spans.push_back(expression.span);
    using enum syntax::Expr::Kind;
    switch (expression.kind) {
    case Integer:
    case Boolean:
        return true;
    case Name: {
        const auto& name = static_cast<const syntax::NameExpr&>(expression);
        return !types_.global_references.contains(&expression) &&
               (name.name == generator_variable ||
                std::ranges::any_of(function.parameters, [&](const auto& p) {
                    return p.name == name.name;
                }));
    }
    case Unary: {
        const auto& unary = static_cast<const syntax::UnaryExpr&>(expression);
        return (unary.operation == syntax::UnaryOperator::Not ||
                unary.operation == syntax::UnaryOperator::BitNot ||
                unary.operation == syntax::UnaryOperator::Negate) &&
               admit(*unary.operand, function, definition, query,
                     generator_variable);
    }
    case Cast:
        return admit(*static_cast<const syntax::CastExpr&>(expression).operand,
                     function, definition, query, generator_variable);
    case Binary: {
        const auto& binary = static_cast<const syntax::BinaryExpr&>(expression);
        // Conditional execution needs a decision recipe beyond this slice.
        return binary.operation != syntax::BinaryOperator::LogicalAnd &&
               binary.operation != syntax::BinaryOperator::LogicalOr &&
               admit(*binary.left, function, definition, query,
                     generator_variable) &&
               admit(*binary.right, function, definition, query,
                     generator_variable);
    }
    default:
        return false;
    }
}

std::optional<ScalarQuery>
ScalarCache::query(const syntax::Function& function,
                   const std::vector<hir::ExprPtr>& arguments, bool residual) {
    CacheTimer timer(telemetry_.query_ns, timings_);
    try {
        if (!identity_ ||
            function.linkage != syntax::Function::Linkage::Internal ||
            !function.body || !function.body->statements.empty() ||
            !function.body->tail)
            return std::nullopt;
        const auto* definition =
            identity_->find(cache::DefinitionKind::Function, function.name);
        if (!definition ||
            !(scalar(definition->result) ||
              (definition->result.is_array() && definition->result.length &&
               scalar(definition->result.element_type()))))
            return std::nullopt;
        if (residual && !scalar(definition->result))
            return std::nullopt;
        std::vector<cache::StageArgument> staged;
        for (const auto& argument : arguments) {
            if (!scalar(argument->type))
                return std::nullopt;
            if (argument->stage == hir::Stage::Runtime) {
                if (!residual)
                    return std::nullopt;
                staged.push_back({argument->type, cache::ArgumentStage::Runtime,
                                  std::nullopt});
                continue;
            }
            if (!argument->constant)
                return std::nullopt;
            // Array-returning calls do not strip argument work from their
            // parameter bindings today. Preserve that evaluator behavior.
            if (definition->result.is_array() &&
                !hir::is_cacheable_result(argument))
                return std::nullopt;
            staged.push_back({argument->type, cache::ArgumentStage::Comptime,
                              argument->constant});
        }
        const auto key = make_query_key(
            *definition, context_, std::move(staged),
            residual ? cache::QueryKind::SpecializeResidual
                     : cache::QueryKind::EvaluatePureSpecialization);
        const auto result_anchor =
            anchor(*definition, function.body->tail->span);
        if (!key || !result_anchor)
            return std::nullopt;
        ScalarQuery result{.key = *key,
                           .result_anchor = *result_anchor,
                           .result_type = definition->result,
                           .residual = residual};
        const auto& tail = *function.body->tail;
        if (definition->result.is_array()) {
            // The sole construction must be the tail. Elements and bounds
            // only admit scalar arithmetic; no nested allocations or calls.
            result.fuel_sites.push_back(*result_anchor);
            result.fuel_spans.push_back(tail.span);
            if (tail.kind == syntax::Expr::Kind::Array) {
                const auto& array = static_cast<const syntax::ArrayExpr&>(tail);
                if (array.elements.size() != definition->result.length)
                    return std::nullopt;
                result.construction_after = 1;
                for (const auto& element : array.elements)
                    if (!admit(*element, function, *definition, result))
                        return std::nullopt;
            } else if (tail.kind == syntax::Expr::Kind::ArrayGenerator) {
                const auto& generator =
                    static_cast<const syntax::ArrayGeneratorExpr&>(tail);
                if (!generator.body->statements.empty() ||
                    !generator.body->tail ||
                    !admit(*generator.begin, function, *definition, result) ||
                    !admit(*generator.end, function, *definition, result))
                    return std::nullopt;
                result.construction_after = result.fuel_sites.size();
                result.repeat_begin = result.fuel_sites.size();
                result.repetitions = definition->result.length;
                result.fuel_sites.push_back(*result_anchor);
                result.fuel_spans.push_back(tail.span);
                if (!admit(*generator.body->tail, function, *definition, result,
                           generator.variable))
                    return std::nullopt;
            } else {
                return std::nullopt;
            }
        } else if (!admit(tail, function, *definition, result)) {
            return std::nullopt;
        }
        const auto suffix = result.fuel_sites.size() - result.repeat_begin;
        if (result.repetitions >
            (std::numeric_limits<std::size_t>::max() - result.repeat_begin) /
                suffix)
            return std::nullopt;
        result.body_fuel = result.repeat_begin + suffix * result.repetitions;
        if (function.body->tail->kind == syntax::Expr::Kind::Name) {
            const auto& name =
                static_cast<const syntax::NameExpr&>(*function.body->tail);
            const auto parameter = std::ranges::find(
                function.parameters, name.name, &syntax::Parameter::name);
            result.result_parameter = static_cast<std::size_t>(
                parameter - function.parameters.begin());
        }
        auto encoded = recipe(result, 1, residual ? 0 : 1);
        if (!encoded)
            return std::nullopt;
        result.recipe = std::move(*encoded);
        if (!residual && scalar(result.result_type)) {
            auto replay =
                replay_detail::recipe(function, types_, *identity_, result.key);
            if (replay)
                result.replay = std::move(*replay);
        }
        if (!residual && tail.kind == syntax::Expr::Kind::ArrayGenerator) {
            auto generator =
                admit_generator_replay(module_, types_, result.key);
            if (generator)
                result.generator = std::move(*generator);
        }
        return result;
    } catch (const std::bad_alloc&) {
        ++telemetry_.errors;
        return std::nullopt;
    }
}

std::optional<ScalarCachedResult>
ScalarCache::load(const ScalarQuery& query,
                  const std::vector<hir::ExprPtr>& arguments) {
    ++telemetry_.lookups;
    try {
        const auto candidate = [&] {
            CacheTimer timer(telemetry_.read_ns, timings_);
            return store_.lookup(query.key, identity_->module_fingerprint);
        }();
        if (!candidate) {
            if (candidate.error().code != cache::StoreErrorCode::NotFound)
                ++telemetry_.errors;
            ++telemetry_.misses;
            return std::nullopt;
        }
        const cache::Bytes* value = nullptr;
        const cache::Bytes* resources = nullptr;
        const auto reject = [&]() -> std::optional<ScalarCachedResult> {
            ++telemetry_.rejected;
            ++telemetry_.misses;
            return std::nullopt;
        };
        for (const auto& artifact : candidate->artifacts) {
            if (artifact.required && artifact.name != "value" &&
                artifact.name != "resources")
                return reject();
            if (!artifact.required || !artifact.bytes)
                continue;
            if (artifact.name == "value" &&
                artifact.object.kind == cache::ObjectKind::Value)
                value = &*artifact.bytes;
            if (artifact.name == "resources" &&
                artifact.object.kind == cache::ObjectKind::ResourceRecipe)
                resources = &*artifact.bytes;
        }
        // Comparing a canonical recipe rejects unknown versions, trailing
        // data, invalid anchors, wrong origins, and any altered resource cost.
        if (!value || !resources || *resources != query.recipe)
            return reject();
        CacheTimer timer(telemetry_.decode_ns, timings_);
        // Validate the exact shape and payload length before value decoding
        // allocates. Integer values use eight canonical bytes at every width.
        cache::wire::Reader shape(*value, {});
        cache::wire::header(shape, cache::ObjectKind::Value);
        const auto type = cache::wire::type(shape);
        const std::size_t count =
            query.result_type.is_array() ? query.result_type.length : 1;
        const std::size_t width =
            (query.result_type.is_array()
                 ? query.result_type.element_type()
                 : query.result_type) == sema::TypeKind::Bool
                ? 1
                : 8;
        if (!shape.good() || type != query.result_type ||
            count > shape.remaining() / width ||
            shape.remaining() != count * width)
            return reject();
        auto decoded = cache::decode_value(
            *value, {.bytes = value->size(), .elements = count});
        const auto* definition = identity_->find(
            cache::DefinitionKind::Function, query.key.definition.name);
        if (!decoded || !definition || decoded->type != definition->result)
            return reject();
        const auto semantic = semantic_result(*value, *resources);
        if (!semantic || *semantic != candidate->semantic_result)
            return reject();
        if (evidence_ == CacheEvidence::ScalarReplay &&
            scalar(query.result_type)) {
            if (!query.replay) {
                ++telemetry_.replay_rejected;
                return reject();
            }
            auto replay =
                replay_detail::load(store_, *identity_, *query.replay);
            if (!replay || replay->value != *decoded) {
                ++telemetry_.replay_rejected;
                return reject();
            }
            ++telemetry_.replay_hits;
        }
        if (evidence_ == CacheEvidence::GeneratorReplay &&
            query.result_type.is_array()) {
            if (!query.generator) {
                ++telemetry_.replay_rejected;
                return reject();
            }
            auto replay = generator_detail::load(store_, *query.generator);
            if (!replay || replay->value != *decoded) {
                ++telemetry_.replay_rejected;
                return reject();
            }
            ++telemetry_.replay_hits;
        }
        const auto span = identity_->resolve(query.result_anchor);
        if (!span || (query.result_parameter &&
                      *query.result_parameter >= arguments.size()))
            return reject();
        return ScalarCachedResult{std::move(*decoded),
                                  query.result_parameter
                                      ? arguments[*query.result_parameter]->span
                                      : *span,
                                  query.body_fuel};
    } catch (const std::bad_alloc&) {
        ++telemetry_.errors;
        ++telemetry_.misses;
        return std::nullopt;
    }
}

void ScalarCache::publish(const ScalarQuery& query, const hir::ExprPtr& result,
                          const ResourceCapture& capture,
                          std::size_t entry_depth,
                          std::size_t specializations) {
    if (mode_ != CacheMode::ReadWrite)
        return;
    try {
        if (!result || result->kind != hir::Expr::Kind::Constant ||
            !hir::is_cacheable_result(result) ||
            result->type != query.result_type || !capture.complete())
            return;
        CacheTimer timer(telemetry_.encode_ns, timings_);
        const auto resources = recipe(query, entry_depth, specializations);
        if (!resources || *resources != query.recipe)
            return;
        const auto value =
            cache::encode_value({result->type, *result->constant});
        if (!value) {
            ++telemetry_.errors;
            return;
        }
        const std::array artifacts{
            cache::ArtifactInput{"value", cache::ObjectKind::Value, *value},
            cache::ArtifactInput{"resources", cache::ObjectKind::ResourceRecipe,
                                 *resources}};
        const auto semantic = semantic_result(*value, *resources);
        if (!semantic) {
            ++telemetry_.errors;
            return;
        }
        timer.stop();
        publish_or_defer(query.key, *semantic, artifacts, false);
        if (query.generator && capture.generator_range) {
            const auto& generator = query.generator->descriptor();
            auto descriptor = encode_generator_replay_descriptor(generator);
            auto binding = generator_replay_descriptor_fingerprint(generator);
            auto expected = generator_replay_value_fingerprint(
                {result->type, *result->constant});
            if (!descriptor || !binding || !expected) {
                ++telemetry_.errors;
                return;
            }
            const GeneratorReplayObservation observation{
                .descriptor = *binding,
                .first = capture.generator_range->first,
                .last = capture.generator_range->second,
                .expected_result = *expected};
            auto observed = encode_generator_replay_observation(observation);
            if (!observed) {
                ++telemetry_.errors;
                return;
            }
            const std::array evidence{
                cache::ArtifactInput{"value", cache::ObjectKind::Value, *value},
                cache::ArtifactInput{
                    "generator", cache::ObjectKind::GeneratorReplayDescriptor,
                    *descriptor},
                cache::ArtifactInput{
                    "observation",
                    cache::ObjectKind::GeneratorReplayObservation, *observed}};
            publish_or_defer(
                generator_replay_key(query.key),
                generator_detail::semantic(*value, *descriptor, *observed),
                evidence, false);
        }
        if (query.replay) {
            auto replay = *query.replay;
            auto expected =
                replay_value_fingerprint({result->type, *result->constant});
            if (!expected) {
                ++telemetry_.errors;
                return;
            }
            replay.expected_result = *expected;
            auto descriptor = encode_replay_descriptor(replay);
            if (!descriptor) {
                ++telemetry_.errors;
                return;
            }
            const std::array evidence{
                cache::ArtifactInput{"value", cache::ObjectKind::Value, *value},
                cache::ArtifactInput{"replay",
                                     cache::ObjectKind::ReplayDescriptor,
                                     *descriptor}};
            publish_or_defer(scalar_replay_key(query.key),
                             replay_detail::semantic(*value, *descriptor),
                             evidence, false);
        }
    } catch (const std::bad_alloc&) {
        ++telemetry_.errors;
    }
}

std::optional<hir::ExprPtr>
ScalarCache::load_residual(const ScalarQuery& query,
                           const syntax::Function& function,
                           const std::vector<hir::ExprPtr>& bindings) {
    ++telemetry_.lookups;
    ++telemetry_.residual_lookups;
    try {
        CacheTimer read_timer(telemetry_.read_ns, timings_);
        auto candidate =
            store_.lookup(query.key, identity_->module_fingerprint);
        read_timer.stop();
        if (!candidate) {
            ++telemetry_.misses;
            if (candidate.error().code != cache::StoreErrorCode::NotFound)
                ++telemetry_.errors;
            return {};
        }
        CacheTimer decode_timer(telemetry_.decode_ns, timings_);
        const cache::Bytes *body = nullptr, *resources = nullptr,
                           *evidence = nullptr;
        for (const auto& artifact : candidate->artifacts)
            if (artifact.bytes && artifact.required) {
                if (artifact.name == "template" &&
                    artifact.object.kind == cache::ObjectKind::ResidualTemplate)
                    body = &*artifact.bytes;
                if (artifact.name == "resources" &&
                    artifact.object.kind == cache::ObjectKind::ResourceRecipe)
                    resources = &*artifact.bytes;
                if (artifact.name == "evidence" &&
                    artifact.object.kind ==
                        cache::ObjectKind::ProvenanceSnapshot)
                    evidence = &*artifact.bytes;
            }
        if (!body || !resources || !evidence || *resources != query.recipe ||
            candidate->semantic_result !=
                residual_digest(*body, *resources, *evidence))
            throw cache::Error::InvalidValue;
        const auto restored = decode_residual_template(
            *body, function, bindings, *identity_, types_);
        if (!restored)
            throw restored.error();
        const auto snapshot = ProvenanceSnapshot::open(*evidence);
        if (!snapshot)
            throw snapshot.error();
        const auto graph = snapshot->restore(*identity_, context_);
        if (!graph)
            throw graph.error();
        const auto expected = residual_evidence(*restored, function, bindings,
                                                *identity_, context_);
        if (!expected || *expected != *evidence)
            throw cache::Error::InvalidValue;
        return *restored;
    } catch (const std::bad_alloc&) {
        ++telemetry_.errors;
    } catch (cache::Error) {
        ++telemetry_.rejected;
    }
    ++telemetry_.misses;
    return {};
}
void ScalarCache::publish_residual(const ScalarQuery& query,
                                   const syntax::Function& function,
                                   const hir::ExprPtr& body,
                                   const std::vector<hir::ExprPtr>& bindings,
                                   const ResourceCapture& capture) {
    if (mode_ != CacheMode::ReadWrite || !body ||
        body->stage != hir::Stage::Runtime || !capture.complete())
        return;
    try {
        CacheTimer timer(telemetry_.encode_ns, timings_);
        const auto encoded =
            encode_residual_template(body, function, bindings, *identity_);
        const auto evidence =
            residual_evidence(body, function, bindings, *identity_, context_);
        if (!encoded || !evidence) {
            ++telemetry_.rejected;
            return;
        }
        const std::array artifacts{
            cache::ArtifactInput{"template",
                                 cache::ObjectKind::ResidualTemplate, *encoded},
            cache::ArtifactInput{"resources", cache::ObjectKind::ResourceRecipe,
                                 query.recipe},
            cache::ArtifactInput{
                "evidence", cache::ObjectKind::ProvenanceSnapshot, *evidence}};
        const auto semantic =
            residual_digest(*encoded, query.recipe, *evidence);
        timer.stop();
        publish_or_defer(query.key, semantic, artifacts, true);
    } catch (const std::bad_alloc&) {
        ++telemetry_.errors;
    } catch (cache::Error) {
        ++telemetry_.rejected;
    }
}

void ScalarCache::publish_or_defer(
    const cache::QueryKey& key, const cache::Digest& semantic,
    std::span<const cache::ArtifactInput> artifacts, bool residual) {
    if (defer_publication_) {
        std::size_t bytes = 0;
        for (const auto& artifact : artifacts)
            bytes += artifact.bytes.size();
        constexpr auto maximum = std::size_t{16UZ * 1024 * 1024};
        if (bytes > maximum || pending_bytes_ > maximum - bytes ||
            pending_.size() >= 4096) {
            ++telemetry_.blocked_writes;
            return;
        }
        pending_.push_back(
            {key, semantic, {artifacts.begin(), artifacts.end()}, residual});
        pending_bytes_ += bytes;
        telemetry_.pending_payload_bytes_peak =
            std::max(telemetry_.pending_payload_bytes_peak, pending_bytes_);
        return;
    }
    CacheTimer publish_timer(telemetry_.publish_ns, timings_);
    auto published =
        store_.publish(key, identity_->module_fingerprint, semantic, artifacts);
    if (!published)
        ++telemetry_.errors;
    else if (*published == cache::PublishResult::Published) {
        if (key.kind == cache::QueryKind::FreezeScalarReplay ||
            key.kind == cache::QueryKind::FreezeGeneratorReplay)
            ++telemetry_.replay_writes;
        else {
            ++telemetry_.writes;
            telemetry_.residual_writes += residual ? 1U : 0U;
        }
    }
}
std::size_t ScalarCache::memory_bytes() const {
    auto bytes =
        sizeof(ScalarCache) + memory::heap(context_) + memory::vector(pending_);
    if (identity_)
        bytes += memory::heap(*identity_);
    for (const auto& item : pending_) {
        bytes += memory::heap(item.key) + memory::vector(item.artifacts);
        for (const auto& artifact : item.artifacts)
            bytes +=
                memory::heap(artifact.name) + memory::vector(artifact.bytes);
    }
    return bytes;
}
void ScalarCache::commit(bool permitted) {
    if (!defer_publication_)
        return;
    defer_publication_ = false;
    if (!permitted)
        telemetry_.blocked_writes += pending_.size();
    else
        for (const auto& item : pending_) {
            try {
                publish_or_defer(item.key, item.semantic, item.artifacts,
                                 item.residual);
            } catch (const std::bad_alloc&) {
                ++telemetry_.errors;
            }
        }
    std::vector<Pending>{}.swap(pending_);
    pending_bytes_ = 0;
}

} // namespace pagos::stage
