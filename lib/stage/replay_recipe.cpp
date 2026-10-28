#include "replay_recipe.h"

#include <algorithm>

namespace pagos::stage {
namespace {
using Op = ScalarReplayOp;
[[noreturn]] void unsupported() {
    throw ReplayError{ReplayErrorCode::UnsupportedOperation, {}, {}};
}
Op unary(syntax::UnaryOperator operation) {
    switch (operation) {
    case syntax::UnaryOperator::Not:
        return Op::Not;
    case syntax::UnaryOperator::BitNot:
        return Op::BitNot;
    case syntax::UnaryOperator::Negate:
        return Op::Negate;
    default:
        unsupported();
    }
}
Op binary(syntax::BinaryOperator operation) {
    using enum syntax::BinaryOperator;
    switch (operation) {
    case Add:
        return Op::Add;
    case Subtract:
        return Op::Subtract;
    case Multiply:
        return Op::Multiply;
    case Divide:
        return Op::Divide;
    case Remainder:
        return Op::Remainder;
    case BitAnd:
        return Op::BitAnd;
    case BitOr:
        return Op::BitOr;
    case BitXor:
        return Op::BitXor;
    case ShiftLeft:
        return Op::ShiftLeft;
    case ShiftRight:
        return Op::ShiftRight;
    case Equal:
        return Op::Equal;
    case NotEqual:
        return Op::NotEqual;
    case Less:
        return Op::Less;
    case LessEqual:
        return Op::LessEqual;
    case Greater:
        return Op::Greater;
    case GreaterEqual:
        return Op::GreaterEqual;
    default:
        unsupported();
    }
}
struct Builder {
    const syntax::Function& function;
    const sema::TypeTable& types;
    const CacheIdentity& identity;
    const cache::QueryKey& key;
    std::optional<std::string_view> iterator;
    ScalarReplayRecipe output;
    cache::Limits limits;
    SourceAnchor anchor(source::Span span) const {
        return replay_detail::source_anchor(identity, key, span);
    }
    std::uint32_t visit(const syntax::Expr& expression, std::size_t depth) {
        if (depth >= limits.depth || output.nodes.size() >= limits.elements ||
            output.nodes.size() >= UINT32_MAX)
            throw ReplayError{ReplayErrorCode::LimitExceeded, {}, {}};
        const auto found = types.expressions.find(&expression);
        if (found == types.expressions.end())
            throw ReplayError{ReplayErrorCode::UnsupportedType, {}, {}};
        ScalarReplayNode node{.operation = Op::Constant,
                              .type = found->second,
                              .position = anchor(expression.span).position};
        using enum syntax::Expr::Kind;
        switch (expression.kind) {
        case Integer: {
            const auto value = IntegerValue::parse_decimal(
                node.type.integer_type,
                static_cast<const syntax::IntegerExpr&>(expression).spelling);
            if (!value)
                throw ReplayError{ReplayErrorCode::InvalidRecipe, {}, {}};
            node.constant = *value;
            break;
        }
        case Boolean:
            node.constant =
                static_cast<const syntax::BooleanExpr&>(expression).value;
            break;
        case Name: {
            if (types.global_references.contains(&expression))
                unsupported();
            const auto& name =
                static_cast<const syntax::NameExpr&>(expression).name;
            if (iterator && name == *iterator) {
                node.operation = Op::Input;
                node.input = static_cast<std::uint32_t>(key.arguments.size());
                break;
            }
            const auto parameter = std::ranges::find(
                function.parameters,
                static_cast<const syntax::NameExpr&>(expression).name,
                &syntax::Parameter::name);
            if (parameter == function.parameters.end())
                unsupported();
            node.operation = Op::Input;
            node.input = static_cast<std::uint32_t>(
                parameter - function.parameters.begin());
            break;
        }
        case Unary: {
            const auto& value =
                static_cast<const syntax::UnaryExpr&>(expression);
            node.operation = unary(value.operation);
            node.operands = {visit(*value.operand, depth + 1)};
            break;
        }
        case Binary: {
            const auto& value =
                static_cast<const syntax::BinaryExpr&>(expression);
            node.operation = binary(value.operation);
            node.operands = {visit(*value.left, depth + 1),
                             visit(*value.right, depth + 1)};
            break;
        }
        default:
            unsupported();
        }
        if (output.nodes.size() >= limits.elements ||
            output.nodes.size() >= UINT32_MAX)
            throw ReplayError{ReplayErrorCode::LimitExceeded, {}, {}};
        const auto id = static_cast<std::uint32_t>(output.nodes.size());
        output.nodes.push_back(std::move(node));
        return id;
    }
};
} // namespace
namespace replay_detail {
SourceAnchor source_anchor(const CacheIdentity& identity,
                           const cache::QueryKey& key, source::Span span) {
    for (const auto& site : identity.sites)
        if (site.anchor.definition == key.definition &&
            site.anchor.definition_fingerprint == key.definition_fingerprint &&
            site.span.begin == span.begin && site.span.end == span.end)
            return site.anchor;
    throw ReplayError{ReplayErrorCode::InvalidAnchor, {}, {}};
}
ScalarReplayRecipe
build_recipe(const syntax::Expr& expression, const syntax::Function& function,
             const sema::TypeTable& types, const CacheIdentity& identity,
             const cache::QueryKey& key,
             std::optional<std::string_view> iterator, cache::Limits limits) {
    if (key.arguments.size() >= UINT32_MAX)
        throw ReplayError{ReplayErrorCode::LimitExceeded, {}, {}};
    Builder builder{function, types, identity, key, iterator, {}, limits};
    builder.output.result_site = builder.anchor(expression.span);
    const auto result_type = types.expressions.find(&expression);
    if (result_type == types.expressions.end())
        throw ReplayError{ReplayErrorCode::UnsupportedType, {}, {}};
    builder.output.result_type = result_type->second;
    (void)builder.visit(expression, 0);
    return std::move(builder.output);
}
} // namespace replay_detail
} // namespace pagos::stage
