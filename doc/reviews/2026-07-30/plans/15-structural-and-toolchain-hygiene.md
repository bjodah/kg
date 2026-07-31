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

## Landed / deferred

Landed on `stricter-emacs-adherence` as eight kg commits (`e736bd9` ..
`b526d19`), three `fe/tiny-regex-c` commits (`b9e9dc6`, `5fe0dfb`,
`b92284f`) and two `fe` commits (`bfcddfc`, `52c7ef7`), pins advanced
tiny → fe → kg in that order.  Both submodules' full `.ci` runners are
green at their new pins; kg's twelve-step runner is green at the tip.

### Landed

- **Phase 1 — convention first, then three extractions.**  The
  convention is a bullet in `AGENTS.md`: a module owns `src/<module>.h`,
  self-contained in the shape `src/localvars.h` already had, included by
  `def.h` only when `def.h`'s own definitions need the type, and included
  directly by consumers, which is what IWYU makes them do anyway.
  `make header-check` (in `make check`) compiles every `src/*.h` as the
  first thing in its own translation unit and prints the count, so a
  header that only works after `def.h` cannot pass and a glob that
  matched nothing cannot look like one.
  `def.h` went 1279 → 1160 lines across three mechanical commits:
  `keybind.h` (17 lines, 4 declarations, 3 consumers), `cmd.h` (72 lines:
  `command_prefix`, `cmdfn`, `command_flags`, `named_cmd`,
  `command_origin`, `command_context`, `command_result` and 6
  declarations, 5 consumers), `syntax.h` (62 lines: the `HL_*` types and
  flags, `struct editor_syntax`, 15 declarations, 16 consumers).
  One thing the plan did not predict and the next extraction should
  copy: **spelling rows as `struct erow *` rather than `def.h`'s `erow`
  typedef is what made the syntax extraction cost no include churn at
  all.**  With the typedef repeated in `syntax.h`, IWYU reattributed
  `erow` from `localvars.h` to `syntax.h` and demanded `localvars.h` be
  dropped from twelve files that still legitimately used it.  With the
  tag, `make iwyu` is clean at 36 files with only the sixteen direct
  `syntax.h` includes added.  `hl_color`, dead since the colour table
  became `editor_syntax_to_color()`, went with it.
  scc stayed at exactly 4238/4238 through all four commits: declarations
  carry no branches, and an include guard's `#ifndef` does not match
  scc's `if ` probe.
- **Phase 2 — the last absolute paths.**  `IWYU` and `IWYU_TOOL` in all
  three Makefiles were `/opt-3/iwyu-21/bin/...`, so `make iwyu` anywhere
  else died as "no such file" rather than as a missing tool.  All three
  now use `command -v` with that box as the documented fallback, and each
  recipe checks and names the tool and the variable that overrides it.
  The hosted workflow's two env overrides are deleted, discovery now
  doing their job.  `AGENTS.md` gains the toolchain list derived from
  `.ci/*.sh` and the Makefile: each tool, its override variable, and the
  step that needs it.  `rg` was in the version census and the hosted
  install list and is used by nothing in the tree; it left both.
  Verified while doing this that the *other* tools already fail by name:
  a missing `scc` reports `invalid scc JSON`, a missing `pmccabe`
  reports `no pmccabe output to check -- is pmccabe installed?`.
  `utils/pty_accept.py`'s Emacs fallback and `run-ci-steps.sh`'s
  `/opt-?/cpython-*` activation are deliberately left: both search first,
  both are glob-guarded, both keep going when the box is not that box.
- **Phase 3 — the drift check found two real gaps.**
  `utils/check_help_drift.py` (`make docs-check`, in `make check`) takes
  the key column out of `src/help.c`'s box art and asks whether each
  sequence is spelled anywhere in `doc/kg.1`.  It measures the key
  column's width from the table rather than assuming it (the two halves
  use 9 and 11), expands the `A/B` shorthands the width forces, and
  keeps the six roff/CUA spellings in a table that can be argued with
  rather than tolerating them.  95 keys checked; it prints the count.
  It found `M-Backspace` (backward-kill-word) and `C-z` (suspend) in the
  help table and in the editor and in no key table in kg(1), and `M-d`
  with only a passing mention in prose.  All three are documented now.
  The kill-ring wording is softened rather than left as a TODO: README
  and kg(1) say the ring holds a single entry, and kg(1) says once what
  that means, until plan 13 bundle A makes it false.
- **Phase 4 — the tiny-regex-c leftovers, and fe's numbering.**
  `tests/test_end_anchor.c` is in `TEST_BINS` and in `make test`;
  `tests/test_print.c` is deleted, having called an `re_print()` this
  fork removed with the debug printer (it does not compile, and has not
  for as long as that has been true).  `MAX_CHAR_CLASS_LEN` and
  `MAX_REGEXP_LEN` are gone.  fe's two `ci-06-*` stages are 06/07/08,
  which is both the order the glob was already producing by accident and
  the order fe's contributor guide already claimed; the guide now says
  that a stage number *is* the execution order, in fe, in tiny-regex-c
  and in the parent.
- **Phase 5 — two decision records, no behaviour change.**  Plan 09 has
  the buffer-slot lifecycle policy (hard cap and refusal, never
  eviction; `(id, generation)` identity, which is what closed the
  compilation-index hazard; `*scratch*` ordinary and killable; killing
  the last buffer exits, deliberately), each stated with what it means
  for plans 10 and 12.  Plan 10 has the async-mutation rule the code
  already follows, stated positively, plus the observation that
  auto-revert's current-buffer-only restriction is now stricter than the
  comment defending it — points have been enumerable since plan 09 — and
  that widening it is a behaviour change belonging to whichever plan
  wants it.  `test/pty/90d-compile-output-during-prompt.yaml` is the
  regression case plan 04 never added: a child printing one second in, an
  `M-x` prompt held open across that moment, and afterwards the edited
  buffer, its point and the prompt all as they were.

### Deferred

- **Generating the help table's text from `struct named_cmd::summary`.**
  Reviewer #6's suggestion, and the right end state, but it needs two
  things that do not exist: built-in keys resolving to command names
  (plan 11 phase 3, not started), and a second ~15-column summary per
  command, the table's cells being that wide against the registry's 60.
  The dumb key check is what landed instead; `AGENTS.md` records the
  prerequisite so the next attempt does not rediscover it.
- **The reverse drift direction** (man page key ⇒ help table) is not
  checked and should not be: kg(1) documents far more than one screen of
  help, so "documented but not in help" is the normal state.
- **`README` claims checked against `kg -V`.**  Not attempted.  `kg -V`
  reports one feature bit (`+lisp`/`-lisp`), which `.ci/ci-08` already
  asserts; there is nothing else there to join against, and inventing a
  feature-string format to make a checker possible would be building the
  gate's subject.
- **tiny-regex-c's `SCC_COMPLEXITY_MAX`** is 345 against a measured 336,
  and that slack predates this plan.  Not tightened here: nothing in
  this work improved that number, and taking another plan's headroom
  without earning it is the mirror of raising a cap without paying for
  it.
- **Auto-revert widening** (`src/bufmgr.c:327-333`), which the oversight
  pass raised: written down in plan 10's design note as a conservative
  restriction that outlived its argument, and deliberately not changed.
