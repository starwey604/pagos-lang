#pragma once

#include "pagos/source/span.h"

#include <string_view>

namespace pagos::syntax {

enum class TokenKind {
    End,
    Identifier,
    Integer,
    KwFn,
    KwRecord,
    KwLet,
    KwStatic,
    KwRuntime,
    KwReturn,
    KwIf,
    KwElse,
    KwFor,
    KwIn,
    KwTrue,
    KwFalse,
    KwBool,
    KwU32,
    KwU8,
    KwU16,
    KwU64,
    KwAs,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Colon,
    Comma,
    Semicolon,
    Arrow,
    Range,
    Dot,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Tilde,
    Ampersand,
    Pipe,
    Caret,
    ShiftLeft,
    ShiftRight,
    Bang,
    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::string_view lexeme;
    source::Span span;
};

[[nodiscard]] std::string_view token_kind_name(TokenKind kind) noexcept;

} // namespace pagos::syntax
