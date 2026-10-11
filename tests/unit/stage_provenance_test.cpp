#include "../../lib/stage/provenance_recorder.h"
#include "pagos/mir/lowering.h"
#include "pagos/sema/type_checker.h"
#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace pagos;
using namespace pagos::stage::provenance;

struct Input {
    explicit Input(std::string text)
        : source(
              source::SourceFile::from_text("provenance.pgs", std::move(text))),
          diagnostics(source), checker(diagnostics, 64) {
        syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        module = parser.parse_module();
        EXPECT_FALSE(diagnostics.has_error());
        EXPECT_TRUE(checker.check(*module));
    }
    source::SourceFile source;
    source::DiagnosticEngine diagnostics;
    sema::TypeChecker checker;
    std::unique_ptr<syntax::Module> module;
};

const stage::ProvenanceRoot& root(const stage::ProvenanceAnalysis& analysis,
                                  std::string_view name) {
    const auto found =
        std::ranges::find(analysis.roots(), name, &stage::ProvenanceRoot::name);
    if (found == analysis.roots().end())
        throw std::logic_error("missing provenance root: " + std::string(name));
    return *found;
}

QueryResult query(const hir::Module& module, std::string_view name,
                  QueryKind kind = QueryKind::Value) {
    return module.provenance->graph().query(root(*module.provenance, name).node,
                                            kind);
}

const stage::StageDecision& decision(const stage::ProvenanceAnalysis& analysis,
                                     std::string_view name) {
    auto node = root(analysis, name).node;
    while (true) {
        if (const auto* result = analysis.decision(node))
            return *result;
        const auto edges = analysis.graph().edges(node);
        const auto value = std::ranges::find_if(edges, [](const auto& edge) {
            return edge.relation == Relation::Data ||
                   edge.relation == Relation::Contract;
        });
        if (value == edges.end())
            throw std::logic_error("missing decision: " + std::string(name));
        node = value->target;
    }
}

TEST(StageDecision, DistinguishesExplicitBoundaryFromExternalInput) {
    Input input("runtime let boundary = 7; let input = external_input(); "
                "let alias = boundary;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto& boundary = decision(*module->provenance, "alias");
    EXPECT_EQ(boundary.rule, stage::StageRule::RuntimeBoundary);
    EXPECT_EQ(boundary.result, hir::Stage::Runtime);
    EXPECT_EQ(boundary.node, query(*module, "boundary").sources[0].node);
    const auto source = query(*module, "input").sources[0].node;
    EXPECT_EQ(module->provenance->graph().node(source).rule, Rule::Origin);
    EXPECT_EQ(module->provenance->decision(source), nullptr);
    EXPECT_EQ(module->provenance->decision({}), nullptr);
}

TEST(StageDecision, RuntimeSelectionRetainsControlAndRejectsEqualBranchDemand) {
    Input input("let flag = external_input() != 0; "
                "let different = if flag { 1 } else { 2 }; "
                "comptime let equal = if flag { 7 } else { 7 };");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E2001");
    for (const auto* name : {"different", "equal"}) {
        const auto& result = decision(*module->provenance, name);
        EXPECT_EQ(result.rule, stage::StageRule::RuntimeSelection);
        EXPECT_EQ(result.result, hir::Stage::Runtime);
        const auto edges = module->provenance->graph().edges(result.node);
        ASSERT_EQ(edges.size(), 3U);
        EXPECT_EQ(edges[0].relation, Relation::Control);
        EXPECT_EQ(module->provenance->graph().query(edges[0].target).sources,
                  query(*module, "flag").sources);
    }
}

TEST(StageDecision,
     ShortCircuitDistinguishesRuntimeControlFromSelectedOperand) {
    Input input("let flag = external_input() != 0; "
                "let selected = true && flag; let controlled = flag && true; "
                "comptime let skipped = false && (external_input() != 0);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(decision(*module->provenance, "selected").rule,
              stage::StageRule::OperandPropagation);
    EXPECT_EQ(decision(*module->provenance, "controlled").rule,
              stage::StageRule::RuntimeSelection);
    EXPECT_TRUE(query(*module, "skipped").sources.empty());
    EXPECT_TRUE(query(*module, "skipped", QueryKind::Effects).sources.empty());
}

TEST(StageDecision, ProjectionRecordsSelectedMemberAndOpaqueOrDynamicCases) {
    Input input("record Config { base: u32, baud: u32 } "
                "let input = external_input(); let config = Config(base: 7, "
                "baud: input); "
                "comptime let base = config.base; let baud = config.baud; "
                "comptime let pure = Config(base: 9, baud: 3).base; "
                "runtime let hidden = config; let opaque = hidden.base; "
                "let array = [7, input]; comptime let fixed = array[0]; "
                "let member = array[1]; let indexed = array[input]; "
                "let branch = if input == 0 { config } else { config }; "
                "let blocked = branch.base;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    for (const auto* name : {"base", "pure", "fixed"}) {
        const auto& result = decision(*module->provenance, name);
        EXPECT_EQ(result.rule, stage::StageRule::MemberProjection);
        EXPECT_EQ(result.result, hir::Stage::Comptime);
        EXPECT_EQ(result.reason, stage::StageReason::KnownMember);
        EXPECT_EQ(result.member, 0U);
        EXPECT_TRUE(query(*module, name).sources.empty());
    }
    for (const auto* name : {"baud", "member"}) {
        const auto& result = decision(*module->provenance, name);
        EXPECT_EQ(result.reason, stage::StageReason::RuntimeMember);
        EXPECT_EQ(result.member, 1U);
        EXPECT_EQ(query(*module, name).sources,
                  query(*module, "input").sources);
    }
    for (const auto* name : {"opaque", "blocked"})
        EXPECT_EQ(decision(*module->provenance, name).reason,
                  stage::StageReason::OpaqueAggregate);
    const auto& indexed = decision(*module->provenance, "indexed");
    EXPECT_EQ(indexed.reason, stage::StageReason::RuntimeIndex);
    EXPECT_FALSE(indexed.member);
    EXPECT_EQ(query(*module, "base", QueryKind::Effects).sources.size(), 1U);
    EXPECT_EQ(query(*module, "indexed", QueryKind::Effects).sources.size(), 2U);
    EXPECT_TRUE(mir::lower(*module));
}

TEST(StageDecision, InlineNestedKnownProjectionRebindsWorkAndRetainsRules) {
    Input input(
        "fn base(x: u32) -> u32 { let array = [7, x]; array[0] } "
        "fn nested(x: u32) -> u32 { base(x) } "
        "let a = external_input(); let b = external_input(); "
        "comptime let first = nested(a); comptime let second = nested(b);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto& first = decision(*module->provenance, "first");
    const auto& second = decision(*module->provenance, "second");
    EXPECT_EQ(first.rule, stage::StageRule::MemberProjection);
    EXPECT_EQ(second.rule, first.rule);
    EXPECT_NE(first.node, second.node);
    EXPECT_EQ(second.result, hir::Stage::Comptime);
    EXPECT_TRUE(query(*module, "second").sources.empty());
    EXPECT_EQ(query(*module, "first", QueryKind::Effects).sources,
              query(*module, "a", QueryKind::Effects).sources);
    EXPECT_EQ(query(*module, "second", QueryKind::Effects).sources,
              query(*module, "b", QueryKind::Effects).sources);
    EXPECT_TRUE(mir::lower(*module));
}

TEST(StageDecision, CachedSelectionUsesOneRuleAndEachCallsActualSource) {
    Input input("fn choose(x: u32) -> u32 { if x == 0 { 1 } else { 2 } } "
                "let a = external_input(); let b = external_input(); "
                "let first = choose(a); let second = choose(b);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
    std::optional<NodeId> shared;
    for (const auto* name : {"first", "second"}) {
        const auto witness = module->provenance->graph().query(
            root(*module->provenance, name).node, QueryKind::Value, true);
        bool found = false;
        for (const auto& state : witness.witness) {
            const auto* item = module->provenance->decision(state.node);
            if (!item || item->rule != stage::StageRule::RuntimeSelection)
                continue;
            if (shared) {
                EXPECT_EQ(item->node, *shared);
            }
            shared = item->node;
            found = true;
        }
        EXPECT_TRUE(found);
    }
    EXPECT_EQ(query(*module, "second").sources[0].site,
              query(*module, "b").sources[0].site);
}

TEST(StageDecision,
     LocalFailedContractRetainsDecisionAfterArgumentSubstitution) {
    Input input(
        "fn require(x: u32) -> u32 { "
        "comptime let selected = if x == 0 { 7 } else { 7 }; selected } "
        "let input = external_input(); let result = require(input);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_TRUE(input.diagnostics.has_error());
    const auto& result = decision(*module->provenance, "selected");
    EXPECT_EQ(result.rule, stage::StageRule::RuntimeSelection);
    EXPECT_EQ(result.result, hir::Stage::Runtime);
    EXPECT_EQ(query(*module, "selected").sources,
              query(*module, "input").sources);
}

TEST(StageDecision,
     SnapshotOwnsDecisionMetadataAfterRepeatedAnalysisAndDestruction) {
    std::shared_ptr<const stage::ProvenanceAnalysis> snapshot;
    NodeId node;
    {
        Input input("let array = [7, external_input()]; comptime let selected "
                    "= array[0];");
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
        auto module = analyzer.analyze(*input.module);
        snapshot = module->provenance;
        node = decision(*snapshot, "selected").node;
        module.reset();
        const auto again = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(again->provenance->decisions().size(),
                  snapshot->decisions().size());
    }
    ASSERT_NE(snapshot->decision(node), nullptr);
    EXPECT_EQ(snapshot->decision(node)->reason,
              stage::StageReason::KnownMember);
    EXPECT_TRUE(
        std::ranges::is_sorted(snapshot->decisions(), {}, [](const auto& item) {
            return item.node.value;
        }));
    EXPECT_TRUE(snapshot->graph()
                    .query(root(*snapshot, "selected").node)
                    .sources.empty());
}

TEST(StageDecision, MissingMigratedEventCannotBeReconstructedFromHir) {
    stage::ProvenanceRecorder recorder;
    auto value = std::make_shared<hir::Expr>();
    value->kind = hir::Expr::Kind::If;
    value->stage = hir::Stage::Runtime;
    value->operands = {hir::make_constant(true, sema::TypeKind::Bool, {}),
                       hir::make_constant(7U, sema::TypeKind::Integer, {}),
                       hir::make_constant(7U, sema::TypeKind::Integer, {})};
    hir::Module module;
    module.result = value;
    EXPECT_THROW((void)recorder.finish(module), std::logic_error);
}

TEST(StageProvenance, CollectsBothDataAndControlOriginsOnContractFailure) {
    Input input("let revision = external_input(); let baud = external_input(); "
                "let clock = if revision == 1 { 80 } else { 40 }; "
                "let divisor = clock / baud; comptime let config = divisor;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E2001");
    const auto& contract = root(*module->provenance, "config");
    EXPECT_TRUE(contract.requires_comptime);
    EXPECT_EQ(module->provenance->graph().node(contract.node).rule,
              Rule::RequireComptime);
    const auto result = query(*module, "config");
    EXPECT_TRUE(result.complete);
    ASSERT_EQ(result.sources.size(), 2U);
    EXPECT_NE(module->provenance->site(result.sources[0].site).span.begin,
              module->provenance->site(result.sources[1].site).span.begin);
    EXPECT_EQ(query(*module, "config", QueryKind::Effects).sources.size(), 3U);
}

TEST(StageProvenance, KeepsExplicitBoundariesOpaqueAndDeadBranchesAbsent) {
    Input input(
        "runtime let boundary = external_input(); let alias = boundary; "
        "comptime let known = if true { 7 } else { external_input() }; "
        "let selected = if boundary == 1 { 7 } else { 7 };");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto result = query(*module, "alias");
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(module->provenance->graph().node(result.sources[0].node).rule,
              Rule::RuntimeBoundary);
    EXPECT_EQ(module->provenance->site(result.sources[0].site).label,
              "boundary");
    EXPECT_EQ(query(*module, "alias", QueryKind::Effects).sources.size(), 1U);
    EXPECT_TRUE(query(*module, "known").sources.empty());
    EXPECT_TRUE(query(*module, "known", QueryKind::Effects).sources.empty());
    EXPECT_EQ(root(*module->provenance, "selected").stage, hir::Stage::Runtime);
    EXPECT_EQ(query(*module, "selected").sources.size(), 1U);
}

TEST(StageProvenance, RebindsNestedCachedFunctionsAndKeepsUnusedArgumentWork) {
    Input input("fn increment(x: u32) -> u32 { x + 1 } "
                "fn choose(a: u32, b: u32) -> u32 { increment(a) } "
                "let a = external_input(); let b = external_input(); "
                "let c = external_input(); let first = choose(a, b); "
                "let second = choose(c, b);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_GT(analyzer.stats().residual_cache_hits, 0U);
    const auto actual = query(*module, "c");
    const auto result = query(*module, "second");
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.sources[0].site, actual.sources[0].site);
    EXPECT_EQ(result.contexts, 2U);
    EXPECT_EQ(query(*module, "second", QueryKind::Effects).sources.size(), 2U);
    EXPECT_TRUE(mir::lower(*module));
}

TEST(StageProvenance, DistinguishesInternalReadsAtDifferentCallSites) {
    Input input("fn read() -> u32 { external_input() } "
                "let a = read(); let b = read(); let both = a + b;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto result = query(*module, "both");
    ASSERT_EQ(result.sources.size(), 2U);
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.sources[0].site, result.sources[1].site);
    EXPECT_NE(result.sources[0].context, result.sources[1].context);
    const auto first_call = result.invocations[result.sources[0].context].call;
    const auto second_call = result.invocations[result.sources[1].context].call;
    EXPECT_NE(first_call, second_call);
    EXPECT_EQ(query(*module, "both", QueryKind::Effects).sources.size(), 2U);
}

TEST(StageProvenance,
     PreservesRecordAndArrayMemberPrecisionAndConstructionWork) {
    Input input("record Config { base: u32, a: u32, b: u32 } "
                "let a = external_input(); let b = external_input(); "
                "let config = Config(base: 4096, a: a, b: b); "
                "comptime let base = config.base; let field = config.b; "
                "let array = [7, a, b]; comptime let fixed = array[0]; "
                "let element = array[2]; let dynamic = array[a];");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto expected = query(*module, "b");
    for (const auto* name : {"base", "fixed"}) {
        EXPECT_TRUE(query(*module, name).sources.empty());
        EXPECT_EQ(query(*module, name, QueryKind::Effects).sources.size(), 2U);
    }
    for (const auto* name : {"field", "element"}) {
        const auto selected = query(*module, name);
        ASSERT_EQ(selected.sources.size(), 1U);
        EXPECT_EQ(selected.sources[0].site, expected.sources[0].site);
        EXPECT_EQ(query(*module, name, QueryKind::Effects).sources.size(), 2U);
    }
    EXPECT_EQ(query(*module, "dynamic", QueryKind::Effects).sources.size(), 3U);
    EXPECT_TRUE(mir::lower(*module));
}

TEST(StageProvenance, SeparatesKnownReturnsFromVolatileWrites) {
    Input input(
        "fn set(p: *mut u32) -> u32 { volatile *p = 42; 7 } "
        "comptime let a = set(4096usize as *mut u32); "
        "comptime let b = set(4096usize as *mut u32); let both = a + b;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(root(*module->provenance, "both").stage, hir::Stage::Comptime);
    EXPECT_TRUE(query(*module, "both").sources.empty());
    const auto work = query(*module, "both", QueryKind::Effects);
    EXPECT_TRUE(work.complete);
    ASSERT_EQ(work.sources.size(), 2U);
    for (const auto& source : work.sources)
        EXPECT_EQ(module->provenance->site(source.site).label,
                  "volatile store");
    EXPECT_TRUE(mir::lower(*module));
}

TEST(StageProvenance,
     BindsLocalContractsBeforeAnAbortedFunctionBodyDisappears) {
    Input input("fn combine(a: u32, b: u32) -> u32 { "
                "comptime let required = a + b; required } "
                "let a = external_input(); let b = external_input(); "
                "let result = combine(a, b);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_TRUE(input.diagnostics.has_error());
    const auto result = query(*module, "required");
    EXPECT_TRUE(result.complete);
    ASSERT_EQ(result.sources.size(), 2U);
    for (const auto& source : result.sources)
        EXPECT_EQ(module->provenance->site(source.site).label,
                  "external_input");
}

TEST(StageProvenance, MarksRecursiveExpansionPartial) {
    Input input("fn count(n: u32) -> u32 { "
                "if n == 0 { 0 } else { count(n - 1) } } "
                "let n = external_input(); let result = count(n);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto result = query(*module, "result");
    EXPECT_FALSE(result.complete);
    EXPECT_TRUE(result.recursive_call_cut);
    EXPECT_FALSE(result.sources.empty());
    EXPECT_FALSE(query(*module, "result", QueryKind::Effects).complete);
    EXPECT_FALSE(std::ranges::any_of(
        module->provenance->decisions(), [](const auto& item) {
            return item.rule == stage::StageRule::ConservativeFallback;
        }));
}

TEST(StageProvenance, ExplainsConservativeRecursiveCompletionOfKnownValues) {
    Input input("fn known(n: u32) -> u32 { "
                "if n == 0 { 0 } else { known(n - 1) }; 7 } "
                "let result = known(external_input());");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(root(*module->provenance, "result").stage, hir::Stage::Runtime);
    const auto result = query(*module, "result");
    ASSERT_EQ(result.sources.size(), 1U);
    const auto* reason = module->provenance->decision(result.sources[0].node);
    ASSERT_NE(reason, nullptr);
    EXPECT_EQ(reason->result, hir::Stage::Runtime);
    EXPECT_EQ(reason->rule, stage::StageRule::ConservativeFallback);
    EXPECT_EQ(reason->reason, stage::StageReason::RecursiveCompletion);
    EXPECT_EQ(module->provenance->site(result.sources[0].site).label,
              "recursive completion known");
    EXPECT_FALSE(query(*module, "result", QueryKind::Effects).complete);
}

TEST(StageProvenance, KeepsEarlyReturnAndLoopControlDependencies) {
    Input input("fn early(end: u32) -> u32 { "
                "for i in 0..end { return 7; } return 9; } "
                "let end = external_input(); let result = early(end);");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto result = query(*module, "result");
    EXPECT_TRUE(result.complete);
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(result.sources[0].site, query(*module, "end").sources[0].site);
    EXPECT_EQ(query(*module, "result", QueryKind::Effects).sources.size(), 2U);
}

TEST(StageProvenance, KeepsStorageValueRequirementsSeparateFromPurity) {
    Input input("static const invalid: u32 = external_input(); "
                "static const valid: u32 = 7;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_TRUE(input.diagnostics.has_error());
    EXPECT_TRUE(root(*module->provenance, "static invalid").requires_comptime);
    EXPECT_EQ(query(*module, "static invalid").sources.size(), 1U);
    EXPECT_EQ(
        query(*module, "static invalid", QueryKind::Effects).sources.size(),
        1U);
    EXPECT_TRUE(query(*module, "static valid").sources.empty());
    EXPECT_TRUE(
        query(*module, "static valid", QueryKind::Effects).sources.empty());
}

TEST(StageProvenance, TreatsCAbiCallsAsOpaqueButKeepsArgumentEffects) {
    Input input("extern fn read(n: u32) -> u32; "
                "let result = read(external_input());");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto result = query(*module, "result");
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(module->provenance->graph().node(result.sources[0].node).rule,
              Rule::RuntimeBoundary);
    EXPECT_EQ(module->provenance->decision(result.sources[0].node), nullptr);
    EXPECT_EQ(query(*module, "result", QueryKind::Effects).sources.size(), 2U);
}

TEST(StageProvenance, RecordsAddressChecksAndNamesMemoryReadsIndependently) {
    Input input("record R { value: u32 } "
                "static const table: [u32; 2] = [5, 6]; "
                "let index = external_input(); let element = &table[index]; "
                "let loaded = *element; "
                "let p = (external_input() as usize) as *const R; "
                "let field = &(*p).value; let sample = volatile *field;");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(query(*module, "element", QueryKind::Effects).sources.size(), 2U);
    EXPECT_EQ(query(*module, "loaded", QueryKind::Effects).sources.size(), 3U);
    EXPECT_EQ(query(*module, "field", QueryKind::Effects).sources.size(), 2U);
    const auto effects = query(*module, "sample", QueryKind::Effects);
    ASSERT_EQ(effects.sources.size(), 3U);
    EXPECT_TRUE(std::ranges::any_of(effects.sources, [&](const auto& source) {
        return module->provenance->site(source.site).label == "volatile read";
    }));
    EXPECT_TRUE(mir::lower(*module));
}

TEST(StageProvenance, SnapshotOutlivesInputAndRepeatedAnalysis) {
    std::shared_ptr<const stage::ProvenanceAnalysis> snapshot;
    {
        Input input("let x = external_input(); let y = x + x;");
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
        auto first = analyzer.analyze(*input.module);
        snapshot = first->provenance;
        first.reset();
        const auto second = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_NE(snapshot.get(), second->provenance.get());
        EXPECT_EQ(snapshot->graph().stats().nodes,
                  second->provenance->graph().stats().nodes);
    }
    const auto result = snapshot->graph().query(root(*snapshot, "y").node);
    EXPECT_TRUE(result.complete);
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(snapshot->site(result.sources[0].site).label, "external_input");
    std::ostringstream explanation;
    stage::explain_provenance(*snapshot, explanation);
    EXPECT_NE(explanation.str().find("binding y: Runtime"), std::string::npos);
}

} // namespace
