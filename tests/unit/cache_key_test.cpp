#include "cache_test_input.h"
#include "pagos/stage/stage_analyzer.h"

namespace {
using namespace pagos;
using namespace pagos::cache;

TEST(CacheKey, DefinitionCatalogMatchesFullIdentityAndValidation) {
    cache_test::Input input("record R { a: u32 } fn f(x: u32) -> u32 { let a = "
                            "[7, x]; a[0] + x } let a = f(3);");
    const auto full = input.identity();
    const auto catalog = stage::build_definition_catalog(
        *input.module, input.checker.types(), cache_test::context());
    ASSERT_TRUE(full);
    ASSERT_TRUE(catalog);
    EXPECT_EQ(catalog->module_fingerprint, full->module_fingerprint);
    ASSERT_EQ(catalog->definitions.size(), full->definitions.size());
    for (std::size_t i = 0; i < full->definitions.size(); ++i) {
        const auto& a = full->definitions[i];
        const auto& b = catalog->definitions[i];
        EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.semantic_fingerprint, b.semantic_fingerprint);
        EXPECT_EQ(a.parameters, b.parameters);
        EXPECT_EQ(a.result, b.result);
    }
    for (const auto limits :
         {Limits{.bytes = 8}, Limits{.elements = 3}, Limits{.depth = 1}}) {
        const auto a =
            stage::build_cache_identity(*input.module, input.checker.types(),
                                        cache_test::context(), limits);
        const auto b = stage::build_definition_catalog(
            *input.module, input.checker.types(), cache_test::context(),
            limits);
        ASSERT_FALSE(a);
        ASSERT_FALSE(b);
        EXPECT_EQ(a.error(), b.error());
    }
    auto wrong = cache_test::context(64);
    EXPECT_EQ(stage::build_definition_catalog(*input.module,
                                              input.checker.types(), wrong)
                  .error(),
              Error::InvalidContext);
    input.module->functions.front()->name_span = {9, 3};
    EXPECT_EQ(stage::build_definition_catalog(
                  *input.module, input.checker.types(), cache_test::context())
                  .error(),
              Error::InvalidAnchor);
    EXPECT_EQ(input.identity().error(), Error::InvalidAnchor);
}

TEST(CacheKey, WhitespaceCommentsAndIntegerSeparatorsPreserveIdentity) {
    cache_test::Input first(
        "fn square(x: u32) -> u32 { x * 1000 } let result = square(7);");
    cache_test::Input second("// comment\n\nfn square( x : u32 ) -> u32 {\n x "
                             "* 1_000\n}\nlet result = square(7);\n");
    const auto a = first.identity();
    const auto b = second.identity();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->module_fingerprint, b->module_fingerprint);
    ASSERT_EQ(a->definitions.size(), b->definitions.size());
    ASSERT_EQ(a->sites.size(), b->sites.size());
    for (std::size_t index = 0; index < a->definitions.size(); ++index) {
        EXPECT_EQ(a->definitions[index].id, b->definitions[index].id);
        EXPECT_EQ(a->definitions[index].semantic_fingerprint,
                  b->definitions[index].semantic_fingerprint);
    }
    for (std::size_t index = 0; index < a->sites.size(); ++index) {
        const auto span = b->resolve(a->sites[index].anchor);
        ASSERT_TRUE(span);
        EXPECT_EQ(span->begin, b->sites[index].span.begin);
        EXPECT_EQ(span->end, b->sites[index].span.end);
    }
    EXPECT_NE(a->sites[0].span.begin, b->sites[0].span.begin);
}

TEST(CacheKey, SourceAnchorsHaveBoundedIndependentWireRecords) {
    cache_test::Input input("fn square(x: u32) -> u32 { x * x }");
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    ASSERT_FALSE(identity->sites.empty());
    const auto& anchor = identity->sites.front().anchor;
    const auto bytes = stage::encode_source_anchor(anchor);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(stage::decode_source_anchor(*bytes), anchor);
    for (std::size_t size = 0; size < bytes->size(); ++size)
        EXPECT_FALSE(
            stage::decode_source_anchor(std::span(*bytes).first(size)));
    EXPECT_EQ(stage::decode_source_anchor(*bytes, {.bytes = bytes->size() - 1})
                  .error(),
              Error::LimitExceeded);
    auto wrong = anchor;
    wrong.definition.module.relative_path = "../wrong.pgs";
    EXPECT_EQ(stage::encode_source_anchor(wrong).error(), Error::InvalidAnchor);
    auto trailing = *bytes;
    trailing.push_back(0);
    EXPECT_FALSE(stage::decode_source_anchor(trailing));
    auto version = *bytes;
    version[4] = 2;
    EXPECT_EQ(stage::decode_source_anchor(version).error(),
              Error::IncompatibleVersion);
}

TEST(CacheKey, DefinitionIdentityAndContentAreSeparate) {
    cache_test::Input first("fn square(x: u32) -> u32 { x * x }");
    cache_test::Input changed("fn square(x: u32) -> u32 { x + x }");
    const auto a = first.identity();
    const auto b = changed.identity();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->definitions[0].id, b->definitions[0].id);
    EXPECT_NE(a->definitions[0].semantic_fingerprint,
              b->definitions[0].semantic_fingerprint);
    EXPECT_EQ(b->resolve(a->sites[0].anchor).error(), Error::InvalidAnchor);
}

TEST(CacheKey, UnrelatedDefinitionsInvalidateModuleButNotFunctionIdentity) {
    cache_test::Input first("fn square(x: u32) -> u32 { x * x }");
    cache_test::Input second("fn helper(x: u32) -> u32 { x + 1 } fn square(x: "
                             "u32) -> u32 { x * x }");
    const auto a = first.identity();
    const auto b = second.identity();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    const auto* square_a = a->find(DefinitionKind::Function, "square");
    const auto* square_b = b->find(DefinitionKind::Function, "square");
    ASSERT_NE(square_a, nullptr);
    ASSERT_NE(square_b, nullptr);
    EXPECT_EQ(square_a->id, square_b->id);
    EXPECT_EQ(square_a->semantic_fingerprint, square_b->semantic_fingerprint);
    EXPECT_NE(a->module_fingerprint, b->module_fingerprint);
    EXPECT_TRUE(b->resolve(a->sites[0].anchor));
}

TEST(CacheKey, FingerprintsKeepResourceStructureContractsAndParameterOrder) {
    for (const auto& pair :
         {std::pair{"fn f() -> u32 { 1 + 1 }", "fn f() -> u32 { 2 }"},
          std::pair{"fn f(a: u32, b: u32) -> u32 { a }",
                    "fn f(b: u32, a: u32) -> u32 { a }"},
          std::pair{"let x = 7;", "runtime let x = 7;"},
          std::pair{"let x = 7;", "comptime let x = 7;"},
          std::pair{"let x = 7;", "let x: u32 = 7;"},
          std::pair{"fn f() -> u32 { let a = 1; a }",
                    "fn f() -> u32 { let b = 1; b }"},
          std::pair{"extern fn f(a: u32) -> u32;",
                    "fn f(a: u32) -> u32 { a }"}}) {
        cache_test::Input first(pair.first);
        cache_test::Input second(pair.second);
        const auto a = first.identity();
        const auto b = second.identity();
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        EXPECT_NE(a->module_fingerprint, b->module_fingerprint) << pair.first;
    }
}

TEST(CacheKey, RecordsGlobalsControlAndEffectAttributesParticipate) {
    for (const auto& pair :
         {std::pair{"record R { x: u32, y: u8 }", "record R { y: u8, x: u32 }"},
          std::pair{"record R { x: u32 }", "record R { x: i32 }"},
          std::pair{"static const x: u32 = 1;", "static mut x: u32 = 1;"},
          std::pair{"static mut x: u32 = 1;",
                    "static mut x: u32 align(16) = 1;"},
          std::pair{"fn f(p: *const u32) -> u32 { *p }",
                    "fn f(p: *const u32) -> u32 { volatile *p }"},
          std::pair{"fn f(p: *mut u32) -> u32 { *p = 1; 7 }",
                    "fn f(p: *mut u32) -> u32 { volatile *p = 1; 7 }"},
          std::pair{"fn f(x: u32) -> u32 { if x == 0 { 1 } else { 2 } }",
                    "fn f(x: u32) -> u32 { if x == 0 { 2 } else { 1 } }"}}) {
        cache_test::Input first(pair.first);
        cache_test::Input second(pair.second);
        const auto a = first.identity();
        const auto b = second.identity();
        ASSERT_TRUE(a) << pair.first;
        ASSERT_TRUE(b) << pair.second;
        EXPECT_NE(a->module_fingerprint, b->module_fingerprint);
    }
}

TEST(CacheKey, CompleteAstShapesAndResolvedGlobalReferencesAreCaptured) {
    cache_test::Input input(
        "record R { x: u32, ready: bool } static mut state: u32 = 1; "
        "fn square(x: u32) -> u32 { return x * x; } "
        "fn table(n: u32) -> [u32; 2] { [for i in 0..n { square(i) }] } "
        "fn f(p: *mut u32) -> u32 { for i in 0..2 { *p = i; } "
        "let r = R(x: state, ready: true); "
        "let a = [1, 2]; let t = table(2); "
        "if !r.ready { (~a[0]) + (t[0] as u32) } else { r.x } }");
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->definitions.size(), 6U);
    // A side-table pointer must belong to this module, never become a disk ID.
    auto types = input.checker.types();
    syntax::Module::Global alien;
    ASSERT_FALSE(types.global_references.empty());
    types.global_references.begin()->second = &alien;
    EXPECT_EQ(
        stage::build_cache_identity(*input.module, types, cache_test::context())
            .error(),
        Error::MissingDefinition);
}

TEST(CacheKey, IncompleteTypesTargetsAndResourceLimitsFailClosed) {
    cache_test::Input input("fn square(x: u32) -> u32 { x * x }");
    auto types = input.checker.types();
    types.expressions.clear();
    EXPECT_EQ(
        stage::build_cache_identity(*input.module, types, cache_test::context())
            .error(),
        Error::MissingType);
    EXPECT_EQ(stage::build_cache_identity(*input.module, input.checker.types(),
                                          cache_test::context(64))
                  .error(),
              Error::InvalidContext);
    EXPECT_EQ(stage::build_cache_identity(*input.module, input.checker.types(),
                                          cache_test::context(), {.depth = 1})
                  .error(),
              Error::LimitExceeded);
    EXPECT_EQ(stage::build_cache_identity(*input.module, input.checker.types(),
                                          cache_test::context(),
                                          {.elements = 1})
                  .error(),
              Error::LimitExceeded);
    EXPECT_EQ(stage::build_cache_identity(*input.module, input.checker.types(),
                                          cache_test::context(), {.bytes = 4})
                  .error(),
              Error::LimitExceeded);
}

TEST(CacheKey, TypedArgumentsDistinguishStageValueWidthSignednessAndUsize) {
    const auto baseline = cache_test::key();
    const auto hash = query_fingerprint(baseline);
    ASSERT_TRUE(hash);
    auto changed = baseline;
    changed.arguments[0].value = IntegerValue(8);
    EXPECT_NE(query_fingerprint(changed), hash);
    for (const auto& type :
         {sema::Type::integer(8), sema::Type::integer(32, true),
          sema::Type::usize(32)}) {
        changed = baseline;
        changed.arguments[0].type = type;
        changed.arguments[0].value =
            *IntegerValue::create(type.integer_type, 7);
        const auto new_hash = query_fingerprint(changed);
        ASSERT_TRUE(new_hash);
        EXPECT_NE(new_hash, hash);
    }
    changed = baseline;
    changed.kind = QueryKind::SpecializeResidual;
    const auto comptime = query_fingerprint(changed);
    changed.arguments[0] = {sema::Type::integer(32), ArgumentStage::Runtime,
                            std::nullopt};
    const auto runtime = query_fingerprint(changed);
    ASSERT_TRUE(runtime);
    EXPECT_NE(runtime, comptime);
    changed.arguments[0].constraint = RuntimeConstraint::ExplicitBoundary;
    EXPECT_NE(query_fingerprint(changed), runtime);
}

TEST(CacheKey, ContextVersionsTargetPropertiesAndModuleIdentityAffectKey) {
    const auto baseline = cache_test::key();
    const auto hash = query_fingerprint(baseline);
    for (unsigned change = 0; change < 10; ++change) {
        auto key = baseline;
        switch (change) {
        case 0:
            key.context.compiler_build = "another-build";
            break;
        case 1:
            key.context.semantic_options = "different-semantics";
            break;
        case 2:
            ++key.context.budget_version;
            break;
        case 3:
            ++key.context.query_version;
            break;
        case 4:
            key.context.target.cpu = "another-cpu";
            break;
        case 5:
            key.context.target.features = "+m";
            break;
        case 6:
            key.context.target.data_layout = "e-p:32:64";
            break;
        case 7:
            key.context.target.little_endian = false;
            break;
        case 8:
            key.context.target = cache_test::context(64).target;
            break;
        case 9:
            key.context.module.workspace = "other-workspace";
            key.definition.module = key.context.module;
            break;
        }
        const auto changed = query_fingerprint(key);
        ASSERT_TRUE(changed);
        EXPECT_NE(changed, hash) << change;
    }
}

TEST(CacheKey, AdapterChecksSignaturesAndRejectsUnimplementedResultTypes) {
    cache_test::Input input("fn square(x: u32) -> u32 { x * x } record R { x: "
                            "u32 } fn make_record() -> R { R(x: 1) }");
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto* function = identity->find(DefinitionKind::Function, "square");
    ASSERT_NE(function, nullptr);
    const auto key = stage::make_query_key(*function, cache_test::context(),
                                           cache_test::key().arguments);
    ASSERT_TRUE(key);
    EXPECT_EQ(decode_query_key(*encode_query_key(*key)), key);
    EXPECT_FALSE(stage::make_query_key(*function, cache_test::context(), {}));
    auto wrong = cache_test::key().arguments;
    wrong[0].type = sema::Type::integer(8);
    EXPECT_FALSE(
        stage::make_query_key(*function, cache_test::context(), wrong));
    const auto* record =
        identity->find(DefinitionKind::Function, "make_record");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(stage::make_query_key(*record, cache_test::context(), {}).error(),
              Error::UnsupportedType);
    cache_test::Input target_sized("fn size() -> usize { 7usize }");
    const auto sized_identity = target_sized.identity();
    ASSERT_TRUE(sized_identity);
    const auto* sized = sized_identity->find(DefinitionKind::Function, "size");
    ASSERT_NE(sized, nullptr);
    EXPECT_TRUE(stage::make_query_key(*sized, cache_test::context(), {}));
    EXPECT_EQ(
        stage::make_query_key(*sized, cache_test::context(64), {}).error(),
        Error::InvalidValue);
}

TEST(CacheKey, IdentityAndAnchorsOutliveSyntaxAndAnalyzerContextIsReadOnly) {
    stage::CacheIdentity snapshot;
    {
        cache_test::Input input(
            "fn square(x: u32) -> u32 { x * x } let result = square(7);");
        const auto identity = input.identity();
        ASSERT_TRUE(identity);
        snapshot = *identity;
        const auto context = cache_test::context();
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types(),
                                      {}, context);
        ASSERT_NE(analyzer.query_context(), nullptr);
        EXPECT_EQ(*analyzer.query_context(), context);
        const auto hir = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(std::get<IntegerValue>(*hir->result->constant).bits(), 49U);
        EXPECT_EQ(analyzer.stats().specializations, 1U);
        EXPECT_EQ(*analyzer.query_context(), context);
    }
    ASSERT_FALSE(snapshot.sites.empty());
    EXPECT_TRUE(snapshot.resolve(snapshot.sites.front().anchor));
    auto wrong = snapshot.sites.front().anchor;
    ++wrong.position;
    wrong.definition.module.workspace = "other";
    EXPECT_EQ(snapshot.resolve(wrong).error(), Error::InvalidAnchor);
}
} // namespace
