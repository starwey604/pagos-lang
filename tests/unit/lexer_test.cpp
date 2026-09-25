#include "pagos/source/diagnostic.h"
#include "pagos/source/source_manager.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/token.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

TEST(Lexer, RecognizesCoreBinding) {
    const auto source = pagos::source::SourceFile::from_text(
        "test.pgs", "runtime let input: u32 = external_input();\n");
    pagos::source::DiagnosticEngine diagnostics(source);
    pagos::syntax::Lexer lexer(source, diagnostics);

    const auto tokens = lexer.tokenize();

    EXPECT_FALSE(diagnostics.has_error());
    const std::vector expected{
        pagos::syntax::TokenKind::KwRuntime,
        pagos::syntax::TokenKind::KwLet,
        pagos::syntax::TokenKind::Identifier,
        pagos::syntax::TokenKind::Colon,
        pagos::syntax::TokenKind::KwU32,
        pagos::syntax::TokenKind::Equal,
        pagos::syntax::TokenKind::Identifier,
        pagos::syntax::TokenKind::LeftParen,
        pagos::syntax::TokenKind::RightParen,
        pagos::syntax::TokenKind::Semicolon,
        pagos::syntax::TokenKind::End,
    };
    ASSERT_EQ(tokens.size(), expected.size());
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        EXPECT_EQ(tokens[index].kind, expected[index]);
    }
}

TEST(Lexer, IgnoresLineCommentsAndTracksSpans) {
    const auto source = pagos::source::SourceFile::from_text(
        "test.pgs", "// staging root\nlet value = 4_294;\n");
    pagos::source::DiagnosticEngine diagnostics(source);
    pagos::syntax::Lexer lexer(source, diagnostics);

    const auto tokens = lexer.tokenize();

    ASSERT_FALSE(diagnostics.has_error());
    ASSERT_GE(tokens.size(), 4U);
    EXPECT_EQ(tokens[0].lexeme, "let");
    EXPECT_EQ(tokens[3].lexeme, "4_294");
    EXPECT_EQ(source.position(tokens[0].span.begin).line, 2U);
}

TEST(Lexer, DistinguishesBitwiseLogicalAndComparisonTokens) {
    using enum pagos::syntax::TokenKind;
    const auto source = pagos::source::SourceFile::from_text(
        "bits.pgs", "& && | || ^ ~ << < <= >> > >= ! !=");
    pagos::source::DiagnosticEngine diagnostics(source);
    pagos::syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    const std::vector expected{Ampersand, AndAnd,     Pipe,      OrOr,
                               Caret,     Tilde,      ShiftLeft, Less,
                               LessEqual, ShiftRight, Greater,   GreaterEqual,
                               Bang,      BangEqual,  End};
    ASSERT_FALSE(diagnostics.has_error());
    ASSERT_EQ(tokens.size(), expected.size());
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        EXPECT_EQ(tokens[index].kind, expected[index]);
        EXPECT_NE(pagos::syntax::token_kind_name(tokens[index].kind), "token");
    }
}

} // namespace
