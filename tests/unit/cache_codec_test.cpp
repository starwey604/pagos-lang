#include "cache_test_input.h"

#include <limits>

namespace {
using namespace pagos;
using namespace pagos::cache;

TEST(CacheCodec, WireBytesAreExplicitAndLittleEndian) {
    const auto bytes = encode_value(
        {sema::Type::integer(8), *IntegerValue::create({8, false}, 42)});
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, (Bytes{'P', 'G', 'C', '1', 1, 0, 0, 0, 1, 2, 8,
                             0,   0,   42,  0,   0, 0, 0, 0, 0, 0}));
    EXPECT_NE(fingerprint("a", Bytes{'b'}), fingerprint("ab", {}));
    EXPECT_EQ(fingerprint("a", Bytes{'b'}).hex().size(), 64U);
}

TEST(CacheCodec, ScalarsAndArraysRoundTripAllDeclaredWidths) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        for (bool is_signed : {false, true}) {
            const auto type = sema::Type::integer(width, is_signed);
            const auto value =
                *IntegerValue::create(type.integer_type, UINT64_MAX);
            for (const auto& input :
                 {TypedValue{type, value},
                  TypedValue{sema::Type::array(type, 2),
                             std::vector<IntegerValue>{value, value}}}) {
                const auto bytes = encode_value(input);
                ASSERT_TRUE(bytes);
                const auto decoded = decode_value(*bytes);
                ASSERT_TRUE(decoded);
                EXPECT_EQ(*decoded, input);
            }
        }
    }
    for (unsigned bits : {32U, 64U}) {
        const auto type = sema::Type::usize(bits);
        const TypedValue input{type,
                               *IntegerValue::create(type.integer_type, 7)};
        const auto bytes = encode_value(input);
        ASSERT_TRUE(bytes);
        EXPECT_EQ(decode_value(*bytes), input);
    }
    for (const auto& input :
         {TypedValue{sema::TypeKind::Bool, true},
          TypedValue{sema::Type::array(sema::TypeKind::Bool, 3),
                     std::vector<bool>{true, false, true}}}) {
        const auto bytes = encode_value(input);
        ASSERT_TRUE(bytes);
        EXPECT_EQ(decode_value(*bytes), input);
    }
}

TEST(CacheCodec, RejectsUnsupportedIncompleteAndMismatchedPayloads) {
    for (const auto& type :
         {sema::Type(sema::TypeKind::Unknown),
          sema::Type(sema::TypeKind::Error), sema::Type(sema::TypeKind::Never),
          sema::Type(sema::TypeKind::Void), sema::Type::record("R"),
          sema::Type::pointer(sema::Type::integer(32), false, 32),
          sema::Type::array(sema::Type::integer(32), 0),
          sema::Type::integer(7)}) {
        const auto bytes = encode_value({type, IntegerValue(7)});
        ASSERT_FALSE(bytes);
        EXPECT_EQ(bytes.error(), Error::UnsupportedType);
    }
    auto noncanonical = sema::Type::integer(32);
    noncanonical.record_name = "ignored";
    EXPECT_EQ(encode_value({noncanonical, IntegerValue(7)}).error(),
              Error::UnsupportedType);
    for (const auto& value :
         {TypedValue{sema::TypeKind::Bool, IntegerValue(7)},
          TypedValue{sema::Type::integer(32), true},
          TypedValue{sema::Type::integer(8), IntegerValue(7)},
          TypedValue{sema::Type::array(sema::Type::integer(32), 2),
                     std::vector<IntegerValue>{7}}})
        EXPECT_EQ(encode_value(value).error(), Error::InvalidValue);
}

TEST(CacheCodec, RejectsEveryTruncationVersionsTagsAndTrailingBytes) {
    const auto original = *encode_value(
        {sema::Type::array(sema::Type::integer(8), 2),
         std::vector<IntegerValue>{*IntegerValue::create({8, false}, 1),
                                   *IntegerValue::create({8, false}, 2)}});
    for (std::size_t size = 0; size < original.size(); ++size)
        EXPECT_FALSE(decode_value(std::span(original).first(size))) << size;
    auto bytes = original;
    bytes[4] = 2;
    EXPECT_EQ(decode_value(bytes).error(), Error::IncompatibleVersion);
    bytes = original;
    bytes[8] = 255;
    EXPECT_FALSE(decode_value(bytes));
    bytes = original;
    bytes.push_back(0);
    EXPECT_FALSE(decode_value(bytes));
    bytes = *encode_value({sema::TypeKind::Bool, true});
    bytes.back() = 2;
    EXPECT_FALSE(decode_value(bytes));
    bytes = *encode_value(
        {sema::Type::integer(8), *IntegerValue::create({8, false}, 1)});
    bytes.back() = 1; // Bits outside the declared width must not be truncated.
    EXPECT_FALSE(decode_value(bytes));
}

TEST(CacheCodec, BoundsLengthsBeforeAllocatingAndEncoding) {
    const auto input = TypedValue{sema::Type::array(sema::Type::integer(32), 2),
                                  std::vector<IntegerValue>{1, 2}};
    const auto bytes = *encode_value(input);
    EXPECT_EQ(encode_value(input, {.bytes = bytes.size() - 1}).error(),
              Error::LimitExceeded);
    EXPECT_EQ(decode_value(bytes, {.bytes = bytes.size() - 1}).error(),
              Error::LimitExceeded);
    EXPECT_EQ(encode_value(input, {.elements = 1}).error(),
              Error::LimitExceeded);
    EXPECT_EQ(decode_value(bytes, {.elements = 1}).error(),
              Error::LimitExceeded);
    auto huge = bytes;
    std::fill(huge.begin() + 10, huge.begin() + 14, 255);
    EXPECT_EQ(decode_value(huge).error(), Error::LimitExceeded);
    // Even with a relaxed count limit a tiny file cannot imply a huge
    // allocation.
    EXPECT_FALSE(decode_value(huge, {.elements = UINT32_MAX}));
}

TEST(CacheCodec, QueryKeysRoundTripWithRuntimeConstraintsAndArrays) {
    auto key = cache_test::key();
    key.kind = QueryKind::SpecializeResidual;
    key.arguments.push_back({sema::Type::array(sema::TypeKind::Bool, 2),
                             ArgumentStage::Comptime,
                             Constant(std::vector<bool>{true, false})});
    key.arguments.push_back({sema::Type::integer(32), ArgumentStage::Runtime,
                             std::nullopt,
                             RuntimeConstraint::ExplicitBoundary});
    const auto bytes = encode_query_key(key);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(decode_query_key(*bytes), key);
    for (std::size_t size = 0; size < bytes->size(); ++size)
        EXPECT_FALSE(decode_query_key(std::span(*bytes).first(size))) << size;
    auto trailing = *bytes;
    trailing.push_back(0);
    EXPECT_FALSE(decode_query_key(trailing));
    EXPECT_EQ(encode_query_key(key, {.string_bytes = 1}).error(),
              Error::LimitExceeded);
    EXPECT_EQ(decode_query_key(*bytes, {.string_bytes = 1}).error(),
              Error::LimitExceeded);
    for (std::size_t index = 0; index < bytes->size(); ++index) {
        auto mutated = *bytes;
        mutated[index] ^= 255;
        if (const auto decoded = decode_query_key(mutated); decoded) {
            const auto canonical = encode_query_key(*decoded);
            ASSERT_TRUE(canonical);
            EXPECT_EQ(*canonical, mutated);
        }
    }
}

TEST(CacheCodec, RuntimeValuesCannotLeakIntoKeysAndTargetsMustMatchUsize) {
    auto key = cache_test::key();
    key.arguments[0].stage = ArgumentStage::Runtime;
    EXPECT_EQ(encode_query_key(key).error(), Error::InvalidValue);
    key.arguments[0].value.reset();
    EXPECT_EQ(encode_query_key(key).error(),
              Error::InvalidValue); // Pure query.
    key.kind = QueryKind::SpecializeResidual;
    EXPECT_TRUE(encode_query_key(key));
    key.arguments[0].type = sema::Type::usize(64);
    EXPECT_EQ(encode_query_key(key).error(), Error::InvalidValue);
    key.arguments[0].type = sema::Type::record("R");
    EXPECT_EQ(encode_query_key(key).error(), Error::UnsupportedType);
}

TEST(CacheCodec, InvalidContextsCannotProduceUsableKeys) {
    for (const auto* path :
         {"", "/input.pgs", "../input.pgs", "src/../input.pgs",
          "src/./input.pgs", "src\\input.pgs"}) {
        auto key = cache_test::key();
        key.context.module.relative_path = path;
        key.definition.module = key.context.module;
        EXPECT_EQ(encode_query_key(key).error(), Error::InvalidContext) << path;
    }
    auto key = cache_test::key();
    key.context.target.triple.clear();
    EXPECT_FALSE(encode_query_key(key));
    key = cache_test::key();
    key.context.compiler_build.clear();
    EXPECT_FALSE(encode_query_key(key));
    key = cache_test::key();
    ++key.context.query_version;
    const auto bytes = encode_query_key(key);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(decode_query_key(*bytes).error(), Error::IncompatibleVersion);
}
} // namespace
