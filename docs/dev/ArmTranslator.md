# ARM64 translator prototype

A feasibility spike for running converted titles natively on Apple silicon without Rosetta. General-purpose Rosetta 2 ends after macOS 27, so the x86-64 Mach-O path (`relinker --macos --to-rosetta`) has a fixed end date.

[`core/translator`](../../core/translator) is a standalone CMake project, separate from the main build:

```sh
cmake -S core/translator -B build-translator -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-translator
ctest --test-dir build-translator --output-on-failure
```

It needs `ld.lld` (`brew install lld`) to link the x86-64 test programs.

## Design

Static recompilation to C++, compiled by clang for arm64, in the style of XenonRecomp and N64Recomp.

- `x64a64` reads an x86-64 ELF, decodes it with [Zydis](../../3rdparty/zydis), discovers functions (entry point, symbols, `R_X86_64_RELATIVE` targets, direct call targets, RIP-relative `lea` of code) and their basic blocks, and recovers clang and GCC jump tables (`lea table(%rip)`, `movslq (base,index,4)`, bound from the dominating `cmp`, following register copies).
- Each guest function becomes one C++ function. Guest registers it uses live in a local `GuestContext` and are written back to the thread's context only at calls, returns and runtime calls, so clang keeps them in host registers.
- Flags are computed only where a later instruction reads them: a backward liveness pass over the function, with calls and function exits treated as killing all flags, as the System V ABI allows. 96% of flag writes in the test programs are elided.
- Guest addresses are host addresses. The image is mapped once and relocated; pointers cross into native libraries unchanged.
- `ret` records the popped address and the call site checks it against the expected return address; a mismatch (stack switching, `longjmp` across translated frames) throws.
- Indirect calls and jumps go through `Runtime::Dispatch`, a hash map from guest address to translated function. An address with no translated code throws: runtime-generated code needs a JIT fallback, which the prototype does not have.
- Untranslated instructions compile to a call that throws, and `--report` counts them, so the tool can also be used as an instruction census on a title.

### Guest to native calls

Imports resolve to addresses in a reserved range. Dispatching to one calls the native function through [`HostCall.S`](../../core/translator/runtime/src/HostCall.S), which loads `rdi, rsi, rdx, rcx, r8, r9` into `x0`–`x5` and `xmm0`–`xmm7` into `q0`–`q7`, then stores `x0`, `x1`, `q0`, `q1` back to `rax`, `rdx`, `xmm0`, `xmm1`. This is correct for any non-variadic function whose arguments fit in registers, because both ABIs assign integer and vector registers independently. It is not correct for:

- variadic functions: Apple arm64 passes every variadic argument on the stack;
- more than six integer or eight vector arguments: System V passes the rest in 8-byte stack slots, Apple arm64 packs them by natural alignment;
- structures passed or returned by value.

Those imports need a handler that reads the guest's System V arguments itself (`Runtime::RegisterContextImport`). The tests use one for `printf` and for an eight-argument function. Applying this to `core/libs/prx` means knowing each export's signature.

### Native to guest calls

A native library that receives a guest function pointer (a callback, a thread entry point, a comparator) cannot call it directly. It has to call `Runtime::CallGuest`, which runs the translated function on the calling thread's guest context and stack. Each host thread gets its own guest context, 8 MiB guest stack and FS block on first use.

## Memory ordering

x86 guarantees that loads are not reordered with other loads and stores are not reordered with other stores; arm64 does not. The `Threads` test publishes a million slots (data, then flag) from one thread while another reads flag then data:

| Build | Out-of-order reads per run (5 runs) |
|---|---|
| Translated, ordering off | 9, 12, 20, 14, 12 |
| Translated, ordering on | 0, 0, 0, 0, 0 |
| The same C compiled natively for arm64 | 5, 6, 1 |

So ordering cannot be dropped. With `APS5_TRANSLATOR_TSO` (default on), aligned scalar guest loads are acquire (`LDAPR`) and stores are release (`STLR`); unaligned and vector accesses add `DMB` barriers (`APS5_TRANSLATOR_VECTOR_TSO`, default on). Accesses based on `rsp` are plain, since only the owning thread uses its stack and the surrounding ordered accesses constrain them. Locked instructions and `xchg` with memory are sequentially consistent atomics.

Apple silicon has a hardware TSO mode, but it is not exposed to ordinary processes, so the prototype does not rely on it.

## Results

Apple silicon, macOS 27.2. The test programs are freestanding C compiled to x86-64 ELF with clang `-O2`, once for baseline x86-64 (SSE2) and once with `-march=znver2` (AVX2, FMA, BMI1/2, as on the PS5's Zen 2 CPU). Each translated run is compared with the same source compiled natively for arm64.

| Test | Covers |
|---|---|
| `Integer` | jump table, recursion, function pointer table, 128-bit multiply, division, 8/16-bit arithmetic, eight-argument import, `printf` with mixed arguments |
| `Float` | scalar and packed SSE/AVX, conversions, comparisons, `fmaf`, mixed integer/float import |
| `Callbacks` | `qsort` in a native library calling a guest comparator, `memcpy`/`memset` imports |
| `Threads` | guest threads started by a native library, `lock xadd`, a spinlock, message passing |
| `Bench` | matrix multiply, CRC32, pointer chasing, timed |

All 10 tests pass, and every instruction in all 10 builds is translated.

Best of five runs, in milliseconds:

| Benchmark | Native arm64 | Translated, ordering on | Translated, ordering off |
|---|---|---|---|
| Matrix multiply, SSE2 | 6 | 14 | 8 |
| Matrix multiply, AVX2 | 7 | 20 | 12 |
| CRC32 | 16 | 17 | 17 |
| Pointer chasing | 32 | 33 | 30 |

Before guest registers were moved into locals, the ordered build took 47 and 68 ms for the two matrix multiplies: acquire loads stop clang from caching the shared context across them.

## A real title

A decrypted eboot of a Unity (IL2CPP) title, `x64a64 eboot.bin -o eboot --shards 256 --report`. Section headers are absent in decrypted eboots, so relocations and symbols come from the dynamic table.

| | |
|---|---|
| Executable code | 24.3 MB |
| Functions found | 34,726 |
| Basic blocks | 837,880 |
| Instructions | 5,435,994 |
| Translated | 98.44% |
| Code reached by static discovery | 96.71% of executable bytes |
| Flag writes elided | 90.6% |
| Translation time, peak memory | 12 s, 389 MB |
| Generated C++ | 256 shards |
| Compile, `clang++ -O2`, 11 cores | 200 s wall, slowest shard 73 s |
| Linked arm64 executable | 103 MB |

The untranslated 1.56% is a long tail of AVX and AVX2 instructions, led by `vpermilps`, `vinsertps`, `vmovshdup`, `vcmpps`, `vpextrd`, `vblendvps` and `vdpps`, plus `movbe`, `bextr`, `cmpxchg16b`, MXCSR loads and stores, and a few MMX instructions. Each is a contained addition to the emitter.

Of 3,917 indirect jumps, 867 have recovered jump tables. Of the rest, 587 go through RIP-relative memory (PLT stubs and GOT), 594 through other memory (virtual tail calls) and 1,869 through registers; 117 of those are shaped like jump tables that recovery missed. Dispatch handles jumps to function entries, but a missed jump table reaches mid-function code and needs better recovery or the JIT fallback.

The eboot imports 597 functions from 39 modules, led by `libc` (159), `libScePosix` (133) and `libSceAgc` (57).

The translated title was compiled and linked but not run: it needs the arm64 system libraries.

## Not covered

- Running a title.
- A JIT fallback for runtime-generated code and undiscovered indirect targets.
- x87, `cmpxchg16b`, string instructions other than `rep movs` and `rep stos`, most of SSE4 and AVX2 beyond what the tests use, and MXCSR rounding modes.
- Exceptions and unwinding through translated frames, `setjmp`/`longjmp`, fibers and signal contexts: these read or write guest registers, which now live in the translator's context.
- TLS beyond a zeroed FS block, and the PS5 module loader (`sce_module`, NID imports).
- Self-modifying code.
