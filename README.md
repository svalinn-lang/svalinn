# Svalinn 1.0.0-rc.1

Svalinn is a compiled, C-syntax systems language with ownership, reference-counted strings, classes, interfaces,
generics, `Result`/`?`, async tasks and a **capability system**: a program states what it may touch
(`allow File.Read;`, `allow Process.Spawn;` ...) and the compiler rejects every call that needs more.

> **Formerly Aegis.** Svalinn is the shield that stands before the sun in Norse mythology. Up to 1.0.0-rc.1
> the language was called Aegis: the compiler `aegisc` is now `svalinnc`, source files `.ag` are now `.svl`, and
> runtime errors start with `svalinn:` instead of `aegis:`.

`svalinnc` exists twice: in C (`src/svalinnc.c`, one file) and in Svalinn (`selfhost/`). Both emit byte-identical
x86-64 assembly and the self-hosted compiler rebuilds itself to a fixed point. Linux x86-64; gcc is used to
assemble and link.

    make                                   # builds ./svalinnc (needs gcc)
    ./svalinnc examples/hello.svl -o hello && ./hello
    ./svalinnc -O file.svl -o file            # optimized build
    ./svalinnc file.svl -S -o file.s          # assembly only
    ./svalinnc --test file.svl -o t && ./t   # run the file's @test functions
    ./svalinnc --version
    make test                              # the whole test suite (about a minute)

## A first program

    module WordCount;
    allow Console;
    allow File.Read;
    import std.fs;

    fn main(args: Vec<String>) -> i32 {
        for i in 1..args.len() {
            match File.read_to_string(args.item(i)) {
                Ok(text) => { println(args.item(i) + ": " + text.len() + " bytes"); }
                Err(e) => { println(args.item(i) + ": " + io_error_text(e)); return 1; }
            }
        }
        return 0;
    }

Remove `allow File.Read;` and the compiler says:

    wc.svl:7:19: error: calling 'File.read_to_string' requires capability 'File.Read' (add 'allow File.Read;' to the module)
        7 |         match File.read_to_string(args.item(i)) {
          |                   ^

A complete version is `examples/wordcount.svl`.

## Language overview
- modules, `import a.b;` (relative to the source file, then the compiler's `lib/`), include-once
- `let` (immutable) / `var`, type inference, `const N = 4 * 16;` (compile-time integer constants)
- `i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `bool`, `char`, `String`; `as` casts
- structs, struct literals `Point { x: 1, y: 2 }`, enums, fixed arrays `[N]T` (bounds-checked)
- `if`, `while`, `for i in a..b`, `for x in array / string / collection`, `break`, `continue`
- `match` on integers, enums, `Result` and `Option` (exhaustiveness-checked)
- functions with any number of parameters, `ref` parameters, struct/class returns
- classes (constructors, destructors, visibility, static methods, single inheritance, `super`), interfaces
  (dynamic dispatch), generic structs/classes/functions with interface constraints (monomorphised)
- `Result<T, E>`, `Ok`/`Err`, `Option<T>`, `Some`/`None`, `?`
- ownership: values with destructors are moved, never copied; compile-time use-after-move errors
- `async fn`, `await`, `yield_now()`: cooperative tasks with structured lifetimes
- `unsafe` blocks/functions: pointers, `&`, `*`, pointer arithmetic, `extern fn` (C functions)
- `print` / `println` of numbers, bools, chars and strings

## Strings
    let name = "Svalinn";
    let s = "Hello, " + name + "! " + 1 + " " + 2.5 + " " + true;   // other types are converted
    println(s.len());               // bytes
    if s.starts_with("Hello") && s != "x" { println(s.slice(0, 5).to_upper()); }
    for c in "abc" { println(c); }  // the bytes (u8)
    match "42".parse_int() { Ok(v) => { println(v + 1); } Err(e) => { } }
- immutable UTF-8 byte strings, reference-counted: copying a String (assignment, passing, storing in a struct,
  array or `Vec`) shares the bytes; the last owner frees them. Literals are static and never freed.
- `+` concatenates (numbers, bools, chars and enums are converted), `== != < <= > >=` compare the contents,
  `s[i]` is a bounds-checked byte, `str(x)` converts a value
- methods: `len is_empty slice(from, to) find(sub) find_from(sub, start) contains starts_with ends_with
  to_upper to_lower trim replace(from, to) repeat(n) char_at(i) parse_int parse_float`
- `p as String` copies a C string (unsafe); a String passed to an `extern fn` arrives as a NUL-terminated
  `const char *` (never null). `extern fn` cannot return String: return `*u8` and convert.
- the runtime is ordinary Svalinn code: `lib/rt/prelude.svl`

## Capabilities
A module lists what it needs; anything else is a compile error. `deny` always wins over `allow`, and
`allow X;` also grants `X.Anything`. Imported modules cannot declare capabilities: the program decides.

| capability | needed for |
|---|---|
| `Console` | `print`, `println`, `eprint`, `eprintln`; includes `Console.Read`: `read_line()` |
| `File.Read` / `File.Write` | `File.open`, `read_to_string`, `exists` / `File.create`, `append`, `write_string`, `remove` |
| `Env.Read` / `Env.Write` | `env_var` / `set_env_var`, `remove_env_var` |
| `Process.Spawn` | `run`, `run_output` (and `fork`/`exec`/`system` from C) |
| `Process.Debug`, `Process.MemoryRead`, `Process.MemoryWrite`, `Process.Inject`, `Process.Signal` | `ptrace`, `process_vm_readv/writev`, `dlopen`, `kill` |
| `Network` (`.Http`, `.Tcp`, `.Udp`) | sockets from C |
| `Native` | calling any C function (`extern fn`) |
| `Unrestricted` | C functions the compiler does not classify (`syscall`, `dlsym`, ...) |

- library functions declare what they need: `fn open(path: String) -> Result<File, IOError> requires File.Read`;
  every call checks the caller's capabilities. The standard library is trusted: its own C calls are not checked,
  its entry points are.
- `protect Process;` makes the program un-debuggable and its memory unreadable by other processes
  (`prctl(PR_SET_DUMPABLE, 0)`); `tests/security/` proves it with a real attacker process.
- the capability set is embedded in the binary (`.svalinn.caps` section).

## Standard library (`lib/std`, written in Svalinn)
| module | contents | capability |
|---|---|---|
| `std.fs` | `File` (`open create append read_all read_line write write_line flush close`), `File.read_to_string`, `File.write_string`, `File.exists`, `File.remove` | `File.Read` / `File.Write` |
| `std.console` | `read_line() -> Result<String, IOError>`, `read_all_input()`, `eprint`, `eprintln` | `Console` |
| `std.env` | `env_var`, `set_env_var`, `remove_env_var` | `Env.Read` / `Env.Write` |
| `std.process` | `run(cmd, ref args) -> Result<i32, IOError>`, `run_output(...) -> Result<Output, IOError>`, `exit_process`, `process_id` | `Process.Spawn` |
| `std.time` | `now_ms`, `now_secs`, `monotonic_ns`, `sleep_ms` | - |
| `std.math` | `Math.sqrt/sin/cos/tan/atan2/pow/exp/log/floor/ceil/abs/fmod/round/min/max`, `PI`, `E` | - |
| `std.vec` | `Vec<T>`: `push pop get item set len clear`; `for x in v` | - |
| `std.error` | `enum IOError { NotFound, PermissionDenied, AlreadyExists, IsDirectory, InvalidInput, EndOfFile, Closed, Other }`, `io_error_text` | - |
| `std.mem`, `std.io`, `std.sys` | raw C-level helpers (unsafe) used by the self-hosted compiler | `Native` |

The command line arrives in `fn main(args: Vec<String>) -> i32` (`args.item(0)` is the program).

## Tests and attributes
    @test
    fn adds() { if 2 + 3 != 5 { panic("2 + 3 != 5"); } }
    @deprecated
    fn old_api() -> i32 { return 1; }    // every call site gets a warning
`svalinnc --test file.svl -o t` builds a runner instead of `main`: each `@test` function (no parameters, returns
nothing) runs in its own process; a `panic`, failed bounds check or crash fails that test only, and the runner
prints `test name ... ok/FAILED` and a summary, exit code 1 if anything failed.

## Runtime errors
An index out of bounds, integer division by zero, stack overflow or `panic(msg)` flushes the program's output,
prints one line (`svalinn: index out of bounds`, `svalinn: panic: msg`, ...) to stderr and exits with code 101.
Destructors do not run. Integer arithmetic wraps around (two's complement), it never traps.

## Diagnostics
Errors show file, line and column, the source line and a caret. The checker reports one error per function
and keeps going (up to 20), so one compile shows the errors of every function; syntax errors stop at the first.
Every negative test in `tests/*.svl` has its exact expected output in `tests/*.err`.

## Classes
    class Player {
        private:
            let name: String;          // 'let' fields are assignable only in the constructor
            var hp: i32 = 100;         // default values run before the constructor body
        public:
            ctor(name: String) { this.name = name; }
            fn damage(n: i32) { this.hp -= n; }
            static fn maxHealth() -> i32 { return 100; }
    }
    let p = Player("Pilot");           // construct (zero-initialised, then defaults, then ctor)
    p.damage(5);  Player.maxHealth();
- value types on the stack (no hidden heap allocation, no GC); passed/returned by value like structs, `ref` works
- visibility labels `public:` `private:` `protected:` `internal:` (also `public static:`); default is private,
  constructors are public. `protected` is visible in subclasses, `internal` currently behaves like `public`
- methods get an implicit `ref this`; calling a method needs no `var` on the receiver, but direct field assignment does

## Result and `?`
    fn divide(a: i32, b: i32) -> Result<i32, String> {
        if b == 0 { return Err("division by zero"); }
        return Ok(a / b);
    }
    fn calc() -> Result<i32, String> { let x = divide(8, 2)?; return Ok(x + 1); }
    match divide(1, 0) { Ok(v) => { println(v); }  Err(e) => { println(e); } }
- `Result<T, E>` is a tagged value; `T` may be `void` (`Ok()`, matched as `Ok() =>`)
- `Ok(x)`/`Err(e)` take their type from context; `expr?` returns early with the same error type

## Option
    fn find(ref v: Vec<i32>, x: i32) -> Option<i64> {
        for i in 0..v.len() { if v.item(i) == x { return Some(i); } }
        return None;
    }
    fn next(o: Option<i32>) -> Option<i32> { let v = o?; return Some(v + 1); }   // None returns None
    match find(ref v, 7) { Some(i) => { println(i); }  None => { println("missing"); } }
- `Option<T>` is a value that may be absent: `Some(x)` or `None`, both typed by their context
- `match` must handle both `Some(name)` (or `Some(_)`) and `None`, or use `_ =>`
- `expr?` on an Option returns `None` early; it works only in a function that returns an Option

## Interfaces and inheritance
    interface Shape { fn area() -> i64; fn name() -> String; }
    class Rect : Shape { ... public: fn area() -> i64 { ... } fn name() -> String { ... } }
    class Square : Rect { public: ctor(s: i64) { super(s, s); } fn name() -> String { return "square"; } }
    fn report(s: Shape) { println(s.area()); }      // dynamic dispatch through a vtable
    fn biggest<T: Shape>(a: T, b: T) -> i64 { ... }  // constraint: static dispatch, checked at instantiation
- interface values borrow the object, so interface types are only allowed as (non-`ref`) parameters: they can
  never dangle, without a garbage collector or lifetime annotations
- single inheritance; overriding works through interfaces; `super.method()` calls the base version

## Destructors and ownership
    class Conn { ... dtor { close(this.fd); } }
    {
        let c = Conn("db");         // destroyed automatically at the end of the scope
        consume(c);                 // passing by value MOVES c: `consume` now owns it
        // c.query();               // error: use of moved value 'c'
    }
- `dtor { ... }` runs when the owner goes out of scope (also on `return`, `break`, `continue` and `?`), then the
  fields are destroyed, then base class destructors. Temporaries are destroyed at the end of their statement.
- values whose type has a destructor are **moved, never copied**; the checker is flow-aware for `if`/`match`,
  runtime drop flags make conditional moves exact. `ref` borrows without moving.
- Strings are the exception: they are reference-counted, so they copy freely (and structs holding Strings too).

## Generics
    struct Pair<A, B> { first: A; second: B; }
    class Box<T> { public: var value: T;  ctor(v: T) { this.value = v; }  fn get() -> T { return this.value; } }
    fn max<T>(a: T, b: T) -> T { if a > b { return a; } return b; }
    let b = Box<i32>(41);  max(3, 9);  max<i64>(5, 2);
- monomorphised (each instantiation is compiled separately, as fast as hand-written code); type arguments of
  functions are inferred from the call or given explicitly

## Async / await
    async fn worker(name: String, n: i32) -> i32 { ... yield_now(); ... }
    let a = worker("a", 3);          // starts a task, returns a Task<i32>
    println(await a);
- stackful cooperative tasks on one thread (256 KiB stacks with guard pages), deadlock detection; a task is
  joined when its handle goes out of scope (structured concurrency). Runtime: `lib/rt/async.s`.

## Floating point
IEEE 754 `f32`/`f64`, NaN-correct comparisons, exact constant emission, System V ABI for C calls,
`println` prints `%.15g`.

## Optimizer (`-O`)
Programs behave exactly as at `-O0` (same output, same traps), checked by differential fuzzing.
- constant folding and propagation, inlining of small functions, register allocation (rbx/r12-r15, xmm8-15)
- value-range analysis removes provably redundant bounds checks
- addressing modes, strength reduction, compare-and-branch, loop rotation and unrolling, `rep stos` fill loops,
  SSE2 vectorization of element-wise float loops (with a runtime bounds proof)

Benchmarks against C, Rust, Go, Java, Node and Python are in `bench/` (`bench/run.sh`), results in
[`bench/RESULTS.md`](bench/RESULTS.md): geometric mean **1.40x C** (Rust 1.06x, Go 1.65x, Java 1.71x,
Node 2.61x), 4.4x faster than `-O0`.

## Self-hosting
`selfhost/svalinnc.svl` is the whole compiler in Svalinn (lexer, parser, checker, optimizer, code generator,
8 500 lines). The C compiler builds it (stage 1), stage 1 builds it again (stage 2): the assembly of C,
stage 1 and stage 2 is identical, at `-O0` and at `-O`. `make test` also diffs the token, AST and typed-AST
dumps and every error message of the two compilers. Check by hand:
`./svalinnc --dump-sema f.svl` vs `./selfhost/semac f.svl`.

## Tests (`make test`)
examples; 96 negative tests with their exact expected compiler output (`tests/*.err`); runtime tests with expected
output under `-O0`, `-O` and both compilers, each run under valgrind (no memory errors, no leaks); command line,
stdin, runtime-error messages and the `--test` runner; the `protect Process` attack test; lexer/parser/checker/code
generator identity between the compilers; both bootstraps; differential fuzzing of the optimizer; String fuzzing
against a Python oracle (with valgrind). CI runs `make test` on every push and pull request
(`.github/workflows/test.yml`).

## Status and roadmap
This is **1.0.0-rc.1**: the compiler is release quality, the language is not frozen yet. The release plan is
[`docs/RELEASE-PLAN.md`](docs/RELEASE-PLAN.md). 1.0.0 ships in three phases:

1. **Freeze the language (rc.2)**: the P0 items, which may break today's code.
   - module namespaces: Java-style imports (`import std.fs.File;`, `import std.fs.*;`), items private unless
     `public` (today functions and types are global across modules)
   - enums with payloads, `Option<T>`, destructuring `match`
   - owning interface values (`Vec<Shape>`, interface fields and returns)
   - function values and closures (capture by copy or move)
   - generic methods
   - `allowAccess`: a module lists the modules allowed to call it
   - fixed rules for `char` (a 32-bit Unicode scalar, `s.chars()`), `usize`/`isize` and integer overflow
     (wraps, plus `checked_*` and an opt-in `--overflow-checks`)
   - a normative language reference
2. **Complete everything (rc.3)**: `HashMap`/`HashSet`/sort, string formatting and `StringBuilder`, paths and
   random numbers; `check --json`; a VS Code extension with a language server; a formatter; debug info (`-g`);
   parser error recovery and warnings; the documentation book; `make install`, packages and a `svalinn` tool.
3. **Stabilize (1.0.0)**: bug fixes only, then release after two weeks without a breaking change.

Known limitations today: Linux x86-64 only (Windows is the next platform, before macOS; until then use WSL2),
gcc is needed to assemble and link, strings are byte strings, no debug info, one syntax error stops the compile.

## Contributing
Development happens on GitHub at `svalinn-lang/svalinn`. `main` is protected: work on a branch and open a pull
request; it merges when the `test` check is green. Every compiler change is made in both `src/svalinnc.c` and
`selfhost/`, and the two must stay byte-identical (see [`CLAUDE.md`](CLAUDE.md)).

## License
MIT, see [`LICENSE`](LICENSE).
