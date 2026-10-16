#include "pagos/stage/evidence_builder.h"
#include "pagos/stage/provenance_query.h"
#include "pagos/stage/provenance_topology.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
using namespace pagos;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
class EvidenceBuilderTest : public ::testing::Test {
  protected:
    EvidenceBuilderTest() {
        static std::atomic<unsigned> sequence{};
        directory =
            fs::temp_directory_path() /
            ("pagos-builder-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~EvidenceBuilderTest() override {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
    EvidenceBuildConfig config() const {
        EvidenceBuildConfig value;
        value.mode = EvidenceBuildMode::Spill;
        value.directory = directory;
        value.block_bytes = 128;
        value.resident_bytes = 4096;
        value.cache_bytes = 256;
        return value;
    }
    fs::path object(const fs::path& task) const {
        for (const auto& entry : fs::recursive_directory_iterator(task))
            if (entry.is_regular_file() &&
                entry.path().filename() != "session.lock")
                return entry.path();
        throw std::runtime_error("missing spill object");
    }
    fs::path directory;
};
NodeId add(EvidenceBuilder& builder, Rule rule, SiteId site,
           std::span<const Edge> edges = {}, std::uint32_t payload = 0,
           SummaryId owner = {}) {
    auto result = builder.add(rule, site, edges, payload, owner);
    if (!result)
        throw std::runtime_error("evidence build failed");
    return *result;
}
QueryResult solve(std::shared_ptr<const EvidenceView> view, NodeId root,
                  QueryKind kind = QueryKind::Value, bool witness = false) {
    QueryTask task(std::move(view), root, kind, witness);
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed)
        (void)task.advance({1, 1, 1, 1});
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    return task.result();
}
void equivalent(const QueryResult& left, const QueryResult& right) {
    EXPECT_EQ(left.sources, right.sources);
    EXPECT_EQ(left.witness, right.witness);
    EXPECT_EQ(left.invocations, right.invocations);
    EXPECT_EQ(left.visited_states, right.visited_states);
    EXPECT_EQ(left.visited_edges, right.visited_edges);
    EXPECT_EQ(left.complete, right.complete);
    EXPECT_EQ(left.recursive_call_cut, right.recursive_call_cut);
}
TEST_F(EvidenceBuilderTest, ExplicitCleanupSkipsLiveViewsAndUnknownNamespaces) {
    auto missing = reclaim_evidence_sessions(directory);
    ASSERT_TRUE(missing);
    EXPECT_FALSE(fs::exists(directory));
    EvidenceBuilder builder(config());
    const auto root = add(builder, Rule::Origin, 1);
    ASSERT_TRUE(builder.seal());
    const auto session = builder.temporary_directory();
    auto result = reclaim_evidence_sessions(directory);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->active, 1U);
    EXPECT_EQ(result->reclaimed, 0U);
    auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    EXPECT_EQ(reclaim_evidence_sessions(directory)->active, 1U);
    // A symlink in an ancestor must not bypass the in-process registry on
    // systems where opening another FD can release process-scoped locks.
    fs::create_directory_symlink(directory.parent_path(), directory / "alias");
    EXPECT_EQ(
        reclaim_evidence_sessions(directory / "alias" / directory.filename())
            ->active,
        1U);
    const auto unknown = directory / "pagos-evidence-future";
    fs::create_directories(unknown);
    std::ofstream(unknown / "session.lock") << std::string(26, '?');
    EXPECT_EQ(reclaim_evidence_sessions(directory)->skipped, 1U);
    const auto initializing = directory / "pagos-evidence-initializing";
    fs::create_directories(initializing);
    std::ofstream(initializing / "session.lock");
    EXPECT_EQ(reclaim_evidence_sessions(directory)->skipped, 2U);
    EXPECT_EQ(solve(frozen->view, root).sources.size(), 1U);
    frozen->view.reset();
    EXPECT_FALSE(fs::exists(session));
    EXPECT_TRUE(fs::exists(unknown));
}

TEST_F(EvidenceBuilderTest, MemoryModeHasNoFilesystemSideEffects) {
    EvidenceBuilder defaults;
    const auto default_root = add(defaults, Rule::Origin, 11);
    const auto default_view = std::move(defaults).freeze();
    ASSERT_TRUE(default_view);
    EXPECT_EQ(solve(default_view->view, default_root).sources[0].site, 11U);
    EXPECT_EQ(default_view->construction.written_blocks, 0U);
    auto options = config();
    options.mode = EvidenceBuildMode::Memory;
    unsigned calls = 0;
    options.store.fault = [&](auto, const auto&) {
        ++calls;
        return std::make_error_code(std::errc::io_error);
    };
    EvidenceBuilder builder(options);
    auto root = add(builder, Rule::Origin, 1);
    for (unsigned i = 0; i < 30; ++i)
        root = add(builder, Rule::Strict, 2, std::array{Edge{root}});
    const auto result = std::move(builder).freeze();
    ASSERT_TRUE(result);
    EXPECT_GT(result->construction.sealed_blocks, 1U);
    EXPECT_EQ(result->construction.written_blocks, 0U);
    EXPECT_GT(result->construction.resident_bytes, 0U);
    EXPECT_EQ(solve(result->view, root).sources[0].site, 1U);
    EXPECT_EQ(result->view->read_stats().objects_read, 0U);
    EXPECT_EQ(calls, 0U);
    EXPECT_FALSE(fs::exists(directory));
    EXPECT_THROW((void)builder.stats(), std::logic_error);
}
TEST_F(EvidenceBuilderTest,
       AppendingReleasesSealedCapacityAndReReadsOldBlocks) {
    auto options = config();
    options.block_bytes = 4096;
    options.resident_bytes = 8192;
    options.cache_bytes = 4096;
    EvidenceBuilder builder(options);
    const auto first = add(builder, Rule::Origin, 41);
    auto root = first;
    for (unsigned i = 0; i < 20000; ++i) {
        root = add(builder, Rule::Use, 42, std::array{Edge{root}});
        if (i % 100 == 0) {
            EXPECT_LE(builder.stats().active_bytes, options.resident_bytes);
        }
    }
    EXPECT_GT(builder.stats().written_blocks, 100U);
    EXPECT_EQ(builder.stats().resident_bytes, 0U);
    EXPECT_GT(builder.stats().released_active_bytes,
              options.resident_bytes * 10);
    EXPECT_LE(builder.stats().peak_owned_bytes, options.resident_bytes);
    auto lease = builder.pin(first);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->node().site, 41U);
    EXPECT_EQ(builder.stats().objects_read, 1U);
    EXPECT_GT(builder.stats().live_bytes, builder.stats().active_bytes);
    builder.evict();
    const auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    EXPECT_EQ(frozen->construction.active_bytes, 0U);
    EXPECT_GT(frozen->construction.directory_bytes, 0U);
    const auto actual = solve(frozen->view, root);
    ASSERT_EQ(actual.sources.size(), 1U);
    EXPECT_EQ(actual.sources[0].node, first);
    EXPECT_EQ(actual.visited_states, 20001U);
    EXPECT_EQ(frozen->view->read_stats().objects_read,
              frozen->construction.written_blocks);
    EXPECT_EQ(lease->node().site, 41U);
}
// Exercise the same finite graph in the original builder and the segmented
// builder. Summaries can reference one another before either is published.
template <class Add, class Reserve, class Define>
NodeId recursive_graph(Add node, Reserve reserve, Define define) {
    const auto left = reserve(), right = reserve();
    const auto p = node(Rule::Parameter, 1, {}, 0, left);
    const auto q = node(Rule::Parameter, 2, {}, 0, right);
    const auto l = node(Rule::Call, 3, std::array{Edge{p}}, right.value, {});
    const auto r = node(Rule::Call, 4, std::array{Edge{q}}, left.value, {});
    const auto effect = node(Rule::Effect, 5, {}, 0, {});
    const auto sequence =
        node(Rule::Sequence, 6,
             std::array{Edge{l}, Edge{effect, Relation::Effect}}, 0, {});
    define(left, sequence);
    define(right, r);
    const auto actual = node(Rule::Origin, 7, {}, 0, {});
    const auto call =
        node(Rule::Call, 8, std::array{Edge{actual}}, left.value, {});
    return node(Rule::Strict, 9, std::array{Edge{call}, Edge{actual}}, 0, {});
}
TEST_F(EvidenceBuilderTest,
       FrozenViewsMatchRecursiveOriginsEffectsAndWitnesses) {
    auto options = config();
    options.block_bytes = 24; // Each nontrivial node crosses a block boundary.
    EvidenceBuilder builder(options);
    const auto entry = recursive_graph(
        [&](Rule rule, SiteId site, std::span<const Edge> edges,
            std::uint32_t payload, SummaryId owner) {
            auto id = add(builder, rule, site, edges, payload, owner);
            EXPECT_TRUE(builder.seal());
            return id;
        },
        [&] { return *builder.reserve_summary(); },
        [&](auto summary, auto root) {
            EXPECT_TRUE(builder.define_summary(summary, root));
        });
    const auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    Builder memory;
    const auto original = recursive_graph(
        [&](Rule rule, SiteId site, std::span<const Edge> edges,
            std::uint32_t payload, SummaryId owner) {
            return memory.add(rule, site, edges, payload, owner);
        },
        [&] { return memory.reserve_summary(); },
        [&](auto summary, auto root) { memory.define_summary(summary, root); });
    EXPECT_EQ(entry, original);
    const auto graph = std::move(memory).freeze();
    for (auto kind : {QueryKind::Value, QueryKind::Effects})
        for (bool witness : {false, true})
            equivalent(solve(frozen->view, entry, kind, witness),
                       graph.query(original, kind, witness));
}
TEST_F(EvidenceBuilderTest,
       ProjectionBoundariesContractsAndEffectsMatchOriginalStorage) {
    EvidenceBuilder builder(config());
    Builder original;
    const auto both = [&](Rule rule, SiteId site,
                          std::span<const Edge> edges = {}) {
        const auto id = add(builder, rule, site, edges);
        EXPECT_EQ(id, original.add(rule, site, edges));
        EXPECT_TRUE(builder.seal());
        return id;
    };
    const auto literal = both(Rule::Literal, 1);
    const auto dynamic = both(Rule::Origin, 2);
    const auto effect = both(Rule::Effect, 3);
    const auto aggregate = both(Rule::Aggregate, 4,
                                std::array{Edge{literal}, Edge{dynamic},
                                           Edge{effect, Relation::Effect}});
    const auto field =
        both(Rule::Projection, 5,
             std::array{Edge{literal}, Edge{aggregate, Relation::Initializer}});
    const auto boundary =
        both(Rule::RuntimeBoundary, 6,
             std::array{Edge{dynamic, Relation::Initializer}});
    const auto contract = both(Rule::RequireComptime, 7,
                               std::array{Edge{field, Relation::Contract}});
    const auto root = both(Rule::Strict, 8,
                           std::array{Edge{contract}, Edge{boundary},
                                      Edge{dynamic, Relation::Control}});
    const auto graph = std::move(original).freeze();
    const auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    for (auto channel : {QueryKind::Value, QueryKind::Effects})
        for (bool witness : {false, true})
            equivalent(solve(frozen->view, root, channel, witness),
                       graph.query(root, channel, witness));
}
TEST_F(EvidenceBuilderTest,
       ActiveSnapshotsAndFrozenLeasesSurviveGrowthAndCleanup) {
    auto options = config();
    options.cache_bytes = 0;
    fs::path task;
    std::optional<NodeLease> held;
    {
        EvidenceBuilder builder(options);
        const auto origin = add(builder, Rule::Origin, 1);
        const auto root = add(builder, Rule::Use, 2, std::array{Edge{origin}});
        auto active = builder.pin(root);
        ASSERT_TRUE(active);
        for (unsigned i = 0; i < 1000; ++i)
            (void)add(builder, Rule::Literal, i + 3);
        ASSERT_EQ(active->edges().size(), 1U);
        EXPECT_EQ(active->edges()[0].target, origin);
        EXPECT_EQ(active->node().site, 2U);
        auto loaded = builder.pin(root);
        ASSERT_TRUE(loaded);
        held = std::move(*loaded);
        task = builder.temporary_directory();
        ASSERT_FALSE(task.empty());
        auto frozen = std::move(builder).freeze();
        ASSERT_TRUE(frozen);
        auto copy = frozen->view;
        frozen->view.reset();
        EXPECT_TRUE(fs::exists(task));
        copy->evict();
        EXPECT_EQ(solve(copy, root).sources[0].node, origin);
    }
    EXPECT_FALSE(fs::exists(task));
    ASSERT_TRUE(held);
    EXPECT_EQ(held->node().site, 2U);
    EXPECT_EQ(held->edges()[0].target.value, 1U);
}
TEST_F(EvidenceBuilderTest,
       WriteFailureFallsBackWithinQuotaAndMixedViewCountsDiskOnly) {
    auto options = config();
    bool reject = true;
    options.store.fault = [&](cache::IoOperation operation, const auto&) {
        return reject && operation == cache::IoOperation::WriteTemporary
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    EvidenceBuilder builder(options);
    const auto origin = add(builder, Rule::Origin, 1);
    ASSERT_TRUE(builder.seal());
    ASSERT_TRUE(builder.last_spill_error());
    EXPECT_EQ(builder.last_spill_error()->code, EvidenceErrorCode::IoFailure);
    EXPECT_FALSE(builder.failure());
    reject = false;
    const auto root = add(builder, Rule::Strict, 2, std::array{Edge{origin}});
    const auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    EXPECT_EQ(frozen->construction.fallback_blocks, 1U);
    EXPECT_EQ(frozen->construction.written_blocks, 1U);
    EXPECT_EQ(solve(frozen->view, root).sources[0].node, origin);
    EXPECT_EQ(frozen->view->read_stats().objects_read, 1U);
    EXPECT_EQ(frozen->view->read_stats().payload_bytes_read,
              frozen->construction.payload_bytes_written);
}
TEST_F(EvidenceBuilderTest,
       QuotasStopEvidenceWithoutPublishingAndPreserveIssuedLeases) {
    auto options = config();
    options.disk_bytes = 0;
    options.block_bytes = sizeof(Node);
    options.resident_bytes = sizeof(Node) * 2;
    EvidenceBuilder builder(options);
    const auto first = add(builder, Rule::Origin, 1);
    auto held = builder.pin(first);
    ASSERT_TRUE(held);
    (void)add(builder, Rule::Literal, 2);
    auto rejected = builder.add(Rule::Literal, 3);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, EvidenceErrorCode::LimitExceeded);
    EXPECT_TRUE(builder.failure());
    EXPECT_EQ(builder.stats().active_bytes, 0U);
    EXPECT_EQ(builder.stats().resident_bytes, 0U);
    EXPECT_EQ(builder.stats().cached_bytes, 0U);
    EXPECT_GT(builder.stats().live_bytes, 0U); // The issued snapshot.
    EXPECT_EQ(held->node().site, 1U);
    EXPECT_FALSE(std::move(builder).freeze());
    EXPECT_FALSE(fs::exists(directory));
    EXPECT_FALSE(builder.reserve_summary());
    EXPECT_FALSE(builder.define_summary({1}, first));
}
TEST_F(EvidenceBuilderTest,
       UnavailablePolicyReportsStorageFailureAndCleansTemporaryFiles) {
    for (auto operation : {cache::IoOperation::WriteTemporary,
                           cache::IoOperation::ObjectPublished}) {
        fs::path task;
        {
            auto options = config();
            options.failure_policy = SpillFailurePolicy::Unavailable;
            options.store.fault = [=](auto current, const auto&) {
                return current == operation
                           ? std::make_error_code(std::errc::no_space_on_device)
                           : std::error_code{};
            };
            EvidenceBuilder builder(options);
            (void)add(builder, Rule::Origin, 1);
            const auto failed = builder.seal();
            ASSERT_FALSE(failed);
            EXPECT_EQ(failed.error().code, EvidenceErrorCode::IoFailure);
            EXPECT_FALSE(std::move(builder).freeze());
            task = builder.temporary_directory();
            ASSERT_FALSE(task.empty());
            EXPECT_FALSE(fs::exists(task / "queries"));
        }
        EXPECT_FALSE(fs::exists(task));
    }
}
TEST_F(EvidenceBuilderTest,
       PostPublicationFaultChargesOrphanLeavesAgainstDiskQuota) {
    auto options = config();
    options.disk_bytes = 200;
    options.store.fault = [](auto operation, const auto&) {
        return operation == cache::IoOperation::ObjectPublished
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    EvidenceBuilder builder(options);
    for (unsigned i = 0; i < 8; ++i) {
        (void)add(builder, Rule::Literal, i + 1);
        ASSERT_TRUE(builder.seal());
    }
    const auto counts = builder.stats();
    EXPECT_LE(counts.charged_disk_bytes, options.disk_bytes);
    EXPECT_EQ(counts.written_blocks, 0U);
    EXPECT_EQ(counts.fallback_blocks, 8U);
    std::size_t disk = 0;
    for (const auto& entry :
         fs::recursive_directory_iterator(builder.temporary_directory()))
        if (entry.is_regular_file() &&
            entry.path().filename() != "session.lock")
            disk += entry.file_size();
    EXPECT_GT(disk, 0U);
    EXPECT_EQ(disk, counts.charged_disk_bytes);
    EXPECT_TRUE(std::move(builder).freeze());
}
TEST_F(EvidenceBuilderTest,
       MissingAndCorruptOldBlocksRejectFreezeWithTypedFailure) {
    for (unsigned damage : {0U, 1U, 2U}) {
        EvidenceBuilder builder(config());
        const auto root = add(builder, Rule::Origin, 1);
        ASSERT_TRUE(builder.seal());
        const auto path = object(builder.temporary_directory());
        if (damage == 1) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "damaged";
        } else if (damage == 2) {
            fs::remove_all(builder.temporary_directory());
        } else {
            fs::remove(path);
        }
        const auto result = builder.pin(root);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, damage == 1
                                           ? EvidenceErrorCode::CorruptBlock
                                           : EvidenceErrorCode::MissingBlock);
        EXPECT_FALSE(std::move(builder).freeze());
    }
}
TEST_F(EvidenceBuilderTest,
       CacheTrimsOwnedBuffersWhilePinnedBlocksRemainReadable) {
    auto options = config();
    options.block_bytes = 4096;
    options.resident_bytes = 4 * sizeof(Node);
    options.cache_bytes = 4096;
    EvidenceBuilder builder(options);
    const auto first = add(builder, Rule::Origin, 1);
    ASSERT_TRUE(builder.seal());
    const auto second = add(builder, Rule::Use, 2, std::array{Edge{first}});
    ASSERT_TRUE(builder.seal());
    auto one = builder.pin(first), two = builder.pin(second);
    ASSERT_TRUE(one);
    ASSERT_TRUE(two);
    EXPECT_EQ(builder.stats().objects_read, 2U);
    EXPECT_EQ(builder.stats().cached_bytes, 2 * sizeof(Node) + sizeof(Edge));
    (void)add(builder, Rule::Strict, 3, std::array{Edge{first}, Edge{second}});
    (void)add(builder, Rule::Strict, 4, std::array{Edge{first}, Edge{second}});
    EXPECT_EQ(builder.stats().cached_bytes, 0U);
    EXPECT_LE(builder.stats().peak_owned_bytes, options.resident_bytes);
    EXPECT_GT(builder.stats().live_bytes, options.resident_bytes);
    EXPECT_TRUE(
        builder.pin(first)); // Reuses an evicted but still pinned block.
    EXPECT_EQ(builder.stats().objects_read, 2U);
    EXPECT_EQ(two->edges()[0].target, first);
}
TEST_F(EvidenceBuilderTest,
       ReadIoFailuresAreAuxiliaryErrorsWithoutRepairWrites) {
    for (bool frozen_failure : {false, true}) {
        auto options = config();
        bool reject_reads = false;
        unsigned writes = 0;
        options.store.fault = [&](auto operation, const auto&) {
            writes += operation == cache::IoOperation::WriteTemporary ? 1U : 0U;
            return reject_reads && operation == cache::IoOperation::ReadFile
                       ? std::make_error_code(std::errc::permission_denied)
                       : std::error_code{};
        };
        EvidenceBuilder builder(options);
        const auto root = add(builder, Rule::Origin, 1);
        ASSERT_TRUE(builder.seal());
        const auto before = writes;
        reject_reads = true;
        if (frozen_failure) {
            const auto frozen = std::move(builder).freeze();
            ASSERT_TRUE(frozen);
            QueryTask task(frozen->view, root);
            EXPECT_EQ(task.advance().progress, QueryProgress::Failed);
            ASSERT_TRUE(task.failure());
            EXPECT_EQ(task.failure()->code, EvidenceErrorCode::IoFailure);
            EXPECT_FALSE(task.result().complete);
        } else {
            const auto result = builder.pin(root);
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, EvidenceErrorCode::IoFailure);
            EXPECT_FALSE(std::move(builder).freeze());
        }
        EXPECT_EQ(writes, before);
    }
}
TEST_F(EvidenceBuilderTest,
       IsolatedTaskNamespacesNeverRemoveTheirParentOrSiblings) {
    fs::create_directories(directory);
    std::ofstream(directory / "user-data") << "keep";
    fs::path one, two;
    {
        EvidenceBuilder first(config());
        (void)add(first, Rule::Origin, 1);
        ASSERT_TRUE(first.seal());
        one = first.temporary_directory();
        {
            EvidenceBuilder second(config());
            (void)add(second, Rule::Origin, 2);
            ASSERT_TRUE(second.seal());
            two = second.temporary_directory();
            EXPECT_NE(one, two);
        }
        EXPECT_TRUE(fs::exists(one));
        EXPECT_FALSE(fs::exists(two));
    }
    EXPECT_FALSE(fs::exists(one));
    EXPECT_TRUE(fs::exists(directory / "user-data"));
}
TEST_F(EvidenceBuilderTest,
       FrozenTopologySupportsCancellationAndConcurrentQueries) {
    auto options = config();
    options.cache_bytes = 0;
    EvidenceBuilder builder(options);
    auto root = add(builder, Rule::Origin, 1);
    for (unsigned i = 0; i < 32; ++i)
        root = add(builder, Rule::Use, i + 2, std::array{Edge{root}});
    auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    EvidenceTopologyTask task(frozen->view, root);
    EXPECT_EQ(task.advance({0, 0, 0, 0}).progress, QueryProgress::Paused);
    EXPECT_EQ(frozen->view->read_stats().objects_read, 0U);
    task.cancel();
    EXPECT_EQ(task.advance().progress, QueryProgress::Cancelled);
    task.resume();
    std::size_t records = 0;
    while (task.progress() != QueryProgress::Completed) {
        auto page = task.advance({1, 1, 1, 1});
        ASSERT_NE(page.progress, QueryProgress::Failed);
        records += page.records.size();
    }
    EXPECT_EQ(records, 65U);
    std::array<bool, 4> complete{};
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < complete.size(); ++i)
        workers.emplace_back([&, i] {
            QueryTask query(frozen->view, root);
            while (query.progress() != QueryProgress::Completed &&
                   query.progress() != QueryProgress::Failed) {
                (void)query.advance({1, 1, 1, 1});
                frozen->view->evict();
            }
            complete[i] = query.progress() == QueryProgress::Completed;
        });
    for (auto& worker : workers)
        worker.join();
    for (auto success : complete)
        EXPECT_TRUE(success);
}
TEST_F(EvidenceBuilderTest,
       InvalidConstructionAndUnboundSummariesAreNotStorageFallbacks) {
    auto options = config();
    options.directory.clear();
    EXPECT_THROW(EvidenceBuilder{options}, std::invalid_argument);
    options = config();
    options.block_bytes = 0;
    EXPECT_THROW(EvidenceBuilder{options}, std::invalid_argument);
    EvidenceBuilder builder(config());
    const auto summary = builder.reserve_summary();
    ASSERT_TRUE(summary);
    const auto root = add(builder, Rule::Parameter, 1, {}, 0, *summary);
    EXPECT_THROW((void)builder.add(Rule::Use, 2, std::array{Edge{{99}}}),
                 std::logic_error);
    EXPECT_THROW((void)std::move(builder).freeze(), std::logic_error);
    EXPECT_FALSE(builder.failure());
    EXPECT_TRUE(builder.define_summary(*summary, root));
    EXPECT_THROW((void)builder.define_summary(*summary, root),
                 std::logic_error);
    EXPECT_TRUE(std::move(builder).freeze());
}
TEST_F(EvidenceBuilderTest, CodecLimitsSealEarlyAndRejectOversizedSingleNodes) {
    auto options = config();
    options.block_bytes = 1024;
    options.store.codec.bytes = 100;
    EvidenceBuilder builder(options);
    auto root = add(builder, Rule::Origin, 1);
    for (unsigned i = 0; i < 20; ++i)
        root = add(builder, Rule::Use, 2, std::array{Edge{root}});
    EXPECT_GT(builder.stats().written_blocks, 5U);
    const auto frozen = std::move(builder).freeze();
    ASSERT_TRUE(frozen);
    EXPECT_EQ(solve(frozen->view, root).sources[0].site, 1U);
    EvidenceBuilder other(options);
    const auto origin = add(other, Rule::Origin, 1);
    const std::vector<Edge> too_many(100, Edge{origin});
    auto rejected = other.add(Rule::Strict, 2, too_many);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, EvidenceErrorCode::LimitExceeded);
    EXPECT_FALSE(std::move(other).freeze());
}
} // namespace
