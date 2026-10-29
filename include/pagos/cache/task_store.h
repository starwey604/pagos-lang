#pragma once

#include "pagos/cache/store.h"

namespace pagos::cache {
inline constexpr std::uint32_t task_store_version = 1;
enum class TaskAccess { ReadOnly, ReadWrite };
enum class TaskStatus : std::uint8_t {
    Paused,
    AwaitingDelivery,
    Cancelled,
    Completed
};
struct TaskHead {
    std::uint64_t generation{};
    ObjectRef manifest;
    TaskStatus status{TaskStatus::Paused};
    bool operator==(const TaskHead&) const = default;
};
struct TaskStoreOptions {
    std::size_t disk_bytes{256UZ * 1024 * 1024};
    StoreOptions store;
};
struct TaskCleanupStats {
    std::size_t examined{}, reclaimed{}, active{}, skipped{}, errors{};
};
enum class TaskCleanupPolicy { TerminalOnly, AllInactive };
class TaskWriter;
// Explicit durable task namespace, independent of the semantic query index.
// Handles retain a shared lifetime lock, including read-only handles; only
// explicit maintenance removes an inactive namespace. Closing never deletes it.
class TaskStore {
  public:
    [[nodiscard]] static std::expected<TaskStore, StoreError>
    open(const std::filesystem::path& parent, Digest task, TaskAccess access,
         TaskStoreOptions options = {});
    [[nodiscard]] std::expected<std::optional<TaskHead>, StoreError>
    head() const;
    [[nodiscard]] std::expected<Bytes, StoreError>
    read(const ObjectRef& ref) const;
    // One writer, optimistic expected generation (zero means no head).
    // It holds the writer lock through all leaf writes and the final head swap.
    [[nodiscard]] std::expected<TaskWriter, StoreError>
    begin(std::uint64_t expected_generation) const;
    [[nodiscard]] const std::filesystem::path& directory() const;
    [[nodiscard]] Digest task() const;

  private:
    friend class TaskWriter;
    struct Impl;
    explicit TaskStore(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};
class TaskWriter {
  public:
    ~TaskWriter();
    TaskWriter(TaskWriter&&) noexcept;
    TaskWriter& operator=(TaskWriter&&) noexcept;
    TaskWriter(const TaskWriter&) = delete;
    TaskWriter& operator=(const TaskWriter&) = delete;
    [[nodiscard]] std::expected<ObjectRef, StoreError> write(ObjectKind kind,
                                                             Bytes bytes);
    [[nodiscard]] std::expected<TaskHead, StoreError> commit(ObjectRef manifest,
                                                             TaskStatus status);
    [[nodiscard]] std::size_t charged_bytes() const;

  private:
    friend class TaskStore;
    struct Impl;
    explicit TaskWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
// Default: recognized inactive tasks with a Cancelled/Completed head only.
// AllInactive explicitly also discards resumable/never-published abandoned
// work. Active handles, symlinks and unknown/incomplete markers are always
// preserved.
[[nodiscard]] std::expected<TaskCleanupStats, StoreError>
reclaim_tasks(const std::filesystem::path& parent,
              TaskCleanupPolicy policy = TaskCleanupPolicy::TerminalOnly);
} // namespace pagos::cache
