# Plan 15 — Structural and toolchain hygiene

Cross-cutting cleanups with no behavior change. Each phase is small and
independently landable; do them opportunistically, but land phase 1's
*convention* before Plans 09–13 start adding modules, or the new modules
will either pile into `def.h` (blowing the per-file scc cap of 520) or
fragment inconsistently.

## Phase 1 — Header convention and `def.h` decomposition

Verified: `src/def.h` is ~1000 lines and is the de-facto header for every
module — there is no `keybind.h`, `cmd.h`, or `syntax.h`; the four
`keybind_*` functions are declared at `def.h:820-824`.

1. Write the convention first (one paragraph in `CLAUDE.md`): a module
   added by Plans 09–13 gets its own `src/<module>.h` with include
   guards, included by `def.h` or by consumers directly; `def.h` keeps
   only the core structs and truly global declarations.
2. Extract in mechanical, compile-verified steps, one commit per header:
   `cmd.h` (command table types), `keybind.h`, `syntax.h`. Move
   declarations only — no renames, no reordering of struct fields.
3. Each new header must compile standalone
   (`gcc -fsyntax-only -include src/<module>.h` or a tiny check target).

Existing exception to preserve: `src/lisp.h` stays the only Lisp-facing
header and `src/lisp.c` the only `fe.h` includer.

## Phase 2 — Tool discoverability

Verified: `utils/pty_accept.py` pins `/opt-3/emacs-31-lucid/bin/emacs`
(fixed by Plan P0 phase 2); `Makefile` pins `IWYU ?= /opt-3/iwyu-21/...`.

1. Every external tool referenced by `Makefile` or `.ci/*.sh` must be
   overridable by environment variable and discovered via `PATH` fallback
   (`?=` plus `command -v`), failing with a message naming the tool.
2. Document the required toolchain (names + tested versions) in
   `CLAUDE.md`/`AGENTS.md`; derive the list from `.ci/*.sh` and the
   Makefile, including `rg` (used by `.ci/ci-06`), GNU `parallel`,
   `pmccabe`, `scc`, `gcovr`/`lcov`, `cbmc` if phase 4 revives it.

## Phase 3 — Documentation coherence

Three plans mutate `README.md`, `doc/kg.1`, `src/help.c`, and
`CLAUDE.md`/`AGENTS.md` (the `C-c <key>`-only binding rule, the `fe.h`
inclusion rule, and README:25's currently false "kill ring" claim). No
plan owns keeping them consistent.

1. Add a drift check: a small script (joins `.ci` via a new numbered
   step, or extends an existing check) asserting that every key sequence
   named in `src/help.c`'s help table appears in `doc/kg.1`, and that
   README claims match `kg -V`-detectable features where feasible. Keep
   it listy and dumb; the goal is catching forgotten updates, not parsing
   English.
2. Fix the current known drift: README/man call the single kill slot a
   "kill ring" (either soften the wording now, or leave a tracked TODO
   that Plan 13 bundle A must close).
3. When a plan changes the `C-c` binding rule (Plan 11 phase 3), the same
   commit updates both `CLAUDE.md` and `AGENTS.md`.

## Phase 4 — tiny-regex-c dead infrastructure

Verified in `fe/tiny-regex-c` (fix in the submodule on its
`adapt-to-fe` branch, then bump pins per the chain kg → fe → tiny):

1. `make verify` (CBMC) does not compile: `re.c:1731` reads `u.ccl`, a
   removed union member; `re.c:1739` passes `regex_t (*)[8]` to
   `re_match()`. Either repair the harness against the current layout or
   delete the target with a commit message saying why; a target that
   cannot build is worse than none.
2. `tests/test_end_anchor.c` and `tests/test_print.c` exist but are not
   in `TEST_BINS`; wire them in or delete them.
3. Remove dead constants `MAX_CHAR_CLASS_LEN`, `MAX_REGEXP_LEN`.
4. `fe/.ci` has two steps numbered `ci-06-*` (fuzz-smoke and
   static-analysis); renumber one so the glob ordering is deterministic,
   and note the convention (numbering = execution order, root and
   subprojects alike) in the fe contributor doc.
5. Do not touch the three expected `(xfail)` layout checks in
   `tests/test_compile.c` (see Plan 06).

## Phase 5 — Buffer-slot and async-mutation policy notes

Not fixes — decision records, so Plans 09/10/12 implement against a
stated policy instead of each inventing one:

1. Buffer slots: `MAX_BUFFERS` 20, `MAX_WINDOWS` 8, fixed arrays, "Too
   many open buffers" refusal, no reuse policy, and
   `compilation_state`'s retained buffer indices are racy against slot
   reuse after `buf_kill`. Write the intended policy (LRU reuse or hard
   cap; `*scratch*` immortality; killing the last buffer) into Plan 09's
   design notes section, with the compilation-index hazard called out.
2. Async mutation: `autorevert_poll()` and `compilation_poll()` mutate
   buffers from inside the input loop, including while minibuffer prompts
   are open (`tty.c:429-436`). State who may mutate a buffer outside a
   keypress, so Plan 10's safe-points and Plan 12's process events have
   one answer. Add the Plan 04 regression case for prompt-time
   compilation output if Plan 04 has not already.

## Definition of done

- New-module header convention documented; `def.h` shrunk by the three
  extractions; headers compile standalone.
- No absolute `/opt-3` path required for any check target.
- Help/man drift check runs in CI; known kill-ring drift resolved or
  tracked.
- `make -C fe/tiny-regex-c verify` builds (or the target is gone), all
  submodule tests wired, pins advanced through fe to kg.
- Policy notes exist and are referenced by Plans 09, 10, and 12.
- Behavior unchanged: `make check`, `WITH_LISP=0`, full CI green.
