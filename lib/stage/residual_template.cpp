#include "residual_template.h"
#include "../cache/wire.h"
#include "pagos/stage/provenance_snapshot.h"
#include "provenance_recorder.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace pagos::stage {
namespace {
using cache::Error;
using cache::wire::Reader;
using cache::wire::Writer;
constexpr std::uint32_t template_version = 1;
constexpr std::uint32_t no_slot = UINT32_MAX;
using Kind = hir::Expr::Kind;
bool scalar(const sema::Type& type) {
    return type.kind == sema::TypeKind::Integer ||
           type.kind == sema::TypeKind::Bool;
}
std::optional<std::uint32_t> slot(const hir::ExprPtr& expression,
                                  const std::vector<hir::ExprPtr>& bindings) {
    for (std::size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i] == expression)
            return static_cast<std::uint32_t>(i);
    return {};
}
std::uint32_t trace_slot(const hir::ExprPtr& expression,
                         const std::vector<hir::ExprPtr>& bindings) {
    if (!expression->trace || !expression->trace->parameter_id)
        return no_slot;
    for (std::size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i]->trace &&
            bindings[i]->trace->parameter_id == expression->trace->parameter_id)
            return static_cast<std::uint32_t>(i);
    return no_slot;
}
SourceAnchor anchor(const CacheIdentity& identity,
                    const syntax::Function& function, source::Span span) {
    for (const auto& site : identity.sites)
        if (site.anchor.definition.kind == cache::DefinitionKind::Function &&
            site.anchor.definition.name == function.name &&
            site.span.begin == span.begin && site.span.end == span.end)
            return site.anchor;
    throw Error::InvalidAnchor;
}
std::vector<hir::ExprPtr> ordered(const hir::ExprPtr& root) {
    std::vector<std::pair<hir::ExprPtr, bool>> pending{{root, false}};
    std::vector<hir::ExprPtr> result;
    std::unordered_set<const hir::Expr*> done, active;
    while (!pending.empty()) {
        auto [node, ready] = pending.back();
        pending.pop_back();
        if (!node)
            throw Error::InvalidValue;
        if (done.contains(node.get()))
            continue;
        if (!ready) {
            if (!active.insert(node.get()).second)
                throw Error::InvalidValue;
            if (active.size() > 256 || done.size() > 1'000'000)
                throw Error::LimitExceeded;
            pending.emplace_back(node, true);
            for (auto child = node->operands.rbegin();
                 child != node->operands.rend(); ++child)
                pending.emplace_back(*child, false);
        } else {
            active.erase(node.get());
            done.insert(node.get());
            result.push_back(node);
        }
    }
    return result;
}
void validate(const hir::ExprPtr& node) {
    if (!scalar(node->type) || !node->falls_through || node->may_return)
        throw Error::InvalidValue;
    const auto children = node->operands.size();
    if ((node->kind != Kind::Unary && node->unary_operation) ||
        (node->kind != Kind::Binary && node->binary_operation))
        throw Error::InvalidValue;
    if (node->kind == Kind::Constant) {
        if (children || node->stage != hir::Stage::Comptime ||
            !node->constant || node->trace)
            throw Error::InvalidValue;
        return;
    }
    if (node->stage != hir::Stage::Runtime || node->constant || !node->trace ||
        !node->trace->parameter_id)
        throw Error::InvalidValue;
    if (node->kind == Kind::Parameter) {
        if (children)
            throw Error::InvalidValue;
        return;
    }
    if (node->kind == Kind::Reference || node->kind == Kind::Unary ||
        node->kind == Kind::Cast) {
        if (children != 1 || node->operands[0]->stage != hir::Stage::Runtime)
            throw Error::InvalidValue;
        if (node->kind == Kind::Reference &&
            node->type != node->operands[0]->type)
            throw Error::InvalidValue;
        if (node->kind == Kind::Unary) {
            if (!node->unary_operation ||
                *node->unary_operation > syntax::UnaryOperator::Negate ||
                node->type != node->operands[0]->type ||
                (*node->unary_operation == syntax::UnaryOperator::Not
                     ? node->type.kind != sema::TypeKind::Bool
                     : node->type.kind != sema::TypeKind::Integer))
                throw Error::InvalidValue;
        }
    } else if (node->kind == Kind::Binary) {
        if (children != 2 || !node->binary_operation ||
            *node->binary_operation >= syntax::BinaryOperator::LogicalAnd ||
            node->operands[0]->type != node->operands[1]->type ||
            (node->operands[0]->stage != hir::Stage::Runtime &&
             node->operands[1]->stage != hir::Stage::Runtime))
            throw Error::InvalidValue;
        const auto comparison =
            *node->binary_operation >= syntax::BinaryOperator::Equal;
        if (comparison ? node->type.kind != sema::TypeKind::Bool
                       : node->type != node->operands[0]->type ||
                             node->type.kind != sema::TypeKind::Integer)
            throw Error::InvalidValue;
    } else
        throw Error::InvalidValue;
    const auto first =
        std::ranges::find_if(node->operands, [](const auto& operand) {
            return operand->stage == hir::Stage::Runtime;
        });
    if (first == node->operands.end() ||
        (*first)->trace->parameter_id != node->trace->parameter_id)
        throw Error::InvalidValue;
}
} // namespace
std::expected<cache::Bytes, Error> encode_residual_template(
    const hir::ExprPtr& body, const syntax::Function& function,
    const std::vector<hir::ExprPtr>& bindings, const CacheIdentity& identity) {
    try {
        if (!body || body->stage != hir::Stage::Runtime ||
            bindings.size() != function.parameters.size())
            throw Error::InvalidValue;
        Writer writer({});
        cache::wire::header(writer, cache::ObjectKind::ResidualTemplate);
        writer.u32(template_version);
        const auto nodes = ordered(body);
        writer.count(nodes.size());
        std::unordered_map<const hir::Expr*, std::uint32_t> ids;
        for (const auto& node : nodes) {
            validate(node);
            ids[node.get()] = static_cast<std::uint32_t>(ids.size() + 1);
            const auto binding = slot(node, bindings);
            const bool argument = binding && (node->kind == Kind::Constant ||
                                              node->kind == Kind::Parameter);
            writer.u8(static_cast<std::uint8_t>(node->kind));
            cache::wire::type(writer, node->type);
            writer.boolean(argument);
            if (argument)
                writer.u32(*binding);
            else {
                const auto encoded = encode_source_anchor(
                    anchor(identity, function, node->span));
                if (!encoded)
                    throw encoded.error();
                writer.count(encoded->size());
                writer.raw(*encoded);
            }
            if (node->kind == Kind::Constant && !argument)
                cache::wire::value(writer, {node->type, *node->constant});
            writer.count(node->operands.size());
            for (const auto& operand : node->operands)
                writer.u32(ids.at(operand.get()));
            writer.u32(node->unary_operation
                           ? static_cast<std::uint32_t>(*node->unary_operation)
                           : no_slot);
            writer.u32(node->binary_operation
                           ? static_cast<std::uint32_t>(*node->binary_operation)
                           : no_slot);
            writer.u32(trace_slot(node, bindings));
            writer.count(node->trace ? node->trace->path.size() : 0);
            if (node->trace)
                for (const auto& step : node->trace->path.to_vector())
                    writer.string(step);
            writer.string(node->variable_name.value_or(""));
        }
        writer.u32(ids.at(body.get()));
        return writer.finish();
    } catch (Error error) {
        return std::unexpected(error);
    } catch (const std::exception&) {
        return std::unexpected(Error::InvalidValue);
    }
}
std::expected<hir::ExprPtr, Error> decode_residual_template(
    std::span<const std::uint8_t> bytes, const syntax::Function& function,
    const std::vector<hir::ExprPtr>& bindings, const CacheIdentity& identity,
    const sema::TypeTable& types) {
    try {
        if (bindings.size() != function.parameters.size())
            throw Error::InvalidValue;
        Reader reader(bytes, {});
        cache::wire::header(reader, cache::ObjectKind::ResidualTemplate);
        if (reader.u32() != template_version)
            reader.fail(Error::IncompatibleVersion);
        const auto count = reader.count(24);
        std::vector<hir::ExprPtr> nodes{nullptr};
        for (std::uint32_t i = 1; i <= count && reader.good(); ++i) {
            auto node = std::make_shared<hir::Expr>();
            node->kind = static_cast<Kind>(reader.u8());
            node->type = cache::wire::type(reader);
            if (!scalar(node->type))
                throw Error::InvalidValue;
            const auto argument = reader.boolean();
            std::optional<std::uint32_t> binding;
            if (argument) {
                binding = reader.u32();
                if (*binding >= bindings.size())
                    throw Error::InvalidValue;
            } else {
                const auto encoded = reader.raw(reader.count());
                const auto location = decode_source_anchor(encoded);
                if (!location)
                    throw location.error();
                if (location->definition.name != function.name ||
                    location->definition.kind !=
                        cache::DefinitionKind::Function)
                    throw Error::InvalidAnchor;
                const auto span = identity.resolve(*location);
                if (!span)
                    throw span.error();
                node->span = *span;
            }
            if (node->kind == Kind::Constant) {
                node->stage = hir::Stage::Comptime;
                if (argument) {
                    const auto& actual = bindings[*binding];
                    if (actual->stage != hir::Stage::Comptime ||
                        actual->type != node->type || !actual->constant)
                        throw Error::InvalidValue;
                    node->span = actual->span;
                    node->constant = actual->constant;
                } else
                    node->constant = cache::wire::value(reader, node->type);
            } else
                node->stage = hir::Stage::Runtime;
            const auto child_count = reader.count(4);
            for (std::uint32_t c = 0; c < child_count && reader.good(); ++c) {
                const auto child = reader.u32();
                if (!child || child >= i)
                    throw Error::InvalidValue;
                node->operands.push_back(nodes[child]);
            }
            const auto unary = reader.u32();
            const auto binary = reader.u32();
            if (unary != no_slot) {
                if (unary >
                    static_cast<std::uint32_t>(syntax::UnaryOperator::Negate))
                    throw Error::InvalidValue;
                node->unary_operation =
                    static_cast<syntax::UnaryOperator>(unary);
            }
            if (binary != no_slot) {
                if (binary >= static_cast<std::uint32_t>(
                                  syntax::BinaryOperator::LogicalAnd))
                    throw Error::InvalidValue;
                node->binary_operation =
                    static_cast<syntax::BinaryOperator>(binary);
            }
            const auto origin = reader.u32();
            const auto steps = reader.count(4);
            std::vector<std::string> path;
            for (std::uint32_t s = 0; s < steps && reader.good(); ++s)
                path.push_back(reader.string());
            const auto name = reader.string();
            if (!name.empty())
                node->variable_name = name;
            if (node->stage == hir::Stage::Runtime) {
                if (origin >= bindings.size() ||
                    bindings[origin]->stage != hir::Stage::Runtime ||
                    !bindings[origin]->trace || path.empty() ||
                    path.front() !=
                        function.name + "." + function.parameters[origin].name)
                    throw Error::InvalidValue;
                node->trace = bindings[origin]->trace;
                node->trace->path = hir::RuntimePath(std::move(path));
            } else if (origin != no_slot || !path.empty())
                throw Error::InvalidValue;
            if (node->kind == Kind::Parameter) {
                if (!binding || origin != *binding ||
                    bindings[*binding]->kind != Kind::Parameter ||
                    bindings[*binding]->type != node->type ||
                    !node->operands.empty() || node->unary_operation ||
                    node->binary_operation)
                    throw Error::InvalidValue;
                nodes.push_back(bindings[*binding]);
            } else {
                if (argument && node->kind != Kind::Constant)
                    throw Error::InvalidValue;
                validate(node);
                if (argument) {
                    if (node->unary_operation || node->binary_operation ||
                        node->variable_name)
                        throw Error::InvalidValue;
                    nodes.push_back(bindings[*binding]);
                } else
                    nodes.push_back(node);
            }
        }
        const auto root = reader.u32();
        const auto status = reader.finish();
        if (!status)
            throw status.error();
        if (!root || root >= nodes.size() ||
            nodes[root]->stage != hir::Stage::Runtime ||
            nodes[root]->type != types.resolve(function.result))
            throw Error::InvalidValue;
        return nodes[root];
    } catch (Error error) {
        return std::unexpected(error);
    } catch (const std::exception&) {
        return std::unexpected(Error::InvalidValue);
    }
}
std::expected<cache::Bytes, Error>
residual_evidence(const hir::ExprPtr& body, const syntax::Function& function,
                  const std::vector<hir::ExprPtr>& bindings,
                  const CacheIdentity& identity,
                  const cache::QueryContext& context) {
    try {
        std::unordered_map<const hir::Expr*, hir::ExprPtr> copies;
        for (const auto& original : ordered(body)) {
            if (original->kind == Kind::Parameter) {
                copies[original.get()] = original;
                continue;
            }
            auto copy = std::make_shared<hir::Expr>(*original);
            copy->operands.clear();
            for (const auto& child : original->operands)
                copy->operands.push_back(copies.at(child.get()));
            if (const auto argument = slot(original, bindings);
                argument && original->stage == hir::Stage::Comptime)
                copy->span = function.parameters[*argument].name_span;
            copies[original.get()] = std::move(copy);
        }
        auto definition = std::make_shared<hir::ResidualFunction>();
        definition->name = function.name;
        definition->body = copies.at(body.get());
        definition->result_type = body->type;
        for (const auto& binding : bindings)
            if (binding->stage == hir::Stage::Runtime)
                definition->parameters.push_back(binding);
        hir::Module module;
        module.residual_functions.push_back(definition);
        ProvenanceRecorder recorder;
        const auto evidence = recorder.finish(module);
        const auto encoded = ProvenanceSnapshot::encode(
            *evidence, identity, context,
            source::SourceFile::from_text("symbolic", ""));
        if (!encoded)
            return std::unexpected(encoded.error());
        return encoded->graph;
    } catch (Error error) {
        return std::unexpected(error);
    } catch (const std::exception&) {
        return std::unexpected(Error::InvalidValue);
    }
}
cache::Digest residual_digest(std::span<const std::uint8_t> body,
                              std::span<const std::uint8_t> recipe,
                              std::span<const std::uint8_t> evidence) {
    Writer writer({});
    writer.count(body.size());
    writer.raw(body);
    writer.count(recipe.size());
    writer.raw(recipe);
    writer.count(evidence.size());
    writer.raw(evidence);
    const auto bytes = writer.finish();
    if (!bytes)
        throw bytes.error();
    return cache::fingerprint("pagos.residual-result.v1", *bytes);
}
} // namespace pagos::stage
