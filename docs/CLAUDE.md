# Svalinn (formerly Aegis) — notes for Claude Code

A compiled, C-syntax systems language with ownership, reference-counted Strings, classes, interfaces,
generics, Result/`?`, async tasks and a compile-time capability system (`allow File.Read;`, `requires`).
The code still uses the old name Aegis (`aegisc`, `.ag`, `aegis:` messages); renaming to Svalinn is the
next planned branch (proposed: compiler `svalinnc`, extension `.svl` — confirm with the user first).
Current version: 1.0.0-rc.1. Linux x86-64 only; on Windows the user works inside WSL2.

## Layout
- `src/aegisc.c` — the bootstrap compiler in C (one file): lex → parse → sema → optimizer (-O) → x86-64
  Intel-syntax assembly, assembled and linked with `gcc -no-pie ... -lm`.
- `selfhost/*.ag` — the SAME compiler written in Aegis (lex.ag, parse.ag, sema.ag, opt.ag, gen.ag, dump.ag,
  types.ag, ast.ag, err.ag; drivers aegisc.ag, lexer.ag, parser.ag, semac.ag).
- `lib/rt/prelude.ag` — runtime in Aegis (Strings, panic, parse), part of every program and trusted.
- `lib/rt/fault.s` (runtime error messages, signal handlers), `lib/rt/async.s` (task runtime).
- `lib/std/*.ag` — standard library: fs, console, env, process, time, math, vec, error (new, capability-
  gated with `requires`), mem, io, sys (low-level, used by the self-hosted compiler).
- `tests/` — negative tests `*.ag` with exact expected output `*.err`; `tests/run/` programs + `.out`;
  `tests/io`, `tests/testmode`, `tests/security`, `tests/fuzz` (optimizer fuzz, String fuzz with a
  Python oracle). `bench/` — benchmarks against C, Rust, Go, Java, Node, Python.
- `RELEASE-1.0.0.md` — release review; the release plan lives in a separate doc ("Aegis 1.0 Release Plan").

## The one rule that matters
Every compiler change is made TWICE — in `src/aegisc.c` and in `selfhost/*.ag` — and the two must stay
byte-identical: same token/AST/typed-AST dumps, same error messages, same assembly at -O0 and -O, and both
bootstraps (C → stage 1 → stage 2, also with -O) must be fixed points. `make test` checks all of it.
Port C changes line by line in the same order; when output differs, diff `./aegisc --dump-sema f.ag`
against `./selfhost/semac f.ag`, or `-S` assembly of `./aegisc` against `./selfhost/aegisc1`.

## Commands
- `make` builds `./aegisc`; `make test` runs everything (about a minute; needs gcc, python3, valgrind).
- `./aegisc file.ag -o out [-O] [-S] [--test] [--dump-tokens|--dump-ast|--dump-sema] [--version]`
- New negative test: add `tests/x.ag`, generate `tests/x.err` with `./aegisc tests/x.ag -o /tmp/x 2> tests/x.err`
  and check the message reads well. New runtime test: `tests/run/x.ag` + `tests/run/x.out`.
- `make clean` before committing; build outputs are in `.gitignore`.

## Conventions
- Positions are `(file << 26) | (column << 16) | line`; errors print file:line:col, a source excerpt and
  a caret. The checker recovers per function (up to 20 errors).
- Strings: pointer to NUL-terminated bytes or null (= empty); 16-byte header before the bytes
  ([-16] refcount, -1 = immortal literal; [-8] length). Copies retain, drops release.
- Values with destructors move; Strings copy (refcounted). Runtime errors print one line and exit 101.
- Files in the compiler's `lib/` are trusted: no capability checks inside; their public entry points declare
  `requires`. `safe extern fn` is allowed only there.
- Selfhost code style: one statement per line, `unsafe fn` for compiler internals, `dief(line, fmt, a, b, c)`
  for formatted errors; avoid `x as String` on `*u8` (it copies) — use `strcmp`, `sb_cat_c`.

## Workflow
- `main` is protected on GitHub (svalinn-lang/svalinn): work on a branch, open a pull request, merge when
  the `test` check is green.
- Commit messages: a short summary line, then what and why.

## Decisions already made (release plan)
Java-style imports (`import std.fs.File;`, `import std.fs.*;`), module items private unless `public`;
`allowAccess` = a module lists the modules allowed to call it; closures capture by copy/move only;
integer overflow wraps (+ checked_* functions, opt-in --overflow-checks); `char` becomes a 32-bit Unicode
scalar with `s.chars()`; docs generator written in the language itself; MIT license; Linux for 1.0,
Windows is the next platform (before macOS).
P0 before 1.0: module namespaces, enums with payloads + Option, owned interface values, closures,
generic methods, allowAccess, char/overflow rules, language reference. Then std library (HashMap, sort,
formatting), `check --json`, VS Code extension + language server, formatter, `-g`, docs, packaging.
