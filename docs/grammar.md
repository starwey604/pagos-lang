# Core Grammar Sketch

Status: normative syntax baseline, including M3 integers, raw pointers and C ABI.
Later milestones should not silently change accepted programs.

## Scope

The compiler supports `bool`, unsigned `u8/u16/u32/u64`, signed `i8/i16/i32/i64`,
target-sized unsigned `usize`, fixed-length scalar arrays, raw integer pointers,
immutable bindings, functions, calls, `if` expressions, explicit stage constraints,
and range-based `for` statements.
Static ranges execute during analysis; Runtime ranges residualize as target
control flow. A `return` inside an expression block or loop exits the nearest
enclosing function, including when the compiler inlines that function.

Source files use UTF-8. Keywords and identifiers are case-sensitive. An
identifier starts with an ASCII letter or `_` and continues with ASCII letters,
digits, or `_`. Decimal integer literals may contain `_` separators and an
optional integer-type suffix, for example `65_535u16` or `127i8`. `//`
starts a line comment. The source extension is `.pgs`.

## Grammar

The grammar uses EBNF: `{ x }` repeats `x`, `[ x ]` makes `x` optional, and
`( x | y )` selects an alternative.

```ebnf
source          = { record | function | statement } EOF ;
record          = "record" identifier "{" record-fields [ "," ] "}" ;
record-fields   = identifier ":" scalar-type
                  { "," identifier ":" scalar-type } ;

function        = [ "export" ] "fn" identifier "(" [ parameters ] ")"
                  "->" type block
                | "extern" "fn" identifier "(" [ parameters ] ")"
                  "->" type ";" ;
parameters      = parameter { "," parameter } ;
parameter       = identifier ":" type ;
integer-type    = "u8" | "u16" | "u32" | "u64"
                | "i8" | "i16" | "i32" | "i64" | "usize" ;
scalar-type     = "bool" | integer-type ;
pointer-type    = "*" ( "const" | "mut" ) integer-type ;
type            = scalar-type | pointer-type | identifier
                | "[" scalar-type ";" integer "]" ;

block           = "{" { statement } [ expression ] "}" ;
statement       = binding ";"
                | "return" expression ";"
                | expression ";"
                | expression "=" expression ";"  (* left side must be *pointer *)
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
multiplicative  = cast { ( "*" | "/" | "%" ) cast } ;
cast            = unary { "as" ( integer-type | pointer-type ) } ;
unary           = ( "!" | "~" | "-" | "*" ) unary | call ;
call            = primary { "(" [ arguments ] ")" | "[" expression "]"
                          | "." identifier } ;
arguments       = expression { "," expression } ;
primary         = integer [ integer-type ] | "true" | "false" | identifier
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

Arithmetic and ordering operators require two integers of the same type.
Ordering returns `bool`; equality requires two scalar operands of the same type.
`!`, `&&`, and `||` require `bool`. An `if` condition must be `bool`, and a
range's bounds must be `u32`. Initializers and return expressions must match
their declared or inferred type. There are no implicit conversions in the
core language.

Ordinary scalar calls with a Runtime scalar result lower to internal residual
functions, including direct and mutual Runtime recursion. Static calls still
evaluate during analysis; aggregate calls retain the inline path and aggregate
recursion remains unsupported. Internal signatures are not a foreign ABI.

Unsuffixed literals default to `u32`; annotations do not retarget literals in
this slice. Use `let byte: u8 = 255u8`, not `let byte: u8 = 255`. A suffixed
literal must fit its type; `256u8` is an error, not a truncation. The suffix
must be adjacent to the digits. Source array lengths remain unsuffixed literals.

`usize` is an unsigned integer with the selected target's pointer width, taken
from LLVM DataLayout (32 bits on RV32, 64 on RV64), not the compiler host.
Write `let size: usize = 1usize` or `external_input() as usize`. It is distinct
from `u32` and `u64` even when their widths match; mixing them requires `as`.
All integer operations and casts use the resolved width at both stages.
For example, `4294967295usize + 1usize` is zero on RV32 and 4294967296 on RV64;
`4294967296usize` itself is rejected on RV32. `usize` works in scalar arrays,
records, function signatures, and shift counts. Array lengths, array indices,
and range bounds/indices remain `u32`; `isize` is not yet supported.

Unary `-` requires a signed integer; `-1` and `-1u8` are errors, while `-1i8`
is valid. A minus directly enclosing a positive signed literal (parentheses
are transparent) is checked as one negative literal: `-128i8` and `-(128i8)`
fit, but positive `128i8`, `-129i8`, and `-(128i8 as i16)` do not.
A further minus is an ordinary wrapping operation; `--128i8` is still -128.
Signed integers use two's complement. Addition, subtraction, multiplication,
and negation wrap modulo 2^width. Signed division truncates toward zero;
remainder is zero or has the dividend's sign. Both `MIN / -1` and `MIN % -1`
report `E4011` when evaluated Static, and trap at Runtime, as zero divisors do.

`value as u8` explicitly retains the low eight bits. Widening sign-extends a
signed source and zero-extends an unsigned source, independently of the
destination's signedness. Equal-width casts preserve bits, so `255u8 as i8`
is -1, `-1i8 as u64` is 18446744073709551615, and `255u8 as i16` is 255.
Same-type casts are allowed. Casts evaluate their operand exactly once,
preserve its stage and effects, bind below unary operators
and above multiplication, and associate left-to-right. Booleans and aggregates
cannot be cast; pointer conversions follow the separate rules below.

`~`, `&`, `|`, `^`, `<<`, and `>>` accept integers and preserve their
type. Binary operands, including shift counts, must have the same type.
Bitwise binary operators are strict, not short-circuiting. Right shift is
arithmetic for signed integers and logical for unsigned integers; left shift
discards high bits. Shift counts must be non-negative and below the width:
an analyzed Static count outside this range reports `E4009`, even with a Runtime
left operand; a Runtime count is checked before shifting and traps if invalid.
Unselected Static branches and skipped short-circuit operands are not evaluated.

## Minimal C ABI (Milestone 3)

```pagos
extern fn platform_read() -> u32;
export fn sample(offset: u32) -> u32 { platform_read() + offset }
```

`extern fn` declares a C symbol without a body; `export fn` defines one using
its exact source name. Both require explicit parameter/result types: `u32` and
`i32` (C `uint32_t` and `int32_t`), or pointers to the supported integer types.
`*const T` / `*mut T` map to `const T*` / `T*` with the corresponding C integer
type; a `usize` pointee uses `uintptr_t`. Zero parameters are allowed;
`void`, `bool`, other by-value widths, by-value `usize`, aggregates, varargs, and
overloads are not. Declarations cannot be repeated or redeclared as definitions
in the same module. `pagos_main` and `pagos_external_input` are reserved names.

Every export is analyzed/emitted, even if uncalled, with all parameters Runtime.
Calls to either boundary are always Runtime and conservatively effectful,
including calls to a constant-returning export with Static arguments. Normal
helpers inside an export still specialize. Foreign code never runs during
compilation. Argument order, checked traps, and early returns remain unchanged.

The validated ABI subset is x86-64 Linux LP64 and generic RV32 ELF soft-float
ILP32 (I with optional M/A/C). Other C-boundary target configurations are
rejected, rather than inferred from storage layout. The caller must supply
matching C prototypes and target options; no headers or linker checks for
cross-language type mismatches are generated. Unwinding across the boundary,
C++ exceptions, and `longjmp` through Pagos frames are outside this contract.

A module with any `extern`/`export` and no top-level statements emits only its
boundary symbols and reachable helpers, not synthetic `pagos_main`. Top-level
statements otherwise retain the explicit test entry, not an automatically run
module initializer. Separate object files can import each other's exports.

## Raw Pointers and Memory (Milestone 3)

```pagos
export fn increment(p: *mut u32) -> u32 {
    let previous = *p;
    *p = previous + 1;
    *p
}
```

Pointers are copyable, non-owning target addresses. `*const T` permits reads;
`*mut T` permits reads/writes. `T` is an integer, including `usize`; `bool`,
records, arrays, and pointers themselves are not supported pointees yet.
An immutable `let p` fixes the pointer value, not the pointed-to bytes.
`*const` does not promise immutable memory and `*mut` does not promise exclusive
access. Aliases may observe each other's writes. There is no borrow checker,
`unsafe` keyword, ownership transfer, automatic release, or implicit allocation.

`*p` reads a value. `*p = value;` writes an exactly matching value type and is
a statement, not an expression. Evaluate the address first, then the value,
then perform the write. An early return during either evaluation skips the
write. Reusing a binding initialized by a read reuses that captured value;
writing `*p` again performs a new source-level read. Ordinary non-volatile
accesses may be optimized when observable behavior is unchanged.

Only `usize as *const T` / `usize as *mut T`, pointer-to-`usize`, identity
casts, and explicit `*mut T as *const T` conversions are supported. Other
integer widths must explicitly convert through `usize`. Direct pointee changes
or adding write permission are rejected; converting through an integer is a
low-level escape whose validity remains the programmer's responsibility.
Same-type pointers support `==` and `!=`, comparing numeric target addresses.
No implicit permission conversion, pointer arithmetic, pointer indexing or
ordering is provided. Explicit `usize` arithmetic computes byte addresses,
wraps at the target width, and carries no bounds or allocation guarantee.

A numeric pointer can be Static: `4096usize as *const u32` does not read memory.
Every dereference/store remains Runtime, even at a Static address. The evaluator
never dereferences a compiler-host pointer. Pointer constants include the target
address width; normal specialization/version budgets apply to pointer calls.
A pointer-valued implicit test entry returns zero after preserving effects.

This slice accesses ordinary initialized memory in address space zero, using
target endianness and the pointee's byte width. Unaligned access is permitted
(LLVM `align 1`); accesses are not atomic and may become multiple instructions.
An analyzed known-null access reports `E4013`; a Runtime null access traps.
Merely forming, passing or comparing a null pointer is valid. Other invalid
accesses are not checked: callers must ensure live, sufficiently large,
readable/writable storage and initialized bytes for reads. Dangling, out-of-bounds,
read-only writes and unsynchronized conflicting accesses violate the contract;
there is no complete memory-safety guarantee or warning coverage promise.

No type-based non-aliasing rule is inferred from the pointee type. C callers
must additionally obey C's own object/access rules. Local address-taking,
allocation/free, pointer-containing aggregates, `defer`/RAII, atomics, address
spaces and volatile/MMIO are deferred. Plain pointer accesses must not be used
as substitutes for volatile device access.

## Minimal Records (Milestone 2)

`record Config { clock_hz: u32, ready: bool }` declares a nonempty immutable
nominal type. `Config(ready: true, clock_hz: 80_000_000)` constructs it, and
`config.clock_hz` selects a field. Every field must be supplied exactly once;
initializers evaluate in source order, independently of declaration order.
Declarations are module-level and may be referenced before their declaration.
Record and function names may not collide. Type names have a separate namespace
from local bindings. Positional record construction is not supported.

Fields are integers or `bool`. Nested records, array fields, arrays of records,
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
must produce an integer or `bool`. The immutable index is Static and
scoped to each iteration. A body may call functions, bind locals, or return from
the enclosing function. A definite return stops generation; Runtime returns
remain residual.

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
costs one slot and width/8 bytes for integers or one for `bool`. Non-continuing arrays
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
field costs one member; integers cost width/8 logical bytes and `bool` one, without
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
conversions, overloading, mutable binding assignment, or generic
parameters. New syntax for those features requires a documented semantic
decision and examples before parser implementation.
