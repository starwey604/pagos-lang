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
