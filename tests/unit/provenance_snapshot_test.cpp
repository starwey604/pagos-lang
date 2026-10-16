#include "cache_test_input.h"
#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/provenance_topology.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
using namespace pagos;
using namespace stage;
using namespace stage::provenance;
std::pair<SnapshotArtifacts, std::shared_ptr<const ProvenanceAnalysis>>
snapshot(cache_test::Input& input) {
    StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    const auto identity = input.identity();
    if (!identity || !module)
        throw std::runtime_error("identity/module");
    const auto encoded = ProvenanceSnapshot::encode(
        *module->provenance, *identity, cache_test::context(), input.source);
    if (!encoded)
        throw std::runtime_error(
            "encode snapshot: " +
            std::to_string(static_cast<int>(encoded.error())));
    return {*encoded, module->provenance};
}
void equivalent(const ProvenanceAnalysis& left,
                const ProvenanceAnalysis& right) {
    ASSERT_EQ(left.roots().size(), right.roots().size());
    ASSERT_EQ(left.decisions().size(), right.decisions().size());
    ASSERT_EQ(left.scopes().size(), right.scopes().size());
    for (std::size_t i = 0; i < left.roots().size(); ++i) {
        const auto& a = left.roots()[i];
        const auto& b = right.roots()[i];
        EXPECT_EQ(a.name, b.name);
        EXPECT_EQ(a.node, b.node);
        EXPECT_EQ(a.stage, b.stage);
        EXPECT_EQ(a.scope, b.scope);
        EXPECT_EQ(a.requires_comptime, b.requires_comptime);
        EXPECT_EQ(a.span.begin, b.span.begin);
        EXPECT_EQ(a.span.end, b.span.end);
        for (const auto channel : {QueryKind::Value, QueryKind::Effects}) {
            for (const bool witness : {false, true}) {
                const auto oracle =
                    left.graph().query(a.node, channel, witness);
                auto task = right.graph().begin_query(b.node, channel, witness);
                while (task.progress() != QueryProgress::Completed)
                    (void)task.advance({1, 1, 1, 1});
                const auto result = task.result();
                EXPECT_EQ(result.sources, oracle.sources);
                EXPECT_EQ(result.witness, oracle.witness);
                EXPECT_EQ(result.invocations, oracle.invocations);
                EXPECT_EQ(result.complete, oracle.complete);
                EXPECT_EQ(result.recursive_call_cut, oracle.recursive_call_cut);
            }
        }
    }
    for (std::size_t i = 0; i < left.decisions().size(); ++i) {
        const auto& a = left.decisions()[i];
        const auto& b = right.decisions()[i];
        EXPECT_EQ(a.node, b.node);
        EXPECT_EQ(a.result, b.result);
        EXPECT_EQ(a.rule, b.rule);
        EXPECT_EQ(a.reason, b.reason);
        EXPECT_EQ(a.member, b.member);
        EXPECT_EQ(a.scope, b.scope);
        EXPECT_EQ(a.span.begin, b.span.begin);
        EXPECT_EQ(a.span.end, b.span.end);
    }
    for (std::size_t i = 0; i < left.scopes().size(); ++i) {
        EXPECT_EQ(left.scopes()[i].decisions, right.scopes()[i].decisions);
        EXPECT_EQ(left.scopes()[i].contracts, right.scopes()[i].contracts);
    }
    std::ostringstream before, after;
    explain_provenance(left, before);
    explain_provenance(right, after);
    EXPECT_EQ(before.str(), after.str());
}
TEST(ProvenanceSnapshot,
     RealP0GraphRoundTripsQueriesDecisionsAndAbortedContracts) {
    for (const std::string text :
         {"fn scale(k: u32, x: u32) -> u32 { k * x } let a = external_input(); "
          "let b = external_input(); let aa = scale(4, a); let bb = scale(4, "
          "b);",
          "runtime let r = 9; let a = [7, external_input()]; comptime let "
          "fixed = a[0]; let dynamic = a[1]; let flag = external_input() == 0; "
          "let choice = if flag { r } else { dynamic };",
          "fn read() -> u32 { external_input() } let a = read(); let b = "
          "read(); let both = a + b;",
          "fn require(x: u32) -> u32 { comptime let local = if x == 0 { 7 } "
          "else { 7 }; local } let r = require(external_input());",
          "fn recur(n: u32) -> u32 { if n == 0 { 1 } else { recur(n - 1) } } "
          "let r = recur(external_input());",
          "fn set(p: *mut u32) -> u32 { volatile *p = 42; 7 } comptime let r = "
          "set(4096usize as *mut u32);"}) {
        cache_test::Input input(text);
        auto [encoded, original] = snapshot(input);
        const auto opened = ProvenanceSnapshot::open(encoded.graph);
        ASSERT_TRUE(opened);
        EXPECT_EQ(opened->sections().size(), 8U);
        const auto identity = input.identity();
        ASSERT_TRUE(identity);
        const auto restored = opened->restore(*identity, cache_test::context());
        ASSERT_TRUE(restored);
        equivalent(*original, **restored);
        EXPECT_TRUE(ProvenanceSnapshot::validate_positions(encoded.positions,
                                                           encoded.semantic));
    }
}
TEST(ProvenanceSnapshot,
     WhitespaceChangesOnlyPositionsAndOwnedHandlesOutliveInputs) {
    cache_test::Input a("fn f(x: u32) -> u32 { if x == 0 { 1 } else { 2 } } "
                        "let r = f(external_input());");
    auto [encoded, original] = snapshot(a);
    cache_test::Input b("\n fn f( x: u32 ) -> u32 {\n if x == 0 { 1 } else { 2 "
                        "}\n }\n let r = f( external_input() );\n");
    auto [fresh, current] = snapshot(b);
    EXPECT_EQ(encoded.graph, fresh.graph);
    EXPECT_NE(encoded.positions, fresh.positions);
    EXPECT_EQ(encoded.semantic, fresh.semantic);
    auto opened = ProvenanceSnapshot::open(encoded.graph);
    ASSERT_TRUE(opened);
    const auto identity = b.identity();
    ASSERT_TRUE(identity);
    auto restored = opened->restore(*identity, cache_test::context());
    ASSERT_TRUE(restored);
    equivalent(*current, **restored);
    auto target = cache_test::context(64);
    EXPECT_FALSE(opened->restore(*identity, target));
    cache_test::Input changed("fn f(x: u32) -> u32 { if x == 0 { 1 } else { 3 "
                              "} } let r = f(external_input());");
    EXPECT_FALSE(opened->restore(*changed.identity(), cache_test::context()));
}
void u32(cache::Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}
void sign(cache::Bytes& bytes) {
    const auto hash =
        cache::fingerprint("pagos.provenance.snapshot.v1",
                           std::span(bytes).first(bytes.size() - 32));
    std::ranges::copy(hash.bytes, bytes.end() - 32);
}
TEST(ProvenanceSnapshot,
     RejectsTruncationDirectoriesVersionsIdsOwnersAndEdges) {
    cache_test::Input input(
        "fn f(x: u32) -> u32 { x + 1 } let r = f(external_input());");
    auto [encoded, original] = snapshot(input);
    (void)original;
    for (std::size_t length = 0; length < encoded.graph.size(); ++length)
        EXPECT_FALSE(ProvenanceSnapshot::open(cache::Bytes(
            encoded.graph.begin(),
            encoded.graph.begin() + static_cast<std::ptrdiff_t>(length))));
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto mutate = [&](auto change, bool open_failure) {
        auto bytes = encoded.graph;
        change(bytes);
        sign(bytes);
        auto opened = ProvenanceSnapshot::open(std::move(bytes));
        if (open_failure) {
            EXPECT_FALSE(opened);
        } else {
            ASSERT_TRUE(opened);
            EXPECT_FALSE(opened->restore(*identity, cache_test::context()));
        }
    };
    mutate([](auto& bytes) { u32(bytes, 9, 999); }, true);
    mutate([](auto& bytes) { u32(bytes, 85, 999); },
           true); // Unknown required section.
    mutate([](auto& bytes) { u32(bytes, 85 + 21, 1); },
           true); // Duplicate directory kind.
    mutate([](auto& bytes) { bytes[85 + 5] = 0; },
           true); // Overlapping section offset.
    const auto opened = ProvenanceSnapshot::open(encoded.graph);
    ASSERT_TRUE(opened);
    const auto find = [&](EvidenceSection section) {
        return static_cast<std::size_t>(
            std::ranges::find(opened->sections(), section,
                              &EvidenceSectionInfo::kind)
                ->offset);
    };
    mutate(
        [&](auto& bytes) {
            u32(bytes, find(EvidenceSection::Root) + 4 + 9, UINT32_MAX);
        },
        false);
    mutate(
        [&](auto& bytes) {
            u32(bytes, find(EvidenceSection::Node) + 4 + 1, UINT32_MAX);
        },
        false);
    mutate(
        [&](auto& bytes) {
            u32(bytes, find(EvidenceSection::Node) + 4 + 17, UINT32_MAX);
        },
        false);
    mutate(
        [&](auto& bytes) {
            u32(bytes, find(EvidenceSection::Edge) + 4, UINT32_MAX);
        },
        false);
    EXPECT_FALSE(ProvenanceSnapshot::open(encoded.graph, {.bytes = 16}));
    auto positions = encoded.positions;
    positions.push_back(0);
    EXPECT_FALSE(
        ProvenanceSnapshot::validate_positions(positions, encoded.semantic));
}
struct TemporaryStore {
    std::filesystem::path path;
    TemporaryStore() {
        static std::atomic<unsigned> serial{};
        path =
            std::filesystem::temp_directory_path() /
            ("pagos-snapshot-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(serial++));
    }
    ~TemporaryStore() { std::filesystem::remove_all(path); }
};
TEST(ProvenanceSnapshot,
     CasSeparatesSourceRevisionsAndReadOnlyLoadRepairsOptionalPositions) {
    TemporaryStore directory;
    cache_test::Input a(
        "fn f(x: u32) -> u32 { x + 1 } let r = f(external_input());");
    const auto [encoded, original] = snapshot(a);
    cache::FileStore store(directory.path);
    ASSERT_TRUE(publish_snapshot(store, encoded));
    cache_test::Input b(
        "\n fn f(x: u32) -> u32 {\n x + 1\n } let r = f(external_input());");
    const auto [shifted, current] = snapshot(b);
    EXPECT_EQ(encoded.graph, shifted.graph);
    ASSERT_TRUE(publish_snapshot(store, shifted));
    std::size_t objects = 0, manifests = 0;
    for (const auto& item :
         std::filesystem::recursive_directory_iterator(directory.path)) {
        if (item.is_regular_file() &&
            item.path().parent_path().parent_path().filename() == "objects")
            ++objects;
        if (item.path().extension() == ".manifest")
            ++manifests;
    }
    EXPECT_EQ(objects, 3U);
    EXPECT_EQ(manifests, 3U);
    unsigned writes = 0;
    cache::StoreOptions options;
    options.fault = [&](cache::IoOperation operation, const auto&) {
        if (operation != cache::IoOperation::ReadFile &&
            operation != cache::IoOperation::LockFile)
            ++writes;
        return std::error_code{};
    };
    cache::FileStore reader(directory.path, options);
    const auto identity = b.identity();
    ASSERT_TRUE(identity);
    const auto restored =
        load_snapshot(reader, *identity, cache_test::context());
    ASSERT_TRUE(restored);
    equivalent(*current, **restored);
    EXPECT_EQ(writes, 0U);
    const auto manifest =
        store.lookup(encoded.key, encoded.key.definition_fingerprint);
    ASSERT_TRUE(manifest);
    ASSERT_EQ(manifest->artifacts.size(),
              1U); // Graph index does not read position objects.
    const auto graph_ref = manifest->artifacts[0].object;
    // Optional position objects can disappear: current anchors still locate
    // sites.
    for (const auto& item : std::filesystem::recursive_directory_iterator(
             directory.path / "objects"))
        if (item.is_regular_file() &&
            item.path().filename() != graph_ref.digest.hex())
            std::filesystem::remove(item.path());
    EXPECT_TRUE(load_snapshot(reader, *identity, cache_test::context()));
    const auto hash = graph_ref.digest.hex();
    std::filesystem::remove(directory.path / "objects" / hash.substr(0, 2) /
                            hash);
    const auto missing =
        load_snapshot(reader, *identity, cache_test::context());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, cache::StoreErrorCode::MissingObject);
}
TEST(ProvenanceSnapshot, RestoredGraphOutlivesSourceAnalyzerAndEncodedHandle) {
    const auto published = [] {
        cache_test::Input input(
            "fn f(x: u32) -> u32 { x + 1 } let r = f(external_input());");
        const auto [encoded, original] = snapshot(input);
        (void)original;
        auto opened = ProvenanceSnapshot::open(encoded.graph);
        if (!opened)
            throw std::runtime_error("snapshot");
        return std::pair{std::move(*opened), *input.identity()};
    }();
    auto restored =
        published.first.restore(published.second, cache_test::context());
    ASSERT_TRUE(restored);
    auto task =
        (*restored)->graph().begin_query((*restored)->roots().back().node);
    restored = std::unexpected(cache::Error::InvalidValue);
    while (task.progress() != QueryProgress::Completed)
        (void)task.advance({1, 1, 1, 1});
    EXPECT_TRUE(task.result().complete);
    ASSERT_EQ(task.result().sources.size(), 1U);
}

} // namespace
