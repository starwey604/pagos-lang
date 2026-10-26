#include "recursive_state.h"

namespace pagos::stage::provenance {

RecursiveSummaryTask::RecursiveSummaryTask(
    std::shared_ptr<const EvidenceView> evidence, SummaryId summary,
    QueryKind channel)
    : impl_(std::make_unique<Impl>(std::move(evidence), summary, channel)) {}
RecursiveSummaryTask::RecursiveSummaryTask(QueryCheckpointBinding binding,
                                           cache::Digest task,
                                           SummaryId summary, QueryKind channel)
    : RecursiveSummaryTask(binding.view_, summary, channel) {
    impl_->binding = std::move(binding);
    impl_->task_id = task;
}
RecursiveSummaryTask::~RecursiveSummaryTask() = default;
RecursiveSummaryTask::RecursiveSummaryTask(RecursiveSummaryTask&&) noexcept =
    default;
RecursiveSummaryTask&
RecursiveSummaryTask::operator=(RecursiveSummaryTask&&) noexcept = default;
QueryProgress RecursiveSummaryTask::advance(std::size_t steps,
                                            std::stop_token cancellation) {
    if (impl_->offloaded && cancellation.stop_requested())
        cancel();
    if (!ensure_resident())
        return impl_->progress;
    auto& task = *impl_;
    if (task.progress == QueryProgress::Completed ||
        task.progress == QueryProgress::Failed ||
        task.progress == QueryProgress::Cancelled)
        return task.progress;
    ++task.stats.slices;
    task.progress = QueryProgress::Running;
    try {
        while (task.progress == QueryProgress::Running) {
            if (cancellation.stop_requested()) {
                task.progress = QueryProgress::Cancelled;
                break;
            }
            if (!steps) {
                task.progress = QueryProgress::Paused;
                break;
            }
            --steps;
            ++task.stats.steps;
            task.step();
        }
    } catch (const ReadFailure& error) {
        task.failure = error.error;
        task.progress = QueryProgress::Failed;
    } catch (const std::bad_alloc&) {
        task.failure = EvidenceError{EvidenceErrorCode::OutOfMemory, {}, {}};
        task.progress = QueryProgress::Failed;
    }
    return task.progress;
}
void RecursiveSummaryTask::cancel() {
    if (impl_->offloaded) {
        auto& progress = impl_->offloaded->resume_progress;
        if (progress != QueryProgress::Completed &&
            progress != QueryProgress::Failed)
            progress = impl_->progress = QueryProgress::Cancelled;
        return;
    }
    if (impl_->progress != QueryProgress::Completed &&
        impl_->progress != QueryProgress::Failed)
        impl_->progress = QueryProgress::Cancelled;
}
void RecursiveSummaryTask::resume() {
    if (impl_->offloaded) {
        auto& progress = impl_->offloaded->resume_progress;
        if (progress == QueryProgress::Cancelled ||
            progress == QueryProgress::Paused)
            progress = QueryProgress::Running;
        impl_->progress = progress;
        return;
    }
    if (impl_->progress == QueryProgress::Cancelled ||
        impl_->progress == QueryProgress::Paused)
        impl_->progress = QueryProgress::Running;
}
QueryProgress RecursiveSummaryTask::progress() const { return impl_->progress; }
const RecursiveStats& RecursiveSummaryTask::stats() const {
    return impl_->stats;
}
const std::optional<EvidenceError>& RecursiveSummaryTask::failure() const {
    return impl_->failure;
}
RecursiveResult RecursiveSummaryTask::result() const {
    if (!ensure_resident())
        throw std::runtime_error("recursive state unavailable");
    const auto& task = *impl_;
    const auto& equation = task.equations.front();
    RecursiveResult result{
        .summary = task.selected,
        .channel = task.channel,
        .atoms = {equation.facts.begin(), equation.facts.end()},
        .issues = equation.issues,
        .fixed_point = task.progress == QueryProgress::Completed,
        .instances_merged = equation.merged};
    if (task.failure)
        result.issues |= bit(RecursiveIssue::UnavailableEvidence);
    for (const auto& atom : result.atoms)
        if (atom.kind == RecursiveAtomKind::Parameter &&
            atom.owner != task.selected.value)
            result.issues |= bit(RecursiveIssue::ForeignScope);
    if (result.fixed_point && !result.issues)
        result.precision = result.instances_merged
                               ? EvidencePrecision::Conservative
                               : EvidencePrecision::ExactForRecordedModel;
    return result;
}
std::span<const RecursiveLink> RecursiveSummaryTask::links() const {
    if (!ensure_resident())
        throw std::runtime_error("recursive state unavailable");
    return impl_->links;
}
std::span<const RecursiveComponent> RecursiveSummaryTask::components() const {
    if (!ensure_resident())
        throw std::runtime_error("recursive state unavailable");
    return impl_->components;
}
const EvidenceView& RecursiveSummaryTask::evidence() const {
    return *impl_->view;
}
} // namespace pagos::stage::provenance
