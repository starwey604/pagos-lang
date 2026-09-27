# Core Grammar Sketch

Status: normative syntax baseline, including M3 storage, pointers, volatile and C ABI.
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
source          = { global | record | function | statement } EOF ;
global          = "global" ( "const" | "mut" ) identifier ":" storage-type
                  { storage-attribute } "=" expression ";" ;
storage-type    = integer-type | "[" integer-type ";" integer "]" ;
storage-attribute = "section" "(" quoted-section-name ")"
                  | "align" "(" decimal-alignment ")" ;
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
unary           = ( "!" | "~" | "-" | "*" | "&" ) unary
                | "volatile" "*" unary | call ;
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

A module with any `extern`/`export` or global storage declaration and no top-level
statements emits its storage, boundary symbols and reachable helpers, not
synthetic `pagos_main`. Top-level
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
spaces are deferred. Plain pointer accesses must not be used
as substitutes for volatile device access.

## Global Integer Storage (Milestone 3)

```pagos
global const limit: u32 = 100;
global mut counter: u32 = 0;

export fn advance() -> u32 {
    *&counter = counter + 1;
    counter
}
export fn counter_address() -> *mut u32 { &counter }
```

`global const` and `global mut` declare module-private objects with program-long
storage duration. An explicit integer, fixed-length integer-array, or
integer-field record type and
initializer are mandatory; scalar zero initialization uses `= 0` with the
appropriate suffix. All integer types, including target `usize`, are allowed;
pointers, `bool` and nested aggregate storage are deferred.
Declarations may be referenced before their textual
definition. Names must not collide with functions, records, globals or reserved
intrinsics/entry symbols. Local bindings and parameters may shadow globals.

Initializers must yield an exactly matching pure Static value (`E2010` on
Runtime work). Pure function calls are allowed under existing evaluation budgets.
Reading another global, forming its address, calling foreign code or hiding a
store behind a Static result is not allowed. No initialization-order dependency,
Runtime constructor, automatic initializer function or destructor is introduced.
All declarations are checked even when unused. These are storage declarations,
not a reinterpretation of existing top-level `let` statements.

Reading a scalar global name loads its current value at Runtime, including a
readonly object: `global const` is not a staging annotation. `&name` produces a pointer
to the unshadowed global, `*const T` for readonly storage and `*mut T` for writable
storage. Global array elements and integer record fields can also be addressed
as described below; local bindings, parameters, temporaries and functions cannot.
Scalar writes use `*&name = value;`
or an alias pointer, not direct name assignment. Volatile accesses such as
`volatile *&counter` follow the existing volatile rules.

Global addresses are symbolic Runtime values resolved by object relocations and
linking, never numeric compiler-host pointers. `&name as usize` executes at
Runtime and cannot satisfy a Static demand. Separate live objects have distinct
addresses, including equal-initialized readonly objects and same-named globals
in separate modules. Pointers may cross existing C ABI function boundaries;
global symbols themselves are not imported/exported. A readonly object must
not be mutated, including through integer casts or foreign aliases.

LLVM uses internal symbols and target ABI alignment. On the tested ELF targets,
ordinary defaults place readonly data in `.rodata`, nonzero writable data in
`.data` and zero writable data in `.bss`. Optimization may remove unobservable
storage or fold provably immutable reads; it must preserve observable address
identity and memory effects. The loader/startup must establish initial contents
before any Pagos function runs. Named data subsections and explicit alignment
are described below. Fixed-address placement, TLS, external globals, dynamic
initialization and allocation remain deferred.

### Global integer arrays

```pagos
global mut buffer: [u8; 16] section(".bss.buffer") align(64) =
    [for i in 0..16 { 0u8 }];
global const squares: [u32; 16] = [for i in 0..16 { i * i }];
export fn sample(index: u32) -> u32 {
    *&buffer[index] = 7u8;
    squares[index] + buffer[index] as u32
}
export fn element(index: u32) -> *mut u8 { &buffer[index] }
```

Array storage is contiguous, in element order, with target integer width and
endianness; `usize` elements use the target pointer width. Array length is a
positive literal as in existing array types. Initializer element types and the
fully evaluated length must match exactly. Existing array/aggregate construction,
fuel and specialization budgets also apply to unused global initializers.
These are construction quotas, not total compiler-memory or target-RAM limits.
Objects larger than `2^(pointer_bits - 1) - 1` bytes are rejected before array
construction, so element byte offsets fit signed target address arithmetic.

`name[index]` reads the current element at Runtime, including readonly objects;
it does not materialize or copy the whole array. `&name[index]` yields a Runtime
integer pointer with the object's write permission. The index is a `u32`, matching
existing array indexing, and is evaluated exactly once. Known Static out-of-range
indices report `E4007`; Runtime out-of-range indices trap before forming the
element address, even when the read or address result is discarded. One-past-end
addresses are not accepted by this syntax. An index that returns early skips
the address calculation and any subsequent read/store.

Writes use `*&name[index] = value;` or an alias pointer. Address evaluation,
including the bounds check, precedes the store's right-hand side. A captured
element value is a snapshot; a later source-level element read observes current
memory. Pointers may cross the existing C ABI; C must respect object bounds,
permissions and lifetime. Bounds metadata is not carried in the raw pointer.
`volatile *&name[index]` uses existing volatile rules, not an aggregate access.

Bare array-name values, whole-array addresses/assignment and implicit decay to
the first element are not supported (`E1015` for bare-name use); write `&name[0]`
explicitly. Local arrays retain their existing value semantics, but cannot be
addressed. Local shadowing does not grant global storage to a local array.
Section and alignment attributes apply to the complete object, not each element;
`.bss.*` requires every initializer element to be zero.

### Global integer-field records

```pagos
record DeviceState { status: u8, counter: u32, capacity: usize }
global mut state: DeviceState section(".data.state") align(64) =
    DeviceState(status: 0u8, counter: 0, capacity: 16usize);
export fn advance() -> u32 {
    *&state.counter = state.counter + 1;
    state.counter
}
export fn counter_address() -> *mut u32 { &state.counter }
```

Stored records contain only integer fields, including `usize`. Fields follow
declaration order, each naturally aligned according to the selected target's
LLVM DataLayout; necessary inter-field and tail padding is included in object
size. No reordering or packing is performed. Layout is deterministic for a target
configuration, not identical across targets. An object's `align(...)` request
does not change its type layout or field offsets. Padding bytes have no specified
language value: do not use raw record bytes for serialization or equality.

The initializer must be pure Static and match the nominal record type exactly;
named arguments retain existing source-order evaluation and declaration-order
field placement. `.bss.*` requires every field value to be zero. Existing
aggregate construction budgets count logical field bytes, not padding or object
alignment. LLVM emission additionally rejects a padded object exceeding
`2^(pointer_bits - 1) - 1` bytes. These checks are not total-memory quotas.

`name.field` creates a fresh Runtime scalar load, including `global const`.
`&name.field` creates a symbolic Runtime integer pointer with the object's write
permission. Writes use `*&name.field = value;` or an alias pointer; volatile
accesses follow existing scalar rules. A captured field value is a snapshot,
while a later field read observes intervening writes. Early return in a store's
right-hand side skips the store. Local shadowing preserves ordinary value-record
semantics and does not make a local record addressable.

Bare global-record values and whole-record addresses are rejected (`E1015`).
Whole-record copies, record-pointer types, local address-taking, nested/array
fields, arrays of records and packed/bitfield layouts remain deferred. Existing
value records with `bool` fields remain supported, but cannot be global storage.
C may use integer field pointers through the existing ABI. Host/RV32 tests compare
this integer-field subset with C layout; this does not enable record-by-value
C parameters or returns, nor promise compatibility with other ABIs or packing.

### Data subsections and alignment

```pagos
global const limit: u32 section(".rodata.config") align(32) = 100;
global mut seed: u32 section(".data.boot") align(64) = 37;
global mut count: u32 section(".bss.boot") align(128) = 0;
```

Optional `section(...)` and `align(...)` attributes follow the declared type,
before `=`. Either order is accepted, each at most once. They are contextual
names, not new reserved keywords. Quoted text is only section metadata, not a
string expression; escapes, newlines and interpolation are unsupported.

Explicit sections currently require ELF output and a nonempty suffix after
`.rodata.` for `const`, or `.data.` / `.bss.` for `mut`. Names contain only ASCII
letters, digits, `_` and `.`. Other section classes, including `.text` and
`.noinit`, are rejected (`E1014`). The `.data.rel.ro*` and `.bss.rel.ro*` prefixes
are reserved: common ELF linkers give them read-only-after-relocation semantics.
A `.bss.*` initializer must evaluate to zero
(`E2011` otherwise); `.data.*` allows zero as well as nonzero data and still
emits initialized storage. Initializers retain the pure Static requirement.
Omitting `section` preserves the backend's ordinary default placement.

`align(N)` requests a minimum object alignment in bytes. `N` is an unsuffixed
decimal literal without separators, a power of two from 1 through 4096. It is
not an expression or a type alignment change. Effective alignment is the larger
of the target ABI alignment and the request; `align(1)` cannot under-align an
integer. The 4096-byte ceiling is a current language limit, not an LLVM limit.

These are input-section names, not fixed addresses or memory-region names.
The linker script determines final output sections and addresses, must preserve
object permissions/alignment, and startup must copy/clear the corresponding
regions. The compiler cannot validate an arbitrary external linker script.
The RV32 script already collects
`.rodata.*`, `.data.*` and `.bss.*`; the UART example now exercises all three.
Placement does not imply retention against optimization or linker garbage
collection, declaration order, adjacency, MMIO, atomicity or synchronization.
Arbitrary sections, retained/uninitialized storage and retention attributes
remain separate future work.

## Volatile Access and MMIO (Milestone 3)

```pagos
// QEMU RV32 virt UART only; these are not portable device addresses.
let status = 268435461usize as *const u8;
let transmit = 268435456usize as *mut u8;
let ready = (volatile *status & 32u8) != 0u8;
if ready { volatile *transmit = 65u8; 0 } else { 0 };
```

`volatile *p` is a read expression; `volatile *p = value;` is a write statement.
`volatile` qualifies this access, not the pointer type or an enclosing block.
It neither adds write permission nor changes ownership. Store operands evaluate
address first, value second, then access; early returns skip the access.
Each executed read/write is observable, including discarded reads and repeated
identical stores. Binding a read captures one snapshot; reusing that binding
does not read again. Static branches may omit unexecuted accesses.

Both operations are always Runtime. Constant addresses, Static-result helpers
and reused specializations must not cause host access, result memoization or
loss of effects. The compiler preserves volatile access counts and their order
relative to other volatile accesses. It is not a general ordering barrier for
unrelated ordinary memory. These guarantees follow
[LLVM volatile semantics](https://llvm.org/docs/LangRef.html#volatile-memory-accesses).

Emission is initially restricted to the same target configurations as the
minimal C ABI: x86-64 Linux LP64 and generic RV32 ELF ILP32 (I/M/A/C).
Integer access widths are 8/16/32 bits, plus 64 bits on x86-64; `usize` uses the
target width. RV32 volatile `u64/i64` accesses are rejected with `E1012`, even
though ordinary RAM supports those types. Natural alignment is required:
address modulo access size must be zero. Known misalignment reports `E4014`;
dynamic misalignment traps before access. The existing null checks also apply,
so address zero remains unavailable even if a device could map it.

LLVM emits naturally aligned integer volatile loads/stores. Supported accesses
must not be split or merged by the compiler; this is not a guarantee about
physical bus transactions or atomicity. Device access permissions, register
width, mapping and side effects remain platform/caller obligations. Reads may
change device state; do not assume `*const` implies a side-effect-free read.
Read-modify-write expressions perform separate operations and may be invalid
for read-clear or write-one-to-clear registers.

No CPU/device fences, cache configuration, atomics, DMA synchronization,
interrupt support or automatic device discovery are provided. QEMU validation
is not a real-chip timing or memory-ordering guarantee. See the standalone
[Pagos UART example](../platforms/qemu-rv32-virt/mmio.pgs).

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
host addresses. Value construction does not allocate addressable storage.
The [global record subset](#global-integer-field-records) defines target storage
layout for integer-only fields; general record pointers, packing and record C
calling conventions remain deferred.
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
