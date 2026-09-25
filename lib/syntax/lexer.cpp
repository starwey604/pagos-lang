#include "pagos/syntax/lexer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace pagos::syntax {
namespace {

constexpr std::array keywords{
    std::pair<std::string_view, TokenKind>{"record", TokenKind::KwRecord},
    std::pair<std::string_view, TokenKind>{"fn", TokenKind::KwFn},
    std::pair<std::string_view, TokenKind>{"let", TokenKind::KwLet},
    std::pair<std::string_view, TokenKind>{"static", TokenKind::KwStatic},
    std::pair<std::string_view, TokenKind>{"runtime", TokenKind::KwRuntime},
    std::pair<std::string_view, TokenKind>{"return", TokenKind::KwReturn},
    std::pair<std::string_view, TokenKind>{"if", TokenKind::KwIf},
    std::pair<std::string_view, TokenKind>{"else", TokenKind::KwElse},
    std::pair<std::string_view, TokenKind>{"for", TokenKind::KwFor},
    std::pair<std::string_view, TokenKind>{"in", TokenKind::KwIn},
    std::pair<std::string_view, TokenKind>{"true", TokenKind::KwTrue},
    std::pair<std::string_view, TokenKind>{"false", TokenKind::KwFalse},
    std::pair<std::string_view, TokenKind>{"bool", TokenKind::KwBool},
    std::pair<std::string_view, TokenKind>{"u32", TokenKind::KwU32},
};

bool is_identifier_start(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalpha(value) != 0 || character == '_';
}

bool is_identifier_continue(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

} // namespace

bool Lexer::at_end() const noexcept { return offset_ >= source_.text().size(); }

char Lexer::peek(std::size_t lookahead) const noexcept {
    const auto index = offset_ + lookahead;
    return index < source_.text().size() ? source_.text()[index] : '\0';
}

char Lexer::advance() noexcept { return source_.text()[offset_++]; }

bool Lexer::match(char expected) noexcept {
    if (peek() != expected) {
        return false;
    }
    ++offset_;
    return true;
}

void Lexer::add(std::vector<Token>& tokens, TokenKind kind, std::size_t begin,
                std::size_t end) {
    tokens.push_back({.kind = kind,
                      .lexeme = source_.text().substr(begin, end - begin),
                      .span = {.begin = begin, .end = end}});
}

void Lexer::skip_ignored() {
    while (!at_end()) {
        const auto character = peek();
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n') {
            advance();
            continue;
        }
        if (character == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }
        break;
    }
}

void Lexer::lex_identifier(std::vector<Token>& tokens) {
    const auto begin = offset_;
    advance();
    while (is_identifier_continue(peek())) {
        advance();
    }
    const auto text = source_.text().substr(begin, offset_ - begin);
    const auto iterator =
        std::ranges::find_if(keywords, [text](const auto& keyword) {
            return keyword.first == text;
        });
    add(tokens,
        iterator == keywords.end() ? TokenKind::Identifier : iterator->second,
        begin, offset_);
}

void Lexer::lex_integer(std::vector<Token>& tokens) {
    const auto begin = offset_;
    advance();
    while (std::isdigit(static_cast<unsigned char>(peek())) != 0 ||
           peek() == '_') {
        advance();
    }
    add(tokens, TokenKind::Integer, begin, offset_);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (!at_end()) {
        skip_ignored();
        if (at_end()) {
            break;
        }

        const auto begin = offset_;
        const auto character = peek();
        if (is_identifier_start(character)) {
            lex_identifier(tokens);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
            lex_integer(tokens);
            continue;
        }

        advance();
        switch (character) {
        case '[':
            add(tokens, TokenKind::LeftBracket, begin, offset_);
            break;
        case ']':
            add(tokens, TokenKind::RightBracket, begin, offset_);
            break;
        case '(':
            add(tokens, TokenKind::LeftParen, begin, offset_);
            break;
        case ')':
            add(tokens, TokenKind::RightParen, begin, offset_);
            break;
        case '{':
            add(tokens, TokenKind::LeftBrace, begin, offset_);
            break;
        case '}':
            add(tokens, TokenKind::RightBrace, begin, offset_);
            break;
        case ':':
            add(tokens, TokenKind::Colon, begin, offset_);
            break;
        case ',':
            add(tokens, TokenKind::Comma, begin, offset_);
            break;
        case ';':
            add(tokens, TokenKind::Semicolon, begin, offset_);
            break;
        case '+':
            add(tokens, TokenKind::Plus, begin, offset_);
            break;
        case '-':
            add(tokens, match('>') ? TokenKind::Arrow : TokenKind::Minus, begin,
                offset_);
            break;
        case '*':
            add(tokens, TokenKind::Star, begin, offset_);
            break;
        case '/':
            add(tokens, TokenKind::Slash, begin, offset_);
            break;
        case '%':
            add(tokens, TokenKind::Percent, begin, offset_);
            break;
        case '~':
            add(tokens, TokenKind::Tilde, begin, offset_);
            break;
        case '^':
            add(tokens, TokenKind::Caret, begin, offset_);
            break;
        case '!':
            add(tokens, match('=') ? TokenKind::BangEqual : TokenKind::Bang,
                begin, offset_);
            break;
        case '=':
            add(tokens, match('=') ? TokenKind::EqualEqual : TokenKind::Equal,
                begin, offset_);
            break;
        case '<':
            add(tokens,
                match('<')   ? TokenKind::ShiftLeft
                : match('=') ? TokenKind::LessEqual
                             : TokenKind::Less,
                begin, offset_);
            break;
        case '>':
            add(tokens,
                match('>')   ? TokenKind::ShiftRight
                : match('=') ? TokenKind::GreaterEqual
                             : TokenKind::Greater,
                begin, offset_);
            break;
        case '&':
            add(tokens, match('&') ? TokenKind::AndAnd : TokenKind::Ampersand,
                begin, offset_);
            break;
        case '|':
            add(tokens, match('|') ? TokenKind::OrOr : TokenKind::Pipe, begin,
                offset_);
            break;
        case '.':
            if (match('.')) {
                add(tokens, TokenKind::Range, begin, offset_);
            } else {
                add(tokens, TokenKind::Dot, begin, offset_);
            }
            break;
        default:
            diagnostics_.error("E1001", "unexpected character",
                               {.begin = begin, .end = offset_});
            break;
        }
    }

    add(tokens, TokenKind::End, offset_, offset_);
    return tokens;
}

} // namespace pagos::syntax
