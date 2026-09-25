# CRC-32 Staging Demonstration

The [executable example](../tests/lit/stage/crc32.pgs) closes Milestone 2's
representative static-table case, not every remaining milestone deliverable.

## Algorithm and scope

The example implements reflected CRC-32/ISO-HDLC, using polynomial
`0xEDB88320`, initial state `0xFFFFFFFF`, and final complement. Decimal Pagos
literals spell these constants; hexadecimal literals are not yet supported.
This matches the CRC used by [zlib](https://github.com/madler/zlib/blob/develop/crc32.c).
The ASCII check message `123456789` produces `0xCBF43926` (`3421780262`).

`crc_entry` computes eight bit steps through bounded Static recursion.
`[for i in 0..256 { crc_entry(i, 8) }]` builds the complete read-only table.
`crc_byte` advances the CRC using a checked table lookup and logical shift.
`crc32_9` chains nine immutable states and complements the result.

The same functions compute the known message at compile time and process nine
Runtime inputs. Each `u32` input contributes its low eight bits. The example
is deliberately fixed-length: there is no claim of a general streaming API,
mutable accumulator, Runtime recursion, byte type, or string support.

## Inspect and execute

From the repository root, after building the debug preset:

```sh
build/debug/pagosc explain-stage tests/lit/stage/crc32.pgs
build/debug/pagosc emit-mir tests/lit/stage/crc32.pgs
build/debug/pagosc emit-llvm tests/lit/stage/crc32.pgs
python3 tests/lit/Inputs/run_residual.py build/debug/pagosc clang \
  tests/lit/stage/crc32.pgs 3421780262 49 50 51 52 53 54 55 56 57
python3 tests/lit/Inputs/run_crc32.py build/debug/pagosc clang \
  tests/lit/stage/crc32.pgs
```

`table` and `known` are Static; `bytes` and `result` are Runtime. LLVM output
contains one deduplicated 256-entry constant, nine input reads, and nine table
loads. Neither recursive table construction nor a compile-time evaluator is
present in the residual program. Bounds checks remain; this is not a claim
that every potentially redundant check has been optimized away.

## Validation and budgets

The differential test uses Python's standard-library `zlib`: it checks all
256 table entries, the folded check vector, and 16 Runtime messages, including
zero bytes, high bits, and deterministically generated data. Each message runs
under external Clang `-O0` and `-O2`; exact input order/count is checked too.
These flags exercise residual code, not an integrated `pagosc -O` pipeline.

The example fits default limits: 41,552 fuel, 2,314 specializations, maximum
depth 9, and 274 reserved array slots / 1,096 logical bytes. Those totals include
both nine-element message arrays as well as the 256-entry table. Regression
tests deliberately lower each budget and require its corresponding diagnostic.
Array quotas are cumulative construction limits, not compiler RSS limits.

## Shift safety

Bitwise operations accept `u32` only. `>>` fills with zeros; `<<` discards high
bits. An analyzed Static count outside `0..32` reports `E4009`; a Runtime count
outside that range traps before shifting. Counts are not reduced modulo 32.
Separate tests cover invalid counts, discarded results, early returns, and
Runtime control flow, including optimized execution of trap paths.
