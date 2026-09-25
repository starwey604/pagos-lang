#include "pagos/source/diagnostic.h"
#include "pagos/source/source_manager.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

namespace {

TEST(Parser, ParsesArrayTypesLiteralsAndPostfixIndexing) {
    using namespace pagos::syntax;
    const auto source = pagos::source::SourceFile::from_text(
        "array.pgs", "fn make(a: [bool; 1_0]) -> [u32; 2] { [1, 2,] }\n"
                     "let value = make([true, false])[1];\n");
    pagos::source::DiagnosticEngine diagnostics(source);
    Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    Parser parser(tokens, diagnostics);
    const auto module = parser.parse_module();
    ASSERT_FALSE(diagnostics.has_error());
    ASSERT_EQ(module->functions.size(), 1U);
    EXPECT_EQ(module->functions[0]->parameters[0].type,
              Type::array(TypeKind::Bool, 10));
    EXPECT_EQ(module->functions[0]->result, Type::array(TypeKind::U32, 2));
    ASSERT_EQ(module->functions[0]->body->tail->kind, Expr::Kind::Array);
    const auto& array =
        static_cast<const ArrayExpr&>(*module->functions[0]->body->tail);
    EXPECT_EQ(array.elements.size(), 2U);
    const auto& binding =
        static_cast<const BindingStmt&>(*module->statements[0]);
    ASSERT_EQ(binding.initializer->kind, Expr::Kind::Index);
    const auto& index = static_cast<const IndexExpr&>(*binding.initializer);
    EXPECT_EQ(index.array->kind, Expr::Kind::Call);
    EXPECT_EQ(index.index->kind, Expr::Kind::Integer);
}

TEST(SyntaxType, ArrayLengthAndElementTypeParticipateInEquality) {
    using namespace pagos::syntax;
    EXPECT_NE(Type::array(TypeKind::U32, 2), Type::array(TypeKind::U32, 3));
    EXPECT_NE(Type::array(TypeKind::Bool, 2), Type::array(TypeKind::U32, 2));
    EXPECT_EQ(type_name(Type::array(TypeKind::Bool, 2)), "[bool; 2]");
}

} // namespace
