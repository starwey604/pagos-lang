# Core Grammar Sketch

Status: normative syntax baseline for Milestones 0 and 1. Later milestones may
extend it, but should not silently change accepted programs.

## Scope

The first compiler slice supports `bool`, `u32`, immutable bindings, functions,
calls, `if` expressions, and explicit stage constraints. A range-based `for`
form is specified so loop staging has a concrete model, but implementation is
deferred until after Milestone 1.

Source files use UTF-8. Keywords and identifiers are case-sensitive. An
identifier starts with an ASCII letter or `_` and continues with ASCII letters,
digits, or `_`. Decimal integer literals may contain `_` separators. `//`
starts a line comment. The source extension is `.pgs`.

## Grammar

The grammar uses EBNF: `{ x }` repeats `x`, `[ x ]` makes `x` optional, and
`( x | y )` selects an alternative.

```ebnf
source          = { function | statement } EOF ;

function        = "fn" identifier "(" [ parameters ] ")"
                  "->" type block ;
parameters      = parameter { "," parameter } ;
parameter       = identifier ":" type ;
type            = "bool" | "u32" ;

block           = "{" { statement } [ expression ] "}" ;
statement       = binding ";"
                | "return" expression ";"
                | expression ";"
                | for-expression ;
binding         = [ "static" | "runtime" ] "let" identifier
                  [ ":" type ] "=" expression ;
for-expression  = "for" identifier "in" expression ".." expression block ;

expression      = if-expression | logical-or ;
if-expression   = "if" expression block "else"
                  ( block | if-expression ) ;
logical-or      = logical-and { "||" logical-and } ;
logical-and     = equality { "&&" equality } ;
equality        = comparison { ( "==" | "!=" ) comparison } ;
comparison      = additive { ( "<" | "<=" | ">" | ">=" ) additive } ;
additive        = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative  = unary { ( "*" | "/" | "%" ) unary } ;
unary           = "!" unary | call ;
call            = primary { "(" [ arguments ] ")" } ;
arguments       = expression { "," expression } ;
primary         = integer | "true" | "false" | identifier
                | "(" expression ")" ;
```

Top-level statements form an implicit entry unit in the research compiler.
Functions must declare parameter and result types. A block's optional final
expression is its value; an `if` used as a value requires `else`, and both arms
must have the same type. A function may instead finish through explicit
`return` statements.

## Core Type Rules

Arithmetic operators require `u32`. Ordering comparisons also require `u32`
and return `bool`; equality requires two operands of the same type. `!`, `&&`,
and `||` require `bool`. An `if` condition must be `bool`, and a range's bounds
must be `u32`. Initializers and return expressions must match their declared or
inferred type. There are no implicit conversions in the core language.

## Precedence and Evaluation Order

The productions above run from lowest to highest precedence. Binary operators
associate left-to-right. Function arguments and binary operands are evaluated
left-to-right. `&&` and `||` short-circuit. Pagos does not reorder observable
effects, although pure residual expressions may later be optimized under the
language's arithmetic rules.

## Reserved Decisions

Milestone 1 does not include user-defined operators, implicit numeric
conversions, overloading, mutable assignment, aggregates, pointers, or generic
parameters. New syntax for those features requires a documented semantic
decision and examples before parser implementation.
