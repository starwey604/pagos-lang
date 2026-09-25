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
source          = { record | function | statement } EOF ;
record          = "record" identifier "{" record-fields [ "," ] "}" ;
record-fields   = identifier ":" scalar-type
                  { "," identifier ":" scalar-type } ;

function        = "fn" identifier "(" [ parameters ] ")"
                  "->" type block ;
parameters      = parameter { "," parameter } ;
parameter       = identifier ":" type ;
scalar-type     = "bool" | "u32" ;
type            = scalar-type | identifier | "[" scalar-type ";" integer "]" ;

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
call            = primary { "(" [ arguments ] ")" | "[" expression "]"
                          | "." identifier } ;
arguments       = expression { "," expression } ;
primary         = integer | "true" | "false" | identifier
                | identifier "(" named-fields [ "," ] ")"
                | "(" expression ")" | "[" arguments [ "," ] "]"
                | "[" "for" identifier "in" expression ".." expression block "]" ;
named-fields    = identifier ":" expression
                  { "," identifier ":" expression } ;
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

## Minimal Records (Milestone 2)

`record Config { clock_hz: u32, ready: bool }` declares a nonempty immutable
nominal type. `Config(ready: true, clock_hz: 80_000_000)` constructs it, and
`config.clock_hz` selects a field. Every field must be supplied exactly once;
initializers evaluate in source order, independently of declaration order.
Declarations are module-level and may be referenced before their declaration.
Record and function names may not collide. Type names have a separate namespace
from local bindings. Positional record construction is not supported.

Fields are `u32` or `bool` only. Nested records, array fields, arrays of records,
empty records, methods, mutation, structural equality, and generics are deferred.
Two differently named records remain distinct even with identical fields.
Function parameters/results, annotations, and continuing `if` arms must agree
on the nominal type. An aggregate-valued implicit entry returns zero.

A record is Static when every field is Static. Field selection preserves
individual stages through aliases, call parameters, and direct returns, but
must retain evaluation of every initializer, including reads, traps, and returns.
Explicit Runtime record boundaries and Runtime branch/call-result selection
keep all projections Runtime. Diagnostics identify the selected field's source.
Records can cross into residual MIR/LLVM as value aggregates without exposing
host addresses; no stable layout, packing, memory size, or C ABI is promised.
Existing fuel/depth/specialization limits apply. Shared aggregate construction
quotas charge every declared field before member allocation or initializer
evaluation; array-only quotas still exclude records. See
[aggregate construction budgets](#aggregate-construction-budgets).

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
index order. Both bounds must evaluate to Static `u32` values, and the end
must exceed the start. Its type has length `end - start`; each body
must produce `u32` or `bool`. The immutable index is Static and scoped to each
iteration. A body may call functions, bind locals, or return from the enclosing
function. A definite return stops generation; Runtime returns remain residual.

```pagos
fn square(i: u32) -> u32 { i * i }
static let bits = 8;
static let table: [u32; 256] = [for i in 0..1 << bits { square(i) }];
let result = table[external_input()];
```

Bounds evaluate once, start then end, in the enclosing scope before the index
binding exists. Static names, arithmetic, function calls, array indexing, and
record fields are accepted. The two values determine the length before the
element vector is allocated or the body runs. Each function specialization
resolves its own length. Bounds use ordinary `u32` arithmetic, including wrap;
an analyzed empty or reversed range reports `E1005`, without unsigned subtraction.
An analyzed Runtime bound reports `E2002` with its dependency path.

Bound evaluation may retain Runtime effects while producing a Static value.
Those effects precede body effects, including on cache hits. A definite return
in a bound exits the enclosing function without reserving the outer array;
a conditional Runtime return remains residual. Bound expressions consume normal
fuel and any constructors they evaluate consume their own aggregate quotas.

Source type annotations and function signatures still require literal lengths;
this is not dependent typing or dynamic allocation. Length constraints involving
computed generators are deferred until their expressions are analyzed: binding
annotations, call arguments, explicit returns, and function tails must agree
with the resolved shape (`E1008`). Runtime branch continuations must agree too.
A Static branch resolves only its selected arm; lengths in skipped arms and
uncalled function bodies are not evaluated. Ordinary name and element-type
checking still covers those bodies, and known literal shape mismatches remain
type errors. No unresolved length reaches residual HIR/MIR values.

The body follows ordinary staging and evaluation-order rules. All-Static
elements become an embedded table; mixed elements preserve per-element stages
and residual effects. Runtime bounds, empty ranges, nested array
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

## Aggregate Construction Budgets

All array and record constructors share `--max-aggregate-members` (default
65,536) and `--max-aggregate-bytes` (default 262,144). An array element or record
field costs one member; `u32` costs four logical bytes and `bool` one, without
target padding. Arrays whose continuation type is `never` reserve four bytes
per element. A record uses its declared scalar field types even when an
initializer returns early. Shared quota exhaustion reports `E4010`.

Reservations are cumulative per analysis and precede member-vector allocation
and initializer/body evaluation. Failed reservations change no counters and
only the first construction-budget failure is reported. Successful reservations
are not refunded on early return, discarded results, or later failure.
Unselected Static branches do not construct; both Runtime branches are charged.
Static loops/generators charge each analyzed construction, while Runtime loop
bodies charge during analysis, not per target iteration. Aliases/projections
and cached results incur no reconstruction charge, but freshly constructed call
arguments are charged even when the call hits a cache entry.

Arrays must satisfy both the array-only and shared quotas; the array-only check
runs first (`E4008` takes precedence when both would fail). Raising an old
array limit alone does not raise the new shared limit. Array counters are a
subset of shared counters, not an additional charge. Statistics report reserved
construction count, members, and logical bytes. AST/HIR overhead, record schemas,
constant/cache copies, target layout, and LLVM allocations remain outside this
accounting; it is not a process-memory cap or target Runtime allocation budget.

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
conversions, overloading, mutable assignment, pointers, or generic
parameters. New syntax for those features requires a documented semantic
decision and examples before parser implementation.
