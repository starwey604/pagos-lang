#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/cache/store.h"
#include "pagos/stage/replay_descriptor.h"

#include <atomic>

namespace {
using namespace pagos;
using namespace pagos::stage;
using Op = ScalarReplayOp;
using Code = ReplayErrorCode;
struct Fixture {
    CacheIdentity identity;
    ReplayDescriptor descriptor;
    cache::TypedValue expected{sema::Type::integer(32), IntegerValue(49)};
    Fixture() {
        cache_test::Input input("fn square(x: u32) -> u32 { x * x }");
        identity = *input.identity();
        const auto* definition =
            identity.find(cache::DefinitionKind::Function, "square");
        descriptor.specialization = *make_query_key(
            *definition, cache_test::context(), cache_test::key().arguments);
        descriptor.verified_inputs = identity.module_fingerprint;
        for (const auto& site : identity.sites)
            if (input.source.text().substr(site.span.begin,
                                           site.span.end - site.span.begin) ==
                "x * x")
                descriptor.result_site = site.anchor;
        descriptor.result_type = expected.type;
        descriptor.expected_result = *replay_value_fingerprint(expected);
        descriptor.nodes = {{.operation = Op::Input,
                             .type = expected.type,
                             .position = descriptor.result_site.position,
                             .input = 0},
                            {.operation = Op::Multiply,
                             .type = expected.type,
                             .position = descriptor.result_site.position,
                             .operands = {0, 0}}};
    }
    auto validate(const ReplayDescriptor* value) const {
        return validate_replay_descriptor(value, identity,
                                          descriptor.specialization,
                                          descriptor.verified_inputs, expected);
    }
};
TEST(ReplayDescriptor, CanonicalRoundTripOwnsBindingsAndOutlivesSyntax) {
    Fixture fixture;
    ASSERT_TRUE(fixture.validate(&fixture.descriptor));
    const auto bytes = encode_replay_descriptor(fixture.descriptor);
    ASSERT_TRUE(bytes);
    EXPECT_EQ((*bytes)[8], 15U);
    const auto restored = decode_replay_descriptor(*bytes);
    ASSERT_TRUE(restored);
    EXPECT_EQ(*restored, fixture.descriptor);
    EXPECT_EQ(encode_replay_descriptor(*restored), bytes);
    EXPECT_EQ(replay_descriptor_fingerprint(*restored),
              replay_descriptor_fingerprint(fixture.descriptor));
    EXPECT_NE(replay_descriptor_fingerprint(*restored),
              cache::fingerprint("other-domain", *bytes));
    EXPECT_TRUE(fixture.validate(&*restored));
}
TEST(ReplayDescriptor, SupportedOperationsWidthsAndBoolHaveExplicitShapes) {
    Fixture fixture;
    for (const auto& type :
         {sema::Type::integer(8), sema::Type::integer(8, true),
          sema::Type::integer(16), sema::Type::integer(16, true),
          sema::Type::integer(32), sema::Type::integer(32, true),
          sema::Type::integer(64), sema::Type::integer(64, true),
          sema::Type::usize(32), sema::Type::usize(64),
          sema::Type(sema::TypeKind::Bool)}) {
        for (unsigned opcode = 1; opcode <= 21; ++opcode) {
            const auto op = static_cast<Op>(opcode);
            const bool boolean = type.kind == sema::TypeKind::Bool;
            const bool equality = op == Op::Equal || op == Op::NotEqual;
            const bool comparison = op >= Op::Less;
            if (boolean && op != Op::Input && op != Op::Constant &&
                op != Op::Not && !equality)
                continue;
            if (!boolean && op == Op::Not)
                continue;
            auto value = fixture.descriptor;
            if (type.integer_type.is_usize)
                value.specialization.context.target =
                    cache_test::context(type.integer_type.width).target;
            value.specialization.arguments = {
                {type, cache::ArgumentStage::Comptime,
                 boolean
                     ? Constant(true)
                     : Constant(*IntegerValue::create(type.integer_type, 7))}};
            auto input =
                ScalarReplayNode{.operation = Op::Input,
                                 .type = type,
                                 .position = value.result_site.position,
                                 .input = 0};
            const auto result_type = equality || comparison
                                         ? sema::Type(sema::TypeKind::Bool)
                                         : type;
            value.result_type = result_type;
            if (op == Op::Input)
                value.nodes = {input};
            else if (op == Op::Constant)
                value.nodes = {
                    {.operation = op,
                     .type = type,
                     .position = input.position,
                     .constant = *value.specialization.arguments[0].value}};
            else
                value.nodes = {
                    input,
                    {.operation = op,
                     .type = result_type,
                     .position = input.position,
                     .operands = opcode <= 5
                                     ? std::vector<std::uint32_t>{0}
                                     : std::vector<std::uint32_t>{0, 0}}};
            const auto bytes = encode_replay_descriptor(value);
            ASSERT_TRUE(bytes) << opcode << ' ' << sema::type_name(type);
            EXPECT_EQ(decode_replay_descriptor(*bytes), value);
        }
    }
}
TEST(ReplayDescriptor, TypedResultAndRecipeFingerprintsDistinguishContent) {
    Fixture fixture;
    const auto baseline = replay_value_fingerprint(fixture.expected);
    for (const auto& value :
         {cache::TypedValue{sema::Type::integer(8),
                            *IntegerValue::create({8, false}, 49)},
          cache::TypedValue{sema::Type::integer(32, true),
                            *IntegerValue::create({32, true}, 49)},
          cache::TypedValue{sema::Type::usize(32),
                            *IntegerValue::create({32, false, true}, 49)},
          cache::TypedValue{sema::Type::integer(32), IntegerValue(50)}})
        EXPECT_NE(replay_value_fingerprint(value), baseline);
    auto changed = fixture.descriptor;
    changed.nodes.back().operation = Op::Add;
    EXPECT_NE(replay_descriptor_fingerprint(changed),
              replay_descriptor_fingerprint(fixture.descriptor));
    // Static admission does not claim to verify source/recipe correspondence or
    // compute the expected value. C4e1b's recorder and C4e2's engine do that.
    EXPECT_TRUE(fixture.validate(&changed));
}
TEST(ReplayDescriptor, MissingOldLiteralDoesNotSynthesizeARecipe) {
    Fixture fixture;
    const auto missing = fixture.validate(nullptr);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, Code::MissingDescriptor);
    const auto old_value = cache::encode_value(fixture.expected);
    ASSERT_TRUE(old_value);
    EXPECT_FALSE(decode_replay_descriptor(*old_value));
}
TEST(ReplayDescriptor,
     RejectsCyclesForwardSlotsUnusedNodesAndPayloadConfusion) {
    Fixture fixture;
    for (const auto bad : {1U, 2U, UINT32_MAX}) {
        auto value = fixture.descriptor;
        value.nodes[1].operands[0] = bad;
        const auto encoded = encode_replay_descriptor(value);
        ASSERT_FALSE(encoded);
        EXPECT_EQ(encoded.error().code, Code::InvalidReference);
        EXPECT_EQ(encoded.error().node, 1U);
    }
    for (unsigned change = 0; change < 8; ++change) {
        auto value = fixture.descriptor;
        switch (change) {
        case 0:
            value.nodes[0].input = 1;
            break;
        case 1:
            value.nodes[0].constant = IntegerValue(7);
            break;
        case 2:
            value.nodes[1].input = 0;
            break;
        case 3:
            value.nodes[0].input.reset();
            break;
        case 4:
            value.nodes[1].operands.pop_back();
            break;
        case 5:
            value.nodes.insert(value.nodes.begin() + 1, value.nodes[0]);
            break;
        case 6:
            value.nodes.clear();
            break;
        case 7:
            value.nodes[1].operation = static_cast<Op>(255);
            break;
        }
        EXPECT_FALSE(encode_replay_descriptor(value)) << change;
    }
}
TEST(ReplayDescriptor,
     RejectsUnsupportedTypesRuntimeMixedTypesAndNoncanonicalFields) {
    Fixture fixture;
    for (const auto& type :
         {sema::Type(sema::TypeKind::Unknown), sema::Type::integer(7),
          sema::Type::record("R"), sema::Type::array(sema::TypeKind::Bool, 2),
          sema::Type::pointer(sema::Type::integer(32), false, 32)}) {
        auto value = fixture.descriptor;
        value.nodes[0].type = type;
        EXPECT_FALSE(encode_replay_descriptor(value));
        EXPECT_FALSE(replay_value_fingerprint({type, IntegerValue(7)}));
    }
    for (unsigned change = 0; change < 9; ++change) {
        auto value = fixture.descriptor;
        switch (change) {
        case 0:
            value.nodes[1].type = sema::TypeKind::Bool;
            break;
        case 1:
            value.nodes[0].type.record_name = "ignored";
            break;
        case 2:
            value.result_type = sema::Type::usize(64);
            break;
        case 3:
            value.nodes[1].operation = Op::Not;
            value.nodes[1].operands = {0};
            break;
        case 4:
            value.specialization.arguments[0] = {
                sema::Type::integer(32), cache::ArgumentStage::Runtime, {}};
            break;
        case 5:
            value.specialization.kind = cache::QueryKind::SpecializeResidual;
            break;
        case 6:
            value.nodes[0].type = sema::Type::integer(32, true);
            break;
        case 7:
            value.nodes[0].operation = Op::Constant;
            value.nodes[0].input.reset();
            value.nodes[0].constant = true;
            break;
        case 8:
            value.nodes[1].type = sema::Type::integer(8);
            break;
        }
        EXPECT_FALSE(encode_replay_descriptor(value)) << change;
    }
}
TEST(ReplayDescriptor,
     CurrentInputsTargetCompilerResultAndDefinitionMustMatch) {
    Fixture fixture;
    for (unsigned change = 0; change < 10; ++change) {
        auto key = fixture.descriptor.specialization;
        auto inputs = fixture.descriptor.verified_inputs;
        auto expected = fixture.expected;
        auto identity = fixture.identity;
        switch (change) {
        case 0:
            key.arguments[0].value = IntegerValue(8);
            break;
        case 1:
            key.context.target = cache_test::context(64).target;
            break;
        case 2:
            key.context.target.cpu = "other";
            break;
        case 3:
            key.context.compiler_build = "other";
            break;
        case 4:
            key.context.semantic_options = "other";
            break;
        case 5:
            inputs.bytes[0] ^= 1;
            break;
        case 6:
            expected.value = IntegerValue(50);
            break;
        case 7:
            key.definition_fingerprint.bytes[0] ^= 1;
            break;
        case 8:
            identity.definitions[0].semantic_fingerprint.bytes[0] ^= 1;
            break;
        case 9:
            identity.module.workspace = "other";
            break;
        }
        const auto valid = validate_replay_descriptor(
            &fixture.descriptor, identity, key, inputs, expected);
        EXPECT_FALSE(valid) << change;
        if (change == 6) {
            EXPECT_EQ(valid.error().code, Code::ResultMismatch);
        }
    }
}
TEST(ReplayDescriptor, AnchorsRebindWhitespaceAndRejectChangedOrMissingSource) {
    Fixture fixture;
    cache_test::Input shifted("// moved\n\nfn square(x: u32) -> u32 { x * x }");
    const auto identity = shifted.identity();
    ASSERT_TRUE(identity);
    EXPECT_TRUE(validate_replay_descriptor(
        &fixture.descriptor, *identity, fixture.descriptor.specialization,
        identity->module_fingerprint, fixture.expected));
    EXPECT_NE(identity->resolve(fixture.descriptor.result_site)->begin,
              fixture.identity.resolve(fixture.descriptor.result_site)->begin);
    cache_test::Input edited("fn square(x: u32) -> u32 { x + x }");
    EXPECT_FALSE(validate_replay_descriptor(
        &fixture.descriptor, *edited.identity(),
        fixture.descriptor.specialization, fixture.descriptor.verified_inputs,
        fixture.expected));
    auto stale = fixture.descriptor;
    stale.nodes[0].position = UINT32_MAX;
    EXPECT_EQ(fixture.validate(&stale).error().code, Code::InvalidAnchor);
    stale = fixture.descriptor;
    ++stale.result_site.position;
    EXPECT_FALSE(encode_replay_descriptor(stale));
    stale = fixture.descriptor;
    stale.result_site.definition.name = "other";
    EXPECT_EQ(encode_replay_descriptor(stale).error().code,
              Code::InvalidAnchor);
    auto missing = fixture.identity;
    missing.sites.clear();
    EXPECT_EQ(validate_replay_descriptor(&fixture.descriptor, missing,
                                         fixture.descriptor.specialization,
                                         fixture.descriptor.verified_inputs,
                                         fixture.expected)
                  .error()
                  .code,
              Code::InvalidAnchor);
}
TEST(ReplayDescriptor, RejectsEveryTruncationVersionsTagsAndTrailingBytes) {
    Fixture fixture;
    const auto original = *encode_replay_descriptor(fixture.descriptor);
    for (std::size_t size = 0; size < original.size(); ++size)
        EXPECT_FALSE(decode_replay_descriptor(std::span(original).first(size)))
            << size;
    for (const auto offset : {0U, 4U, 8U, 9U, 13U}) {
        auto bytes = original;
        bytes[offset] = 255;
        EXPECT_FALSE(decode_replay_descriptor(bytes)) << offset;
    }
    auto bytes = original;
    bytes.push_back(0);
    EXPECT_FALSE(decode_replay_descriptor(bytes));
    for (unsigned change = 0; change < 4; ++change) {
        auto value = fixture.descriptor;
        switch (change) {
        case 0:
            ++value.version;
            break;
        case 1:
            ++value.semantics;
            break;
        case 2:
            ++value.specialization.context.query_version;
            break;
        case 3:
            ++value.specialization.context.budget_version;
            break;
        }
        EXPECT_EQ(encode_replay_descriptor(value).error().code,
                  Code::IncompatibleVersion);
    }
}
TEST(ReplayDescriptor, WireRejectsOversizedLengthsCountsAndInvalidReferences) {
    Fixture fixture;
    const auto original = *encode_replay_descriptor(fixture.descriptor);
    cache::wire::Reader reader(original, {});
    cache::wire::header(reader, cache::ObjectKind::ReplayDescriptor);
    (void)reader.u32();
    (void)reader.u32();
    const auto key_size = reader.u64();
    (void)reader.raw(key_size);
    const auto anchor_size = reader.u64();
    (void)reader.raw(anchor_size);
    (void)reader.raw(32);
    (void)cache::wire::type(reader);
    (void)reader.raw(32);
    const auto count_offset = original.size() - reader.remaining();
    EXPECT_EQ(reader.u32(), 2U);
    const auto first_node = original.size() - reader.remaining();
    (void)reader.u8();
    (void)cache::wire::type(reader);
    (void)reader.u32();
    (void)reader.u32();
    const auto second_node = original.size() - reader.remaining();
    (void)reader.u8();
    (void)cache::wire::type(reader);
    (void)reader.u32();
    const auto operand_offset = original.size() - reader.remaining();
    for (const auto offset : {std::size_t{17}, count_offset, first_node,
                              second_node, operand_offset}) {
        auto corrupt = original;
        corrupt[offset] = 255;
        EXPECT_FALSE(decode_replay_descriptor(corrupt)) << offset;
    }
    auto corrupt = original;
    std::fill_n(corrupt.begin() + 17, 8, 255);
    EXPECT_EQ(decode_replay_descriptor(corrupt).error().code,
              Code::LimitExceeded);
    corrupt = original;
    std::fill_n(corrupt.begin() + static_cast<std::ptrdiff_t>(count_offset), 4,
                255);
    EXPECT_EQ(decode_replay_descriptor(corrupt).error().code,
              Code::LimitExceeded);
    corrupt = original;
    corrupt[operand_offset] = 1; // Self reference in node 1.
    EXPECT_EQ(decode_replay_descriptor(corrupt).error().code,
              Code::InvalidReference);
}
TEST(ReplayDescriptor, BoundedCodecAndIterativeReachabilityHandleLongRecipes) {
    Fixture fixture;
    const auto original = *encode_replay_descriptor(fixture.descriptor);
    for (const auto limits :
         {cache::Limits{.bytes = original.size() - 1},
          cache::Limits{.elements = 1}, cache::Limits{.string_bytes = 2}}) {
        EXPECT_FALSE(encode_replay_descriptor(fixture.descriptor, limits));
        EXPECT_FALSE(decode_replay_descriptor(original, limits));
        EXPECT_FALSE(validate_replay_descriptor(
            &fixture.descriptor, fixture.identity,
            fixture.descriptor.specialization,
            fixture.descriptor.verified_inputs, fixture.expected, limits));
    }
    auto value = fixture.descriptor;
    value.nodes.resize(1);
    for (std::uint32_t i = 1; i < 20000; ++i)
        value.nodes.push_back({.operation = Op::Negate,
                               .type = value.result_type,
                               .position = value.result_site.position,
                               .operands = {i - 1}});
    const auto bytes = encode_replay_descriptor(value, {.depth = 1});
    ASSERT_TRUE(bytes);
    EXPECT_EQ(decode_replay_descriptor(*bytes, {.depth = 1}), value);
    EXPECT_FALSE(encode_replay_descriptor(value, {.elements = 19999}));
}
TEST(ReplayDescriptor, FilesystemCasRoundTripIsExplicitAndIndependentOfStage) {
    Fixture fixture;
    static std::atomic<unsigned> sequence{};
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("pagos-replay-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto bytes = encode_replay_descriptor(fixture.descriptor);
    ASSERT_TRUE(bytes);
    const cache::FileStore store(directory);
    const auto object =
        store.write_object(cache::ObjectKind::ReplayDescriptor, *bytes);
    ASSERT_TRUE(object);
    const auto loaded = cache::FileStore(directory).read_object(*object);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(*loaded, *bytes);
    const auto decoded = decode_replay_descriptor(*loaded);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(fixture.validate(&*decoded));
    const auto hex = object->digest.hex();
    std::filesystem::remove(directory / "objects" / hex.substr(0, 2) / hex);
    EXPECT_FALSE(
        store.read_object(*object)); // No synthesized old-value detail.
}
} // namespace
