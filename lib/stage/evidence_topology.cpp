#include "pagos/stage/provenance_topology.h"

#include <set>
#include <stdexcept>

namespace pagos::stage::provenance {
struct EvidenceTopologyTask::Impl {
    std::shared_ptr<const EvidenceView> view;
    std::vector<NodeId> queue;
    std::set<std::uint32_t> visited;
    std::size_t cursor{}, edge{}, port{};
    std::optional<NodeLease> lease;
    QueryProgress progress{QueryProgress::Running};
    std::optional<EvidenceError> failure;
    TopologyStats stats;
    std::uint64_t sequence{};
    void enqueue(NodeId node) {
        if (visited.insert(node.value).second)
            queue.push_back(node);
    }
};
EvidenceTopologyTask::EvidenceTopologyTask(
    std::shared_ptr<const EvidenceView> view, NodeId root)
    : impl_(std::make_unique<Impl>()) {
    if (!view || !root.value || root.value > view->node_count())
        throw std::out_of_range("invalid topology root");
    impl_->view = std::move(view);
    impl_->enqueue(root);
}
EvidenceTopologyTask::~EvidenceTopologyTask() = default;
EvidenceTopologyTask::EvidenceTopologyTask(EvidenceTopologyTask&&) noexcept =
    default;
EvidenceTopologyTask&
EvidenceTopologyTask::operator=(EvidenceTopologyTask&&) noexcept = default;
void EvidenceTopologyTask::cancel() {
    if (impl_->progress == QueryProgress::Running ||
        impl_->progress == QueryProgress::Paused)
        impl_->progress = QueryProgress::Cancelled;
}
void EvidenceTopologyTask::resume() {
    if (impl_->progress == QueryProgress::Paused ||
        impl_->progress == QueryProgress::Cancelled)
        impl_->progress = QueryProgress::Running;
}
QueryProgress EvidenceTopologyTask::progress() const { return impl_->progress; }
const TopologyStats& EvidenceTopologyTask::stats() const {
    return impl_->stats;
}
TopologyPage EvidenceTopologyTask::advance(QuerySlice slice,
                                           std::stop_token cancellation) {
    auto& task = *impl_;
    if (task.progress == QueryProgress::Paused)
        resume();
    TopologyPage page;
    try {
        while (task.progress == QueryProgress::Running) {
            if (cancellation.stop_requested()) {
                cancel();
                break;
            }
            if (task.cursor == task.queue.size()) {
                task.progress = QueryProgress::Completed;
                break;
            }
            if (!slice.output)
                break;
            const auto id = task.queue[task.cursor];
            if (!task.lease) {
                if (!slice.states)
                    break;
                auto lease = task.view->pin(id);
                if (!lease) {
                    task.failure = lease.error();
                    task.progress = QueryProgress::Failed;
                    break;
                }
                task.lease = std::move(*lease);
                --slice.states;
                --slice.output;
                page.records.push_back(
                    {TopologyRecordKind::Node, id, {}, {}, QueryKind::Value});
                ++task.stats.nodes;
                continue;
            }
            if (task.edge < task.lease->edges().size()) {
                if (!slice.edges)
                    break;
                const auto edge = task.lease->edges()[task.edge];
                page.records.push_back(
                    {TopologyRecordKind::Edge, id, edge, {}, QueryKind::Value});
                task.enqueue(edge.target);
                ++task.edge;
                --slice.edges;
                --slice.output;
                ++task.stats.edges;
                continue;
            }
            const auto& node = task.lease->node();
            if ((node.rule == Rule::Call || node.rule == Rule::SummaryRef) &&
                task.port < 2) {
                if (!slice.edges)
                    break;
                if (!node.payload ||
                    node.payload > task.view->summaries().size()) {
                    task.failure = EvidenceError{
                        EvidenceErrorCode::InvalidReference, {}, {}};
                    task.progress = QueryProgress::Failed;
                    break;
                }
                const auto& summary = task.view->summary({node.payload});
                const auto target =
                    task.port == 0 ? summary.value : summary.effects;
                page.records.push_back(
                    {TopologyRecordKind::SummaryLink,
                     id,
                     {target},
                     summary.id,
                     task.port == 0 ? QueryKind::Value : QueryKind::Effects});
                task.enqueue(target);
                ++task.port;
                --slice.edges;
                --slice.output;
                ++task.stats.summary_links;
                continue;
            }
            task.lease.reset();
            task.edge = task.port = 0;
            ++task.cursor;
        }
    } catch (const std::bad_alloc&) {
        task.progress = QueryProgress::Failed;
        task.failure = EvidenceError{EvidenceErrorCode::OutOfMemory, {}, {}};
        page.records.clear();
    }
    if (task.progress == QueryProgress::Running)
        task.progress = QueryProgress::Paused;
    if (!page.records.empty())
        page.sequence = ++task.sequence;
    page.progress = task.progress;
    page.failure = task.failure;
    return page;
}
} // namespace pagos::stage::provenance
