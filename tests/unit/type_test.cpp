#include "pagos/mir/lowering.h"
#include "pagos/sema/type.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace {

TEST(Type, UsizeRequiresExplicitTargetAndKeepsNominalIdentity) {
    using pagos::sema::Type;
    const auto source = pagos::syntax::Type::usize();
    EXPECT_EQ(source.integer_width, 0U);
    EXPECT_EQ(Type(source), pagos::sema::TypeKind::Error);
    EXPECT_EQ(Type(source, 16), pagos::sema::TypeKind::Error);
    for (unsigned bits : {32U, 64U}) {
        EXPECT_EQ(Type(source, bits), Type::usize(bits));
        EXPECT_NE(Type(source, bits), Type::integer(bits));
        EXPECT_EQ(Type(pagos::syntax::Type::array(source, 2), bits),
                  Type::array(Type::usize(bits), 2));
        EXPECT_EQ(pagos::sema::type_name(Type(source, bits)), "usize");
    }
}

TEST(Type, SemanticAnnotationsAreDistinctAndComposable) {
    static_assert(!std::is_same_v<pagos::syntax::Type, pagos::sema::Type>);
    using pagos::sema::Type;
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        for (bool is_signed : {false, true}) {
            auto scalar = Type::integer(width, is_signed);
            const auto annotation =
                pagos::syntax::Type::integer(width, is_signed);
            EXPECT_EQ(Type(annotation), scalar);
            EXPECT_EQ(Type(pagos::syntax::Type::array(annotation, 7)),
                      Type::array(scalar, 7));
            auto array = Type::array(scalar, 7);
            EXPECT_EQ(array.element_type(), scalar);
            EXPECT_EQ(array.length, 7U);
            EXPECT_NE(array, Type::array(Type::integer(width, !is_signed), 7));
        }
    }
    EXPECT_EQ(pagos::sema::type_name(Type::array(Type::integer(16, true), 7)),
              "[i16; 7]");
    EXPECT_EQ(
        Type(pagos::syntax::Type::array(pagos::syntax::TypeKind::Integer, 7)),
        Type::array(Type::integer(32), 7));
    EXPECT_NE(Type::record("A"), Type::record("B"));
}

TEST(Type, ConcreteMirRejectsDeferredLengthsAndInvalidWidths) {
    using pagos::mir::Type;
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        EXPECT_TRUE(Type::array(Type::integer(width), 7).valid());
        EXPECT_FALSE(Type::array(Type::integer(width), 0).valid());
    }
    EXPECT_FALSE(Type::integer(0).valid());
    EXPECT_FALSE(Type::integer(65).valid());
    EXPECT_FALSE(Type(Type::Invalid).valid());
    Type record{Type::Record};
    record.record_name = "Mixed";
    record.fields = {Type::integer(8), Type::integer(64, true), Type::Bool};
    EXPECT_TRUE(record.valid());
}

TEST(Type, UnresolvedHirCannotSilentlyBecomeU32) {
    for (auto kind :
         {pagos::sema::TypeKind::Error, pagos::sema::TypeKind::Unknown}) {
        pagos::hir::Module module;
        module.result = pagos::hir::make_constant(std::uint32_t{1}, kind, {});
        EXPECT_FALSE(pagos::mir::lower(module));
    }
    pagos::hir::Module module;
    module.result = pagos::hir::make_constant(
        std::vector<pagos::IntegerValue>{1},
        pagos::sema::Type::array(pagos::sema::Type::integer(32), 0), {});
    EXPECT_FALSE(pagos::mir::lower(module));
}

} // namespace
