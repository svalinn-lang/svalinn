# Svalinn 1.0 Release Plan

As of 2026-10-29. The living version of this plan is the shared doc "Aegis 1.0 Release Plan" on claude.ai;
this copy is in the repository so Claude Code and contributors can read it. Update both when the plan
changes.

Svalinn 1.0.0 ships when the language is frozen, documented end to end and usable in VS Code. Three phases
get there from today's 1.0.0-rc.1 (released as Aegis, now renamed in the code), each ending in a release candidate
that must pass its gate.

## Where rc.1 stands

The compiler is release quality; the language, its documentation and its tooling are not. A 1.0 promises
that code written today keeps compiling, so everything that would change the language or the standard
library API has to land first.

Already done in rc.1:

- **Two identical compilers.** The C compiler and the self-hosted compiler emit byte-identical output, and
  both bootstraps (-O0 and -O) are fixed points.
- **Language core.** Classes, interfaces, generics, Result and `?`, ownership with destructors,
  reference-counted Strings, async tasks, struct literals, `const`, `for x in`, `main(args: Vec<String>)`,
  `@test` and `@deprecated`.
- **Capabilities.** `allow`/`deny`, `requires` on library functions, `protect Process` (attack-tested), and a
  capability-gated standard library (fs, console, env, process, time, math, vec).
- **Diagnostics and runtime errors.** Column and caret, several errors per compile, and clear messages
  instead of signals at runtime.
- **Speed.** 1.40x C on the benchmark set, faster than Go, Java and Node.
- **Evidence.** 90 negative tests with pinned messages, valgrind-clean runtime tests under 4 builds,
  optimizer and String fuzzing, CI on every push.

What 1.0 adds on top: a frozen language (P0 below), a complete standard library and documentation, and
editor support, all in one installable package.

## What is missing, by priority

28 items remain: 8 are P0 blockers that change the language, 11 are P1 work a 1.0 needs, and 9 can wait
for 1.x. Every compiler change is made twice (C and self-hosted) and must keep the two byte-identical.

| # | Item | Priority | Area | Why it matters | Size | Status |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Module namespaces: Java-style imports, qualified names, `public` vs private items | P0 | Language | Names are global today, so any new std function can break user code | L | Not started: `import std.fs.File;` and top-level `public` are rejected |
| 2 | Enums with payloads, `Option<T>`, destructuring `match` | P0 | Language | "Absent" is faked with `Result` and `NotFound`; the std API shape depends on it | M | In progress: `Option<T>` with `Some`/`None`, `match` and `?` done; enums with payloads and destructuring `match` next |
| 3 | Owning interface values (`Vec<Shape>`, interface fields and returns) | P0 | Language | Mixed collections are the core OOP and game case; impossible today | M | Not started: interface types are parameter-only |
| 4 | Function values and closures | P0 | Language | Sort, callbacks and iterators need them; adding them later means a second API style | L | Not started: no `fn` types, no closure syntax |
| 5 | Generic methods (`fn map<U>(...)` in a class) | P0 | Language | Collection APIs depend on them | M | Not started: methods take no type parameters |
| 6 | `allowAccess` module permissions (spec) | P0 | Capabilities | Spec feature; semantics now decided (see Decisions) | S | Not started |
| 7 | Fixed rules for `char`, UTF-8, `usize`/`isize` and integer overflow | P0 | Language | Cannot change after 1.0 without breaking programs | S | Partly: `usize`/`isize` exist and arithmetic wraps; missing 32-bit `char`, `s.chars()`, `checked_*`, `--overflow-checks` |
| 8 | Normative language reference | P0 | Docs | 1.0 freezes what is written down | M | Not started |
| 9 | `HashMap`, `HashSet`, sort, binary search, `Vec` insert/remove | P1 | Std library | Expected in every 1.0 standard library | M | Not started: `Vec` has push/pop/get/set/clear only |
| 10 | String formatting, `StringBuilder`, split/join/lines | P1 | Std library | Building strings in a loop is O(n²) today | M | Not started |
| 11 | Directories and paths, random numbers | P1 | Std library | Basic scripting and games need them | S | Not started |
| 12 | `check` mode and JSON diagnostics | P1 | Tooling | The base every editor integration builds on | S | Not started |
| 13 | VS Code extension with a language server | P1 | Tooling | Requested for the release; see the VS Code section | L | Not started: no `editors/` |
| 14 | Formatter (`fmt`) | P1 | Tooling | One code style for docs, examples and users | M | Not started |
| 15 | Debug info `-g` (DWARF lines and locals) | P1 | Tooling | Breakpoints and stepping in VS Code and gdb | M | Not started |
| 16 | Parser error recovery; warnings for unused names and unreachable code | P1 | Compiler | One syntax error currently stops the compile | M | Not started: a warning mechanism exists (`@deprecated`), unused names are silent |
| 17 | Guide, std API reference, capability reference, error index, docs site | P1 | Docs | Requested for the release; see the docs section | L | Not started: the README is the only user documentation |
| 18 | Install and packaging: `make install`, tarball and .deb, `svalinn new/build/run/test` | P1 | Release | Today the compiler only runs from its source tree | M | Not started |
| 19 | CHANGELOG, versioning and compatibility policy | P1 | Release | Keeps the compatibility promise checkable | S | Partly: CI and MIT license done; no CHANGELOG or policy |
| 20 | Network module (TCP, UDP, HTTP client) | P2 | Std library | Capabilities exist; the library does not | M | Not started (the `Network` capabilities exist) |
| 21 | Threads and async I/O (epoll event loop, timers) | P2 | Runtime | Tasks are cooperative on one thread today | L | Not started |
| 22 | Graphics, audio and input libraries (spec example) | P2 | Std library | The spec's game use case | L | Not started |
| 23 | `allowExternal` and the OS manifest | P2 | Capabilities | Needs operating-system support beyond the compiler | L | Not started |
| 24 | Windows backend first (PE files, Windows x64 calling convention), then macOS and ARM64; own assembler and linker | P2 | Compiler | Linux only today; Windows comes before macOS | L | Not started |
| 25 | Package manager and registry | P2 | Tooling | Sharing libraries between projects | L | Not started |
| 26 | Optimizer: CSE, loop-invariant motion, FMA/AVX, tail calls | P2 | Compiler | Closes the remaining gap to C | M | Not started (loop-invariant scalars are only hoisted inside the float vectorizer) |
| 27 | Checked-arithmetic build mode, slices and views | P2 | Language | Additive; can come in 1.x | M | Not started |
| 28 | Tree-sitter grammar, JetBrains plugin, rename and refactoring in the language server | P2 | Tooling | Other editors; VS Code comes first | M | Not started |

Size: S = one feature and its tests; M = several files in both compilers plus tests; L = a new subsystem.
Status was checked against the compiler on `main` (2026-10-29) with a small program per feature; an item is
marked **Done** when it is merged with tests, and the table is updated in the same pull request.

## Release roadmap

Three phases end in three gates. Phase A changes the language and ends at rc.2, after which nothing
breaking is allowed. Phase B completes the library, the editor support and the docs, ending at rc.3.
Phase C only fixes bugs, and 1.0.0 ships after two quiet weeks.

| Workstream | A · Freeze the language | B · Complete everything | C · Stabilize |
| --- | --- | --- | --- |
| Language | Namespaces and visibility; enums with data, Option; closures, generic methods; owned interfaces; allowAccess, char rules | Parser error recovery; warnings; check mode, JSON output; debug info (-g) | Bug fixes only |
| Std library | Port std to namespaces; Option-based APIs | HashMap, HashSet, sort; formatting, StringBuilder; paths, random, std.json | Bug fixes only |
| Editor tooling | Tier 1: highlighting, snippets, Run and Test | Tier 2: language server; Tier 3: debugger, fmt | Publish to Marketplace and Open VSX |
| Documentation | Getting started and tour of the stable parts | Reference, std API, capabilities, error index; cookbook, docs site | Final review; all samples pass in CI |
| Release | CI on every commit (done), versioning, license (done) | make install, .deb; `svalinn new/build/run/test` | Clean-machine installs; CHANGELOG |

| Gate | Criteria |
| --- | --- |
| rc.2 | All P0 merged; reference draft |
| rc.3 | All P1 merged; docs; VS Code tiers 1–3 |
| 1.0.0 | Release checklist complete; two weeks without a breaking change |

Editor tier 1 and the getting-started docs can begin now; the language server and the reference wait for
rc.2, because they depend on the final names and syntax.

## Syntax and language documentation

The docs ship as a book in `docs/` (Markdown), rendered to HTML, bundled in the release and published
online. Every code sample is compiled and run by `make test`, so the docs cannot drift from the compiler.

| Part | Contents | Kind |
| --- | --- | --- |
| 1. Getting started | Install, hello world, the compiler's command line, project layout, `svalinn new/build/run/test`, setting up VS Code | Tutorial |
| 2. Language tour | One chapter each: types and literals, `let`/`var`/`const`, functions and `ref`, control flow, structs and literals, enums and `match`, arrays and `Vec`, Strings, classes and visibility, inheritance and interfaces, generics, ownership and destructors, `Result` and `?`, modules, capabilities, `unsafe` and C interop, async, testing | Guide |
| 3. Language reference | Lexical structure (tokens, literals, escapes, comments); the complete grammar in EBNF; types and conversions; operator precedence; statements and items; move and ownership rules; capability rules; memory layout and calling convention; runtime errors; what behaviour is defined | Normative spec |
| 4. Standard library reference | One page per module: every type and function with its signature, required capability, errors and an example; generated from `///` doc comments by the compiler's `doc` command | Generated |
| 5. Capability reference | Every capability, what it grants, what implies what, the classification of C functions | Reference |
| 6. Error index | Every compiler error with a code (`E0042`), a program that triggers it and the fix; error messages link to it | Reference |
| 7. Cookbook and examples | Command-line tools, text processing, files, running processes, a game loop skeleton; the `examples/` gallery | Recipes |
| 8. Internals | Compiler architecture, the two compilers, bootstrap, tests, how to contribute | Contributor guide |

How the docs stay correct:

- **Tested samples.** `tests/docs/run.sh` extracts every code block. Each must compile; a block ending in
  `// output:` lines must print exactly those lines; a block marked `fail` must give the error it names.
- **Generated API pages.** The standard library reference comes from the source, so a signature in the docs
  is always the real one.
- **One grammar.** The EBNF in part 3 is the source of truth; every production gets a passing and a failing
  test in `tests/`.
- **Error codes.** The error index and the compiler share one table, so every error has a page and no page
  is orphaned.

## VS Code and IDE support

Ship a VS Code extension backed by a language server, not a separate IDE. The server speaks the Language
Server Protocol, so the same work also gives Neovim, Helix, Zed, Sublime Text and JetBrains IDEs (through
their LSP plugins) the same features.

| Tier | What the user gets | How it works | Ships in | Size |
| --- | --- | --- | --- | --- |
| 1 | Syntax highlighting, bracket matching and comment toggling, snippets (`fn`, `class`, `match`, `@test`), Run and Test commands, errors in the Problems panel on save | TextMate grammar and language configuration; commands call the compiler and its `check --json` mode | rc.2 | S |
| 2 | Errors as you type, hover with types, go to definition, document outline, completion (locals, members, imports, capability names), signature help | An `lsp` mode of the compiler: a language server over stdio, built on the self-hosted compiler's front end, re-checking the open file on each change | rc.3 | L |
| 3 | Breakpoints, stepping and variables in the debugger; format on save | `-g` emits DWARF; a launch configuration for the C/C++ (gdb) or CodeLLDB debugger; `fmt` as the formatter | 1.0.0 | M |
| 4 | Rename, find references, quick fixes ("add `allow File.Read;`"), semantic highlighting, inlay type hints; Tree-sitter grammar | Extends the same server | 1.x | M |

What it takes:

- **Compiler hooks.** A `check` mode that stops after the checker, JSON output for diagnostics, and a symbol
  index (definitions, types, ranges) that the server queries.
- **A JSON module.** The language server is written in Svalinn, so `std.json` (parse and print) joins the
  standard library.
- **Packaging.** The extension lives in `editors/vscode/`, is published to the Visual Studio Marketplace and
  Open VSX, and a `.vsix` file ships in the release tarball.
- **Windows.** Until the native Windows port lands, the extension runs on Windows through VS Code's WSL
  extension, with the compiler inside WSL2.
- **Tests.** Grammar snapshot tests for highlighting, and scripted JSON-RPC sessions against the language
  server that run in `make test`.

## Release checklist

1.0.0 is done when every box is ticked on the final release candidate.

- [ ] All P0 items merged, and the language reference describes exactly what the compiler accepts
- [ ] All P1 items merged
- [x] `make test` runs in CI on every commit
- [ ] Both compilers byte-identical and both bootstraps fixed points on the final candidate
- [ ] Every documentation part written; every code sample in the docs compiles and prints what it claims
- [ ] Standard library reference generated with no undocumented public function
- [ ] VS Code extension tiers 1–3 published on the Marketplace and Open VSX
- [ ] Release tarball installs on clean Ubuntu, Debian and Fedora machines and in WSL2 on Windows; hello
      world, tests and the debugger work there
- [ ] Benchmarks rerun with no regression above 5% against rc.1
- [ ] Fuzzing clean at release scale: 10,000 optimizer seeds and 2,000 String seeds
- [ ] Capability review: every standard library entry point carries the right `requires`; `protect Process`
      and capability tests pass
- [ ] CHANGELOG, versioning and compatibility policy published (license done: MIT)
- [ ] Two weeks on the final release candidate without a breaking change

## Decisions

All decisions are made, and the code is renamed from Aegis to Svalinn: the compiler is `svalinnc`, source
files end in `.svl`, and a `svalinn` build tool follows later.

| # | Decision | Outcome |
| --- | --- | --- |
| 1 | Imports and visibility | Java-style imports: `import std.fs.File;` brings in one name, `import std.fs.*;` all public names, and a fully qualified `std.fs.File.open(...)` always works. Module items are private unless marked `public`, the same keyword classes already use |
| 2 | What `allowAccess` means | A module lists the modules allowed to call it (`allowAccess Inventory;`); calls from anywhere else are compile errors. Blocking other processes from reading memory beyond `protect Process` needs operating-system support, or in the long run an operating system written in Svalinn |
| 3 | Closure captures | Copy or move only; a closure that borrows with `ref` cannot be stored or returned |
| 4 | Integer overflow | Wraps everywhere; `checked_add` and friends; opt-in `--overflow-checks` |
| 5 | `char` and Unicode | `char` is a 32-bit Unicode scalar; `s.chars()` walks UTF-8; `s[i]` and `s.bytes()` stay byte-level |
| 6 | Docs tooling | HTML generator written in Svalinn on top of the compiler's `doc` command |
| 7a | Version control | Git on GitHub (`svalinn-lang/svalinn`), developed in VS Code with Claude Code; `main` is protected by a ruleset: pull requests only, the `test` check must pass |
| 7b | The language's name | Svalinn, the shield that stands before the sun in Norse mythology. The GitHub name `svalinn` belongs to a nuclear-engineering tools group, so the organisation is `svalinn-lang` |
| 7c | License | MIT |
| 8 | Platforms | Linux for 1.0. Windows is the first platform after Linux, ahead of macOS; until then Svalinn runs on Windows inside WSL2 |
