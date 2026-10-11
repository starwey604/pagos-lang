#include "pagos/stage/provenance_analysis.h"

#include "provenance_recorder.h"

#include <llvm/ADT/SmallVector.h>

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pagos::stage {
using namespace provenance;

ProvenanceAnalysis::ProvenanceAnalysis(FrozenGraph graph,
                                       std::vector<ProvenanceSite> sites,
                                       std::vector<ProvenanceRoot> roots)
    : graph_(std::move(graph)), sites_(std::move(sites)),
      roots_(std::move(roots)) {}

const ProvenanceSite& ProvenanceAnalysis::site(SiteId id) const {
    return sites_.at(id);
}

struct ProvenanceRecorder::Impl {
    struct Memo {
        std::weak_ptr<const hir::Expr> expression;
        NodeId node;
    };
    struct Parameter {
        std::weak_ptr<const hir::Expr> expression;
        SummaryId owner;
        std::uint32_t slot{};
    };
    struct Summary {
        std::weak_ptr<const hir::ResidualFunction> definition;
        SummaryId id;
    };
    Builder builder;
    std::vector<ProvenanceSite> sites{ProvenanceSite{}};
    std::vector<ProvenanceRoot> roots;
    std::unordered_map<const hir::Expr*, Memo> memo;
    std::unordered_map<const hir::Expr*, Parameter> parameters;
    std::vector<Summary> summaries;
    std::unordered_map<const hir::ResidualFunction*, std::size_t> summary_ids;
    std::vector<std::unordered_map<std::uint32_t, NodeId>> active_arguments;

    SiteId site(source::Span span, std::string label) {
        if (sites.size() >= std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("provenance site capacity exhausted");
        const auto id = static_cast<SiteId>(sites.size());
        sites.push_back({span, std::move(label)});
        return id;
    }

    NodeId known(const hir::ExprPtr& value) const {
        const auto found = memo.find(value.get());
        // A discarded folded expression's address can be reused. Weak identity
        // validation avoids keeping every temporary HIR expression alive.
        return found != memo.end() &&
                       found->second.expression.lock().get() == value.get()
                   ? found->second.node
                   : NodeId{};
    }

    SummaryId
    summary(const std::shared_ptr<const hir::ResidualFunction>& definition) {
        const auto found = summary_ids.find(definition.get());
        if (found != summary_ids.end() &&
            summaries[found->second].definition.lock().get() ==
                definition.get())
            return summaries[found->second].id;
        const auto id = builder.reserve_summary();
        summary_ids[definition.get()] = summaries.size();
        summaries.push_back({definition, id});
        for (std::size_t index = 0; index < definition->parameters.size();
             ++index) {
            const auto& parameter = definition->parameters[index];
            parameters[parameter.get()] = {parameter, id,
                                           static_cast<std::uint32_t>(index)};
        }
        return id;
    }

    std::string label(const hir::ExprPtr& value) const {
        using enum hir::Expr::Kind;
        if (value->kind == Load)
            return "pointer read";
        if (value->kind == VolatileLoad)
            return "volatile read";
        if (value->kind == FieldAddress)
            return "record field address";
        if (value->kind == GlobalElementAddress)
            return "static storage element address " + *value->variable_name;
        if (value->kind == Reference && value->trace &&
            !value->trace->path.empty())
            return value->trace->path.back();
        if (value->variable_name)
            return *value->variable_name;
        if (value->trace &&
            (value->kind == ExternalInput || value->kind == RuntimeBoundary ||
             value->kind == GlobalAddress))
            return value->trace->origin_name;
        switch (value->kind) {
        case Constant:
            return "known value";
        case Binary:
            return "binary computation";
        case Unary:
            return "unary computation";
        case Cast:
            return "cast";
        case If:
            return "if selection";
        case Sequence:
            return "ordered evaluation";
        case Array:
            return "array construction";
        case Record:
            return "record construction";
        case Field:
            return "field projection";
        case Index:
            return "array index";
        case Store:
            return "pointer store";
        case VolatileStore:
            return "volatile store";
        case RangeLoop:
            return "runtime loop";
        case Return:
            return "return value";
        case ReturnScope:
            return "return control";
        default:
            return "runtime computation";
        }
    }

    NodeId effect(SiteId location, std::string label) {
        return builder.add(Rule::Effect,
                           site(sites[location].span, std::move(label)));
    }

    void return_control(const hir::ExprPtr& value,
                        llvm::SmallVectorImpl<Edge>& edges) {
        std::vector<hir::ExprPtr> pending(value->operands);
        std::unordered_set<const hir::Expr*> visited;
        while (!pending.empty()) {
            auto current = std::move(pending.back());
            pending.pop_back();
            if (!current || !current->may_return ||
                !visited.insert(current.get()).second)
                continue;
            if (current->kind == hir::Expr::Kind::If ||
                current->kind == hir::Expr::Kind::Return)
                edges.push_back({known(current), Relation::Control});
            if (current->kind == hir::Expr::Kind::RangeLoop) {
                for (std::size_t index = 0; index < 2; ++index)
                    edges.push_back(
                        {known(current->operands[index]), Relation::Control});
            }
            pending.insert(pending.end(), current->operands.begin(),
                           current->operands.end());
        }
    }

    NodeId construct(const hir::ExprPtr& value) {
        using enum hir::Expr::Kind;
        const auto location = site(value->span, label(value));
        llvm::SmallVector<Edge, 4> edges;
        for (const auto& operand : value->operands)
            edges.push_back({known(operand), Relation::Data});
        switch (value->kind) {
        case Constant:
            return builder.add(Rule::Literal, location);
        case Reference:
        case Return:
            return builder.add(Rule::Use, location, edges);
        case RuntimeBoundary:
            for (auto& edge : edges)
                edge.relation = Relation::Initializer;
            return builder.add(Rule::RuntimeBoundary, location, edges);
        case ExternalInput:
        case Load:
        case VolatileLoad:
            edges.push_back({effect(location, label(value)), Relation::Effect});
            return builder.add(Rule::Origin, location, edges);
        case GlobalAddress:
            return builder.add(Rule::Origin, location, edges);
        case GlobalElementAddress:
            if (value->operands.front()->stage == hir::Stage::Runtime)
                edges.push_back({effect(location, "checked array index"),
                                 Relation::Effect});
            return builder.add(Rule::Origin, location, edges);
        case FieldAddress:
            edges.push_back(
                {effect(location, "checked field address"), Relation::Effect});
            return builder.add(Rule::Origin, location, edges);
        case Parameter: {
            const auto found = parameters.find(value.get());
            if (found != parameters.end() &&
                found->second.expression.lock().get() == value.get())
                return builder.parameter(found->second.owner,
                                         found->second.slot, location);
            // C ABI parameters are external sources, not symbolic internals.
            return builder.add(value->trace && value->trace->parameter_id
                                   ? Rule::Incomplete
                                   : Rule::Origin,
                               location);
        }
        case Call: {
            const auto callee = value->callee.lock();
            if (!callee)
                return builder.add(Rule::Incomplete, location, edges);
            if (callee->c_abi) {
                for (auto& edge : edges)
                    edge.relation = Relation::Initializer;
                edges.push_back({effect(location, "C call " + callee->name),
                                 Relation::Effect});
                return builder.add(Rule::RuntimeBoundary, location, edges);
            }
            llvm::SmallVector<NodeId, 4> arguments;
            for (const auto& edge : edges)
                arguments.push_back(edge.target);
            return builder.call(summary(callee), arguments, location);
        }
        case If:
            edges.front().relation = Relation::Control;
            return builder.add(Rule::Select, location, edges);
        case Sequence:
            for (std::size_t index = 0; index + 1 < edges.size(); ++index)
                edges[index].relation = Relation::Effect;
            return builder.add(Rule::Sequence, location, edges);
        case Array:
        case Record:
            return builder.add(Rule::Aggregate, location, edges);
        case Field:
            return builder.project(edges.front().target, value->field_index,
                                   location);
        case Index: {
            const auto& index = value->operands[1];
            if (index->stage == hir::Stage::Comptime) {
                const auto member = static_cast<std::uint32_t>(
                    std::get<IntegerValue>(*index->constant).bits());
                const auto selected =
                    builder.project(edges[0].target, member, location);
                edges[0].target = selected;
            }
            if (index->stage == hir::Stage::Runtime)
                edges.push_back({effect(location, "checked array index"),
                                 Relation::Effect});
            return builder.add(Rule::Strict, location, edges);
        }
        case ReturnScope:
            return_control(value, edges);
            return builder.add(Rule::Select, location, edges);
        case RangeLoop:
            edges[0].relation = Relation::Control;
            edges[1].relation = Relation::Control;
            return builder.add(Rule::Effect, location, edges);
        case Store:
        case VolatileStore:
            return builder.add(Rule::Effect, location, edges);
        case LoopIndex:
            return builder.add(Rule::Incomplete, location);
        case Binary:
            if (value->binary_operation == syntax::BinaryOperator::Divide ||
                value->binary_operation == syntax::BinaryOperator::Remainder ||
                value->binary_operation == syntax::BinaryOperator::ShiftLeft ||
                value->binary_operation == syntax::BinaryOperator::ShiftRight)
                edges.push_back(
                    {effect(location, "checked arithmetic"), Relation::Effect});
            [[fallthrough]];
        case Unary:
        case Cast:
            return builder.add(Rule::Strict, location, edges);
        }
        throw std::logic_error("unhandled HIR provenance kind");
    }

    NodeId evidence(const hir::ExprPtr& value) {
        if (!value)
            return {};
        struct Pending {
            hir::ExprPtr expression;
            bool ready{};
        };
        std::vector<Pending> pending{{value, false}};
        while (!pending.empty()) {
            auto current = std::move(pending.back());
            pending.pop_back();
            if (known(current.expression).value != 0)
                continue;
            if (!current.ready) {
                if (current.expression->kind == hir::Expr::Kind::Call) {
                    if (const auto callee = current.expression->callee.lock();
                        callee && !callee->c_abi)
                        (void)summary(callee);
                }
                pending.push_back({current.expression, true});
                for (auto operand = current.expression->operands.rbegin();
                     operand != current.expression->operands.rend(); ++operand)
                    pending.push_back({*operand, false});
            } else {
                const auto node = construct(current.expression);
                memo[current.expression.get()] = {current.expression, node};
            }
        }
        return known(value);
    }

    // Only local contract explanations need concrete active arguments. Normal
    // cached bodies remain symbolic and calls bind them during graph queries.
    NodeId concrete(NodeId root) {
        std::unordered_map<std::uint32_t, NodeId> replacements;
        for (const auto& frame : active_arguments)
            for (const auto& [parameter, argument] : frame)
                replacements[parameter] = argument;
        if (replacements.empty())
            return root;
        std::vector<std::pair<NodeId, bool>> pending{{root, false}};
        while (!pending.empty()) {
            const auto [id, ready] = pending.back();
            pending.pop_back();
            if (replacements.contains(id.value))
                continue;
            if (!ready) {
                pending.emplace_back(id, true);
                for (const auto& edge : builder.edges(id))
                    pending.emplace_back(edge.target, false);
            } else {
                const auto node = builder.node(id);
                llvm::SmallVector<Edge, 4> edges;
                bool changed = false;
                for (auto edge : builder.edges(id)) {
                    const auto target = replacements.at(edge.target.value);
                    changed |= target != edge.target;
                    edge.target = target;
                    edges.push_back(edge);
                }
                replacements[id.value] =
                    changed ? builder.add(node.rule, node.site, edges,
                                          node.payload, node.owner)
                            : id;
            }
        }
        return replacements.at(root.value);
    }
};

ProvenanceRecorder::ProvenanceRecorder() : impl_(std::make_unique<Impl>()) {}
ProvenanceRecorder::~ProvenanceRecorder() = default;

void ProvenanceRecorder::binding(const syntax::BindingStmt& binding,
                                 const hir::ExprPtr& value, bool top_level) {
    const bool required = binding.binding_kind == syntax::BindingKind::Comptime;
    if (!top_level && !required)
        return;
    auto root = impl_->evidence(value);
    if (!top_level)
        root = impl_->concrete(root);
    const auto location = impl_->site(binding.name_span, binding.name);
    root = impl_->builder.add(
        required ? Rule::RequireComptime : Rule::Use, location,
        std::array{Edge{root, required ? Relation::Contract : Relation::Data}});
    impl_->roots.push_back(
        {binding.name, binding.name_span, value->stage, root, required});
}

void ProvenanceRecorder::storage(const syntax::Module::Global& global,
                                 const hir::ExprPtr& initializer) {
    const auto value = impl_->evidence(initializer);
    const auto location =
        impl_->site(global.name_span, "static storage " + global.name);
    const auto root =
        impl_->builder.add(Rule::RequireComptime, location,
                           std::array{Edge{value, Relation::Contract}});
    impl_->roots.push_back({"static " + global.name, global.name_span,
                            initializer->stage, root, true});
}

void ProvenanceRecorder::enter_call(
    const std::shared_ptr<const hir::ResidualFunction>& callee,
    const std::unordered_map<const hir::Expr*, hir::ExprPtr>& arguments) {
    if (!callee->parameters.empty())
        (void)impl_->summary(callee);
    std::unordered_map<std::uint32_t, NodeId> frame;
    for (const auto& parameter : callee->parameters) {
        const auto argument =
            impl_->concrete(impl_->evidence(arguments.at(parameter.get())));
        frame[impl_->evidence(parameter).value] = argument;
    }
    impl_->active_arguments.push_back(std::move(frame));
}

void ProvenanceRecorder::leave_call() { impl_->active_arguments.pop_back(); }

void ProvenanceRecorder::projection(const hir::ExprPtr& value,
                                    const hir::ExprPtr& aggregate,
                                    std::uint32_t member,
                                    const hir::ExprPtr& index) {
    if (aggregate->stage == hir::Stage::Comptime &&
        !hir::has_residual_work(aggregate) &&
        (!index || !hir::has_residual_work(index)))
        return;
    const auto selected = impl_->builder.project(
        impl_->evidence(aggregate), member,
        impl_->site(value->span, "known member projection"));
    NodeId root = selected;
    if (index)
        root = impl_->builder.add(
            Rule::Strict, impl_->site(value->span, "known array index"),
            std::array{Edge{selected}, Edge{impl_->evidence(index)}});
    impl_->memo[value.get()] = {value, root};
}

void ProvenanceRecorder::loop_index(const hir::ExprPtr& index,
                                    const hir::ExprPtr& begin,
                                    const hir::ExprPtr& end) {
    const auto root = impl_->builder.add(
        Rule::Select, impl_->site(index->span, "loop index"),
        std::array{Edge{impl_->evidence(begin), Relation::Control},
                   Edge{impl_->evidence(end), Relation::Control}});
    impl_->memo[index.get()] = {index, root};
}

std::shared_ptr<const ProvenanceAnalysis>
ProvenanceRecorder::finish(const hir::Module& module) {
    if (module.result)
        (void)impl_->evidence(module.result);
    for (const auto& function : module.residual_functions)
        if (!function->c_abi)
            (void)impl_->summary(function);
    // Processing a body may register more nested summaries. Copy each record
    // before traversal, since the summary vector can grow during that
    // traversal.
    for (std::size_t index = 0; index < impl_->summaries.size(); ++index) {
        const auto summary = impl_->summaries[index];
        const auto definition = summary.definition.lock();
        auto root = definition && definition->body
                        ? impl_->evidence(definition->body)
                        : impl_->builder.add(
                              Rule::Incomplete,
                              impl_->site({}, "aborted function analysis"));
        if (definition && definition->recursive && definition->body &&
            definition->body->stage == hir::Stage::Comptime)
            root = impl_->builder.add(
                Rule::RuntimeBoundary,
                impl_->site(definition->body->span,
                            "recursive completion " + definition->name),
                std::array{Edge{root, Relation::Initializer}});
        impl_->builder.define_summary(summary.id, root);
    }
    return std::shared_ptr<const ProvenanceAnalysis>(new ProvenanceAnalysis(
        std::move(impl_->builder).freeze(), std::move(impl_->sites),
        std::move(impl_->roots)));
}

void explain_provenance(const ProvenanceAnalysis& analysis,
                        std::ostream& output) {
    const auto print_sources = [&](const QueryResult& result) {
        for (const auto& source : result.sources) {
            const auto& site = analysis.site(source.site);
            output << "    " << site.label << " @" << site.span.begin << ':'
                   << site.span.end;
            for (auto frame = source.context; frame != 0;
                 frame = result.invocations[frame].parent) {
                const auto& call = analysis.site(
                    analysis.graph().node(result.invocations[frame].call).site);
                output << " via " << call.label << " @" << call.span.begin
                       << ':' << call.span.end;
            }
            output << '\n';
        }
    };
    for (const auto& root : analysis.roots()) {
        output << "binding " << root.name << ": " << hir::stage_name(root.stage)
               << '\n';
        if (root.requires_comptime)
            output << "  contract: Comptime\n";
        const auto value = analysis.graph().query(root.node);
        output << "  value sources: " << value.sources.size() << " ("
               << (value.complete ? "complete" : "partial") << ")\n";
        print_sources(value);
        const auto effects =
            analysis.graph().query(root.node, QueryKind::Effects);
        output << "  residual effects: " << effects.sources.size() << " ("
               << (effects.complete ? "complete" : "partial") << ")\n";
        print_sources(effects);
    }
}

} // namespace pagos::stage
