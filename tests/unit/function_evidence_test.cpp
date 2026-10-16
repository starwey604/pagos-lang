#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/stage/function_evidence.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_topology.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <tuple>

namespace {
using namespace pagos;
using namespace pagos::stage;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
const std::string program =
    "fn scale(k: u32, x: u32) -> u32 { k * x } "
    "fn outer(x: u32) -> u32 { scale(4, x) } "
    "let a = external_input(); let b = external_input(); "
    "let aa = outer(a); let bb = outer(b); let cc = scale(5, a);";
struct Prepared {
    CacheIdentity identity;
    std::shared_ptr<const ProvenanceAnalysis> analysis;
};
Prepared prepare(cache_test::Input& input) {
    StageAnalyzer analyzer(input.diagnostics, input.checker.types(), {},
                           cache_test::context());
    auto module = analyzer.analyze(*input.module);
    if (!module)
        throw std::runtime_error("module missing");
    auto identity = input.identity();
    if (!identity)
        throw std::runtime_error("identity missing");
    return {*identity, module->provenance};
}
struct Restored {
    FrozenGraph graph;
    std::vector<ProvenanceSite> sites;
    std::vector<ProvenanceRoot> roots;
};
Restored restore_for_test(const FunctionEvidence& evidence) {
    Builder builder;
    std::map<std::string, SummaryId> summaries;
    const auto hash = [](const cache::QueryKey& key) {
        return cache::query_fingerprint(key)->hex();
    };
    for (const auto& entry : evidence.catalog()) {
        const auto& member = evidence.member(entry);
        if (member.key.definition.kind == cache::DefinitionKind::Function)
            summaries.emplace(hash(member.key), builder.reserve_summary());
    }
    std::map<std::string, std::vector<NodeId>> nodes;
    std::vector<ProvenanceSite> sites{{}};
    std::vector<ProvenanceRoot> roots;
    for (const auto& group : evidence.groups()) {
        auto details = evidence.read_detail(group);
        if (!details)
            throw std::runtime_error("detail missing");
        const auto resolve = [&](EvidenceScopeRef ref) {
            if (ref == 0)
                return SummaryId{};
            const auto key =
                (ref & (1U << 31U))
                    ? group.imports[(ref & ~(1U << 31U)) - 1].key.hex()
                    : hash(group.members[ref - 1].key);
            return summaries.at(key);
        };
        for (std::size_t index = 0; index < group.members.size(); ++index) {
            const auto& member = group.members[index];
            const auto& detail = (*details)[index];
            auto& mapped = nodes[hash(member.key)];
            mapped.push_back({});
            std::vector<SiteId> site_ids{0};
            for (std::size_t i = 1; i < detail.sites.size(); ++i) {
                site_ids.push_back(static_cast<SiteId>(sites.size()));
                sites.push_back(detail.sites[i]);
            }
            for (const auto& node : detail.nodes) {
                std::vector<Edge> edges;
                for (auto edge :
                     std::span(detail.edges)
                         .subspan(node.edge_begin, node.edge_count)) {
                    edge.target = mapped.at(edge.target.value);
                    edges.push_back(edge);
                }
                const auto payload =
                    node.rule == Rule::Call || node.rule == Rule::SummaryRef
                        ? resolve(node.payload).value
                        : node.payload;
                mapped.push_back(builder.add(node.rule, site_ids.at(node.site),
                                             edges, payload,
                                             resolve(node.owner.value)));
            }
            for (const auto& root : member.roots)
                roots.push_back({root.name, root.span, root.stage,
                                 mapped.at(root.node.value),
                                 root.requires_comptime, resolve(root.scope)});
        }
    }
    for (const auto& entry : evidence.catalog()) {
        const auto& member = evidence.member(entry);
        if (member.key.definition.kind != cache::DefinitionKind::Function)
            continue;
        EXPECT_EQ(member.value, member.effects);
        builder.define_summary(
            summaries.at(hash(member.key)),
            nodes.at(hash(member.key)).at(member.value.value));
    }
    return {std::move(builder).freeze(), std::move(sites), std::move(roots)};
}
using SemanticState =
    std::tuple<unsigned, std::size_t, std::size_t, std::string,
               std::vector<std::pair<std::size_t, std::size_t>>>;
std::vector<SemanticState>
semantic_result(const FrozenGraph& graph, std::span<const ProvenanceSite> sites,
                const QueryResult& result, bool witness) {
    std::vector<SemanticState> output;
    const auto add = [&](NodeId node, std::uint32_t context) {
        const auto& record = graph.node(node);
        const auto& site = sites[record.site];
        std::vector<std::pair<std::size_t, std::size_t>> path;
        while (context) {
            const auto& invocation = result.invocations[context];
            const auto& call = sites[graph.node(invocation.call).site];
            path.emplace_back(call.span.begin, call.span.end);
            context = invocation.parent;
        }
        output.emplace_back(static_cast<unsigned>(record.rule), site.span.begin,
                            site.span.end, site.label, path);
    };
    if (witness) {
        for (const auto& state : result.witness)
            add(state.node, state.context);
    } else {
        for (const auto& source : result.sources)
            add(source.node, source.context);
        std::ranges::sort(output);
    }
    return output;
}
class FunctionEvidenceTest : public ::testing::Test {
  protected:
    FunctionEvidenceTest() {
        static std::atomic<unsigned> sequence{};
        directory =
            fs::temp_directory_path() /
            ("pagos-function-evidence-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~FunctionEvidenceTest() override {
        std::error_code error;
        fs::remove_all(directory, error);
    }
    void publish(const Prepared& prepared) {
        auto result = FunctionEvidence::publish(
            cache::FileStore(directory), *prepared.analysis, prepared.identity,
            cache_test::context());
        ASSERT_TRUE(result) << static_cast<int>(result.error().code);
    }
    auto load(const Prepared& prepared, cache::StoreOptions options = {}) {
        return FunctionEvidence::load(cache::FileStore(directory, options),
                                      prepared.identity, cache_test::context());
    }
    fs::path object(const cache::ObjectRef& ref) const {
        const auto hash = ref.digest.hex();
        return directory / "objects" / hash.substr(0, 2) / hash;
    }
    void replace_module_summary(const Prepared& prepared,
                                const FunctionEvidence& evidence,
                                cache::Bytes bytes) {
        cache::FileStore store(directory);
        const EvidenceGroup* module = nullptr;
        for (const auto& group : evidence.groups())
            for (const auto& member : group.members)
                if (member.key.definition.kind == cache::DefinitionKind::Module)
                    module = &group;
        ASSERT_NE(module, nullptr);
        auto replacement = store.write_object(
            cache::ObjectKind::EvidenceSummary, std::move(bytes));
        ASSERT_TRUE(replacement);
        const auto key =
            FunctionEvidence::key_for(prepared.identity, cache_test::context());
        auto candidate =
            store.lookup(key, prepared.identity.module_fingerprint);
        ASSERT_TRUE(candidate);
        auto root = *candidate->artifacts[0].bytes;
        cache::wire::Writer writer({});
        writer.u8(static_cast<std::uint8_t>(replacement->kind));
        writer.raw(replacement->digest.bytes);
        writer.u64(replacement->size);
        const auto new_bytes = *writer.finish();
        const auto member_key =
            *cache::query_fingerprint(module->members[0].key);
        auto position =
            std::search(root.begin(), root.end(), member_key.bytes.begin(),
                        member_key.bytes.end());
        ASSERT_NE(position, root.end());
        ASSERT_GE(position - root.begin(), 45);
        std::ranges::copy(new_bytes, position - 45);
        const auto fingerprint = cache::query_fingerprint(key)->hex();
        fs::remove(directory / "queries" / fingerprint.substr(0, 2) /
                   fingerprint /
                   (prepared.identity.module_fingerprint.hex() + ".manifest"));
        const auto semantic =
            cache::fingerprint("pagos.function-evidence-root.v1", root);
        const std::array artifacts{cache::ArtifactInput{
            "root", cache::ObjectKind::EvidenceRoot, std::move(root), true}};
        ASSERT_TRUE(store.publish(key, prepared.identity.module_fingerprint,
                                  semantic, artifacts));
    }
    fs::path directory;
};

TEST_F(FunctionEvidenceTest,
       CapturesActualSpecializationsAndExcludesRuntimeOriginsFromKeys) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(prepared.analysis->scopes().size(), 4U);
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    EXPECT_EQ(evidence->catalog().size(), 4U);
    std::vector<cache::QueryKey> scale_keys;
    for (const auto& entry : evidence->catalog()) {
        const auto& member = evidence->member(entry);
        if (member.key.definition.name == "scale") {
            scale_keys.push_back(member.key);
            ASSERT_EQ(member.key.arguments.size(), 2U);
            EXPECT_EQ(member.key.kind,
                      cache::QueryKind::FreezeFunctionEvidence);
            EXPECT_EQ(member.key.arguments[0].stage,
                      cache::ArgumentStage::Comptime);
            EXPECT_EQ(member.key.arguments[1].stage,
                      cache::ArgumentStage::Runtime);
            EXPECT_FALSE(member.key.arguments[1].value);
            EXPECT_GT(member.value.value, 0U);
            EXPECT_EQ(member.value, member.effects);
            EXPECT_TRUE(member.recorded_complete);
        }
    }
    ASSERT_EQ(scale_keys.size(), 2U);
    EXPECT_NE(scale_keys[0].arguments[0].value,
              scale_keys[1].arguments[0].value);
    EXPECT_NE(cache::query_fingerprint(scale_keys[0]),
              cache::query_fingerprint(scale_keys[1]));
    // Repeated outer(a)/outer(b) share one actual scope, even with different
    // caller origins.
    EXPECT_EQ(std::ranges::count(prepared.analysis->scopes(),
                                 std::string("outer"), &ProvenanceScope::name),
              1);
}

TEST_F(FunctionEvidenceTest,
       HotClosureNeverReadsColdTopologyAndRecordsDecisionEntry) {
    cache_test::Input input("fn pick(x: u32) -> u32 { if x == 0 { x + 1 } else "
                            "{ x - 1 } } let r = pick(external_input());");
    const auto prepared = prepare(input);
    publish(prepared);
    auto first = load(prepared);
    ASSERT_TRUE(first);
    std::set<std::string> cold;
    for (const auto& group : first->groups())
        cold.insert(group.detail.digest.hex());
    unsigned cold_reads = 0, writes = 0;
    cache::StoreOptions options;
    options.fault = [&](cache::IoOperation operation, const fs::path& path) {
        if (operation == cache::IoOperation::ReadFile &&
            cold.contains(path.filename().string()))
            ++cold_reads;
        if (operation != cache::IoOperation::ReadFile &&
            operation != cache::IoOperation::LockFile)
            ++writes;
        return std::error_code{};
    };
    auto evidence = load(prepared, options);
    ASSERT_TRUE(evidence);
    EXPECT_EQ(cold_reads, 0U);
    EXPECT_EQ(writes, 0U);
    EXPECT_EQ(evidence->metadata_objects_read(), evidence->groups().size() + 1);
    bool found_decision = false;
    for (const auto& group : evidence->groups()) {
        auto details = evidence->read_detail(group);
        ASSERT_TRUE(details);
        EXPECT_EQ(details->size(), group.members.size());
        for (const auto& member : group.members)
            if (member.key.definition.name == "pick") {
                EXPECT_FALSE(member.decisions.empty());
                found_decision = true;
                EXPECT_TRUE(member.roots.empty());
            }
    }
    EXPECT_TRUE(found_decision);
    EXPECT_EQ(cold_reads, evidence->groups().size());
    EXPECT_EQ(writes, 0U);
}

TEST_F(FunctionEvidenceTest,
       MissingRequiredSummaryRejectsClosureButMissingDetailDoesNot) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    const auto& group = evidence->groups()[0];
    fs::remove(object(group.detail));
    EXPECT_TRUE(load(prepared));
    auto detail = evidence->read_detail(group);
    ASSERT_FALSE(detail);
    EXPECT_EQ(detail.error().code, cache::StoreErrorCode::MissingObject);
    fs::remove(object(group.object));
    auto missing = load(prepared);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, cache::StoreErrorCode::MissingObject);
}

TEST_F(FunctionEvidenceTest,
       ValidRecursiveSummariesFreezeTogetherWithoutImportHashCycles) {
    cache_test::Input input(
        "fn even(n: u32) -> u32 { if n == 0 { 1 } else { odd(n - 1) } } "
        "fn odd(n: u32) -> u32 { if n == 0 { 0 } else { even(n - 1) } } "
        "let answer = even(external_input());");
    const auto prepared = prepare(input);
    ASSERT_FALSE(input.diagnostics.has_error());
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    std::size_t recursive_groups = 0;
    for (const auto& group : evidence->groups()) {
        if (group.members.size() == 2) {
            ++recursive_groups;
            EXPECT_TRUE(group.imports.empty());
            auto details = evidence->read_detail(group);
            ASSERT_TRUE(details);
            EXPECT_EQ(details->size(), 2U);
            for (const auto& member : group.members)
                EXPECT_EQ(member.key.arguments[0].stage,
                          cache::ArgumentStage::Runtime);
        }
    }
    EXPECT_EQ(recursive_groups, 1U);
}

TEST_F(FunctionEvidenceTest, WhitespaceReusesObjectsAndRebindsCurrentAnchors) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    cache_test::Input shifted("// new revision\n\n" + program);
    const auto changed = prepare(shifted);
    EXPECT_EQ(changed.identity.module_fingerprint,
              prepared.identity.module_fingerprint);
    auto current = load(changed);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->groups().size(), evidence->groups().size());
    bool checked = false;
    for (const auto& group : current->groups()) {
        auto detail = current->read_detail(group);
        ASSERT_TRUE(detail);
        for (const auto& member : group.members)
            for (const auto& root : member.roots) {
                EXPECT_GE(root.span.begin, 17U);
                checked = true;
            }
    }
    EXPECT_TRUE(checked);
    EXPECT_EQ(FunctionEvidence::publish(cache::FileStore(directory),
                                        *changed.analysis, changed.identity,
                                        cache_test::context()),
              cache::PublishResult::AlreadyPresent);
    auto old_view = FunctionEvidenceView::open(std::move(*evidence));
    auto current_view = FunctionEvidenceView::open(std::move(*current));
    ASSERT_TRUE(old_view);
    ASSERT_TRUE(current_view);
    auto old_site = old_view->view()->pin(old_view->roots()[0].node);
    auto current_site =
        current_view->view()->pin(current_view->roots()[0].node);
    ASSERT_TRUE(old_site);
    ASSERT_TRUE(current_site);
    ASSERT_NE(old_site->site(), nullptr);
    ASSERT_NE(current_site->site(), nullptr);
    EXPECT_EQ(current_site->site()->span.begin,
              old_site->site()->span.begin + 17);
    old_view->view()->evict();
    current_view->view()->evict();
    EXPECT_EQ(current_site->site()->span.end, old_site->site()->span.end + 17);
}

TEST_F(FunctionEvidenceTest,
       WrongVersionAndImportKeyFailEvenWithValidCasChecksums) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    const EvidenceGroup* module = nullptr;
    for (const auto& group : evidence->groups())
        if (group.members[0].key.definition.kind ==
            cache::DefinitionKind::Module)
            module = &group;
    ASSERT_NE(module, nullptr);
    ASSERT_FALSE(module->imports.empty());
    auto original = cache::FileStore(directory).read_object(module->object);
    ASSERT_TRUE(original);
    auto changed = *original;
    changed[9] = 2;
    replace_module_summary(prepared, *evidence, changed);
    auto wrong = load(prepared);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, cache::StoreErrorCode::IncompatibleVersion);
    changed = *original;
    // Header/model 17 + dependencies 32 + detail ref 41 + import count 4.
    changed[94 + 41 + 4] ^= 1;
    replace_module_summary(prepared, *evidence, changed);
    wrong = load(prepared);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, cache::StoreErrorCode::CorruptObject);
}

TEST_F(FunctionEvidenceTest,
       CallSiteDecisionCopiesStayOutsideSharedFunctionDecisionEntry) {
    cache_test::Input input(
        "fn choose(x: u32) -> u32 { let y = if x == 0 { 7 } else { 9 }; y } "
        "let a = choose(external_input()); let b = choose(external_input());");
    const auto prepared = prepare(input);
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    std::size_t recorded = 0;
    for (const auto& group : evidence->groups())
        for (const auto& member : group.members)
            recorded += member.decisions.size();
    EXPECT_EQ(recorded, prepared.analysis->decisions().size());
    for (const auto& group : evidence->groups())
        EXPECT_TRUE(evidence->read_detail(group));
}

TEST_F(FunctionEvidenceTest,
       ChangedDependenciesAndTargetsMissWithoutCreatingFiles) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    cache_test::Input changed(program + " let extra = 3;");
    const auto edit = prepare(changed);
    EXPECT_EQ(load(edit).error().code, cache::StoreErrorCode::NotFound);
    auto context = cache_test::context();
    context.target.cpu = "different-cpu";
    EXPECT_EQ(FunctionEvidence::load(cache::FileStore(directory),
                                     prepared.identity, context)
                  .error()
                  .code,
              cache::StoreErrorCode::NotFound);
    EXPECT_FALSE(fs::exists(directory / "tmp-new"));
}

TEST_F(FunctionEvidenceTest, FailedLeafPublicationNeverPublishesRootManifest) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    cache::StoreOptions options;
    options.fault = [](cache::IoOperation operation, const fs::path&) {
        return operation == cache::IoOperation::PublishObject
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    auto published = FunctionEvidence::publish(
        cache::FileStore(directory, options), *prepared.analysis,
        prepared.identity, cache_test::context());
    ASSERT_FALSE(published);
    EXPECT_FALSE(fs::exists(directory / "queries"));
    EXPECT_EQ(load(prepared).error().code, cache::StoreErrorCode::NotFound);
}

TEST_F(FunctionEvidenceTest, ConcurrentPublishAndReadUseOneImmutableClosure) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    std::array<std::optional<cache::StoreError>, 4> errors;
    std::array<bool, 4> valid{};
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < errors.size(); ++index)
        threads.emplace_back([&, index] {
            auto result = FunctionEvidence::publish(
                cache::FileStore(directory), *prepared.analysis,
                prepared.identity, cache_test::context());
            if (!result)
                errors[index] = result.error();
            else {
                auto evidence = load(prepared);
                valid[index] = evidence.has_value();
            }
        });
    for (auto& thread : threads)
        thread.join();
    for (std::size_t index = 0; index < errors.size(); ++index) {
        EXPECT_FALSE(errors[index]);
        EXPECT_TRUE(valid[index]);
    }
}

TEST_F(FunctionEvidenceTest,
       LocalIdsAndImportsReconstructEquivalentNativeQueries) {
    for (const auto& text :
         {program,
          std::string("fn pick(x: u32) -> u32 { if x == 0 { x + 1 } else { "
                      "external_input() } } let a = pick(external_input()); "
                      "let b = pick(external_input());"),
          std::string("fn even(n: u32) -> u32 { if n == 0 { 1 } else { odd(n - "
                      "1) } } fn odd(n: u32) -> u32 { if n == 0 { 0 } else { "
                      "even(n - 1) } } let r = even(external_input());"),
          std::string(
              "fn demand(x: u32) -> u32 { comptime let local = if x == 0 { 7 } "
              "else { 7 }; local } let r = demand(external_input());")}) {
        SCOPED_TRACE(text);
        fs::remove_all(directory);
        cache_test::Input input(text);
        const auto prepared = prepare(input);
        publish(prepared);
        auto evidence = load(prepared);
        ASSERT_TRUE(evidence);
        const auto restored = restore_for_test(
            *evidence); // Codec oracle only, never a lazy IO claim.
        ASSERT_EQ(restored.roots.size(), prepared.analysis->roots().size());
        for (std::size_t index = 0; index < restored.roots.size(); ++index) {
            const auto& original = prepared.analysis->roots()[index];
            const auto& root = restored.roots[index];
            EXPECT_EQ(root.name, original.name);
            EXPECT_EQ(root.requires_comptime, original.requires_comptime);
            for (const auto channel : {QueryKind::Value, QueryKind::Effects}) {
                for (const bool witness : {false, true}) {
                    const auto expected = prepared.analysis->graph().query(
                        original.node, channel, witness);
                    const auto actual =
                        restored.graph.query(root.node, channel, witness);
                    EXPECT_EQ(semantic_result(prepared.analysis->graph(),
                                              prepared.analysis->sites(),
                                              expected, witness),
                              semantic_result(restored.graph, restored.sites,
                                              actual, witness));
                    EXPECT_EQ(actual.complete, expected.complete);
                    EXPECT_EQ(actual.recursive_call_cut,
                              expected.recursive_call_cut);
                }
            }
        }
    }
}

TEST_F(FunctionEvidenceTest,
       LimitsFailClosedAndUnsupportedIdentitiesDoNotPublishRoots) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    auto limited = FunctionEvidence::publish(
        cache::FileStore(directory), *prepared.analysis, prepared.identity,
        cache_test::context(), {.elements = 1});
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, cache::StoreErrorCode::LimitExceeded);
    EXPECT_FALSE(fs::exists(directory / "queries"));
    publish(prepared);
    auto evidence =
        FunctionEvidence::load(cache::FileStore(directory), prepared.identity,
                               cache_test::context(), {.elements = 1});
    EXPECT_FALSE(evidence);
    auto changed = prepared.identity;
    changed.definitions.erase(changed.definitions.begin());
    EXPECT_FALSE(FunctionEvidence::load(cache::FileStore(directory), changed,
                                        cache_test::context()));
}

TEST_F(FunctionEvidenceTest,
       ActualCheckedAstIdentityCannotBeReplacedDuringExport) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    cache_test::Input changed(
        "fn scale(k: u32, x: u32) -> u32 { k + x } "
        "fn outer(x: u32) -> u32 { scale(4, x) } "
        "let a = external_input(); let b = external_input(); "
        "let aa = outer(a); let bb = outer(b); let cc = scale(5, a);");
    const auto edit = prepare(changed);
    auto invalid = FunctionEvidence::publish(cache::FileStore(directory),
                                             *prepared.analysis, edit.identity,
                                             cache_test::context());
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().codec, cache::Error::InvalidContext);
    EXPECT_FALSE(fs::exists(directory));
    StageAnalyzer legacy(input.diagnostics, input.checker.types());
    const auto without_context = legacy.analyze(*input.module);
    EXPECT_FALSE(FunctionEvidence::publish(
        cache::FileStore(directory), *without_context->provenance,
        prepared.identity, cache_test::context()));
}

TEST_F(FunctionEvidenceTest,
       WrongDecisionOwnerFailsEvenWhenItReferencesAnExistingMember) {
    cache_test::Input input(
        "let a = if external_input() == 0 { 1 } else { 2 };");
    const auto prepared = prepare(input);
    publish(prepared);
    auto evidence = load(prepared);
    ASSERT_TRUE(evidence);
    ASSERT_EQ(evidence->groups().size(), 1U);
    const auto& group = evidence->groups()[0];
    ASSERT_FALSE(group.members[0].decisions.empty());
    auto bytes = *cache::FileStore(directory).read_object(group.object);
    cache::wire::Reader reader(bytes, {});
    reader.raw(17 + 32 + 41);
    ASSERT_EQ(reader.u32(), 0U); // No Imports.
    ASSERT_EQ(reader.u32(), 1U);
    reader.raw(reader.u32());  // Member's canonical key blob.
    reader.raw(4 + 4 + 4 + 1); // Node count, two ports, coverage capability.
    ASSERT_GT(reader.u32(), 0U);
    reader.raw(4 + 1 + 1 + 1);
    if (reader.boolean())
        reader.raw(4);
    const auto owner_offset = bytes.size() - reader.remaining();
    ASSERT_EQ(reader.u32(), 0U);
    bytes[owner_offset] = 1; // Existing module member is not a function owner.
    replace_module_summary(prepared, *evidence, bytes);
    auto invalid = load(prepared);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, cache::StoreErrorCode::CorruptObject);
}

std::vector<SemanticState> view_result(const FunctionEvidenceView& view,
                                       const QueryResult& result,
                                       bool witness) {
    std::vector<SemanticState> output;
    const auto add = [&](NodeId id, std::uint32_t context) {
        const auto lease = view.view()->pin(id);
        if (!lease || !lease->site())
            throw std::runtime_error("missing site");
        const auto& site = *lease->site();
        std::vector<std::pair<std::size_t, std::size_t>> path;
        while (context) {
            const auto& invocation = result.invocations[context];
            auto call = view.view()->pin(invocation.call);
            if (!call || !call->site())
                throw std::runtime_error("missing call");
            path.emplace_back(call->site()->span.begin, call->site()->span.end);
            context = invocation.parent;
        }
        output.emplace_back(static_cast<unsigned>(lease->node().rule),
                            site.span.begin, site.span.end, site.label, path);
    };
    if (witness)
        for (auto state : result.witness)
            add(state.node, state.context);
    else {
        for (auto source : result.sources)
            add(source.node, source.context);
        std::ranges::sort(output);
    }
    return output;
}
QueryResult solve_view(QueryTask& task) {
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed)
        (void)task.advance({1, 1, 1, 1});
    return task.result();
}
TEST_F(FunctionEvidenceTest,
       LazyViewQueriesMatchMemoryOriginsEffectsWitnessesAndRecursion) {
    for (const auto& text :
         {program,
          std::string("fn pick(x: u32) -> u32 { if x == 0 { x + 1 } else { "
                      "external_input() } } let a = pick(external_input()); "
                      "let b = pick(external_input());"),
          std::string(
              "fn recurse(x: u32) -> u32 { if x == 0 { 0 } else { recurse(x - "
              "1) } } let a = recurse(external_input());")}) {
        fs::remove_all(directory);
        cache_test::Input input(text);
        const auto prepared = prepare(input);
        publish(prepared);
        auto loaded = load(prepared);
        ASSERT_TRUE(loaded);
        auto view = FunctionEvidenceView::open(std::move(*loaded));
        ASSERT_TRUE(view);
        EXPECT_EQ(view->view()->read_stats().objects_read, 0U);
        ASSERT_EQ(view->roots().size(), prepared.analysis->roots().size());
        for (std::size_t i = 0; i < view->roots().size(); ++i) {
            for (auto channel : {QueryKind::Value, QueryKind::Effects}) {
                for (bool witness : {false, true}) {
                    QueryTask task(view->view(), view->roots()[i].node, channel,
                                   witness);
                    const auto actual = solve_view(task);
                    ASSERT_EQ(task.progress(), QueryProgress::Completed);
                    const auto expected = prepared.analysis->graph().query(
                        prepared.analysis->roots()[i].node, channel, witness);
                    EXPECT_EQ(view_result(*view, actual, witness),
                              semantic_result(prepared.analysis->graph(),
                                              prepared.analysis->sites(),
                                              expected, witness));
                    EXPECT_EQ(actual.complete, expected.complete);
                    EXPECT_EQ(actual.recursive_call_cut,
                              expected.recursive_call_cut);
                }
            }
        }
        EXPECT_EQ(view->decisions().size(),
                  prepared.analysis->decisions().size());
        const auto decisions = [](auto records) {
            std::multiset<
                std::tuple<hir::Stage, StageRule, std::size_t, std::size_t,
                           StageReason, std::optional<std::uint32_t>, bool>>
                result;
            for (const auto& item : records)
                result.emplace(item.result, item.rule, item.span.begin,
                               item.span.end, item.reason, item.member,
                               item.instantiated);
            return result;
        };
        EXPECT_EQ(decisions(view->decisions()),
                  decisions(prepared.analysis->decisions()));
        for (const auto& scope : view->scopes()) {
            if (!scope.id.value)
                continue;
            ASSERT_TRUE(scope.specialization);
            const auto match = std::ranges::find_if(
                prepared.analysis->scopes(), [&](const auto& original) {
                    return original.specialization &&
                           original.specialization->definition ==
                               scope.specialization->definition &&
                           original.specialization->arguments ==
                               scope.specialization->arguments;
                });
            ASSERT_NE(match, prepared.analysis->scopes().end());
            EXPECT_EQ(scope.specialization->definition_fingerprint,
                      match->specialization->definition_fingerprint);
            EXPECT_EQ(scope.decisions.size(), match->decisions.size());
        }
    }
}
TEST_F(FunctionEvidenceTest,
       LazyViewExcludesUnrelatedColdObjectAndPreservesPinnedSites) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    auto loaded = load(prepared);
    ASSERT_TRUE(loaded);
    for (const auto& group : loaded->groups())
        for (const auto& member : group.members)
            if (member.key.definition.name == "scale" &&
                member.key.arguments[0].value == Constant(IntegerValue(5)))
                fs::remove(object(group.detail));
    auto view = FunctionEvidenceView::open(std::move(*loaded));
    ASSERT_TRUE(view);
    const auto found = std::ranges::find(view->roots(), std::string("aa"),
                                         &ProvenanceRoot::name);
    ASSERT_NE(found, view->roots().end());
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        QueryTask task(view->view(), found->node);
        (void)solve_view(task);
        EXPECT_EQ(task.progress(), QueryProgress::Completed);
    }
    EXPECT_EQ(view->view()->read_stats().objects_read,
              3U); // Module, outer and scale(4).
    auto lease = view->view()->pin(found->node);
    ASSERT_TRUE(lease);
    ASSERT_NE(lease->site(), nullptr);
    const auto label = lease->site()->label;
    view->view()->evict();
    EXPECT_EQ(view->view()->read_stats().cached_bytes, 0U);
    EXPECT_GT(view->view()->read_stats().live_bytes, 0U);
    fs::remove_all(directory);
    view =
        std::unexpected(EvidenceError{EvidenceErrorCode::MissingBlock, {}, {}});
    EXPECT_EQ(lease->site()->label, label);
}
TEST_F(FunctionEvidenceTest,
       MissingQueryDetailFailsButHotDecisionsRemainReadable) {
    cache_test::Input input("fn pick(x: u32) -> u32 { if x == 0 { 1 } else { 2 "
                            "} } let a = pick(external_input());");
    const auto prepared = prepare(input);
    publish(prepared);
    auto loaded = load(prepared);
    ASSERT_TRUE(loaded);
    for (const auto& group : loaded->groups())
        fs::remove(object(group.detail));
    auto view = FunctionEvidenceView::open(std::move(*loaded));
    ASSERT_TRUE(view);
    std::ostringstream output, telemetry;
    EXPECT_TRUE(explain_function_evidence(*view, output, telemetry, "pick",
                                          "decisions", 1));
    EXPECT_NE(output.str().find("decision rule="), std::string::npos);
    EXPECT_EQ(view->view()->read_stats().objects_read, 0U);
    QueryTask task(view->view(), view->roots()[0].node);
    EXPECT_FALSE(solve_view(task).complete);
    ASSERT_TRUE(task.failure());
    EXPECT_EQ(task.failure()->code, EvidenceErrorCode::MissingBlock);
    EXPECT_EQ(task.advance().details, DetailAvailability::Unavailable);
}
TEST_F(FunctionEvidenceTest, LazyTopologyResumesAndDoesNotExpandSiblingCalls) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    auto loaded = load(prepared);
    ASSERT_TRUE(loaded);
    auto view = FunctionEvidenceView::open(std::move(*loaded), 0);
    ASSERT_TRUE(view);
    const auto found = std::ranges::find(view->roots(), std::string("aa"),
                                         &ProvenanceRoot::name);
    ASSERT_NE(found, view->roots().end());
    EvidenceTopologyTask task(view->view(), found->node);
    EXPECT_EQ(task.advance({0, 0, 0, 0}).progress, QueryProgress::Paused);
    EXPECT_EQ(view->view()->read_stats().objects_read, 0U);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(task.advance({}, stop.get_token()).progress,
              QueryProgress::Cancelled);
    task.resume();
    std::size_t nodes = 0, edges = 0, links = 0;
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed) {
        auto page = task.advance({1, 1, 1, 1});
        EXPECT_LE(page.records.size(), 1U);
        for (auto record : page.records) {
            nodes += record.kind == TopologyRecordKind::Node ? 1U : 0U;
            edges += record.kind == TopologyRecordKind::Edge ? 1U : 0U;
            links += record.kind == TopologyRecordKind::SummaryLink ? 1U : 0U;
        }
        view->view()->evict();
    }
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    EXPECT_EQ(task.stats().nodes, nodes);
    EXPECT_EQ(task.stats().edges, edges);
    EXPECT_EQ(links, 4U);
    EXPECT_GT(nodes, 0U);
    EXPECT_EQ(view->view()->read_stats().live_bytes, 0U);
}
TEST_F(FunctionEvidenceTest,
       SharedLazyReaderSupportsConcurrentQueriesAndEviction) {
    cache_test::Input input(program);
    const auto prepared = prepare(input);
    publish(prepared);
    auto loaded = load(prepared);
    ASSERT_TRUE(loaded);
    auto view = FunctionEvidenceView::open(std::move(*loaded));
    ASSERT_TRUE(view);
    std::array<bool, 4> complete{};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < complete.size(); ++i)
        threads.emplace_back([&, i] {
            QueryTask task(view->view(), view->roots().back().node);
            while (task.progress() != QueryProgress::Completed &&
                   task.progress() != QueryProgress::Failed) {
                (void)task.advance({1, 1, 1, 1});
                view->view()->evict();
            }
            complete[i] = task.progress() == QueryProgress::Completed;
        });
    for (auto& thread : threads)
        thread.join();
    for (auto result : complete)
        EXPECT_TRUE(result);
}
} // namespace
