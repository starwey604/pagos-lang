#include "recursive_codec.h"

namespace pagos::stage::provenance {
using namespace recursive_wire;

void RecursiveSummaryTask::Impl::write_state(
    cache::wire::Writer& writer) const {
    writer.u8(static_cast<std::uint8_t>(progress));
    writer.u8(static_cast<std::uint8_t>(phase));
    auto saved = stats;
    counters(saved, [&](std::size_t& value) { writer.u64(value); });
    writer.u64(phase == Phase::Discover || phase == Phase::Forward ||
                       phase == Phase::Reverse || phase == Phase::Summaries
                   ? cursor
                   : 0);
    const bool active = phase == Phase::Discover && lease.has_value();
    writer.boolean(active);
    writer.u64(active ? edge : 0);
    writer.boolean(active && summary_link);
    writer.count(equations.size());
    for (const auto& equation : equations) {
        writer.u32(equation.id.value);
        writer.u32(equation.issues);
        writer.boolean(equation.merged);
        writer.boolean(equation.queued);
        writer.count(equation.facts.size());
        for (const auto& value : equation.facts)
            atom(writer, value);
    }
    writer.count(links.size());
    for (const auto& link : links) {
        writer.u32(link.from.value);
        writer.u32(link.to.value);
        writer.u8(static_cast<std::uint8_t>(link.kind));
        writer.u8(static_cast<std::uint8_t>(link.relation));
        writer.u32(link.slot);
    }
    writer.count(components.size());
    for (const auto& component : components) {
        writer.boolean(component.cyclic);
        writer.count(component.nodes.size());
        for (const auto node : component.nodes)
            writer.u32(node.value);
    }
    writer.count(seen.size());
    for (const auto value : seen)
        writer.boolean(value != 0);
    writer.count(stack.size());
    for (const auto frame : stack) {
        writer.u64(frame.node);
        writer.u64(frame.edge);
    }
    writer.count(order.size());
    for (const auto value : order)
        writer.u64(value);
    writer.u64(component_cursor);
    writer.u64(member);
    writer.count(work.size());
    for (const auto value : work)
        writer.u64(value);
    writer.boolean(transfer.node != absent);
    if (transfer.node != absent) {
        writer.u64(transfer.node);
        writer.u64(transfer.input);
        writer.u64(transfer.notify);
        index(writer, transfer.bound);
        writer.boolean(transfer.started);
        writer.boolean(transfer.changed);
        writer.boolean(transfer.notifying);
        if (transfer.started && !transfer.notifying)
            position(
                writer,
                equations[input_at(equations[transfer.node], transfer.input)]
                    .facts,
                transfer.atom);
        if (transfer.bound != absent)
            position(writer, equations[transfer.bound].facts,
                     transfer.argument);
    }
}

void RecursiveSummaryTask::Impl::read_state(cache::wire::Reader& reader) {
    progress = static_cast<QueryProgress>(reader.u8());
    phase = static_cast<Phase>(reader.u8());
    counters(stats, [&](std::size_t& value) { value = size(reader); });
    cursor = size(reader);
    const bool active = reader.boolean();
    edge = size(reader);
    summary_link = reader.boolean();
    equations.clear();
    ids.clear();
    links.clear();
    components.clear();
    summaries.clear();
    known_summaries.clear();
    auto count = reader.count(14);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
        Equation equation{.id = {reader.u32()}, .node = {}};
        require(equation.id.value && equation.id.value <= view->node_count() &&
                ids.emplace(equation.id.value, equations.size()).second);
        equation.issues = reader.u32();
        equation.merged = reader.boolean();
        equation.queued = reader.boolean();
        const auto facts = reader.count(13);
        std::optional<RecursiveAtom> previous;
        for (std::uint32_t j = 0; j < facts && reader.good(); ++j) {
            const auto value = atom(reader);
            require(!previous || *previous < value);
            equation.facts.insert(value);
            previous = value;
        }
        equations.push_back(std::move(equation));
    }
    count = reader.count(14);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i)
        links.push_back({{reader.u32()},
                         {reader.u32()},
                         static_cast<RecursiveLinkKind>(reader.u8()),
                         static_cast<Relation>(reader.u8()),
                         reader.u32()});
    count = reader.count(5);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
        RecursiveComponent component;
        component.cyclic = reader.boolean();
        const auto nodes = reader.count(4);
        for (std::uint32_t j = 0; j < nodes && reader.good(); ++j)
            component.nodes.push_back({reader.u32()});
        components.push_back(std::move(component));
    }
    count = reader.count();
    for (std::uint32_t i = 0; i < count && reader.good(); ++i)
        seen.push_back(reader.boolean());
    count = reader.count(16);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i)
        stack.push_back({size(reader), size(reader)});
    count = reader.count(8);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i)
        order.push_back(size(reader));
    component_cursor = size(reader);
    member = size(reader);
    count = reader.count(8);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i)
        work.push_back(size(reader));
    std::optional<RecursiveAtom> saved_atom, saved_argument;
    if (reader.boolean()) {
        transfer.node = size(reader);
        require(transfer.node != absent);
        transfer.input = size(reader);
        transfer.notify = size(reader);
        transfer.bound = index(reader);
        transfer.started = reader.boolean();
        transfer.changed = reader.boolean();
        transfer.notifying = reader.boolean();
        if (transfer.started && !transfer.notifying)
            saved_atom = position(reader);
        if (transfer.bound != absent)
            saved_argument = position(reader);
    }
    finish(reader);
    validate_state(active);
    rebind(saved_atom, saved_argument);
}

void RecursiveSummaryTask::Impl::rebind(
    const std::optional<RecursiveAtom>& saved_atom,
    const std::optional<RecursiveAtom>& saved_argument) {
    if (transfer.node != absent && transfer.started && !transfer.notifying) {
        const auto& destination = equations[transfer.node];
        const auto source = input_at(destination, transfer.input);
        transfer.atom = locate(equations[source].facts, saved_atom);
        if (transfer.bound != absent) {
            require(transfer.atom != equations[source].facts.end());
            const auto value = *transfer.atom;
            require(destination.node.rule == Rule::Call &&
                    destination.callee == source &&
                    value.kind == RecursiveAtomKind::Parameter &&
                    value.owner == destination.node.payload &&
                    value.slot < destination.arguments.size() &&
                    transfer.bound == destination.arguments[value.slot]);
            transfer.argument =
                locate(equations[transfer.bound].facts, saved_argument);
        }
    }
}

void RecursiveSummaryTask::Impl::validate_state(bool active) {
    const auto n = equations.size();
    require(n && equations.front().id == root(selected) &&
            progress <= QueryProgress::Completed && phase <= Phase::Done &&
            (progress == QueryProgress::Completed) == (phase == Phase::Done));
    require(stats.nodes <= n && stats.links == links.size() &&
            stats.components == components.size() && stats.queue_peak <= n &&
            stats.queue_peak >= work.size());
    for (const auto value : {stats.nodes, stats.links, stats.components,
                             stats.evaluations, stats.facts_added})
        require(value <= stats.steps);
    const bool discovering = phase == Phase::Discover;
    if (discovering)
        require(cursor <= n && (!active || cursor < n) &&
                stats.nodes == cursor + (active ? 1U : 0U));
    else
        require(!active && stats.nodes == n);
    if (!active)
        require(!edge && !summary_link);
    std::size_t next_link = 0, admitted = 1, fact_count = 0;
    std::vector<std::set<RecursiveAtom>> seeds(n);
    std::vector<std::uint32_t> local_issues(n);
    std::set<RecursiveAtom> universe;
    register_summary(selected);
    for (std::size_t i = 0; i < stats.nodes; ++i) {
        require(i < admitted);
        auto loaded = view->pin(equations[i].id);
        if (!loaded)
            throw CheckpointError{CheckpointErrorCode::UnavailableEvidence,
                                  loaded.error()};
        equations[i].node = loaded->node();
        const auto node = loaded->node();
        const auto edges = loaded->edges();
        auto& issues = local_issues[i];
        if (node.rule > Rule::Incomplete)
            issues |= bit(RecursiveIssue::UnsupportedRule);
        if (node.rule == Rule::Incomplete)
            issues |= bit(RecursiveIssue::IncompleteNode);
        if (node.rule == Rule::Parameter) {
            if (!node.owner.value ||
                node.owner.value > view->summaries().size())
                issues |= bit(RecursiveIssue::UnboundParameter);
            else
                seeds[i].insert({RecursiveAtomKind::Parameter, node.owner.value,
                                 node.payload, 0});
        }
        if (channel == QueryKind::Value ? node.rule == Rule::Origin ||
                                              node.rule == Rule::RuntimeBoundary
                                        : node.rule == Rule::Effect)
            seeds[i].insert(
                {RecursiveAtomKind::SourceFamily, 0, 0, equations[i].id.value});
        universe.insert(seeds[i].begin(), seeds[i].end());
        const bool current = discovering && active && i == cursor;
        const bool references =
            node.rule == Rule::Call || node.rule == Rule::SummaryRef;
        if (current && summary_link)
            require(references && edge == 0);
        const auto accept = [&](NodeId to, RecursiveLinkKind kind,
                                Relation relation, std::uint32_t slot) {
            require(next_link < links.size());
            require(links[next_link++] ==
                    RecursiveLink{equations[i].id, to, kind, relation, slot});
            const auto found = ids.find(to.value);
            require(found != ids.end());
            const auto target = found->second;
            if (target >= admitted) {
                require(target == admitted);
                ++admitted;
            }
            equations[i].inputs.push_back(target);
            equations[target].users.push_back(i);
            if (kind == RecursiveLinkKind::Summary)
                equations[i].callee = target;
            if (kind == RecursiveLinkKind::Argument)
                equations[i].arguments.push_back(target);
        };
        if (references && !(current && summary_link)) {
            if (!node.payload || node.payload > view->summaries().size())
                issues |= bit(RecursiveIssue::MissingSummary);
            else {
                register_summary({node.payload});
                accept(root({node.payload}), RecursiveLinkKind::Summary,
                       Relation::Data, 0);
            }
        }
        const bool physical =
            node.rule != Rule::Parameter && node.rule != Rule::SummaryRef;
        if (current)
            require(edge <= edges.size() && (physical || edge == 0));
        const auto prefix = physical ? (current ? edge : edges.size()) : 0;
        for (std::size_t j = 0; j < prefix; ++j) {
            const auto next = edges[j];
            if (next.relation > Relation::Initializer)
                issues |= bit(RecursiveIssue::UnsupportedRelation);
            if (node.rule == Rule::Call)
                accept(next.target, RecursiveLinkKind::Argument, next.relation,
                       static_cast<std::uint32_t>(j));
            else if (followed(channel, next.relation))
                accept(next.target, RecursiveLinkKind::Dependency,
                       next.relation, 0);
        }
        if (current)
            lease = std::move(*loaded);
    }
    require(admitted == n && next_link == links.size());
    const bool early = phase == Phase::Discover || phase == Phase::Forward ||
                       phase == Phase::Reverse || phase == Phase::Summaries;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& equation = equations[i];
        require((equation.issues & ~std::uint32_t{127}) == 0 &&
                (equation.issues & local_issues[i]) == local_issues[i]);
        for (const auto& seed : seeds[i])
            require(equation.facts.contains(seed));
        for (const auto& value : equation.facts)
            require(universe.contains(value));
        fact_count += equation.facts.size();
        if (early || i >= stats.nodes)
            require(equation.facts == seeds[i] &&
                    equation.issues == local_issues[i] && !equation.merged &&
                    !equation.queued);
    }
    require(stats.facts_added == fact_count);
    if (discovering)
        require(seen.empty() && order.empty() && stack.empty() &&
                components.empty());
    else
        require(seen.size() == n);
    std::vector<bool> ordered(n), stacked(n), assigned(n), queued(n);
    io.validation_payload_bytes =
        seeds.capacity() * sizeof(decltype(seeds)::value_type) +
        local_issues.capacity() * sizeof(std::uint32_t) +
        universe.size() * sizeof(RecursiveAtom) +
        (ordered.capacity() + stacked.capacity() + assigned.capacity() +
         queued.capacity()) /
            8;
    for (const auto& seed : seeds)
        io.validation_payload_bytes += seed.size() * sizeof(RecursiveAtom);
    for (const auto value : order) {
        require(value < n && !ordered[value]);
        ordered[value] = true;
    }
    if (!discovering && phase != Phase::Forward) {
        require(order.size() == n);
        require(
            std::ranges::all_of(seen, [](auto value) { return value == 1; }));
    }
    if (phase == Phase::Forward) {
        require(cursor <= n && components.empty());
        for (std::size_t i = 0; i < cursor; ++i)
            require(seen[i]);
    }
    std::size_t cyclic = 0;
    for (std::size_t c = 0; c < components.size(); ++c) {
        const auto& component = components[c];
        require(!component.nodes.empty());
        bool self = false;
        for (const auto node : component.nodes) {
            const auto found = ids.find(node.value);
            require(found != ids.end() && !assigned[found->second]);
            const auto eq = found->second;
            assigned[eq] = true;
            equations[eq].component = c;
            self |= std::ranges::find(equations[eq].inputs, eq) !=
                    equations[eq].inputs.end();
        }
        const bool complete = phase != Phase::Reverse || stack.empty() ||
                              c + 1 < components.size();
        require(component.nodes.size() > 1
                    ? component.cyclic
                    : (!component.cyclic || self) &&
                          (!complete || component.cyclic == self));
        cyclic += component.cyclic ? 1U : 0U;
    }
    require(stats.cyclic_components == cyclic);
    if (phase != Phase::Discover && phase != Phase::Forward &&
        phase != Phase::Reverse)
        require(
            std::ranges::all_of(assigned, [](bool value) { return value; }));
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const auto frame = stack[i];
        require(frame.node < n && !stacked[frame.node]);
        stacked[frame.node] = true;
        require(phase == Phase::Forward || phase == Phase::Reverse);
        const auto& adjacent = phase == Phase::Forward
                                   ? equations[frame.node].inputs
                                   : equations[frame.node].users;
        require(frame.edge <= adjacent.size());
        for (std::size_t j = 0; j < frame.edge; ++j)
            require(phase == Phase::Forward ? seen[adjacent[j]] != 0
                                            : assigned[adjacent[j]]);
        if (phase == Phase::Forward)
            require(seen[frame.node] && !ordered[frame.node]);
        else
            require(equations[frame.node].component + 1 == components.size());
        if (i + 1 < stack.size())
            require(frame.edge &&
                    adjacent[frame.edge - 1] == stack[i + 1].node);
    }
    if (phase == Phase::Forward)
        for (std::size_t i = 0; i < n; ++i)
            require((seen[i] != 0) == (ordered[i] || stacked[i]));
    if (phase == Phase::Reverse) {
        require(cursor <= order.size());
        for (std::size_t i = cursor; i < order.size(); ++i)
            require(assigned[order[i]]);
    }
    if (phase != Phase::Forward && phase != Phase::Reverse)
        require(stack.empty());
    if (phase == Phase::Discover || phase == Phase::Forward)
        require(components.empty());
    for (const auto& link : links) {
        const auto from = equations[ids.at(link.from.value)].component;
        const auto to = equations[ids.at(link.to.value)].component;
        if (from != absent && to != absent)
            require(from <= to);
    }
    if (phase != Phase::Discover && phase != Phase::Forward)
        for (std::size_t i = 0; i < n; ++i)
            if (assigned[i] && (phase != Phase::Reverse || stack.empty() ||
                                equations[i].component + 1 < components.size()))
                for (const auto user : equations[i].users)
                    require(assigned[user]);
    const auto registered = phase == Phase::Summaries ? cursor
                            : early                   ? 0
                                                      : summaries.size();
    require(registered <= summaries.size());
    for (std::size_t i = 0; i < registered; ++i) {
        const auto id = summaries[i];
        const auto equation = ids.find(root(id).value);
        require(equation != ids.end() &&
                equations[equation->second].component != absent);
        components[equations[equation->second].component].summaries.push_back(
            id);
    }
    for (const auto node : work) {
        require(node < n && !queued[node]);
        queued[node] = true;
        require(equations[node].component == component_cursor);
    }
    for (std::size_t i = 0; i < n; ++i)
        require(equations[i].queued == queued[i]);
    if (phase == Phase::Seed || phase == Phase::Solve) {
        require(component_cursor <= components.size());
        if (phase == Phase::Solve || member) {
            require(component_cursor < components.size() &&
                    member <= components[component_cursor].nodes.size());
            if (phase == Phase::Solve)
                require(member == components[component_cursor].nodes.size());
        }
        if (phase == Phase::Seed) {
            require(transfer.node == absent && work.size() == member);
            for (std::size_t i = 0; i < member; ++i)
                require(work[i] ==
                        ids.at(components[component_cursor].nodes[i].value));
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto c = equations[i].component;
            if (c < component_cursor ||
                (phase == Phase::Seed && member && c == component_cursor))
                require(equations[i].facts == seeds[i] &&
                        equations[i].issues == local_issues[i] &&
                        !equations[i].merged);
        }
    } else
        require(work.empty() && !member && !component_cursor &&
                transfer.node == absent);
    if (early)
        require(!stats.evaluations && !stats.queue_peak);
    if (transfer.node != absent) {
        require(phase == Phase::Solve && transfer.node < n &&
                equations[transfer.node].component == component_cursor &&
                stats.evaluations != 0);
        const auto& destination = equations[transfer.node];
        const auto count = input_count(destination);
        require(transfer.input <= count &&
                transfer.notify <= destination.users.size());
        if (transfer.notifying)
            require(transfer.input == count && transfer.bound == absent &&
                    !transfer.started &&
                    (transfer.changed || transfer.notify == 0));
        else {
            require(transfer.notify == 0);
            if (transfer.started || transfer.bound != absent)
                require(transfer.input < count);
            if (transfer.bound != absent)
                require(transfer.started && transfer.bound < n);
        }
    }
}

void RecursiveSummaryTask::Impl::write_identity(cache::wire::Writer& writer,
                                                std::uint32_t version) const {
    cache::wire::header(writer, cache::ObjectKind::QueryTaskManifest);
    writer.u32(version);
    writer.u32(recursive_engine_version);
    writer.u32(query_engine_version);
    writer.u32(provenance_record_model);
    writer.raw(binding->closure_.bytes);
    writer.raw(binding->positions_.bytes);
    writer.raw(binding->revision_.bytes);
    writer.raw(task_id.bytes);
    writer.u32(selected.value);
    writer.u8(static_cast<std::uint8_t>(channel));
}

void RecursiveSummaryTask::Impl::identity(cache::wire::Reader& reader,
                                          const QueryCheckpointBinding& binding,
                                          cache::Digest task, SummaryId summary,
                                          QueryKind channel,
                                          std::uint32_t version) {
    cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
    const auto format = reader.u32(), engine = reader.u32(),
               layout = reader.u32(), model = reader.u32();
    if (!reader.good())
        finish(reader);
    if (format != version || engine != recursive_engine_version ||
        layout != query_engine_version || model != provenance_record_model)
        throw CheckpointError{CheckpointErrorCode::IncompatibleVersion, {}};
    const auto closure = digest(reader), positions = digest(reader),
               revision = digest(reader), identity = digest(reader);
    const SummaryId selected{reader.u32()};
    const auto kind = static_cast<QueryKind>(reader.u8());
    if (!reader.good())
        finish(reader);
    if (closure != binding.closure_ || positions != binding.positions_ ||
        revision != binding.revision_ || identity != task ||
        selected != summary || kind != channel)
        throw CheckpointError{CheckpointErrorCode::BindingMismatch, {}};
    require(selected.value &&
            selected.value <= binding.view_->summaries().size() &&
            (kind == QueryKind::Value || kind == QueryKind::Effects));
}

std::expected<cache::Bytes, CheckpointError>
RecursiveSummaryTask::checkpoint(cache::Limits limits) const {
    if (auto ready = ensure_resident(); !ready)
        return std::unexpected(ready.error());
    try {
        const auto& task = *impl_;
        if (!task.binding || task.failure || task.result_acknowledged ||
            task.progress == QueryProgress::Failed)
            return std::unexpected(
                CheckpointError{CheckpointErrorCode::InvalidState, {}});
        cache::wire::Writer writer(limits);
        task.write_identity(writer, manifest_version);
        task.write_state(writer);
        auto bytes = writer.finish();
        if (!bytes)
            throw codec_error(bytes.error());
        if (limits.bytes < 32 || bytes->size() > limits.bytes - 32)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        const auto hash = cache::fingerprint(checksum_domain, *bytes);
        bytes->insert(bytes->end(), hash.bytes.begin(), hash.bytes.end());
        return std::move(*bytes);
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}

std::expected<RecursiveSummaryTask, CheckpointError>
RecursiveSummaryTask::restore(QueryCheckpointBinding binding,
                              cache::Digest task, SummaryId summary,
                              QueryKind channel,
                              std::span<const std::uint8_t> bytes,
                              cache::Limits limits) {
    try {
        if (bytes.size() > limits.bytes)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        require(bytes.size() >= 32);
        const auto payload = bytes.first(bytes.size() - 32);
        const auto hash = cache::fingerprint(checksum_domain, payload);
        require(std::ranges::equal(hash.bytes, bytes.last(32)));
        cache::wire::Reader reader(payload, limits);
        Impl::identity(reader, binding, task, summary, channel,
                       manifest_version);
        RecursiveSummaryTask result(std::move(binding), task, summary, channel);
        result.impl_->read_state(reader);
        return result;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}
} // namespace pagos::stage::provenance
