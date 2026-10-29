# Aegis 1.0.0 release review

Date: 2026-10-29 · reviewed build: `aegisc 1.0.0-rc.1` (C compiler and self-hosted compiler, identical output)

## Verdict

**The compiler is release quality. The language isn't ready to freeze yet.** This build is tagged
**1.0.0-rc.1**. A 1.0.0 release promises that programs written today keep compiling. Today, seven gaps
would force breaking changes to the language or the standard library after that promise. Once those seven
blockers (below) are done, release **1.0.0-rc.2**. Ship **1.0.0** when one rc cycle passes without a breaking
change.

| area | status | notes |
|---|---|---|
| correctness of generated code | ready | 15 runtime test programs × 4 builds (C/self-hosted × -O0/-O), all under valgrind; optimizer fuzzing; String fuzzing against a Python oracle |
| self-hosting | ready | C → stage 1 → stage 2 is a fixed point at -O0 and -O; tokens, AST, typed AST, errors and assembly are byte-identical between the two compilers |
| memory safety of safe code | ready, with documented rules | bounds-checked arrays and strings, ownership and moves, borrow-only interfaces, reference-counted strings; integer overflow wraps (defined, never UB) |
| runtime errors | ready | out of bounds, division by zero, stack overflow and panic flush output, print one line and exit with 101 |
| capability system | ready | allow/deny, extern classification, `requires` on library entry points, trusted standard library, `protect Process` (attack-tested) |
| diagnostics | ready | file:line:column, source excerpt with a caret, up to 20 errors (one per function), warnings (`@deprecated`); 90 negative tests with exact expected output |
| standard library | partial | fs, console, env, process, time, math, vec, error. Missing: map, sorting, formatting, networking |
| coverage of the language spec | partial | missing `allowAccess` (module permissions), `allowExternal` / OS manifest, graphics/audio/input libraries |
| tooling | partial | `--test` runner. No debug info, formatter, language server or package manager |
| platforms | Linux x86-64 only | gcc is needed to assemble and link |
| performance | ready | 1.40x C (geometric mean over 8 benchmarks), ahead of Go, Java and Node; compiles itself (8 500 lines) in under 0.1 s |

## What this round added (items 3, 4, 1, 2, 5 of the plan)

1. **(3) Entry point and loops.** `fn main(args: Vec<String>) -> i32` gives the command line, and `for x in`
   works over arrays, Strings and any class with `len()` / `item(i)` (`Vec`).
2. **(4) Literals and constants.** Struct literals `P { x: 1, y: 2 }` (missing, unknown and duplicate fields
   are errors) and compile-time `const` integer expressions, also usable as array sizes.
3. **(1) A real String.** Strings are reference-counted immutable byte strings with `+` (auto-conversion),
   content comparison, bounds-checked indexing and 16 methods. `str(x)` converts a value. Copies through
   variables, fields, arrays, `Vec`, `Result`, tasks and generics are exact: the String fuzzer runs 600 programs
   and valgrind checks them for leaks. The runtime is Aegis code (`lib/rt/prelude.ag`).
4. **(2) Capability-gated standard library.** New modules `std.fs`, `std.console`, `std.env`, `std.process`,
   `std.time` and `std.error`. Functions declare `requires File.Read` and similar, and every call site is checked.
   The standard library is trusted, and `safe extern` lets it expose C functions such as `sqrt` to safe code.
   Importing `std.vec` no longer needs `allow Native`. A String passed to C is never null.
5. **(5) Diagnostics.** Errors now show column, source excerpt and caret, and the checker reports multiple
   errors. Every stale "stage 0" message is gone. Error messages are pinned by golden files (`tests/*.err`).

Extras that the review turned up and that were fixed in this round:
- Runtime errors used to lose buffered output and die with `SIGFPE`/`SIGSEGV`/`SIGILL`. They now print a
  message (`lib/rt/fault.s`, signal handlers on an alternate stack).
- The spec's attributes now exist: `@test` with `aegisc --test` (each test runs in its own process) and
  `@deprecated` (call-site warnings).
- `--version` and `--help` were added. The wordcount example matches `wc` byte for byte.

## Blockers for 1.0.0

Each of these changes what programs mean or what the standard library looks like. Doing any of them after
1.0.0 would break existing code.

| # | blocker | why it blocks | size |
|---|---|---|---|
| B1 | **module namespaces**: functions and types are global across modules (`fn read_line` in a program that imports `std.console` is "redefined", `class File` clashes with `std.fs`) | today, adding any function to the standard library can break user programs, so no compatibility promise is possible | L |
| B2 | **`Option<T>` and generic enums** | the library currently uses `Result<_, IOError.NotFound>` for "absent". Changing that later breaks APIs | M |
| B3 | **owning interface values** (`Vec<Shape>`, interface fields) | interface values can only be parameters, so heterogeneous collections are impossible. This is the core of the OOP and game use case | M |
| B4 | **function values / closures** | sorting, callbacks and iteration APIs depend on them. Adding them later means a second API style | L |
| B5 | **collections and text**: `HashMap<K, V>`, sort, a string builder or formatting | these are expected in any 1.0 standard library. `s = s + x` in a loop is O(n²) today | M |
| B6 | **`allowAccess` module permissions** (spec) | the semantics are undecided: which module grants access, and whether `import` implies it. This needs a decision before 1.0 | S (once decided) |
| B7 | **language reference** (grammar, semantics, capability list, ABI) | 1.0 freezes something, and that something needs to be written down. The README is a tour, not a specification | M |

## Not blockers (1.x)

These can come in 1.x releases:
- debug info (`-g`, DWARF line tables)
- a network module (the capabilities already exist)
- the graphics, audio and input libraries from the spec
- `allowExternal` and the OS manifest (needs operating-system support; on Linux, `protect Process` is the part
  that can be enforced)
- generic methods
- UTF-8 character iteration
- macOS and Windows backends, and an own assembler and linker instead of gcc
- formatter, language server, package manager
- more optimizer passes (CSE, LICM, FMA)

## Known issues in rc.1 (documented, not release blockers)

- **Error recovery is coarse.** The checker stops a function at its first error. A syntax error stops the
  compile.
- **Integer overflow wraps silently.** `+ - *` are two's-complement. Checked arithmetic could become an opt-in
  later.
- **Global Strings are not freed at exit.** valgrind reports them as "possibly lost".
- **Async programs confuse valgrind.** They need `--max-stackframe=100000`, otherwise task-stack switches look
  like uninitialised reads.
- **The standard library is trusted.** It skips capability checks internally, so correct `requires`
  annotations on its entry points are the library's responsibility. This is reviewed in `lib/std/*.ag`.

## Evidence (all from `make test` on this build)

- **Examples:** 13 programs compile and run.
- **Negative tests:** 90, each with its exact expected compiler output (`tests/*.err`).
- **Runtime tests:** 15 programs. Each is built with the C and self-hosted compilers at `-O0` and `-O`, its
  output is compared, and each run is under valgrind with no errors and no definite leaks.
- **Command line and stdin, runtime-error messages, and the `--test` runner:** checked with both compilers.
- **`protect Process`:** a real attacker process (`ptrace`, `process_vm_readv`, `/proc/pid/mem`) succeeds
  against the open victim and is denied by the protected one.
- **Identity between the compilers:** lexer, parser, checker (including all error messages) and code generator
  at `-O0` and `-O` are byte-identical on every source file, and both bootstraps are fixed points.
- **Optimizer fuzzing:** 150 random programs per run must behave the same at `-O0` and `-O`. Earlier runs used
  thousands of seeds.
- **String fuzzing:** 160 programs per run (600 in the release check) against a Python oracle, with valgrind
  on every tenth.
- **Benchmarks:** the outputs of all 8 match C (`bench/run.sh`).

## Recommended order

Do B1 first, because names affect everything else. Then B2 and B3 together (generic enums make owning
interface values easier to type), then B4 and B5, then B6 and B7. Release rc.2 after that. Ship 1.0.0 if rc.2
needs no breaking change.
