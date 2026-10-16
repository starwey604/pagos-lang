#include "../../lib/cache/wire.h"
#include "pagos/cache/task_store.h"

#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

namespace {
using namespace pagos::cache;
namespace fs = std::filesystem;
const auto task_id = fingerprint("test.task-store.1", {});
Bytes payload(unsigned value = 0) {
    wire::Writer writer({});
    wire::header(writer, ObjectKind::QueryTaskManifest);
    writer.u32(value);
    return *writer.finish();
}
class TaskStoreTest : public ::testing::Test {
  protected:
    TaskStoreTest() {
        static std::atomic<unsigned> sequence{};
        parent =
            fs::temp_directory_path() /
            ("pagos-task-store-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~TaskStoreTest() override {
        std::error_code ec;
        fs::remove_all(parent, ec);
    }
    TaskStore open(TaskAccess access = TaskAccess::ReadWrite,
                   TaskStoreOptions options = {}) {
        auto result =
            TaskStore::open(parent, task_id, access, std::move(options));
        if (!result)
            throw std::runtime_error("task store unavailable");
        return *result;
    }
    TaskHead publish(TaskStore& store, std::uint64_t generation = 0,
                     TaskStatus status = TaskStatus::Paused) {
        auto transaction = store.begin(generation);
        if (!transaction)
            throw std::runtime_error("writer unavailable");
        auto ref =
            transaction->write(ObjectKind::QueryTaskManifest,
                               payload(static_cast<unsigned>(generation)));
        if (!ref)
            throw std::runtime_error("leaf unavailable");
        auto head = transaction->commit(*ref, status);
        if (!head)
            throw std::runtime_error("head unavailable");
        return *head;
    }
    fs::path parent;
};
TEST_F(TaskStoreTest, MissingReadOnlyDoesNotCreateAnythingAndCannotWrite) {
    const auto missing = TaskStore::open(parent, task_id, TaskAccess::ReadOnly);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, StoreErrorCode::NotFound);
    EXPECT_FALSE(fs::exists(parent));
    auto writer = open();
    const auto head = publish(writer);
    auto reader = open(TaskAccess::ReadOnly);
    EXPECT_EQ(**reader.head(), head);
    EXPECT_EQ(*reader.read(head.manifest), payload());
    EXPECT_EQ(reader.begin(1).error().code, StoreErrorCode::ReadOnly);
    EXPECT_FALSE(fs::exists(writer.directory() / "queries"));
    EXPECT_EQ(head.generation, 1U);
}
TEST_F(TaskStoreTest,
       SerializesWritersRejectsStaleHeadsAndReadsOldGenerationDuringWrites) {
    TaskStoreOptions options;
    options.store.lock_timeout = std::chrono::milliseconds(0);
    auto store = open(TaskAccess::ReadWrite, options);
    const auto first = publish(store);
    auto other = open(TaskAccess::ReadWrite, options);
    {
        auto transaction = store.begin(1);
        ASSERT_TRUE(transaction);
        EXPECT_EQ(other.begin(1).error().code, StoreErrorCode::LockBusy);
        std::atomic<unsigned> successes{};
        std::vector<std::jthread> threads;
        for (unsigned i = 0; i < 4; ++i)
            threads.emplace_back([&] {
                auto head = other.head();
                if (head && *head == first)
                    ++successes;
            });
        threads.clear();
        EXPECT_EQ(successes, 4U);
        auto ref =
            transaction->write(ObjectKind::QueryTaskManifest, payload(1));
        ASSERT_TRUE(ref);
        ASSERT_TRUE(transaction->commit(*ref, TaskStatus::Paused));
    }
    EXPECT_EQ(other.begin(1).error().code, StoreErrorCode::StaleGeneration);
    EXPECT_EQ((*other.head())->generation, 2U);
    EXPECT_EQ(*other.read(first.manifest), payload());
}
TEST_F(TaskStoreTest,
       HeadFailureKeepsOldGenerationExceptAnAlreadyCompletedSwap) {
    for (const auto operation :
         {IoOperation::TemporaryCreated, IoOperation::WriteTemporary,
          IoOperation::CloseTemporary, IoOperation::PublishManifest,
          IoOperation::ManifestPublished}) {
        bool fail = false;
        TaskStoreOptions options;
        options.store.fault = [&](IoOperation actual, const fs::path& path) {
            return fail && actual == operation && path.filename() == "head"
                       ? std::make_error_code(std::errc::no_space_on_device)
                       : std::error_code{};
        };
        auto store = open(TaskAccess::ReadWrite, options);
        const auto old = store.head();
        ASSERT_TRUE(old);
        const auto previous = old->has_value() ? (**old).generation : 0;
        const auto first = publish(store, previous);
        fail = true;
        {
            auto transaction = store.begin(first.generation);
            ASSERT_TRUE(transaction);
            auto ref = transaction->write(
                ObjectKind::QueryTaskManifest,
                payload(static_cast<unsigned>(first.generation)));
            ASSERT_TRUE(ref);
            EXPECT_FALSE(transaction->commit(*ref, TaskStatus::Completed));
        }
        const auto actual = store.head();
        ASSERT_TRUE(actual);
        ASSERT_TRUE(*actual);
        EXPECT_EQ((**actual).generation,
                  first.generation +
                      (operation == IoOperation::ManifestPublished ? 1U : 0U));
        EXPECT_TRUE(store.read((**actual).manifest));
        for (const auto& entry :
             fs::directory_iterator(store.directory() / "tmp")) {
            (void)entry;
            FAIL() << "normal IO failure leaked a temporary";
        }
    }
}
TEST_F(TaskStoreTest,
       QuotaIncludesOwnedFilesAndCrashDebrisWithoutOverwritingHead) {
    TaskStoreOptions options;
    options.disk_bytes = 2048;
    auto store = open(TaskAccess::ReadWrite, options);
    const auto first = publish(store);
    {
        auto transaction = store.begin(1);
        ASSERT_TRUE(transaction);
        auto bytes = payload(2);
        bytes.resize(4096);
        EXPECT_EQ(
            transaction->write(ObjectKind::QueryTaskManifest, std::move(bytes))
                .error()
                .code,
            StoreErrorCode::LimitExceeded);
    }
    EXPECT_EQ(**store.head(), first);
    std::ofstream debris(store.directory() / "tmp" / "killed-write",
                         std::ios::binary);
    debris << std::string(2048, 'x');
    debris.close();
    EXPECT_EQ(store.begin(1).error().code, StoreErrorCode::LimitExceeded);
    EXPECT_EQ(**store.head(), first);
}
TEST_F(TaskStoreTest,
       ExplicitCleanupProtectsHandlesPausedWorkAndUnknownDirectories) {
    EXPECT_EQ(reclaim_tasks(parent)->reclaimed, 0U);
    EXPECT_FALSE(fs::exists(parent));
    fs::path task_path;
    {
        auto store = open();
        task_path = store.directory();
        (void)publish(store);
        {
            auto reader = open(TaskAccess::ReadOnly);
            EXPECT_EQ(
                reclaim_tasks(parent, TaskCleanupPolicy::AllInactive)->active,
                1U);
        }
        // Closing another same-process handle must not release our OS lease.
        EXPECT_EQ(reclaim_tasks(parent, TaskCleanupPolicy::AllInactive)->active,
                  1U);
    }
    EXPECT_EQ(reclaim_tasks(parent)->skipped, 1U);
    EXPECT_TRUE(fs::exists(task_path));
    {
        auto store = open();
        (void)publish(store, 1, TaskStatus::Completed);
        EXPECT_EQ(reclaim_tasks(parent)->active, 1U);
    }
    const auto unknown = parent / ("pagos-query-" + std::string(64, 'a'));
    fs::create_directory(unknown);
    std::ofstream(unknown / "session.lock") << "future marker\n";
    const auto link = parent / ("pagos-query-" + std::string(64, 'b'));
    fs::create_directory_symlink(unknown, link);
    const auto cleaned = reclaim_tasks(parent);
    ASSERT_TRUE(cleaned);
    EXPECT_EQ(cleaned->reclaimed, 1U);
    EXPECT_EQ(cleaned->skipped, 2U);
    EXPECT_TRUE(fs::exists(unknown));
    EXPECT_TRUE(fs::is_symlink(link));
}
TEST_F(TaskStoreTest,
       DefaultCleanupKeepsUnpublishedWorkAndAllInactiveCanDiscardIt) {
    {
        auto store = open();
        auto transaction = store.begin(0);
        ASSERT_TRUE(transaction);
        ASSERT_TRUE(
            transaction->write(ObjectKind::QueryTaskManifest, payload()));
    }
    EXPECT_EQ(reclaim_tasks(parent)->skipped, 1U);
    EXPECT_EQ(reclaim_tasks(parent, TaskCleanupPolicy::AllInactive)->reclaimed,
              1U);
}
TEST_F(TaskStoreTest,
       CompletedTraversalWithUndeliveredPagesIsNotCleanupEligible) {
    {
        auto store = open();
        (void)publish(store, 0, TaskStatus::AwaitingDelivery);
    }
    EXPECT_EQ(reclaim_tasks(parent)->skipped, 1U);
    EXPECT_EQ(reclaim_tasks(parent, TaskCleanupPolicy::AllInactive)->reclaimed,
              1U);
}
TEST_F(TaskStoreTest,
       ConcurrentLastReaderCloseAndReopenPreserveTheLeaseRegistry) {
    TaskHead first;
    {
        auto store = open();
        first = publish(store);
    }
    std::atomic<unsigned> successes{};
    std::vector<std::jthread> readers;
    for (unsigned thread = 0; thread < 4; ++thread)
        readers.emplace_back([&] {
            for (unsigned iteration = 0; iteration < 64; ++iteration) {
                auto reader =
                    TaskStore::open(parent, task_id, TaskAccess::ReadOnly);
                if (!reader)
                    continue;
                auto head = reader->head();
                if (head && *head == first && reader->read(first.manifest))
                    ++successes;
            }
        });
    readers.clear();
    EXPECT_EQ(successes, 256U);
    EXPECT_EQ(reclaim_tasks(parent)->skipped, 1U);
    EXPECT_EQ(reclaim_tasks(parent, TaskCleanupPolicy::AllInactive)->reclaimed,
              1U);
}
} // namespace
