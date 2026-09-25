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

TEST(Parser, PreservesCompactArrayGeneratorSyntax) {
    using namespace pagos::syntax;
    const auto source = pagos::source::SourceFile::from_text(
        "generator.pgs", "let table = [for i in 1_0..1_000 { i * i }];");
    pagos::source::DiagnosticEngine diagnostics(source);
    Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    Parser parser(tokens, diagnostics);
    const auto module = parser.parse_module();
    ASSERT_FALSE(diagnostics.has_error());
    const auto& binding =
        static_cast<const BindingStmt&>(*module->statements.at(0));
    ASSERT_EQ(binding.initializer->kind, Expr::Kind::ArrayGenerator);
    const auto& generator =
        static_cast<const ArrayGeneratorExpr&>(*binding.initializer);
    EXPECT_EQ(generator.begin, 10U);
    EXPECT_EQ(generator.end, 1000U);
    EXPECT_EQ(generator.variable, "i");
    EXPECT_TRUE(generator.body->statements.empty());
    ASSERT_NE(generator.body->tail, nullptr);
    EXPECT_EQ(generator.body->tail->kind, Expr::Kind::Binary);
}

TEST(SyntaxType, ArrayLengthAndElementTypeParticipateInEquality) {
    using namespace pagos::syntax;
    EXPECT_NE(Type::array(TypeKind::U32, 2), Type::array(TypeKind::U32, 3));
    EXPECT_NE(Type::array(TypeKind::Bool, 2), Type::array(TypeKind::U32, 2));
    EXPECT_EQ(type_name(Type::array(TypeKind::Bool, 2)), "[bool; 2]");
}

TEST(Parser, EqualityBindsMoreTightlyThanBitwiseAnd) {
    using namespace pagos::syntax;
    const auto source = pagos::source::SourceFile::from_text(
        "bits.pgs", "let value = 1 & 2 == 3;");
    pagos::source::DiagnosticEngine diagnostics(source);
    Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    Parser parser(tokens, diagnostics);
    const auto module = parser.parse_module();
    ASSERT_FALSE(diagnostics.has_error());
    const auto& binding =
        static_cast<const BindingStmt&>(*module->statements.at(0));
    ASSERT_EQ(binding.initializer->kind, Expr::Kind::Binary);
    const auto& bit = static_cast<const BinaryExpr&>(*binding.initializer);
    EXPECT_EQ(bit.operation, BinaryOperator::BitAnd);
    ASSERT_EQ(bit.right->kind, Expr::Kind::Binary);
    EXPECT_EQ(static_cast<const BinaryExpr&>(*bit.right).operation,
              BinaryOperator::Equal);
}

} // namespace
