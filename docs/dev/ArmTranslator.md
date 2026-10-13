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

- `x64a64` reads an x86-64 ELF, decodes it with [Zydis](../../3rdparty/zydis), discovers functions (entry point, symbols, `R_X86_64_RELATIVE` targets, `.eh_frame` FDEs, direct call targets, RIP-relative `lea` of code) and their basic blocks, and recovers clang and GCC jump tables (`lea table(%rip)`, `movslq (base,index,4)`, bound from the dominating `cmp`, following register copies).
- Each guest function becomes one C++ function. Guest registers it uses live in a local `GuestContext` and are written back to the thread's context only at calls, returns and runtime calls, so clang keeps them in host registers.
- Flags are computed only where a later instruction reads them: a backward liveness pass over the function, with calls and function exits treated as killing all flags, as the System V ABI allows. 96% of flag writes in the test programs are elided.
- Guest addresses are host addresses. The image is mapped once and relocated; pointers cross into native libraries unchanged.
- `ret` records the popped address and the call site checks it against the expected return address; a mismatch (stack switching, `longjmp` across translated frames) throws.
- Indirect calls and jumps go through `Runtime::Dispatch`, a hash map from guest address to translated function. An address with no translated code throws: runtime-generated code needs a JIT fallback, which the prototype does not have.
- Untranslated instructions compile to a call that throws, and `--report` counts them, so the tool can also be used as an instruction census on a title.

### Guest to native calls

Imports resolve to addresses in a reserved range. Dispatching to one calls the native function through [`HostCall.S`](../../core/translator/runtime/src/HostCall.S), which loads `rdi, rsi, rdx, rcx, r8, r9` into `x0`–`x5`, the guest's first two stack arguments into `x6` and `x7`, the next eight stack slots onto the host stack, and `xmm0`–`xmm7` into `q0`–`q7`, then stores `x0`, `x1`, `q0`, `q1` back to `rax`, `rdx`, `xmm0`, `xmm1`. This is correct for any non-variadic function with up to sixteen integer arguments, because both ABIs assign integer and vector registers independently and Apple arm64 gives 8-byte arguments past the eighth an 8-byte stack slot as System V does. It is not correct for:

- variadic functions: Apple arm64 passes every variadic argument on the stack;
- integer arguments narrower than 8 bytes past the eighth: Apple arm64 packs them by natural alignment. The exports that have them declare those parameters as `GuestStackArg<T>` ([`VabiMacros.hpp`](../../core/libs/prx/libc/include/general/VabiMacros.hpp)), an 8-byte slot on Apple arm64 and `T` elsewhere;
- more than eight vector arguments, and structures passed or returned by value.

Those imports need a handler that reads the guest's System V arguments itself (`Runtime::RegisterContextImport`). The tests use one for `printf` and for an eight-argument function.

The variadic exports of `core/libs/prx` (the `printf` and `scanf` families, `open`, `fcntl`, `syslog`, `makecontext`) start their argument list with `APS5_SYSV_VA_START` from [`GuestVarArgs.hpp`](../../core/libs/prx/libc/include/general/GuestVarArgs.hpp). On Windows it is `__builtin_sysv_va_start`. On arm64 it asks the runtime for a System V `va_list` built from the calling guest's registers and stack; the runtime checks that the guest register holding the last fixed argument matches it, and the call throws when no guest caller is found. Both use the guest formatting engine that reads System V `va_list`s, so `vprintf` and the other `va_list` exports also accept guest lists.

### Native to guest calls

A native library that receives a guest function pointer (a callback, a thread entry point, a comparator) calls it like any other function pointer. Guest images are mapped without execute permission, so the call faults on the instruction fetch. The runtime's `SIGBUS`/`SIGSEGV` handler ([`CallbackBridge.cpp`](../../core/translator/runtime/src/CallbackBridge.cpp)) checks that the faulting address is a translated function and redirects the thread to a bridge that moves `x0`–`x5` and `q0`–`q7` into the guest's `rdi`–`r9` and `xmm0`–`xmm7`, runs the function with `Runtime::CallGuest` and returns `rax` and `xmm0` in `x0` and `q0`. A call into the import range goes straight to the native function. Each host thread gets its own guest context, 8 MiB guest stack and FS block on first use. Every callback costs a signal; the arguments are subject to the same limits as guest to native calls.

### Titles and modules

A title is translated image by image: the eboot and each guest module get their own symbol prefix, function table and image-base variable, which the translated code adds to every address. [`TitleMain.cpp`](../../core/translator/runtime/src/TitleMain.cpp) loads the arm64 system libraries, maps the eboot, resolves imports by NID (runtime handlers first, then exports of loaded guest images, then the libraries; a missing NID becomes a placeholder that throws when called), registers the image with `libc`'s guest image registry, which macOS `dl_iterate_phdr` and the module APIs read, and enters `_start` with the System V process stack. `sceKernelLoadStartModule` reaches the runtime's loader through `dlopen`: it maps and relocates the module, runs `DT_INIT` with the pending module arguments and then `DT_INIT_ARRAY`, as the relinker's Mach-O modules do. The main image keeps static TLS below the thread pointer; modules get dynamic TLS through `DTPMOD64`/`DTPOFF64` and the runtime's `__tls_get_addr`.

On macOS the title driver runs the guest on a secondary thread with a 64 MiB stack and keeps the process's main thread in the Cocoa run loop, because AppKit creates and drives windows only on the main thread; `libSceVideoOut` sends its SDL window, event and Vulkan surface calls there with `dispatch_sync` ([`MainThread.hpp`](../../core/libs/prx/libSceVideoOut/include/MainThread.hpp)).

`setjmp` and `longjmp` are runtime handlers. Each call's return address starts a block; a translated frame whose call returns somewhere unexpected resumes at a call site of its own when the address matches and the restored stack pointer lies within its frame, and otherwise returns to its caller, so a `longjmp` unwinds translated frames until it reaches the `setjmp` caller.

C++ exceptions use the same resumption. The translator reads `.eh_frame` and each function's LSDA and makes every landing pad a block and a resume point of its function. On arm64, `libc`'s unwinder starts from the guest registers at the import call (`__cxa_throw`, `_Unwind_Resume`) and walks the guest stack with the guest's own CFI, mapping the guest's personality import back to the native personality routine. To install a landing pad it hands the registers to the runtime, which throws a host exception that the import dispatcher catches; the dispatcher writes the guest registers and returns to the landing pad, and translated frames unwind to the one that owns it. An exception does not cross a native frame between guest frames, such as a guest callback called by `qsort`.

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

A decrypted eboot of a Unity (IL2CPP) title, `x64a64 eboot.bin -o eboot --shards 256 --report`. These are the figures from the first translation; later instruction additions raise the translated share to 99.99%. Section headers are absent in decrypted eboots, so relocations and symbols come from the dynamic table.

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

All but 5 of the eboot's 597 imports resolve against the arm64 libraries. The title runs through Unity's start-up, its argument and launcher output, and into loading `Il2CppUserAssemblies.prx` (10.5 million instructions) and the other Unity modules.

## Playing a game

A game folder is a PS5 dump with `sce_sys/param.json` and a decrypted `eboot.bin`, either beside it or in a `decrypted` subfolder. Its bundled `.prx` and `.sprx` modules that `build-arm64` has no library for are translated with it.

### The AnyPS5 app

```sh
tools/macos/build_launcher_app.sh
open build-launcher/AnyPS5.app
```

Add a game with the + button or by dropping its folder on the window, press Prepare, then Play. Preparing a game the first time takes about 30 to 40 minutes, almost all of it compiling; preparing it again after a translator or runtime change keeps every compiled shard whose source did not change. The app uses the checkout it was built from (Settings can point it at another), which needs `build-arm64` configured and `ninja`, `cmake` and Xcode's command line tools.

### From a terminal

```sh
tools/macos/title.py build <game folder>
tools/macos/title.py run <game folder>
```

### Settings

[`tools/macos/options.json`](../../tools/macos/options.json) lists every setting the app shows: display (frame pacing, frames in flight, fullscreen, scaling), graphics accuracy, MoltenVK performance switches, caches and build options (optimization, CPU tuning, memory ordering, flag computation, compiler jobs). Each choice names the environment variables it sets when the game runs, or the compiler and translator flags it adds when the game is prepared. Game defaults live in `~/Library/Application Support/AnyPS5/settings.json` and a game's own choices in its `settings.json`; both can add free-form `NAME=value` lines under `environment`. `title.py settings <game folder>` prints what a game ends up with, and `title.py status <game folder>` says whether its build matches its build settings and the translator.

While a game runs, Capture Hang Report in the app samples every thread with `sample` and saves the stacks with the end of the game log, which shows where a stuck game waits.

Both commands keep their files in `~/Library/Application Support/AnyPS5/Titles/<title id>`: the linked `title` executable, the translated sources and objects in `build`, and in `run` the guest's working directory with its `app0` link to the game, save data and shader caches. `build` prints `progress <phase> <done> <total>` lines for the app and rebuilds only what changed; `--force` translates again even when nothing did.

## Not covered

- A JIT fallback for runtime-generated code and undiscovered indirect targets.
- x87, MMX, string instructions other than `rep movs` and `rep stos`, MXCSR rounding modes and exception flags.
- Fibers, `getcontext`/`setcontext`: these read or write guest registers, which live in the translator's context; the arm64 libraries throw for them.
- The guest's 80-bit `long double`: the arm64 libraries throw where it is used.
- Initial-exec TLS (`TPOFF64`) in modules.
- Self-modifying code.
