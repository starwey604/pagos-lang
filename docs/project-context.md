# Pagos Project Context

This document preserves the project's origin and major decisions so work can
continue after the repository is moved or the original design conversation is
unavailable.

## Origin

Pagos began as an embedded systems language idea motivated by work with C++,
Robomaster, real-world embodied intelligence systems, HALs, the Linux kernel,
Buildroot, and Zephyr.

The original observation was that embedded projects contain a large amount of
information that is known before deployment: hardware topology, register
layouts, enabled drivers, protocol descriptions, lookup tables, memory maps,
and product configuration. Existing systems can exploit this information, but
often distribute the process across C/C++ templates and macros, Kconfig,
Devicetree, CMake, Python, linker scripts, and generated headers.

Pagos explores whether one typed language can express both normal systems code
and the configuration/program-generation work around it, with compile-time
execution as the default outcome rather than an opt-in special case.

## Naming decision

The project was initially called **Amber**. It was renamed **Pagos** to avoid a
collision with existing programming languages and to better express the stage
model.

Ancient Greek `πάγος` means that which is fixed or firmly set and can also mean
frost or ice. Pagos uses the following metaphor:

- Values begin frozen: they are candidates for compile-time evaluation.
- Explicit runtime sources introduce heat.
- Runtime dependence thaws downstream values along the dataflow graph.
- A `static` constraint requires a value to remain frozen.

Formal specifications should use **Static**, **Runtime**, **binding-time
analysis**, **partial evaluation**, and **residual program**. Frozen/thawed is a
documentation and diagnostic metaphor, not a substitute for precise semantics.

The canonical spelling is **Pagos**, not `PagOS`. Proposed tool names are
`pagosc` for the compiler and `.pgs` for source files. These names remain
provisional until the toolchain exists and package/domain conflicts are checked.

## Decisions already made

1. Pagos is a staged systems programming language, not just a build DSL.
2. Stages belong to values, expressions, control flow, and effects, not vaguely
   to "all statements."
3. The base binding-time lattice is `Static <= Runtime`.
4. `runtime` introduces a dynamic source; `static` imposes a compile-time
   constraint; unqualified bindings are inferred.
5. Analysis should be value- and field-sensitive. A runtime field should not
   unnecessarily thaw an entire configuration graph.
6. Mixed-stage functions are partially evaluated and leave residual runtime
   code rather than becoming wholly runtime.
7. Immutable bindings are the default. SSA form is used to make propagation
   precise; mutability semantics remain an open design question.
8. Compile-time effects require explicit capabilities and dependency tracking.
   Arbitrary undeclared filesystem, environment, network, or process access is
   incompatible with reproducible incremental builds.
9. Only liftable/embeddable compile-time values may cross into runtime data.
   Compiler objects and host pointers cannot leak into target code.
10. The first compile-time engine should be an interpreter or bytecode VM for
    determinism, diagnostics, resource limits, and cross-target correctness.
    LLVM ORC JIT may later accelerate safe closed computations.
11. LLVM is the residual runtime backend. Pagos-specific staging information
    must survive in higher-level IR until partial evaluation is complete.
12. The MVP should use a custom typed HIR and SSA MIR. MLIR is deferred until
    multiple stable domain IRs justify its complexity.
13. C ABI interoperability comes first. Direct arbitrary C++ ABI compatibility
    is explicitly deferred.
14. Embedded configuration should initially generate artifacts for existing
    C/C++, Zephyr, and Linux workflows rather than requiring ecosystem
    replacement.
15. Raw-pointer lifetime management stays with the programmer. Do not introduce
    an `unsafe` keyword or mandatory borrow checker as a default roadmap step.
    Keep explicit type/memory rules and useful diagnostics; scoped cleanup,
    allocators and RAII-style tools are separate future design choices, not
    a promise of complete memory safety.

## Corrected assumptions about existing systems

Linux Kconfig is primarily a build-time configuration system and produces
generated configuration inputs. Devicetree source is normally compiled to a
binary DTB, passed or embedded by platform firmware/boot code, and parsed by the
kernel during boot. It is not generally loaded as raw DTS from the root
filesystem.

Zephyr already moves Kconfig and Devicetree information into compile-time
generated headers and device objects. Pagos's opportunity is therefore not to
invent static device configuration, but to offer a typed and unified model with
better composition, diagnostics, dependency tracking, and controlled
static/dynamic mixing.

Static platform description may reduce some parsing, probing, and binary size,
but it cannot by itself guarantee fast Linux boot. Decompression, driver probe,
firmware, storage, the root filesystem, and userspace initialization must be
measured separately.

## Central research question

> Given a small number of explicit runtime sources, can Pagos reliably and
> explainably partially evaluate the rest of the program into small residual
> code comparable to carefully written C or C++?

Every early implementation decision should help answer this question.

## Reference material

- [Zig compile-time model](https://ziglang.org/documentation/master/#comptime)
- [LLVM language reference](https://llvm.org/docs/LangRef.html)
- [LLVM ORC design](https://llvm.org/docs/ORCv2.html)
- [MLIR dialect design](https://mlir.llvm.org/docs/DefiningDialects/)
- [Linux Kconfig documentation](https://docs.kernel.org/kbuild/kconfig.html)
- [Linux Devicetree usage model](https://docs.kernel.org/devicetree/usage-model.html)
- [Zephyr Kconfig documentation](https://docs.zephyrproject.org/latest/build/kconfig/index.html)
- [Zephyr Devicetree build flow](https://docs.zephyrproject.org/latest/build/dts/intro-scope-purpose.html)
