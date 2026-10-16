#include "pagos/stage/provenance_analysis.h"

#include "memory_accounting.h"
#include "pagos/stage/provenance_topology.h"
#include "provenance_recorder.h"

#include <llvm/ADT/SmallVector.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pagos::stage {
using namespace provenance;

struct ProvenanceAnalysis::StorageState {
    mutable std::mutex mutex;
    std::optional<FrozenGraph> graph;
    std::shared_ptr<const EvidenceView> view;
    std::optional<EvidenceError> failure;
    EvidenceBuildStats construction;
    bool spilled{};
};
ProvenanceAnalysis::ProvenanceAnalysis(
    FrozenGraph graph, std::vector<ProvenanceSite> sites,
    std::vector<ProvenanceRoot> roots, std::vector<StageDecision> decisions,
    std::vector<ProvenanceScope> scopes,
    std::optional<ProvenanceIdentity> identity)
    : storage_(std::make_shared<StorageState>()), sites_(std::move(sites)),
      roots_(std::move(roots)), decisions_(std::move(decisions)),
      scopes_(std::move(scopes)), identity_(std::move(identity)) {
    storage_->view = EvidenceView::memory(graph);
    storage_->graph = std::move(graph);
}
ProvenanceAnalysis::ProvenanceAnalysis(
    std::shared_ptr<const EvidenceView> view, std::vector<ProvenanceSite> sites,
    std::vector<ProvenanceRoot> roots, std::vector<StageDecision> decisions,
    std::vector<ProvenanceScope> scopes,
    std::optional<ProvenanceIdentity> identity, EvidenceBuildStats construction,
    std::optional<EvidenceError> failure)
    : storage_(std::make_shared<StorageState>()), sites_(std::move(sites)),
      roots_(std::move(roots)), decisions_(std::move(decisions)),
      scopes_(std::move(scopes)), identity_(std::move(identity)) {
    storage_->view = std::move(view);
    storage_->construction = construction;
    storage_->failure = failure;
    storage_->spilled = true;
}
std::shared_ptr<const EvidenceView> ProvenanceAnalysis::evidence() const {
    return storage_->view;
}
bool ProvenanceAnalysis::available() const {
    const std::lock_guard lock(storage_->mutex);
    return !storage_->failure;
}
bool ProvenanceAnalysis::graph_materialized() const {
    const std::lock_guard lock(storage_->mutex);
    return storage_->graph.has_value();
}
bool ProvenanceAnalysis::spilled() const { return storage_->spilled; }
std::optional<EvidenceError> ProvenanceAnalysis::failure() const {
    const std::lock_guard lock(storage_->mutex);
    return storage_->failure;
}
const EvidenceBuildStats& ProvenanceAnalysis::construction_stats() const {
    return storage_->construction;
}
ProvenanceMemoryStats ProvenanceAnalysis::memory_stats() const {
    auto result = recording_memory_;
    result.sites_bytes = memory::vector(sites_);
    for (const auto& site : sites_)
        result.sites_bytes += memory::heap(site.label);
    result.roots_bytes = memory::vector(roots_);
    for (const auto& root : roots_)
        result.roots_bytes += memory::heap(root.name);
    result.decisions_bytes = memory::vector(decisions_);
    result.scopes_bytes = memory::vector(scopes_);
    for (const auto& scope : scopes_) {
        result.scopes_bytes += memory::heap(scope.name) +
                               memory::vector(scope.decisions) +
                               memory::vector(scope.contracts);
        if (scope.specialization) {
            const auto& spec = *scope.specialization;
            result.specialization_bytes += memory::heap(spec.definition_name) +
                                           memory::vector(spec.arguments);
            if (spec.definition)
                result.specialization_bytes += memory::heap(*spec.definition);
            for (const auto& arg : spec.arguments)
                result.specialization_bytes +=
                    memory::heap(arg.type) +
                    (arg.value ? memory::heap(*arg.value) : 0);
        }
    }
    if (identity_)
        result.identity_bytes = memory::heap(identity_->context);
    result.view_directory_bytes = storage_->view->directory_bytes();
    const std::lock_guard lock(storage_->mutex);
    if (storage_->graph) {
        const auto graph = storage_->graph->stats();
        result.graph_bytes = graph.node_capacity_bytes +
                             graph.edge_capacity_bytes +
                             graph.summary_capacity_bytes;
        result.intern_bytes = graph.intern_capacity_bytes;
    }
    return result;
}
bool ProvenanceAnalysis::validate_details() const {
    const std::lock_guard lock(storage_->mutex);
    if (storage_->failure)
        return false;
    std::optional<NodeLease> previous;
    for (std::uint32_t id = 1; id <= storage_->view->node_count(); ++id) {
        auto lease = storage_->view->pin({id});
        if (!lease) {
            storage_->failure = lease.error();
            return false;
        }
        previous = std::move(*lease);
    }
    return true;
}
void ProvenanceAnalysis::mark_unavailable(EvidenceError error) const {
    const std::lock_guard lock(storage_->mutex);
    if (!storage_->failure)
        storage_->failure = error;
}
const FrozenGraph& ProvenanceAnalysis::graph() const {
    const std::lock_guard lock(storage_->mutex);
    if (storage_->graph)
        return *storage_->graph;
    if (storage_->failure)
        throw EvidenceUnavailable(*storage_->failure);
    try {
        Builder builder;
        for (const auto& summary : storage_->view->summaries()) {
            (void)summary;
            (void)builder.reserve_summary();
        }
        std::optional<NodeLease> previous;
        for (std::uint32_t id = 1; id <= storage_->view->node_count(); ++id) {
            auto lease = storage_->view->pin({id});
            if (!lease) {
                storage_->failure = lease.error();
                throw EvidenceUnavailable(*storage_->failure);
            }
            const auto& node = lease->node();
            (void)builder.add(node.rule, node.site, lease->edges(),
                              node.payload, node.owner);
            previous = std::move(*lease);
        }
        for (const auto& summary : storage_->view->summaries()) {
            if (summary.value != summary.effects)
                throw std::logic_error("unsupported evidence ports");
            builder.define_summary(summary.id, summary.value);
        }
        storage_->graph = std::move(builder).freeze();
        return *storage_->graph;
    } catch (const std::bad_alloc&) {
        storage_->failure =
            EvidenceError{EvidenceErrorCode::OutOfMemory, {}, {}};
        throw EvidenceUnavailable(*storage_->failure);
    }
}

const ProvenanceScope& ProvenanceAnalysis::scope(SummaryId id) const {
    return scopes_.at(id.value);
}
ProvenanceQueryTask::ProvenanceQueryTask(
    std::shared_ptr<const ProvenanceAnalysis> analysis,
    source::SourceFile source, NodeId root, QueryKind kind, bool witness)
    : analysis_(std::move(analysis)), source_(std::move(source)),
      query_(analysis_
                 ? QueryTask(analysis_->evidence(), root, kind, witness)
                 : throw std::invalid_argument("missing provenance analysis")) {
}

const StageDecision* ProvenanceAnalysis::decision(NodeId node) const noexcept {
    const auto found = std::ranges::lower_bound(
        decisions_, node.value, {},
        [](const auto& item) { return item.node.value; });
    return found != decisions_.end() && found->node == node ? &*found : nullptr;
}

const ProvenanceSite& ProvenanceAnalysis::site(SiteId id) const {
    return sites_.at(id);
}

namespace {
struct RecordingFailure {
    EvidenceError error;
};
template <class T> T recorded(std::expected<T, EvidenceError> result) {
    if (!result)
        throw RecordingFailure{result.error()};
    return std::move(*result);
}
void recorded(std::expected<void, EvidenceError> result) {
    if (!result)
        throw RecordingFailure{result.error()};
}
struct RecorderLease {
    std::optional<NodeLease> owned;
    Node copy;
    llvm::SmallVector<Edge, 4> inputs;
    const Node& node() const { return owned ? owned->node() : copy; }
    std::span<const Edge> edges() const {
        return owned ? owned->edges() : std::span<const Edge>(inputs);
    }
};
class RecorderBuilder {
  public:
    explicit RecorderBuilder(EvidenceBuildConfig config) {
        if (config.mode == EvidenceBuildMode::Spill)
            spill.emplace(std::move(config));
    }
    NodeId add(Rule rule, SiteId site, std::span<const Edge> edges = {},
               std::uint32_t payload = 0, SummaryId owner = {}) {
        return spill ? recorded(spill->add(rule, site, edges, payload, owner))
                     : memory.add(rule, site, edges, payload, owner);
    }
    SummaryId reserve_summary() {
        return spill ? recorded(spill->reserve_summary())
                     : memory.reserve_summary();
    }
    void define_summary(SummaryId id, NodeId root) {
        if (spill)
            recorded(spill->define_summary(id, root));
        else
            memory.define_summary(id, root);
    }
    RecorderLease pin(NodeId id) {
        RecorderLease result;
        if (spill)
            result.owned = recorded(spill->pin(id));
        else {
            result.copy = memory.node(id);
            const auto edges = memory.edges(id);
            result.inputs.assign(edges.begin(), edges.end());
        }
        return result;
    }
    NodeId parameter(SummaryId owner, std::uint32_t slot, SiteId site) {
        return spill ? add(Rule::Parameter, site, {}, slot, owner)
                     : memory.parameter(owner, slot, site);
    }
    NodeId call(SummaryId owner, std::span<const NodeId> arguments,
                SiteId site) {
        llvm::SmallVector<Edge, 4> edges;
        for (auto argument : arguments)
            edges.push_back({argument});
        return add(Rule::Call, site, edges, owner.value);
    }
    Builder memory;
    std::optional<EvidenceBuilder> spill;
};
} // namespace

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
        NodeId completion;
    };
    explicit Impl(EvidenceBuildConfig config)
        : spill_enabled(config.mode == EvidenceBuildMode::Spill),
          measure_memory(config.measure_memory), builder(std::move(config)) {}
    bool spill_enabled;
    bool measure_memory;
    std::size_t next_prune{1024}, memo_peak{}, pruned{};
    RecorderBuilder builder;
    std::optional<EvidenceError> failure;
    EvidenceBuildStats failed_stats;
    template <class F> void attempt(F&& work) {
        if (failure)
            return;
        try {
            work();
        } catch (const RecordingFailure& error) {
            disable(error.error);
        } catch (const std::bad_alloc&) {
            if (!spill_enabled)
                throw;
            disable({EvidenceErrorCode::OutOfMemory, {}, {}});
        }
    }
    void disable(EvidenceError error) {
        failure = error;
        if (builder.spill)
            failed_stats = builder.spill->stats();
        builder.spill.reset();
        decltype(memo){}.swap(memo);
        decltype(parameters){}.swap(parameters);
        decltype(summaries){}.swap(summaries);
        decltype(summary_ids){}.swap(summary_ids);
        decltype(active_arguments){}.swap(active_arguments);
        decltype(active_scopes){}.swap(active_scopes);
    }
    std::optional<ProvenanceIdentity> identity;
    std::vector<DefinitionIdentity> definitions;
    std::vector<ProvenanceSite> sites{ProvenanceSite{}};
    std::vector<ProvenanceRoot> roots;
    std::vector<StageDecision> decisions;
    std::vector<ProvenanceScope> scopes{{{}, "module", {}, {}}};
    std::vector<SummaryId> active_scopes;
    SummaryId scope() const {
        return active_scopes.empty() ? SummaryId{} : active_scopes.back();
    }
    std::unordered_map<const hir::Expr*, Memo> memo;
    std::unordered_map<const hir::Expr*, Parameter> parameters;
    std::vector<Summary> summaries;
    std::unordered_map<const hir::ResidualFunction*, std::size_t> summary_ids;
    std::vector<std::unordered_map<std::uint32_t, NodeId>> active_arguments;

    void remember(const hir::ExprPtr& value, NodeId node) {
        memo[value.get()] = {value, node};
        memo_peak = std::max(memo_peak, memo.size());
        if (memo.size() >= next_prune)
            prune_expired();
    }
    void prune_expired() {
        pruned += std::erase_if(memo, [](const auto& entry) {
            return entry.second.expression.expired();
        });
        // Geometric threshold makes all-live scans amortized linear. A final
        // scan also releases array element HIR discarded during folding.
        next_prune = std::max(std::size_t{1024}, memo.size() * 2);
    }
    ProvenanceMemoryStats memory_stats() const {
        ProvenanceMemoryStats result;
        result.memo_peak_entries = memo_peak;
        result.pruned_memo_entries = pruned;
        if (!measure_memory)
            return result;
        result.recorder_measured = true;
        result.memo_entries = memo.size();
        result.parameter_entries = parameters.size();
        for (const auto& [unused, entry] : memo) {
            (void)unused;
            result.expired_memo_entries += entry.expression.expired();
        }
        result.recorder_maps_bytes =
            memory::map(memo) + memory::map(parameters) +
            memory::map(summary_ids) + memory::vector(summaries);
        result.recorder_frames_bytes =
            memory::vector(active_scopes) + memory::vector(active_arguments);
        for (const auto& frame : active_arguments)
            result.recorder_frames_bytes += memory::map(frame);
        result.definition_identity_bytes = memory::vector(definitions);
        for (const auto& id : definitions)
            result.definition_identity_bytes += memory::heap(id);
        return result;
    }

    void record(const hir::ExprPtr& value, NodeId node, StageRule rule,
                StageReason reason = StageReason::None,
                std::optional<std::uint32_t> member = {}) {
        if (known(value).value != 0)
            throw std::logic_error("stage decision already recorded");
        remember(value, node);
        decisions.push_back(
            {node, value->stage, rule, value->span, reason, member, scope()});
    }

    NodeId copy_node(NodeId original, std::span<const Edge> edges) {
        const auto lease = builder.pin(original);
        const auto node = lease.node();
        const auto copy =
            builder.add(node.rule, node.site, edges, node.payload, node.owner);
        const auto found = std::ranges::lower_bound(
            decisions, original.value, {},
            [](const auto& item) { return item.node.value; });
        if (found != decisions.end() && found->node == original) {
            auto decision = *found;
            decision.node = copy;
            decision.scope = scope();
            decision.instantiated = true;
            decisions.push_back(decision);
        }
        return copy;
    }

    // Direct graph edges are acyclic; recursive summaries are referenced by ID.
    NodeId substitute(NodeId root,
                      std::unordered_map<std::uint32_t, NodeId>& replacements,
                      bool clone) {
        std::vector<std::pair<NodeId, bool>> pending{{root, false}};
        while (!pending.empty()) {
            const auto [id, ready] = pending.back();
            pending.pop_back();
            if (replacements.contains(id.value))
                continue;
            if (!ready) {
                pending.emplace_back(id, true);
                const auto lease = builder.pin(id);
                for (const auto& edge : lease.edges())
                    pending.emplace_back(edge.target, false);
            } else {
                llvm::SmallVector<Edge, 4> edges;
                bool changed = clone;
                const auto lease = builder.pin(id);
                for (auto edge : lease.edges()) {
                    const auto target = replacements.at(edge.target.value);
                    changed |= target != edge.target;
                    edge.target = target;
                    edges.push_back(edge);
                }
                replacements[id.value] = changed ? copy_node(id, edges) : id;
            }
        }
        return replacements.at(root.value);
    }

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
        summaries.push_back({definition, id, {}});
        scopes.push_back({id, definition->name, {}, {}});
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
        case If:
        case Field:
        case Index:
            throw std::logic_error("missing semantic stage decision");
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
        case Sequence:
            for (std::size_t index = 0; index + 1 < edges.size(); ++index)
                edges[index].relation = Relation::Effect;
            return builder.add(Rule::Sequence, location, edges);
        case Array:
        case Record:
            return builder.add(Rule::Aggregate, location, edges);
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
                remember(current.expression, node);
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
        return substitute(root, replacements, false);
    }
};

ProvenanceRecorder::ProvenanceRecorder(EvidenceBuildConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
ProvenanceRecorder::~ProvenanceRecorder() = default;

void ProvenanceRecorder::binding(const syntax::BindingStmt& binding,
                                 const hir::ExprPtr& value, bool top_level) {
    impl_->attempt([&] {
        const bool required =
            binding.binding_kind == syntax::BindingKind::Comptime;
        if (!top_level && !required)
            return;
        auto root = impl_->evidence(value);
        if (!top_level)
            root = impl_->concrete(root);
        const auto location = impl_->site(binding.name_span, binding.name);
        root = impl_->builder.add(
            required ? Rule::RequireComptime : Rule::Use, location,
            std::array{
                Edge{root, required ? Relation::Contract : Relation::Data}});
        impl_->roots.push_back({binding.name, binding.name_span, value->stage,
                                root, required,
                                top_level ? SummaryId{} : impl_->scope()});
    });
}

void ProvenanceRecorder::storage(const syntax::Module::Global& global,
                                 const hir::ExprPtr& initializer) {
    impl_->attempt([&] {
        const auto value = impl_->evidence(initializer);
        const auto location =
            impl_->site(global.name_span, "static storage " + global.name);
        const auto root =
            impl_->builder.add(Rule::RequireComptime, location,
                               std::array{Edge{value, Relation::Contract}});
        impl_->roots.push_back({"static " + global.name,
                                global.name_span,
                                initializer->stage,
                                root,
                                true,
                                {}});
    });
}

void ProvenanceRecorder::enter_call(
    const std::shared_ptr<const hir::ResidualFunction>& callee,
    const std::unordered_map<const hir::Expr*, hir::ExprPtr>& arguments) {
    impl_->attempt([&] {
        const auto owner = impl_->summary(callee);
        std::unordered_map<std::uint32_t, NodeId> frame;
        for (const auto& parameter : callee->parameters) {
            const auto argument =
                impl_->concrete(impl_->evidence(arguments.at(parameter.get())));
            frame[impl_->evidence(parameter).value] = argument;
        }
        impl_->active_arguments.push_back(std::move(frame));
        impl_->active_scopes.push_back(owner);
    });
}

void ProvenanceRecorder::leave_call() {
    impl_->attempt([&] {
        impl_->active_arguments.pop_back();
        impl_->active_scopes.pop_back();
    });
}

void ProvenanceRecorder::identity(const syntax::Module& module,
                                  const sema::TypeTable& types,
                                  const cache::QueryContext& context) {
    // Auxiliary metadata failures never change Stage or its logical budgets.
    try {
        auto identity = build_definition_catalog(module, types, context);
        if (identity) {
            impl_->identity =
                ProvenanceIdentity{context, identity->module_fingerprint};
            impl_->definitions = std::move(identity->definitions);
        }
    } catch (const std::bad_alloc&) {
        impl_->identity.reset();
    }
}

void ProvenanceRecorder::specialization(
    const std::shared_ptr<const hir::ResidualFunction>& callee,
    const syntax::Function& declaration,
    std::span<const hir::ExprPtr> arguments) {
    impl_->attempt([&] {
        const auto id = impl_->summary(callee);
        auto& scope = impl_->scopes.at(id.value);
        if (scope.specialization ||
            arguments.size() != declaration.parameters.size())
            throw std::logic_error(
                "invalid evidence specialization registration");
        ProvenanceScope::Specialization recorded{declaration.name, {}};
        for (const auto& argument : arguments) {
            recorded.arguments.push_back(
                {argument->type,
                 argument->stage == hir::Stage::Comptime
                     ? cache::ArgumentStage::Comptime
                     : cache::ArgumentStage::Runtime,
                 argument->stage == hir::Stage::Comptime ? argument->constant
                                                         : std::nullopt});
        }
        if (impl_->identity) {
            const auto found = std::ranges::find_if(
                impl_->definitions, [&](const auto& definition) {
                    return definition.id.kind ==
                               cache::DefinitionKind::Function &&
                           definition.id.name == declaration.name;
                });
            if (found != impl_->definitions.end()) {
                recorded.definition = found->id;
                recorded.definition_fingerprint = found->semantic_fingerprint;
            }
        }
        scope.specialization = std::move(recorded);
    });
}

void ProvenanceRecorder::runtime_boundary(const hir::ExprPtr& value,
                                          const hir::ExprPtr& initializer) {
    impl_->attempt([&] {
        const auto root =
            impl_->builder.add(Rule::RuntimeBoundary,
                               impl_->site(value->span, impl_->label(value)),
                               std::array{Edge{impl_->evidence(initializer),
                                               Relation::Initializer}});
        impl_->record(value, root, StageRule::RuntimeBoundary);
    });
}

void ProvenanceRecorder::selection(const hir::ExprPtr& value, StageRule rule,
                                   const hir::ExprPtr& condition,
                                   const hir::ExprPtr& then_value,
                                   const hir::ExprPtr& else_value) {
    impl_->attempt([&] {
        const auto root = impl_->builder.add(
            Rule::Select, impl_->site(value->span, "if selection"),
            std::array{Edge{impl_->evidence(condition), Relation::Control},
                       Edge{impl_->evidence(then_value)},
                       Edge{impl_->evidence(else_value)}});
        impl_->record(value, root, rule);
    });
}

void ProvenanceRecorder::projection(const hir::ExprPtr& value,
                                    const hir::ExprPtr& aggregate,
                                    StageReason reason,
                                    std::optional<std::uint32_t> member,
                                    const hir::ExprPtr& selected,
                                    const hir::ExprPtr& index) {
    impl_->attempt([&] {
        const auto location = impl_->site(value->span, "member projection");
        llvm::SmallVector<Edge, 4> edges;
        if (reason == StageReason::KnownMember ||
            reason == StageReason::RuntimeMember) {
            const auto source =
                selected ? impl_->evidence(selected)
                         : impl_->builder.add(Rule::Literal, location);
            edges.push_back({source});
            edges.push_back(
                {impl_->evidence(aggregate), Relation::Initializer});
        } else {
            edges.push_back({impl_->evidence(aggregate)});
        }
        if (index)
            edges.push_back({impl_->evidence(index)});
        if (reason == StageReason::RuntimeIndex)
            edges.push_back({impl_->effect(location, "checked array index"),
                             Relation::Effect});
        const auto root = impl_->builder.add(Rule::Projection, location, edges,
                                             member.value_or(0));
        impl_->record(value, root, StageRule::MemberProjection, reason, member);
    });
}

void ProvenanceRecorder::conservative_completion(
    const std::shared_ptr<const hir::ResidualFunction>& callee) {
    impl_->attempt([&] {
        (void)impl_->summary(callee);
        const auto body = impl_->evidence(callee->body);
        const auto root = impl_->builder.add(
            Rule::RuntimeBoundary,
            impl_->site(callee->body->span,
                        "recursive completion " + callee->name),
            std::array{Edge{body, Relation::Initializer}});
        impl_->decisions.push_back({root,
                                    hir::Stage::Runtime,
                                    StageRule::ConservativeFallback,
                                    callee->body->span,
                                    StageReason::RecursiveCompletion,
                                    {},
                                    impl_->summary(callee)});
        impl_->summaries[impl_->summary_ids.at(callee.get())].completion = root;
    });
}

void ProvenanceRecorder::instantiate_inline(
    const hir::ExprPtr& body,
    const std::unordered_map<const hir::Expr*, hir::ExprPtr>& arguments,
    const std::unordered_map<const hir::Expr*, hir::ExprPtr>& instances) {
    impl_->attempt([&] {
        // Plain folded results have no migrated decision or residual identity.
        const auto existing = impl_->known(body);
        if (body->kind == hir::Expr::Kind::Constant &&
            (existing.value == 0 ||
             impl_->builder.pin(existing).node().rule == Rule::Literal))
            return;
        const auto root = impl_->evidence(body);
        std::unordered_map<std::uint32_t, NodeId> replacements;
        for (const auto& [parameter, argument] : arguments) {
            const auto found = impl_->memo.find(parameter);
            if (found != impl_->memo.end() &&
                found->second.expression.lock().get() == parameter) {
                const auto node = found->second.node;
                replacements[node.value] = impl_->evidence(argument);
            }
        }
        (void)impl_->substitute(root, replacements, true);
        for (const auto& [original, copy] : instances) {
            if (arguments.contains(original))
                continue;
            const auto found = impl_->memo.find(original);
            if (found == impl_->memo.end() ||
                found->second.expression.lock().get() != original)
                continue;
            if (const auto replacement =
                    replacements.find(found->second.node.value);
                replacement != replacements.end())
                impl_->remember(copy, replacement->second);
        }
    });
}

void ProvenanceRecorder::loop_index(const hir::ExprPtr& index,
                                    const hir::ExprPtr& begin,
                                    const hir::ExprPtr& end) {
    impl_->attempt([&] {
        const auto root = impl_->builder.add(
            Rule::Select, impl_->site(index->span, "loop index"),
            std::array{Edge{impl_->evidence(begin), Relation::Control},
                       Edge{impl_->evidence(end), Relation::Control}});
        impl_->remember(index, root);
    });
}

std::shared_ptr<const ProvenanceAnalysis>
ProvenanceRecorder::finish(const hir::Module& module) {
    impl_->attempt([&] {
        if (module.result)
            (void)impl_->evidence(module.result);
        for (const auto& function : module.residual_functions)
            if (!function->c_abi)
                (void)impl_->summary(function);
        // Processing a body may register more nested summaries. Copy each
        // record before traversal, since the summary vector can grow during
        // that traversal.
        for (std::size_t index = 0; index < impl_->summaries.size(); ++index) {
            const auto summary = impl_->summaries[index];
            const auto definition = summary.definition.lock();
            impl_->active_scopes.push_back(summary.id);
            const auto root =
                summary.completion.value != 0 ? summary.completion
                : definition && definition->body
                    ? impl_->evidence(definition->body)
                    : impl_->builder.add(
                          Rule::Incomplete,
                          impl_->site({}, "aborted function analysis"));
            impl_->active_scopes.pop_back();
            impl_->builder.define_summary(summary.id, root);
        }
        for (const auto& decision : impl_->decisions)
            impl_->scopes.at(decision.scope.value)
                .decisions.push_back(decision.node);
        for (std::size_t i = 0; i < impl_->roots.size(); ++i)
            if (impl_->roots[i].requires_comptime)
                impl_->scopes.at(impl_->roots[i].scope.value)
                    .contracts.push_back(i);
    });
    impl_->prune_expired();
    const auto recording_memory = impl_->memory_stats();
    const auto publish = [&](ProvenanceAnalysis* result) {
        result->recording_memory_ = recording_memory;
        return std::shared_ptr<const ProvenanceAnalysis>(result);
    };
    if (impl_->builder.spill && !impl_->failure) {
        impl_->failed_stats = impl_->builder.spill->stats();
        auto frozen = std::move(*impl_->builder.spill).freeze();
        if (frozen) {
            return publish(new ProvenanceAnalysis(
                std::move(frozen->view), std::move(impl_->sites),
                std::move(impl_->roots), std::move(impl_->decisions),
                std::move(impl_->scopes), std::move(impl_->identity),
                frozen->construction, {}));
        }
        if (!impl_->builder.spill->consumed())
            impl_->failed_stats = impl_->builder.spill->stats();
        impl_->builder.spill.reset();
        impl_->disable(frozen.error());
    }
    if (impl_->failure) {
        const auto failure = *impl_->failure;
        const auto nodes = static_cast<std::uint32_t>(
            std::max(std::size_t{1}, impl_->failed_stats.nodes));
        auto unavailable = recorded(EvidenceView::segmented(
            {{{1}, nodes, {cache::ObjectKind::EvidenceBlock, {}, 0}, false}},
            {},
            [failure](std::size_t)
                -> std::expected<std::shared_ptr<const EvidenceBlock>,
                                 EvidenceError> {
                return std::unexpected(failure);
            },
            0));
        return publish(new ProvenanceAnalysis(
            std::move(unavailable), std::move(impl_->sites),
            std::move(impl_->roots), std::move(impl_->decisions),
            std::move(impl_->scopes), std::move(impl_->identity),
            impl_->failed_stats, failure));
    }
    return publish(new ProvenanceAnalysis(
        std::move(impl_->builder.memory).freeze(), std::move(impl_->sites),
        std::move(impl_->roots), std::move(impl_->decisions),
        std::move(impl_->scopes), std::move(impl_->identity)));
}

void explain_provenance_experimental(const ProvenanceAnalysis& analysis,
                                     std::ostream& output,
                                     std::ostream& telemetry,
                                     ProvenanceQueryOptions options) {
    if (!options.slice)
        throw std::invalid_argument("zero provenance slice");
    if (!analysis.available()) {
        output << "provenance: unavailable error="
               << static_cast<unsigned>(analysis.failure()->code) << '\n';
        return;
    }
    try {
        const QuerySlice slice{options.slice, options.slice, options.slice,
                               options.slice};
        auto solve = [&](NodeId root, QueryKind kind,
                         const std::shared_ptr<const FormulaIndex>& index) {
            if (options.mode == ProvenanceQueryOptions::Mode::Legacy &&
                !analysis.spilled()) {
                const auto result = analysis.graph().query(root, kind);
                if (options.telemetry)
                    telemetry
                        << "provenance-query mode=legacy channel="
                        << (kind == QueryKind::Value ? "value" : "effects")
                        << " states=" << result.visited_states
                        << " followed_edges=" << result.visited_edges
                        << " contexts=" << result.contexts << '\n';
                return result;
            }
            auto task = index ? index->begin_sources(root, kind)
                              : QueryTask(analysis.evidence(), root, kind);
            if (options.mode == ProvenanceQueryOptions::Mode::Legacy)
                task.legacy_limit(1'000'000);
            while (task.progress() != QueryProgress::Completed) {
                const auto page = task.advance(slice);
                if (page.progress == QueryProgress::Failed)
                    throw EvidenceUnavailable(task.failure().value_or(
                        EvidenceError{EvidenceErrorCode::OutOfMemory, {}, {}}));
            }
            if (options.telemetry) {
                const auto& stats = task.stats();
                telemetry << "provenance-query channel="
                          << (kind == QueryKind::Value ? "value" : "effects")
                          << " states=" << stats.visited_states
                          << " scanned_edges=" << stats.scanned_edges
                          << " bindings=" << stats.bindings
                          << " contexts=" << task.invocations().size() - 1
                          << " queue_peak=" << stats.queue_peak
                          << " slices=" << stats.slices
                          << " formula_uses=" << stats.formula_uses
                          << " formula_fallbacks=" << stats.formula_fallbacks
                          << '\n';
            }
            return task.result();
        };
        const auto print_sources = [&](const QueryResult& result) {
            for (const auto& source : result.sources) {
                const auto& site = analysis.site(source.site);
                output << "    " << site.label << " @" << site.span.begin << ':'
                       << site.span.end;
                for (auto frame = source.context; frame != 0;
                     frame = result.invocations[frame].parent) {
                    auto lease = analysis.evidence()->pin(
                        result.invocations[frame].call);
                    if (!lease)
                        throw EvidenceUnavailable(lease.error());
                    const auto& call = analysis.site(lease->node().site);
                    output << " via " << call.label << " @" << call.span.begin
                           << ':' << call.span.end;
                }
                output << '\n';
            }
        };
        for (const auto& root : analysis.roots()) {
            output << "binding " << root.name << ": "
                   << hir::stage_name(root.stage) << '\n';
            if (root.requires_comptime)
                output << "  contract: Comptime\n";
            if (analysis.spilled() &&
                options.mode == ProvenanceQueryOptions::Mode::Topology) {
                EvidenceTopologyTask topology(analysis.evidence(), root.node);
                while (topology.progress() != QueryProgress::Completed) {
                    auto page = topology.advance(slice);
                    if (page.progress == QueryProgress::Failed)
                        throw EvidenceUnavailable(
                            page.failure.value_or(EvidenceError{
                                EvidenceErrorCode::OutOfMemory, {}, {}}));
                    for (const auto& record : page.records) {
                        output
                            << "  topology "
                            << (record.kind == TopologyRecordKind::Node ? "node"
                                : record.kind == TopologyRecordKind::Edge
                                    ? "edge"
                                    : "summary-link")
                            << ' ' << record.node.value;
                        if (record.kind != TopologyRecordKind::Node)
                            output << " -> " << record.edge.target.value;
                        if (record.kind == TopologyRecordKind::SummaryLink)
                            output << " summary=" << record.summary.value
                                   << " port="
                                   << (record.port == QueryKind::Value
                                           ? "value"
                                           : "effects");
                        output << '\n';
                    }
                }
                if (options.telemetry) {
                    const auto& stats = topology.stats();
                    telemetry << "provenance-topology nodes=" << stats.nodes
                              << " edges=" << stats.edges
                              << " summary_links=" << stats.summary_links
                              << " formulas=0\n";
                }
                continue;
            }
            std::shared_ptr<const FormulaIndex> index;
            if (options.mode == ProvenanceQueryOptions::Mode::Compressed ||
                options.mode == ProvenanceQueryOptions::Mode::Topology) {
                auto topology = analysis.graph().begin_topology(root.node);
                while (topology.progress() != QueryProgress::Completed) {
                    const auto page = topology.advance(slice);
                    if (page.progress == QueryProgress::Failed)
                        throw std::runtime_error(
                            "provenance topology allocation failed");
                    if (options.mode == ProvenanceQueryOptions::Mode::Topology)
                        for (const auto& record : page.records) {
                            output << "  topology "
                                   << (record.kind == TopologyRecordKind::Node
                                           ? "node"
                                       : record.kind == TopologyRecordKind::Edge
                                           ? "edge"
                                           : "summary-link")
                                   << ' ' << record.node.value;
                            if (record.kind != TopologyRecordKind::Node)
                                output << " -> " << record.edge.target.value;
                            if (record.kind == TopologyRecordKind::SummaryLink)
                                output << " summary=" << record.summary.value
                                       << " port="
                                       << (record.port == QueryKind::Value
                                               ? "value"
                                               : "effects");
                            output << '\n';
                        }
                }
                index = topology.index();
                if (options.telemetry) {
                    const auto& stats = index->stats();
                    telemetry << "provenance-topology nodes=" << stats.nodes
                              << " edges=" << stats.edges
                              << " summary_links=" << stats.summary_links
                              << " formulas=" << stats.formulas
                              << " terms=" << stats.formula_terms
                              << " source_families=" << stats.source_families
                              << " formula_capacity_bytes="
                              << stats.formula_capacity_bytes
                              << " preparation_states=" << stats.formula_states
                              << " preparation_edges=" << stats.formula_edges
                              << " reuses=" << stats.formula_reuses << '\n';
                }
                if (options.mode == ProvenanceQueryOptions::Mode::Topology)
                    continue;
            }
            const auto value = solve(root.node, QueryKind::Value, index);
            output << "  value sources: " << value.sources.size() << " ("
                   << (value.complete ? "complete" : "partial") << ")\n";
            print_sources(value);
            const auto effects = solve(root.node, QueryKind::Effects, index);
            output << "  residual effects: " << effects.sources.size() << " ("
                   << (effects.complete ? "complete" : "partial") << ")\n";
            print_sources(effects);
        }
    } catch (const EvidenceUnavailable& unavailable) {
        analysis.mark_unavailable(unavailable.error());
        output << "provenance: unavailable error="
               << static_cast<unsigned>(unavailable.error().code) << '\n';
    }
}

void explain_provenance(const ProvenanceAnalysis& analysis,
                        std::ostream& output) {
    explain_provenance_experimental(analysis, output, output, {});
}

} // namespace pagos::stage
