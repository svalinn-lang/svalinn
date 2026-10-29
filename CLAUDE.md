# Svalinn (formerly Aegis) — notes for Claude Code

A compiled, C-syntax systems language with ownership, reference-counted Strings, classes, interfaces,
generics, Result/`?`, async tasks and a compile-time capability system (`allow File.Read;`, `requires`).
The language was called Aegis until 1.0.0-rc.1; the code is renamed: compiler `svalinnc`, source files `.svl`,
runtime errors `svalinn:`, capability section `.svalinn.caps`. One remnant is intentional: generated
assembly symbols keep the `ag_` prefix (`ag_main`, `ag__drop_X`), shared by both compilers and `lib/rt/*.s`.
Current version: 1.0.0-rc.1. Linux x86-64 only; on Windows the user works inside WSL2.

## Layout
- `src/svalinnc.c` — the bootstrap compiler in C (one file): lex → parse → sema → optimizer (-O) → x86-64
  Intel-syntax assembly, assembled and linked with `gcc -no-pie ... -lm`.
- `selfhost/*.svl` — the SAME compiler written in Svalinn (lex.svl, parse.svl, sema.svl, opt.svl, gen.svl, dump.svl,
  types.svl, ast.svl, err.svl; drivers svalinnc.svl, lexer.svl, parser.svl, semac.svl).
- `lib/rt/prelude.svl` — runtime in Svalinn (Strings, panic, parse), part of every program and trusted.
- `lib/rt/fault.s` (runtime error messages, signal handlers), `lib/rt/async.s` (task runtime).
- `lib/std/*.svl` — standard library: fs, console, env, process, time, math, vec, error (new, capability-
  gated with `requires`), mem, io, sys (low-level, used by the self-hosted compiler).
- `tests/` — negative tests `*.svl` with exact expected output `*.err`; `tests/run/` programs + `.out`;
  `tests/io`, `tests/testmode`, `tests/security`, `tests/fuzz` (optimizer fuzz, String fuzz with a
  Python oracle). `bench/` — benchmarks against C, Rust, Go, Java, Node, Python (results in
  `bench/RESULTS.md`). `examples/*.svl` — sample programs (also built by `make test`).
- `docs/RELEASE-PLAN.md` — the 1.0 release plan (priorities, roadmap, checklist, decisions). It is a copy of
  the shared doc "Aegis 1.0 Release Plan" on claude.ai: when the plan changes, update both.
- `.github/workflows/test.yml` — CI: runs `make test` on every push and pull request.
- `README.md` — user-facing overview of the language as it is today; keep it in sync with the plan's status.

## The one rule that matters
Every compiler change is made TWICE — in `src/svalinnc.c` and in `selfhost/*.svl` — and the two must stay
byte-identical: same token/AST/typed-AST dumps, same error messages, same assembly at -O0 and -O, and both
bootstraps (C → stage 1 → stage 2, also with -O) must be fixed points. `make test` checks all of it.
Port C changes line by line in the same order; when output differs, diff `./svalinnc --dump-sema f.svl`
against `./selfhost/semac f.svl`, or `-S` assembly of `./svalinnc` against `./selfhost/svalinnc1`.

## Commands
- `make` builds `./svalinnc`; `make test` runs everything (about a minute; needs gcc, python3, valgrind).
- `./svalinnc file.svl -o out [-O] [-S] [--test] [--dump-tokens|--dump-ast|--dump-sema] [--version]`
- New negative test: add `tests/x.svl`, generate `tests/x.err` with `./svalinnc tests/x.svl -o /tmp/x 2> tests/x.err`
  and check the message reads well. New runtime test: `tests/run/x.svl` + `tests/run/x.out`.
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
- The repo lives in WSL (`/home/mathias_giller/svalinn`). Git for Windows refuses the `\\wsl.localhost\...`
  path ("dubious ownership"), so run git, make and the compiler inside WSL, e.g.
  `wsl -d Ubuntu --cd /home/mathias_giller/svalinn -- bash -lc "git status"`.

## Decisions already made (summary of `docs/RELEASE-PLAN.md`)
Java-style imports (`import std.fs.File;`, `import std.fs.*;`), module items private unless `public`;
`allowAccess` = a module lists the modules allowed to call it; closures capture by copy/move only;
integer overflow wraps (+ checked_* functions, opt-in --overflow-checks); `char` becomes a 32-bit Unicode
scalar with `s.chars()`; docs generator written in the language itself; MIT license; Linux for 1.0,
Windows is the next platform (before macOS).
P0 before 1.0: module namespaces, enums with payloads + Option, owned interface values, closures,
generic methods, allowAccess, char/overflow rules, language reference. Then std library (HashMap, sort,
formatting), `check --json`, VS Code extension + language server, formatter, `-g`, docs, packaging.
