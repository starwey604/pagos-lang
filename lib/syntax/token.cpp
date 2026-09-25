#include "pagos/syntax/token.h"

namespace pagos::syntax {

std::string_view token_kind_name(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::LeftBracket:
        return "`[`";
    case TokenKind::RightBracket:
        return "`]`";
    case TokenKind::End:
        return "end of file";
    case TokenKind::Identifier:
        return "identifier";
    case TokenKind::Integer:
        return "integer";
    case TokenKind::KwFn:
        return "`fn`";
    case TokenKind::KwLet:
        return "`let`";
    case TokenKind::KwStatic:
        return "`static`";
    case TokenKind::KwRuntime:
        return "`runtime`";
    case TokenKind::KwReturn:
        return "`return`";
    case TokenKind::KwIf:
        return "`if`";
    case TokenKind::KwElse:
        return "`else`";
    case TokenKind::KwFor:
        return "`for`";
    case TokenKind::KwIn:
        return "`in`";
    case TokenKind::KwTrue:
        return "`true`";
    case TokenKind::KwFalse:
        return "`false`";
    case TokenKind::KwBool:
        return "`bool`";
    case TokenKind::KwU32:
        return "`u32`";
    case TokenKind::LeftParen:
        return "`(`";
    case TokenKind::RightParen:
        return "`)`";
    case TokenKind::LeftBrace:
        return "`{`";
    case TokenKind::RightBrace:
        return "`}`";
    case TokenKind::Colon:
        return "`:`";
    case TokenKind::Comma:
        return "`,`";
    case TokenKind::Semicolon:
        return "`;`";
    case TokenKind::Arrow:
        return "`->`";
    case TokenKind::Range:
        return "`..`";
    case TokenKind::Plus:
        return "`+`";
    case TokenKind::Minus:
        return "`-`";
    case TokenKind::Star:
        return "`*`";
    case TokenKind::Slash:
        return "`/`";
    case TokenKind::Percent:
        return "`%`";
    case TokenKind::Bang:
        return "`!`";
    case TokenKind::Equal:
        return "`=`";
    case TokenKind::EqualEqual:
        return "`==`";
    case TokenKind::BangEqual:
        return "`!=`";
    case TokenKind::Less:
        return "`<`";
    case TokenKind::LessEqual:
        return "`<=`";
    case TokenKind::Greater:
        return "`>`";
    case TokenKind::GreaterEqual:
        return "`>=`";
    case TokenKind::AndAnd:
        return "`&&`";
    case TokenKind::OrOr:
        return "`||`";
    }
    return "token";
}

} // namespace pagos::syntax
