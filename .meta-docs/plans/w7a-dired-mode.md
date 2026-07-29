# W7-A Implementation Plan — Dired mode

> **SUPERSEDED (2026-07-29)** by
> [dired-mode-c-core.md](file:///work/.meta-docs/plans/dired-mode-c-core.md).
> This draft predates the kg- prefix removal, the Emacs-shaped Lisp API
> rework, and the C-built-in precedent set by the git commit and git
> rebase modes; its primitive names and detection pattern are stale.
> Kept for the bare-key analysis, which still holds.

> Target audience: a junior developer new to this codebase, ideally one who
> has already read
> [w7b-git-commit-mode.md](file:///work/.meta-docs/plans/w7b-git-commit-mode.md)
> (W7-B, implemented) since this plan reuses its detection pattern.
> Parent plan: [wishlist-implementation-plan.md](file:///work/.meta-docs/plans/wishlist-implementation-plan.md) (W7, Option A).
> Design source: [doc/WISHLIST.md](file:///work/doc/WISHLIST.md) and the
> `wishlist-implementation-plan.md` W7 section (Option A sketch).
> Estimated effort: 20–28 hours, split into 8 phases that each leave the
> tree green (`make check` passes after every phase). Larger than W7-B
> because it needs new Lisp primitives *and* a new C-side key-dispatch
> mechanism that does not exist yet (see "The bare-key problem" below).

## What you are building

`M-x dired` (and a `C-x d` binding) opens a read-only buffer listing a
directory's entries, one per line, with Emacs-like navigation:

1. **Detection**: a "Dired" mode/buffer named `*Dired: /path/*`, shown in
   the mode line, entered via an `M-x dired` command that prompts for a
   directory (reusing the existing path-picker prompt).
2. **Listing**: one entry per line, directories suffixed with `/`,
   produced by shelling out to `ls` (or a minimal in-Lisp formatter over
   `(kg-shell "ls -1p ...")` output — no new directory-reading C code).
3. **Navigation**: `RET` opens the file/directory at point (a directory
   re-runs dired on the new path; a file switches to a normal buffer).
4. **Marking**: `d` marks the entry at point for deletion (visual marker
   in column 0); `u` unmarks; `x` deletes every marked file after a
   confirmation prompt and refreshes the listing.
5. **Refresh**: `g` re-runs the directory listing in place.
6. **Up a level**: `^` (caret) opens the parent directory.

Out of scope (do **not** build these):

- No recursive delete of marked directories — `x` shells out to `rm` for
  plain files only; a marked directory is skipped with a warning message
  (matches the "no sandboxing, but don't build a footgun" spirit of
  [doc/TODO.md](file:///work/doc/TODO.md)'s Lisp trust model).
- No rename/copy/chmod, no multi-directory dired, no wildcard matching,
  no sorting toggles. Emacs dired has decades of features; this is a
  minimal read-mostly listing with delete.
- No new mode.c "major mode" framework. Like W7-B, ride the existing
  per-buffer `editor.syntax` pointer for mode identity.

## The bare-key problem (read this before writing any code)

W7-B's `C-c C-c` / `C-c C-k` worked as **built-in** C-side interceptions
because kg's key-binding system has a hard invariant, enforced in
`src/keybind.c` and restated in `CLAUDE.md`:

> User key bindings go through `src/keybind.c`, the single canonical
> key-sequence parser; only "C-c \<key\>" sequences are bindable.

`keybind_parse()` (keybind.c:30) rejects anything that is not `C-c` followed
by exactly one more key. Dired wants bare `RET`, `d`, `u`, `x`, `g`, `^` —
none of which fit that shape, and none of which should steal those keys
globally (only inside a dired buffer).

**Resolution, following the W7-B precedent exactly:** dired is a
syntax-flag-detected mode (`SHL_DIRED`, mirroring `SHL_GITCOMMIT`), and the
handful of bare keys it needs are intercepted **in C**, inside the
existing `if (editor.readonly) { ... }` block in
`editor_process_keypress()` (kbd.c:409-419) — dired buffers are read-only,
so this block already runs for every dired keypress. That block already
hardcodes one such case: `if (c == ENTER) { buf_ibuffer_select(); return; }`
for the `*Buffer List*`/`*IBuffer*` buffer. Dired adds a second,
syntax-flag-gated branch alongside it.

The actual *behavior* behind each key (listing, marking state, deletion)
still lives in Lisp, via `(kg-define-command "dired-find-file" (fn () ...))`
etc. The C-side dispatcher is a **fixed, 6-entry lookup table** translating
`{RET, d, u, x, g, ^}` to 6 fixed command names, then calling
`cmd_execute_named(name, fd)` — the same function `M-x` already uses. Two
properties fall out of this for free, without any `KG_USE_LISP`
conditional in kbd.c (which must stay Lisp-free per `CLAUDE.md`):

- `cmd_execute_named()` already falls through to `kg_lisp_run_command()`
  for names not in the static C command table (cmd.c:616-630), and that
  function returns 1 (a harmless no-op) whenever Lisp is inactive or the
  name is not a registered Lisp command (lisp.c:787-799, and the
  `WITH_LISP=0` stub at lisp.c:891-896). So `make WITH_LISP=0 clean all`
  keeps building and dired keys become silent no-ops — no crash, no
  special-casing needed.
- If a user opens dired *without* `dired.fe` loaded (e.g. `kg -Q`), the
  same fallthrough applies: keys do nothing instead of erroring. Consider
  having `dired-mode` (the `M-x` command that opens the buffer, see Phase
  3) check `kg_lisp_active()` — no, wait, `cmd.c` is compiled either way;
  the check that matters is whether `(kg-load "dired")` succeeded, which
  is a Lisp-side concern in `dired.fe`'s own init.

## Read these first

- `AGENTS.md` (CLAUDE.md) — build/test workflow, style rules, and the
  bindability invariant above.
- [w7b-git-commit-mode.md](file:///work/.meta-docs/plans/w7b-git-commit-mode.md)
  — the sibling plan this one's detection/highlighting pattern is copied
  from. Read it fully before starting; several phases below say "same as
  W7-B phase N" rather than repeat the explanation.
- `src/lisp.c:155-250` — the native-primitive pattern (`native_message`,
  `native_insert`, `native_point`, `native_goto`): argument extraction via
  `FeGetNextArgument`/`FeRequireNoArguments`, error handling via
  `FeHandleError`, and the `register_natives()` registration block
  (lisp.c:593-606) you will extend.
- `src/lisp.c:252-379` — `allowed_commands[]` and `native_command()`: the
  allow-list `(kg-command NAME)` checks against. You will add entries.
- `src/bufmgr.c:1245-1368` — `buf_open_special()`, `buf_open_list()`,
  `buf_open_help()`, `buf_ibuffer_select()`: the existing pattern for a
  named, syntax-tagged, read-only, re-populatable buffer. Dired's
  `(kg-switch-buffer NAME)` primitive (Phase 1) is a Lisp-callable,
  generalized version of `buf_open_special()`.
- `src/shell.c:174-230` — `shell_run()`: fork/exec `/bin/sh -c CMD` with
  piped stdin/stdout, already used by `M-!` and `M-|`. Reused verbatim by
  the new `(kg-shell CMD)` primitive.
- `src/kbd.c:409-419` — the `editor.readonly` block where the bare-key
  dispatch table is added, right next to the existing ibuffer `ENTER`
  case.
- `src/keybind.c` (whole file, it's short) — confirms why bare keys cannot
  go through `kg-bind-key`.
- `fe/fe.h` and `doc/fe-upstream.md` — Fe's C API surface kg is allowed to
  use (`FE_API_VERSION 1`); do not add new Fe core features, only new
  `kg-*` natives in `src/lisp.c`.
- `test/test_lisp.c` — the native harness pattern for testing new
  primitives without a real terminal.
- `utils/pty_accept.py` and existing `test/pty/lisp-*.yaml` cases,
  especially ones using `config_files:` to plant `init.fe`/packages — the
  pattern the dired PTY cases (Phase 8) will follow.

## Key design decisions (already made — follow them)

| Decision | Rationale |
|---|---|
| Dired is detected the same way git-commit mode is: `editor.syntax->flags & SHL_DIRED`, set on a dedicated `HLDB` entry. | Consistency with W7-B; `syntax_is_git_commit()` / `syntax_is_dired()` become a matched pair, and both mode line and highlighting come for free from existing machinery. |
| Dired's `HLDB` entry has **no filename patterns** (`filematch = NULL` is not allowed by existing code — see Phase 2 note) and is never auto-selected by `editor_select_syntax_highlight()`. It is assigned explicitly, in C, when the buffer is created. | Unlike git-commit files, dired buffers aren't opened from disk by path; they're synthesized. Reusing the filename-matching path would require inventing a fake filename convention and risk false-positive matches. |
| The bare-key dispatch table lives in `kbd.c`, is fixed at 6 entries, and calls `cmd_execute_named()` — never a raw Lisp eval, never a direct `kg_lisp_run_command()` call. | Keeps kbd.c free of Lisp-specific control flow (`cmd_execute_named` already owns that boundary), and means `M-x dired-mark` works identically to pressing `d`, so the commands are independently testable and documuented in one place (`M-x` completion). |
| New Lisp primitives are content/shell/buffer primitives only: `kg-line-content`, `kg-buffer-line-count`, `kg-shell`, `kg-switch-buffer`, `kg-set-readonly`, `kg-clear-buffer`. No new primitive is dired-specific. | Matches the wishlist's Option A step list and keeps the C surface reusable for future packages (a future `magit-status.fe` could use the same primitives). |
| `(kg-shell CMD)` reuses `shell_run()` (shell.c) verbatim: `/bin/sh -c CMD`, no argv-array form, stdin empty, stdout captured, stderr discarded. | Matches `M-!`'s existing trust model (kg already documents that Lisp/M-! run arbitrary shell commands with full privileges — no new attack surface). Do not add a `kg-shell-argv` variant; out of scope. |
| Deletion (`x`) shells out to `rm --` (not `rm -rf`) so marked directories fail loudly instead of being recursively deleted by accident. | Matches the "no recursive delete" scope cut above; a failed `rm` for a directory produces a clear error via `kg-shell`'s captured output, shown with `kg-message`. |
| `dired.fe` ships at `share/kg/site-lisp/dired.fe` and is loaded explicitly via `(kg-load "dired")` from the user's `init.fe`, or lazily on first `M-x dired` if the `dired-find-file` command isn't yet defined. | Matches the existing `(kg-load "name")` convention (`doc/kg.1` L56-61); kg ships no packages by default today (`wishlist-implementation-plan.md` W7 "Current state": "No `.fe` packages ship with kg"), and this plan does not change that default-off posture. |

---

## Phase 1 — Lisp primitives: buffer content and shell (`src/lisp.c`)

Add four natives, following the `native_message`/`native_insert` pattern
(lisp.c:155-198) exactly: extract arguments with `FeGetNextArgument`,
call `FeRequireNoArguments(context, arguments)` last, return `FeNil` or a
`FeMakeString`/`FeMakeDouble`.

1. `(kg-line-content [ROW])` — `native_line_content`. With no argument,
   return the current row's text as a string (1-based row like
   `kg-point`, so `ROW` present means `editor.row[(int)ROW - 1]`). Out of
   range → `FeHandleError`. Use `FeMakeString(context, row->chars)` —
   `row->chars` is not NUL-safe against embedded NULs but neither is any
   other kg buffer content today, so this matches existing behavior.
2. `(kg-buffer-line-count)` — `native_line_count`. Return
   `FeMakeDouble(context, (FeDouble)editor.numrows)`.
3. `(kg-shell CMD)` — `native_shell`. Extract `CMD` as a string via
   `copy_fe_string` (the helper `native_insert` already uses). Call
   `shell_run(cmd, NULL, 0, &out_len)` (shell.c:174; confirm the `in`
   parameter accepts `NULL` for empty stdin — check `shell.c` around the
   `in_wr` pipe write loop before assuming so). Free `cmd`. Return the
   captured stdout as a `FeMakeString`; free the buffer from
   `shell_run()`. On a `shell_run` failure (non-NULL error path), surface
   it via `FeHandleError`, matching how `native_insert` surfaces
   read-only errors.
4. `(kg-set-readonly BOOL)` — `native_set_readonly`. `FeToBool`-style
   check (see how other natives coerce Fe truthiness — Fe's nil is
   falsy, everything else truthy per `fe.h`); set `editor.readonly`
   directly. Return `FeNil`.

Register all four in `register_natives()` (lisp.c:593-606).

**Tests**: extend `test/test_lisp.c` with eval-based cases:
`(kg-line-content)` on a pre-seeded row, `(kg-buffer-line-count)` on a
multi-row buffer, `(kg-shell "echo hi")` returns `"hi\n"`,
`(kg-set-readonly t)` then attempt an edit and confirm it's blocked (or
just assert `editor.readonly == 1` if the test harness doesn't drive real
keypresses — check how existing lisp tests assert editor state).

Run `make check` before moving on — these four primitives have no
callers yet, so nothing else should change behavior.

## Phase 2 — Buffer creation and clearing primitives (`src/lisp.c`, `src/bufmgr.c`)

1. `src/bufmgr.c` — factor a non-`static` entry point out of
   `buf_open_special()` (bufmgr.c:1246), or add a thin new function
   `buf_open_named(const char *name, struct editor_syntax *syn)` that
   calls the existing static `buf_open_special()` with a no-op `populate`
   (an empty function pointer target) and a fixed status message. Declare
   it in `def.h` under `/* bufmgr.c */`. This is the C entry point
   `(kg-switch-buffer NAME)` calls into — the Lisp caller inserts content
   itself afterward via `(kg-insert ...)`/`editor_insert_row`-backed
   primitives, it does not pass a populate callback (Fe closures cannot
   cross the Lisp/C boundary as C function pointers).
2. `(kg-switch-buffer NAME)` — `native_switch_buffer` in lisp.c. Calls
   `buf_open_named(name, NULL)` (syntax assigned separately, see Phase 3)
   and returns `FeNil`.
3. `(kg-clear-buffer)` — `native_clear_buffer`. Frees every row
   (`editor_free_row` in a loop, mirroring `buf_open_special()`'s own
   clear-and-repopulate block at bufmgr.c:1283-1288) and resets
   `editor.numrows = 0`, `editor.cx = editor.cy = 0`. Must work whether or
   not the buffer is currently read-only — temporarily clear
   `editor.readonly`, do the row deletion, restore it, since
   `editor_free_row` itself doesn't consult the read-only flag but any
   future safety check might; be explicit rather than relying on that.

Register both. Add `share/kg/lisp/` (or wherever `(kg-load ...)` resolves
relative to, per `doc/kg.1`'s `<config>/kg/lisp/name.fe` — confirm the
exact base directory in `bufmgr.c`/`lisp.c`'s load-path resolution code,
`doc/kg.1` L56-59 says `<config>/kg/lisp/name.fe`, not a repo-shipped
`share/` path — **this plan's own "ships at share/kg/site-lisp/dired.fe"
line in the decisions table above needs reconciling with that at
implementation time; trust the code over this plan if they disagree**) to
your notes for Phase 7.

**Tests**: `test/test_lisp.c` cases creating a named buffer via eval,
confirming `editor.filename` and `buf_count` change, then clearing it and
confirming `editor.numrows == 0`.

## Phase 3 — Dired detection and syntax entry (`src/def.h`, `src/syntax.c`)

Mirrors W7-B Phase 2 almost exactly.

1. `def.h`: `#define SHL_DIRED (1 << 5)` (next free bit after
   `SHL_GITCOMMIT`). Declare `[[nodiscard]] int syntax_is_dired(void);`.
2. `syntax.c`: add a `DIRED_HL_extensions[] = { NULL };` — **check
   whether `editor_select_syntax_highlight()`'s loop
   (syntax.c:1357-1370ish, look for the current line numbers post-W7-B)
   tolerates an empty filematch array; it currently does `while
   (s->filematch[i])` with no bounds check, so `{ NULL }` is safe (loop
   body never runs) but confirm nothing else dereferences `filematch[0]`
   unconditionally elsewhere (e.g. help text generation, `-V` flag
   handling) before relying on this.**
3. Append the `HLDB[]` entry: `{ "Dired", DIRED_HL_extensions, NULL, "",
   "", "", SHL_DIRED }`. Append-only, same reasoning as W7-B (test
   indices).
4. `syntax_is_dired()`: `return editor.syntax && (editor.syntax->flags &
   SHL_DIRED);` — copy `syntax_is_git_commit()` verbatim with the flag
   swapped.
5. No highlighter function needed yet (Phase 6 adds minimal coloring for
   marked/directory entries — decide there whether it's worth a
   `dired_syntax()` dispatch case or just plain text).

**Tests**: `test/test_syntax.c` — one test confirming `HLDB[N].name ==
"Dired"` at the new fixed index (guard against future index drift, same
idiom as the Makefile/Markdown/Git-commit guards), and that
`syntax_is_dired()` is false by default and true once `editor.syntax =
&HLDB[N]`.

Run `make check`.

## Phase 4 — `M-x dired` and buffer setup (`src/lisp.c` or a small new `src/dired.c`?)

**Decision point for the implementer**: the actual "list a directory"
logic could live in C (a new `cmd_dired(int fd)` static command in
`cmd.c`, prompting for a directory path via the existing
`editor_read_line_path()` picker, then calling `shell_run()` directly)
*or* be pure Lisp (`dired.fe` defines `dired-mode` as a
`kg-define-command`, and it is *that* Lisp command, not a new C
`cmdtable[]` entry, that users invoke via `M-x dired`).

**Recommendation: pure Lisp.** It matches the wishlist's framing ("Fe
packages: dired-mode") and needs zero new C beyond Phase 1-3's primitives
plus one more:

1. `(kg-buffer-name)` already exists (lisp.c:200) — reused to detect
   "am I in a dired buffer" from Lisp if needed, though `dired.fe` will
   more likely track directory state in a Fe global/closure.
2. New primitive: `(kg-prompt-directory)` or reuse a generic
   `(kg-read-line PROMPT)` if one doesn't already exist — **check first**:
   grep `native_` for anything wrapping `editor_read_line`/
   `editor_read_line_path` before adding a new primitive; if nothing
   exists, add `native_read_line_path` wrapping
   `editor_read_line_path()` (bufmgr.c, used by `C-x C-f`'s picker) so
   `dired-mode` gets the same ido-style directory picker as every other
   file prompt, instead of a bare `editor_read_line`.
3. `dired.fe` skeleton (Phase 7 has the full listing):
   ```
   (kg-define-command "dired-mode"
     (fn ()
       (let (dir (kg-prompt-directory))
         (dired--open dir))))
   ```
   with `dired--open` calling `(kg-switch-buffer (string-append "*Dired: " dir "*"))`,
   `(kg-clear-buffer)`, `(kg-shell (string-append "ls -1p " dir))`
   split into lines and inserted one per row, then `(kg-set-readonly t)`.
   Exact Fe string/list primitives available (`string-append`, line
   splitting) need to be checked against `fe/fe.h`'s builtin set — Fe is
   a small Lisp, it may not have a built-in line-splitter; if not, do the
   splitting in C (a fifth Phase 1 primitive, `(kg-shell-lines CMD)`
   returning a Fe list of line strings) rather than hand-rolling string
   scanning in Fe.
4. **Missing piece**: nothing yet sets `editor.syntax = &HLDB[dired_idx]`
   on the new buffer — `kg-switch-buffer` (Phase 2) creates it with
   `syntax = NULL`. Add a fifth primitive, `(kg-set-mode NAME)`, doing a
   linear `HLDB[]` name lookup (mirrors `editor_select_syntax_highlight`'s
   loop shape) and assigning `editor.syntax`, OR simpler: give
   `kg-switch-buffer` an optional second argument
   `(kg-switch-buffer NAME MODE-NAME)`. Pick whichever keeps the Fe side
   simpler; document the choice here once made (this plan intentionally
   leaves it open — both are small).

**Tests**: a PTY case (promoted from Phase 8, write it now since it's the
natural checkpoint) that loads a minimal `dired.fe` via `config_files:`,
runs `M-x dired-mode`, types a known temp directory path, and asserts
`expected_screen_contains: ["Dired"]` (tmux backend, mode line check —
same shape as W7-B's optional 6e case).

## Phase 5 — Bare-key dispatch table (`src/kbd.c`)

1. Add a small static table near the top of `kbd.c`:
   ```c
   static const struct { int key; const char *command; } dired_keys[] = {
       { ENTER, "dired-find-file" },
       { 'd', "dired-mark" },
       { 'u', "dired-unmark" },
       { 'x', "dired-execute" },
       { 'g', "dired-refresh" },
       { '^', "dired-up-directory" },
   };
   ```
2. In the `if (editor.readonly) { ... }` block (kbd.c, the block
   containing the existing `if (c == ENTER) { buf_ibuffer_select();
   return; }` case), add *before* the generic
   `key_would_edit_readonly_buffer(c)` rejection:
   ```c
   if (syntax_is_dired()) {
       size_t i;
       for (i = 0; i < sizeof(dired_keys)/sizeof(dired_keys[0]); i++) {
           if (c == dired_keys[i].key) {
               (void)cmd_execute_named(dired_keys[i].command, fd);
               return;
           }
       }
   }
   ```
   Ordering matters: this must run before the existing bare `ENTER` →
   `buf_ibuffer_select()` case, or add a `!syntax_is_dired()` guard to
   that case instead — a buffer cannot be both ibuffer and dired
   (mutually exclusive syntax pointers), but the *first matching branch*
   in an `if`-chain wins, so be deliberate about order and cite this
   plan's reasoning in a comment when you do it, since a future reader
   won't otherwise know the two are intentionally exclusive.
3. Confirm `cmd_execute_named` is already declared in `def.h` (it is,
   `/* cmd.c */` block) — no new header work needed.

**Tests**: PTY cases only (bare-key dispatch needs a real buffer and
keypress loop — not reachable from `test/test_kbd.c`-style stubs if that
harness doesn't wire up `editor.readonly`/`editor.syntax` convincingly;
check whether one exists before assuming you need a new C unit test
file). Write one case per key in Phase 8, or fold them into the single
end-to-end dired PTY case if that keeps the suite smaller (matches
`CLAUDE.md`'s "keep changes small" spirit — prefer fewer, higher-value
PTY cases over one per key if an end-to-end flow already exercises all
six).

## Phase 6 — Marking state and highlighting (`src/lisp.c`, `src/syntax.c`, `dired.fe`)

Marking ("which rows are marked for deletion") needs to persist across
`g` (refresh) and be visible. Two sub-decisions:

1. **Where does mark state live?** Recommend: entirely in `dired.fe` as a
   Fe list/set of marked filenames (not row indices, which shift on
   refresh). `dired-mark` reads the current line via `(kg-line-content)`,
   extracts the filename, and appends to a Fe-side list; `dired-execute`
   walks that list, calls `(kg-shell (string-append "rm -- " (shell-quote
   f)))` per entry (check Fe has no built-in shell-quoting — write one in
   Fe, it's a handful of lines, or add it as a sixth Phase 1 C primitive
   if Fe string manipulation proves too painful; **do not** shell out
   with unquoted filenames, that's a command-injection footgun even
   though this is trusted local Lisp — quote defensively anyway since
   directory listings can contain attacker-controlled filenames on a
   shared or downloaded directory).
2. **Visual marker**: since row content itself is the `ls` output text
   with no reserved marker column, either (a) have `dired.fe` re-render
   each row's text with a literal `D ` / `  ` prefix when
   marking/unmarking (simplest — no C changes, `dired-mark` deletes and
   re-inserts the current row via existing row-mutation primitives — but
   check `kg-insert` only *inserts*, it doesn't *replace*; you may need a
   `(kg-delete-line)` primitive too, or fake replacement via delete-row +
   insert-row primitives not yet exposed — **this likely means Phase 1
   needs a sixth primitive, `(kg-replace-line [ROW] TEXT)`, discovered
   here rather than up front; add it there when implementing, don't
   retrofit awkwardly in Phase 6**), or (b) add a minimal `dired_syntax()`
   highlighter coloring the first byte of a row `HL_WARNING` (red) when
   it starts with a marker character the Lisp side writes. Recommend (a):
   it needs no new highlighter and reuses the same row-mutation
   primitive multiple future packages will want anyway.

**Tests**: PTY case marking two entries, executing, confirming the
listing (via `expected_screen_contains`/`expected_screen_not_contains`,
tmux backend — file deletion in a temp dir isn't observable through
`expected_saved` since dired buffers aren't saved-to-disk in the normal
sense) no longer shows the deleted names after `g` refresh.

## Phase 7 — `dired.fe` package (`share/kg/lisp/dired.fe`, or the correct
base path found in Phase 2)

Assemble the full package from the pieces built in Phases 1-6:
`dired-mode`, `dired-find-file`, `dired-mark`, `dired-unmark`,
`dired-execute`, `dired-refresh`, `dired-up-directory`. Keep it under
~150 lines — if it's growing much larger, some primitive is missing and
too much string/list manipulation is happening in Fe; prefer adding a
targeted C primitive over writing a hand-rolled parser in Lisp (matches
this repo's general C23-first, minimal-Lisp-surface posture — Lisp glues
C primitives together, it does not reimplement C-shaped logic).

Do **not** auto-load `dired.fe` from kg's own init sequence — it stays
opt-in via the user's `init.fe` calling `(kg-load "dired")`, matching
every other package in this repo's (currently empty) package ecosystem.

## Phase 8 — PTY acceptance tests and documentation

1. `test/pty/dired-*.yaml` cases (`backend: tmux` where you need
   `expected_screen_contains`, `pexpect` otherwise), covering: open
   dired on a temp dir with known contents, navigate with `C-n`/`C-p`,
   `RET` into a subdirectory and back out with `^`, mark+execute
   deletion. Use `config_files:` to plant `dired.fe` plus a minimal
   `init.fe` doing `(kg-load "dired")`, following the existing
   `lisp-init-load-pkg.yaml` pattern — read that file first.
2. `src/help.c` — no *dedicated* key-binding cells make sense here (the
   six dired keys are buffer-local, unlike global bindings the help table
   otherwise documents) — a single footer line is enough, matching
   W7-B's git-commit footer:
   `"  Dired (after (kg-load \"dired\")): RET open · d mark · x execute"`.
   Confirm this actually fits the 79-column convention before committing
   to the wording; shorten if not.
3. `doc/kg.1` — a `Git Commit Mode`-shaped `.Ss Dired Mode` subsection
   after it, documenting the six keys, the opt-in load step, and the
   scope cuts (no recursive delete, no rename).
4. `README.md` — one bullet, conditional/opt-in framing (unlike git
   commit mode, dired is *not* on by default): "Dired-mode Lisp package
   (opt-in via `(kg-load \"dired\")`): browse and delete files from a
   directory listing".
5. `doc/TODO.md` — there is no existing dired TODO bullet to check off
   (only the combined "Git commit mode (and the wider EDITOR-server
   crowd)" one, which W7-B already closed) — add a new one-line entry
   under a "Shipped" or changelog-style note if `doc/TODO.md` has that
   convention, or skip if it's purely forward-looking (check the file's
   existing structure before deciding).
6. `wishlist-implementation-plan.md` — flip W7 Option A from "future
   work" to implemented in the "Implementation status" header, same as
   this session did for Option B.

## Final verification

```
make clean && make            # WITH_LISP=1 default build
make check                    # native + PTY suites
make WITH_LISP=0 clean all    # feature must not depend on Lisp
make check                    #   (kbd.c's dispatch table is Lisp-free;
                               #    dired.fe itself simply won't load, and
                               #    M-x dired-mode won't exist — confirm
                               #    this degrades to "command not found",
                               #    not a crash)
.ci/run-ci-steps.sh           # full CI: analyzers, sanitizers, -Werror
```

**Complexity budget warning**: W7-B's implementation pushed
`SCC_COMPLEXITY_MAX` from 2650 to 2680 (see `Makefile`, and
`git log -S SCC_COMPLEXITY_MAX -- Makefile` for the repo's established
precedent of bumping this constant in the same commit that earns it,
never speculatively). W7-A adds more surface (six new natives, a new
`HLDB` entry, a bare-key dispatch loop) and will likely need a similar
bump — measure with `make complexity-check pmccabe-check` after each
phase rather than guessing a number up front, and only raise the limit
by however much the actually-implemented code needs.

Manual end-to-end sanity check:

```
$ mkdir -p /tmp/dired-test/sub && touch /tmp/dired-test/{a,b,c}.txt
$ kg   # then: M-x dired-mode RET /tmp/dired-test/ RET
       # expect: a.txt, b.txt, c.txt, sub/ listed
       # RET on sub/ descends; ^ returns
       # d on a.txt, x, y to confirm: a.txt removed from disk and listing
```

## Edge cases checklist (test manually, add tests where cheap)

- [ ] Directory with zero entries (empty dir) — listing buffer created
      but empty; `RET`/`d`/`x` on an empty buffer must not crash
      (`kg-line-content` on a buffer with `numrows == 0`).
- [ ] Directory path containing spaces or shell metacharacters — the
      `(kg-shell (string-append "ls -1p " dir))` construction in Phase 4
      is exactly the kind of unquoted interpolation Phase 6 warns
      against; apply the same quoting discipline to the *listing* shell
      call, not just the *delete* one — this is worth a dedicated PTY
      case with a directory name containing a space.
- [ ] `x` with zero marked entries — should no-op with a message, not
      prompt for confirmation of nothing.
- [ ] `g` refresh while marks are set — decide and document whether
      marks survive a refresh (Emacs dired keeps marks that still exist
      in the new listing); this plan does not mandate an answer, but
      `dired.fe` must pick one deliberately rather than by accident of
      implementation order.
- [ ] Two dired buffers open on different directories (`C-x b` between
      them) — mode line and per-buffer state (marks, if stored in a
      buffer-local Fe structure rather than a single global) must not
      bleed between them. If marks are a single global Fe list keyed only
      by filename (Phase 6's simplest option), this **will** bleed across
      buffers — flag this explicitly to the user/reviewer rather than
      discovering it late; may justify keying marks by
      `(cons buffer-name filename)` instead.
- [ ] `WITH_LISP=0` build: `M-x dired-mode` — confirm the M-x picker's
      "undefined command" message path, not a hang or crash.
- [ ] Deleting the directory dired is currently listing out from under
      itself (e.g. two dired buffers, one deletes a dir the other is
      inside) — out of scope to *handle* gracefully, but confirm it fails
      safely (shell error surfaced via `kg-message`, not a crash) rather
      than silently.

## Files touched (summary)

| File | Change |
|---|---|
| `src/lisp.c` | 6+ new natives (`kg-line-content`, `kg-buffer-line-count`, `kg-shell`, `kg-set-readonly`, `kg-switch-buffer`, `kg-clear-buffer`, likely `kg-replace-line` and `kg-shell-lines` discovered in Phases 4/6), `allowed_commands[]` additions if any go through `kg-command` instead of being raw natives |
| `src/bufmgr.c` | `buf_open_named()` (or equivalent factored out of `buf_open_special()`) |
| `src/def.h` | `SHL_DIRED`, `syntax_is_dired()` declaration, `buf_open_named()` declaration |
| `src/syntax.c` | `DIRED_HL_extensions[]`, `HLDB[]` entry (appended last), `syntax_is_dired()` |
| `src/kbd.c` | `dired_keys[]` dispatch table, one new branch in the `editor.readonly` block |
| `src/cmd.c` | none expected (dired-mode is a Lisp-defined command, not a static one) — revisit if Phase 4's decision point goes the C-side route instead |
| `share/kg/lisp/dired.fe` (path TBD, see Phase 2) | the package itself |
| `test/test_lisp.c` | primitive-level tests for Phase 1/2 additions |
| `test/test_syntax.c` | `HLDB` index guard + `syntax_is_dired()` tests |
| `test/pty/dired-*.yaml` | end-to-end acceptance cases |
| `src/help.c`, `doc/kg.1`, `README.md`, `doc/TODO.md`, `wishlist-implementation-plan.md` | documentation |

No Makefile change is expected beyond the complexity-budget constant
(see "Final verification" above) — no new `.c` files are planned, and PTY
YAML cases are globbed automatically. If Phase 4's decision point lands
on a new `src/dired.c` instead of pure Lisp, add it to `SRCS` in
`Makefile` and revisit the complexity-budget math (a whole new
translation unit changes the file-complexity accounting, not just the
total).
