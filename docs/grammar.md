# Core Grammar Sketch

Status: normative syntax baseline, including the Milestone 2 loop and array
slices. Later milestones should not silently change accepted programs.

## Scope

The compiler supports `bool`, `u32`, fixed-length scalar arrays, immutable
bindings, functions, calls, `if` expressions, explicit stage constraints, and
range-based `for` statements.
Static ranges execute during analysis; Runtime ranges residualize as target
control flow. A `return` inside an expression block or loop exits the nearest
enclosing function, including when the compiler inlines that function.

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
scalar-type     = "bool" | "u32" ;
type            = scalar-type | "[" scalar-type ";" integer "]" ;

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
logical-and     = bitwise-or { "&&" bitwise-or } ;
bitwise-or      = bitwise-xor { "|" bitwise-xor } ;
bitwise-xor     = bitwise-and { "^" bitwise-and } ;
bitwise-and     = equality { "&" equality } ;
equality        = comparison { ( "==" | "!=" ) comparison } ;
comparison      = shift { ( "<" | "<=" | ">" | ">=" ) shift } ;
shift           = additive { ( "<<" | ">>" ) additive } ;
additive        = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative  = unary { ( "*" | "/" | "%" ) unary } ;
unary           = ( "!" | "~" ) unary | call ;
call            = primary { "(" [ arguments ] ")" | "[" expression "]" } ;
arguments       = expression { "," expression } ;
primary         = integer | "true" | "false" | identifier
                | "(" expression ")" | "[" arguments [ "," ] "]"
                | "[" "for" identifier "in" integer ".." integer block "]" ;
```

Top-level statements form an implicit entry unit in the research compiler.
Functions must declare parameter and result types. A block's optional final
expression is its value; an `if` used as a value requires `else`, and both arms
must have the same type. A function may instead finish through explicit
`return` statements.

A returning branch does not produce a value for an enclosing expression;
only branches that continue must agree on a value type. Internally this is
represented by `never`, which is not a source-level type. Result-completeness
checking treats range loops conservatively as possibly empty: functions with
returns only inside a loop must also provide a fallthrough result. Both
branches of a terminating `if` can instead return directly.

## Core Type Rules

Arithmetic operators require `u32`. Ordering comparisons also require `u32`
and return `bool`; equality requires two scalar operands of the same type.
`!`, `&&`, and `||` require `bool`. An `if` condition must be `bool`, and a
range's bounds must be `u32`. Initializers and return expressions must match
their declared or inferred type. There are no implicit conversions in the
core language.

`~`, `&`, `|`, `^`, `<<`, and `>>` require `u32` operands and return `u32`.
Bitwise binary operators are strict, not short-circuiting. Right shift fills
with zeros; left shift discards high bits. Shift counts must be below 32:
an analyzed Static count outside this range reports `E4009`, even with a Runtime
left operand; a Runtime count is checked before shifting and traps if invalid.
Unselected Static branches and skipped short-circuit operands are not evaluated.

## Fixed-Length Arrays (Milestone 2)

Arrays are immutable, nonempty, homogeneous one-dimensional values. Their
type includes the scalar element type and a positive `u32` literal length:
`[u32; 3]` or `[bool; 2]`. Literals infer their length and element type;
lengths in annotations, parameters, returns, and branch joins must match.
Empty/nested arrays, array equality, repetition syntax, and element assignment
are not part of this slice.

```pagos
fn square(x: u32) -> u32 { x * x }
fn lookup(table: [u32; 3], index: u32) -> u32 { table[index] }
static let squares: [u32; 3] = [square(0), square(1), square(2)];
static let folded = squares[2];
let selected = lookup(squares, external_input());
```

Elements evaluate left-to-right, then indexing evaluates the array before its
`u32` index. A constructed array is Static only when every element is Static,
but a Static index into a mixed array inherits the selected element's stage.
Aliases, call parameters, and directly returned arrays preserve that precision.
Selecting a Static element folds its value while preserving evaluation of the
entire array and the index, including unused reads, traps, and early returns.
An explicit `runtime let` array boundary, Runtime branch/call-result selection,
or Runtime index keeps the read Runtime, even when values happen to agree.
An analyzed Static index outside the type's length reports `E4007`; a Runtime
index is checked before memory access and traps if out of bounds.
Static tables used by residual indexing are embedded as deduplicated read-only
LLVM constants. Runtime arrays remain ordinary immutable value aggregates.
The implicit entry returns zero when its final value is an array.

### Bulk array generation

`[for i in 0..256 { i * i }]` constructs a 256-element array in ascending
index order. Both bounds must be `u32` literals (separators are allowed), and
the end must exceed the start. Its type has length `end - start`; each body
must produce `u32` or `bool`. The immutable index is Static and scoped to each
iteration. A body may call functions, bind locals, or return from the enclosing
function. A definite return stops generation; Runtime returns remain residual.

```pagos
fn square(i: u32) -> u32 { i * i }
static let table: [u32; 256] = [for i in 0..256 { square(i) }];
let result = table[external_input()];
```

The body follows ordinary staging and evaluation-order rules. All-Static
elements become an embedded table; mixed elements preserve per-element stages
and residual effects. Runtime or computed bounds, empty ranges, nested array
elements, and mutation are not supported. Both Runtime branches are analyzed
and budgeted; unselected Static branches do not generate or consume budget.

Array construction has cumulative per-analysis quotas, charged before reserve
or body evaluation: `--max-array-elements` (default 65,536 slots) and
`--max-array-bytes` (default 262,144 logical data bytes). Each planned element
costs one slot and four bytes for `u32` or one for `bool`. Non-continuing arrays
whose element type is `never` conservatively reserve four bytes per slot.
Reservations are not refunded on early return. Literals and Runtime arrays
share the quotas; aliases and specialization-cache hits do not reconstruct
arrays and incur no new reservation. Each generated iteration also costs fuel,
in addition to ordinary body evaluation. Exceeding a quota reports `E4008`.

These are construction-work/data budgets, not a bound on process memory: AST,
HIR nodes, constant copies, cache keys, and LLVM allocations are not counted.
Full host-memory accounting remains future work.

## Milestone 1 Runtime Intrinsic

`external_input()` is a temporary, zero-argument runtime-source intrinsic with
result type `u32`. It exists to make the first residual program executable and
explainable before effectful target APIs and foreign declarations are designed.
User code cannot redefine it.

## Precedence and Evaluation Order

The productions above run from lowest to highest precedence. Binary operators
associate left-to-right. Function arguments and binary operands are evaluated
left-to-right, as do array elements. `&&` and `||` short-circuit. Pagos does not
reorder observable effects, although pure residual expressions may later be
optimized under the language's arithmetic rules.

Bitwise precedence follows C: from low to high, `||`, `&&`, `|`, `^`, `&`,
equality, ordering, shifts, addition/subtraction, multiplication/division,
unary operators, and calls/indexing. Write `(value & 1) != 0` for a bit test;
`value & 1 != 0` instead attempts to combine `u32` and `bool` and is rejected.

## Reserved Decisions

Milestone 1 does not include user-defined operators, implicit numeric
conversions, overloading, mutable assignment, records, pointers, or generic
parameters. New syntax for those features requires a documented semantic
decision and examples before parser implementation.
