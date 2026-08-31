#pragma once

#include "pagos/source/diagnostic.h"
#include "pagos/syntax/token.h"

#include <vector>

namespace pagos::syntax {

class Lexer {
  public:
    Lexer(const source::SourceFile& source,
          source::DiagnosticEngine& diagnostics)
        : source_(source), diagnostics_(diagnostics) {}

    [[nodiscard]] std::vector<Token> tokenize();

  private:
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;
    char advance() noexcept;
    bool match(char expected) noexcept;
    void skip_ignored();
    void lex_identifier(std::vector<Token>& tokens);
    void lex_integer(std::vector<Token>& tokens);
    void add(std::vector<Token>& tokens, TokenKind kind, std::size_t begin,
             std::size_t end);

    const source::SourceFile& source_;
    source::DiagnosticEngine& diagnostics_;
    std::size_t offset_{};
};

} // namespace pagos::syntax
