#include "recursive_codec.h"

#include <array>

namespace pagos::stage::provenance {
namespace {
using namespace recursive_wire;
constexpr std::array widths{22U, 13U, 14U, 13U, 4U, 1U, 16U, 8U, 8U};
constexpr std::size_t segment_header = 54;
struct Segment {
    std::uint32_t first{}, count{};
    cache::ObjectRef object;
};
struct Positions {
    bool active{};
    std::optional<RecursiveAtom> source, argument;
};
template <class Task>
cache::Bytes control(const Task& task, cache::Limits limits) {
    cache::wire::Writer writer(limits);
    task.write_identity(writer, task.result_acknowledged ? receipt_version
                                                         : segmented_version);
    writer.u8(static_cast<std::uint8_t>(task.progress));
    writer.u8(static_cast<std::uint8_t>(task.phase));
    auto stats = task.stats;
    counters(stats, [&](std::size_t& value) { writer.u64(value); });
    using Phase = decltype(task.phase);
    writer.u64(task.phase == Phase::Discover || task.phase == Phase::Forward ||
                       task.phase == Phase::Reverse ||
                       task.phase == Phase::Summaries
                   ? task.cursor
                   : 0);
    const bool active = task.phase == Phase::Discover && task.lease.has_value();
    writer.boolean(active);
    writer.u64(active ? task.edge : 0);
    writer.boolean(active && task.summary_link);
    writer.u64(task.component_cursor);
    writer.u64(task.member);
    const auto& transfer = task.transfer;
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
            position(writer,
                     task.equations[task.input_at(task.equations[transfer.node],
                                                  transfer.input)]
                         .facts,
                     transfer.atom);
        if (transfer.bound != absent)
            position(writer, task.equations[transfer.bound].facts,
                     transfer.argument);
    }
    if (task.result_acknowledged)
        writer.u64(1);
    auto bytes = writer.finish();
    if (!bytes)
        throw codec_error(bytes.error());
    return std::move(*bytes);
}
template <class Task>
Positions control(Task& task, cache::wire::Reader& reader) {
    task.progress = static_cast<QueryProgress>(reader.u8());
    task.phase = static_cast<decltype(task.phase)>(reader.u8());
    counters(task.stats, [&](std::size_t& value) { value = size(reader); });
    task.cursor = size(reader);
    Positions positions;
    positions.active = reader.boolean();
    task.edge = size(reader);
    task.summary_link = reader.boolean();
    task.component_cursor = size(reader);
    task.member = size(reader);
    if (reader.boolean()) {
        auto& transfer = task.transfer;
        transfer.node = size(reader);
        require(transfer.node != absent);
        transfer.input = size(reader);
        transfer.notify = size(reader);
        transfer.bound = index(reader);
        transfer.started = reader.boolean();
        transfer.changed = reader.boolean();
        transfer.notifying = reader.boolean();
        if (transfer.started && !transfer.notifying)
            positions.source = position(reader);
        if (transfer.bound != absent)
            positions.argument = position(reader);
    }
    return positions;
}
template <class Hash> std::size_t hash_payload(const Hash& values) {
    return values.size() * sizeof(typename Hash::value_type) +
           values.bucket_count() * sizeof(void*);
}
} // namespace

RecursiveStateMemory RecursiveSummaryTask::state_memory() const {
    const auto& task = *impl_;
    RecursiveStateMemory memory;
    memory.offloaded = task.offloaded.has_value();
    if (memory.offloaded)
        return memory;
    memory.equations = task.equations.capacity() * sizeof(Impl::Equation);
    for (const auto& equation : task.equations) {
        memory.facts += equation.facts.size() * sizeof(RecursiveAtom);
        memory.relations +=
            (equation.inputs.capacity() + equation.users.capacity() +
             equation.arguments.capacity()) *
            sizeof(std::size_t);
    }
    memory.relations += task.links.capacity() * sizeof(RecursiveLink);
    memory.indexes = hash_payload(task.ids) +
                     task.known_summaries.size() * sizeof(std::uint32_t) +
                     task.summaries.capacity() * sizeof(SummaryId);
    memory.scc = task.components.capacity() * sizeof(RecursiveComponent) +
                 task.seen.capacity() +
                 task.stack.capacity() * sizeof(Impl::Frame) +
                 task.order.capacity() * sizeof(std::size_t);
    for (const auto& component : task.components)
        memory.scc += component.nodes.capacity() * sizeof(NodeId) +
                      component.summaries.capacity() * sizeof(SummaryId);
    memory.worklist = task.work.size() * sizeof(std::size_t);
    return memory;
}
const RecursiveStorageStats& RecursiveSummaryTask::storage_stats() const {
    return impl_->io;
}
const std::optional<CheckpointError>&
RecursiveSummaryTask::storage_failure() const {
    return impl_->storage_error;
}

std::expected<cache::TaskHead, CheckpointError>
RecursiveSummaryTask::save_segmented(const cache::TaskStore& store,
                                     std::uint64_t generation,
                                     std::size_t segment_bytes,
                                     cache::Limits limits) const {
    if (auto ready = ensure_resident(); !ready)
        return std::unexpected(ready.error());
    auto& task = *impl_;
    if (!task.binding || task.failure || task.progress == QueryProgress::Failed)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    if (store.task() != task.task_id)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::BindingMismatch, {}});
    task.io = {};
    try {
        if (segment_bytes < segment_header + widths[0] ||
            segment_bytes > limits.bytes)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        std::array<std::size_t, 9> totals{task.equations.size(),
                                          0,
                                          task.links.size(),
                                          task.components.size(),
                                          0,
                                          task.seen.size(),
                                          task.stack.size(),
                                          task.order.size(),
                                          task.work.size()};
        for (const auto& equation : task.equations) {
            if (equation.facts.size() > limits.elements - totals[1])
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            totals[1] += equation.facts.size();
        }
        for (const auto& component : task.components) {
            if (component.nodes.size() > limits.elements - totals[4])
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            totals[4] += component.nodes.size();
        }
        const auto prefix = control(task, limits);
        std::size_t metadata = prefix.size();
        for (unsigned table = 0; table < totals.size(); ++table) {
            const auto capacity =
                (segment_bytes - segment_header) / widths[table];
            const auto segments =
                totals[table] / capacity + (totals[table] % capacity != 0);
            if (totals[table] > limits.elements || totals[table] > UINT32_MAX ||
                limits.bytes - metadata < 8 ||
                segments > (limits.bytes - metadata - 8) / 49)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            metadata += 8 + segments * 49;
            task.io.logical_bytes += totals[table] * widths[table];
        }
        if (limits.bytes - metadata < 32)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        auto transaction = store.begin(generation);
        if (!transaction)
            throw store_error(transaction.error());
        const auto old = store.head();
        if (!old)
            throw store_error(old.error());
        if (*old) {
            if ((**old).manifest.size > limits.bytes)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            auto bytes = store.read((**old).manifest);
            if (!bytes)
                throw store_error(bytes.error());
            task.io.bytes_read += bytes->size();
            task.io.peak_codec_buffers = prefix.capacity() + bytes->capacity();
            const auto payload = verified(*bytes, limits);
            cache::wire::Reader version_reader(payload, limits);
            cache::wire::header(version_reader,
                                cache::ObjectKind::QueryTaskManifest);
            const auto version = version_reader.u32();
            if (!version_reader.good())
                finish(version_reader);
            if (version != manifest_version && version != segmented_version &&
                version != receipt_version)
                throw CheckpointError{CheckpointErrorCode::IncompatibleVersion,
                                      {}};
            cache::wire::Reader identity(payload, limits);
            Impl::identity(identity, *task.binding, task.task_id, task.selected,
                           task.channel, version);
            const auto progress = static_cast<QueryProgress>(identity.u8());
            require(progress <= QueryProgress::Completed &&
                    task_status(progress, version == receipt_version) ==
                        (**old).status);
            // A saved delivery watermark is monotone, even if a caller holds
            // an older resident snapshot and supplies the latest generation.
            if (version == receipt_version && !task.result_acknowledged)
                throw CheckpointError{CheckpointErrorCode::InvalidState, {}};
        }
        const auto write = [&](cache::ObjectKind kind, cache::Bytes bytes) {
            task.io.bytes_submitted += bytes.size();
            task.io.peak_codec_buffers =
                std::max(task.io.peak_codec_buffers,
                         prefix.capacity() + bytes.capacity());
            const auto before = transaction->charged_bytes();
            auto object = transaction->write(kind, std::move(bytes));
            if (!object)
                throw store_error(object.error());
            const auto added = transaction->charged_bytes() - before;
            task.io.cas_bytes_added += added;
            if (!added)
                ++task.io.reused_objects;
            return *object;
        };
        std::array<std::vector<Segment>, 9> directories;
        for (unsigned table = 0; table < totals.size(); ++table) {
            const auto capacity =
                (segment_bytes - segment_header) / widths[table];
            std::size_t first = 0, filled = 0, count = 0;
            cache::wire::Writer block(limits);
            const auto start = [&] {
                count = std::min(capacity, totals[table] - first);
                filled = 0;
                block = cache::wire::Writer(limits);
                cache::wire::header(block,
                                    cache::ObjectKind::QueryStateSegment);
                block.u32(segmented_version);
                block.raw(task.task_id.bytes);
                block.u8(static_cast<std::uint8_t>(table));
                block.count(first);
                block.count(count);
            };
            if (totals[table])
                start();
            const auto emit = [&](auto encode) {
                encode(block);
                if (++filled != count)
                    return;
                auto bytes = block.finish();
                if (!bytes)
                    throw codec_error(bytes.error());
                const auto object = write(cache::ObjectKind::QueryStateSegment,
                                          std::move(*bytes));
                directories[table].push_back({static_cast<std::uint32_t>(first),
                                              static_cast<std::uint32_t>(count),
                                              object});
                ++task.io.segment_writes;
                first += count;
                if (first < totals[table])
                    start();
            };
            if (table == 0) {
                std::size_t begin = 0;
                for (const auto& equation : task.equations) {
                    emit([&](auto& out) {
                        out.u32(equation.id.value);
                        out.u32(equation.issues);
                        out.boolean(equation.merged);
                        out.boolean(equation.queued);
                        out.u64(begin);
                        out.count(equation.facts.size());
                    });
                    begin += equation.facts.size();
                }
            } else if (table == 1) {
                for (const auto& equation : task.equations)
                    for (const auto& value : equation.facts)
                        emit([&](auto& out) { atom(out, value); });
            } else if (table == 2) {
                for (const auto& link : task.links)
                    emit([&](auto& out) {
                        out.u32(link.from.value);
                        out.u32(link.to.value);
                        out.u8(static_cast<std::uint8_t>(link.kind));
                        out.u8(static_cast<std::uint8_t>(link.relation));
                        out.u32(link.slot);
                    });
            } else if (table == 3) {
                std::size_t begin = 0;
                for (const auto& component : task.components) {
                    emit([&](auto& out) {
                        out.boolean(component.cyclic);
                        out.u64(begin);
                        out.count(component.nodes.size());
                    });
                    begin += component.nodes.size();
                }
            } else if (table == 4) {
                for (const auto& component : task.components)
                    for (const auto node : component.nodes)
                        emit([&](auto& out) { out.u32(node.value); });
            } else if (table == 5) {
                for (const auto value : task.seen)
                    emit([&](auto& out) { out.boolean(value != 0); });
            } else if (table == 6) {
                for (const auto frame : task.stack)
                    emit([&](auto& out) {
                        out.u64(frame.node);
                        out.u64(frame.edge);
                    });
            } else if (table == 7) {
                for (const auto value : task.order)
                    emit([&](auto& out) { out.u64(value); });
            } else {
                for (const auto value : task.work)
                    emit([&](auto& out) { out.u64(value); });
            }
            require(first == totals[table]);
            task.io.directory_bytes +=
                directories[table].capacity() * sizeof(Segment);
        }
        cache::wire::Writer manifest(limits);
        manifest.raw(prefix);
        for (unsigned table = 0; table < totals.size(); ++table) {
            manifest.count(totals[table]);
            manifest.count(directories[table].size());
            for (const auto& segment : directories[table]) {
                manifest.u32(segment.first);
                manifest.u32(segment.count);
                manifest.u8(static_cast<std::uint8_t>(segment.object.kind));
                manifest.raw(segment.object.digest.bytes);
                manifest.u64(segment.object.size);
            }
        }
        auto object = write(cache::ObjectKind::QueryTaskManifest,
                            sealed(manifest.finish(), limits));
        auto head = transaction->commit(
            object, task_status(task.progress, task.result_acknowledged));
        if (!head)
            throw store_error(head.error());
        return *head;
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
                              const cache::TaskStore& store,
                              const cache::TaskHead& head, SummaryId summary,
                              QueryKind channel, cache::Limits limits) {
    try {
        require(head.manifest.kind == cache::ObjectKind::QueryTaskManifest);
        if (head.manifest.size > limits.bytes)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        auto bytes = store.read(head.manifest);
        if (!bytes)
            throw store_error(bytes.error());
        const auto payload = verified(*bytes, limits);
        cache::wire::Reader version_reader(payload, limits);
        cache::wire::header(version_reader,
                            cache::ObjectKind::QueryTaskManifest);
        const auto version = version_reader.u32();
        if (!version_reader.good())
            finish(version_reader);
        if (version == manifest_version) {
            auto result = restore(std::move(binding), store.task(), summary,
                                  channel, *bytes, limits);
            if (!result)
                return result;
            require(task_status(result->progress()) == head.status);
            result->impl_->io.bytes_read = bytes->size();
            result->impl_->io.peak_codec_buffers = bytes->capacity();
            return result;
        }
        if (version != segmented_version && version != receipt_version)
            throw CheckpointError{CheckpointErrorCode::IncompatibleVersion, {}};
        cache::wire::Reader reader(payload, limits);
        Impl::identity(reader, binding, store.task(), summary, channel,
                       version);
        RecursiveSummaryTask result(std::move(binding), store.task(), summary,
                                    channel);
        auto& task = *result.impl_;
        task.equations.clear();
        task.ids.clear();
        task.summaries.clear();
        task.known_summaries.clear();
        auto positions = control(task, reader);
        if (version == receipt_version) {
            require(reader.u64() == 1 &&
                    task.progress == QueryProgress::Completed);
            task.result_acknowledged = true;
        }
        require(task_status(task.progress, task.result_acknowledged) ==
                head.status);
        task.io.bytes_read = bytes->size();
        task.io.peak_codec_buffers = bytes->capacity();
        std::array<std::size_t, 9> totals;
        std::array<std::vector<Segment>, 9> directories;
        for (unsigned table = 0; table < totals.size(); ++table) {
            totals[table] = reader.count(0);
            const auto segments = reader.count(49);
            if (!reader.good())
                finish(reader);
            require(segments <= totals[table]);
            std::size_t next = 0;
            for (std::uint32_t i = 0; i < segments; ++i) {
                Segment segment{reader.u32(), reader.u32(), {}};
                segment.object = {cache::wire::object_kind(reader),
                                  digest(reader), reader.u64()};
                if (!reader.good())
                    finish(reader);
                require(segment.first == next && segment.count &&
                        segment.count <= totals[table] - next &&
                        segment.object.kind ==
                            cache::ObjectKind::QueryStateSegment &&
                        segment.object.size ==
                            segment_header +
                                std::uint64_t{segment.count} * widths[table]);
                if (segment.object.size > limits.bytes)
                    throw CheckpointError{CheckpointErrorCode::LimitExceeded,
                                          {}};
                next += segment.count;
                directories[table].push_back(segment);
            }
            require(next == totals[table]);
            task.io.logical_bytes += totals[table] * widths[table];
            task.io.directory_bytes +=
                directories[table].capacity() * sizeof(Segment);
        }
        finish(reader);
        std::vector<std::size_t> fact_counts, component_counts;
        std::size_t expected_facts = 0, expected_nodes = 0, equation_cursor = 0,
                    component_cursor = 0;
        for (unsigned table = 0; table < totals.size(); ++table) {
            for (const auto& segment : directories[table]) {
                auto leaf = store.read(segment.object);
                if (!leaf)
                    throw store_error(leaf.error());
                ++task.io.segment_reads;
                task.io.bytes_read += leaf->size();
                task.io.peak_codec_buffers =
                    std::max(task.io.peak_codec_buffers,
                             bytes->capacity() + leaf->capacity());
                cache::wire::Reader input(*leaf, limits);
                cache::wire::header(input,
                                    cache::ObjectKind::QueryStateSegment);
                if (input.u32() != segmented_version)
                    throw CheckpointError{
                        CheckpointErrorCode::IncompatibleVersion, {}};
                require(digest(input) == store.task() && input.u8() == table &&
                        input.u32() == segment.first &&
                        input.u32() == segment.count);
                for (std::uint32_t i = 0; i < segment.count && input.good();
                     ++i) {
                    if (table == 0) {
                        Impl::Equation equation{.id = {input.u32()},
                                                .node = {}};
                        require(equation.id.value &&
                                equation.id.value <= task.view->node_count() &&
                                task.ids
                                    .emplace(equation.id.value,
                                             task.equations.size())
                                    .second);
                        equation.issues = input.u32();
                        equation.merged = input.boolean();
                        equation.queued = input.boolean();
                        const auto first = size(input);
                        const auto count = input.u32();
                        require(first == expected_facts &&
                                count <= totals[1] - expected_facts);
                        expected_facts += count;
                        fact_counts.push_back(count);
                        task.equations.push_back(std::move(equation));
                    } else if (table == 1) {
                        while (equation_cursor < fact_counts.size() &&
                               task.equations[equation_cursor].facts.size() ==
                                   fact_counts[equation_cursor])
                            ++equation_cursor;
                        require(equation_cursor < task.equations.size());
                        auto& facts = task.equations[equation_cursor].facts;
                        const auto value = atom(input);
                        require(facts.empty() || *facts.rbegin() < value);
                        facts.insert(value);
                    } else if (table == 2) {
                        task.links.push_back(
                            {{input.u32()},
                             {input.u32()},
                             static_cast<RecursiveLinkKind>(input.u8()),
                             static_cast<Relation>(input.u8()),
                             input.u32()});
                    } else if (table == 3) {
                        RecursiveComponent component;
                        component.cyclic = input.boolean();
                        const auto first = size(input);
                        const auto count = input.u32();
                        require(first == expected_nodes &&
                                count <= totals[4] - expected_nodes);
                        expected_nodes += count;
                        component_counts.push_back(count);
                        task.components.push_back(std::move(component));
                    } else if (table == 4) {
                        while (component_cursor < component_counts.size() &&
                               task.components[component_cursor].nodes.size() ==
                                   component_counts[component_cursor])
                            ++component_cursor;
                        require(component_cursor < task.components.size());
                        task.components[component_cursor].nodes.push_back(
                            {input.u32()});
                    } else if (table == 5)
                        task.seen.push_back(input.boolean());
                    else if (table == 6)
                        task.stack.push_back({size(input), size(input)});
                    else if (table == 7)
                        task.order.push_back(size(input));
                    else
                        task.work.push_back(size(input));
                }
                finish(input);
            }
            if (table == 0)
                require(expected_facts == totals[1]);
            if (table == 3)
                require(expected_nodes == totals[4]);
        }
        task.validate_state(positions.active);
        task.rebind(positions.source, positions.argument);
        task.io.validation_payload_bytes +=
            (fact_counts.capacity() + component_counts.capacity()) *
            sizeof(std::size_t);
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

std::expected<cache::TaskHead, CheckpointError>
RecursiveSummaryTask::offload(const cache::TaskStore& store,
                              std::uint64_t generation,
                              std::size_t segment_bytes, cache::Limits limits) {
    if (auto ready = ensure_resident(); !ready)
        return std::unexpected(ready.error());
    try {
        auto empty = std::make_unique<Impl>(impl_->view, impl_->selected,
                                            impl_->channel, false);
        empty->binding = impl_->binding;
        empty->task_id = impl_->task_id;
        empty->stats = impl_->stats;
        empty->progress = impl_->progress;
        empty->result_acknowledged = impl_->result_acknowledged;
        empty->offloaded = Impl::Offloaded{store, {}, limits, impl_->progress};
        auto saved = save_segmented(store, generation, segment_bytes, limits);
        if (!saved)
            return saved;
        empty->offloaded->head = *saved;
        empty->io = impl_->io;
        impl_.swap(empty);
        return saved;
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    }
}
std::expected<void, CheckpointError>
RecursiveSummaryTask::ensure_resident() const {
    if (!impl_->offloaded)
        return {};
    const auto& state = *impl_->offloaded;
    auto result = restore(*impl_->binding, state.store, state.head,
                          impl_->selected, impl_->channel, state.limits);
    if (!result) {
        impl_->storage_error = result.error();
        impl_->progress = state.resume_progress == QueryProgress::Cancelled
                              ? QueryProgress::Cancelled
                              : QueryProgress::Paused;
        return std::unexpected(result.error());
    }
    result->impl_->progress = state.resume_progress;
    impl_ = std::move(result->impl_);
    return {};
}
} // namespace pagos::stage::provenance
