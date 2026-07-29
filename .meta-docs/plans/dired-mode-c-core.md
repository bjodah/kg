# Implementation Plan — Dired mode (C core)

> Target audience: a developer who has read `CLAUDE.md` and skimmed one
> of the recent mode commits (`git log --stat` for the git-rebase-todo
> mode is the best single reference — this plan follows the same shape).
> Supersedes [w7a-dired-mode.md](file:///work/.meta-docs/plans/w7a-dired-mode.md)
> (2026-07-08 draft), which planned dired as a Lisp package; see "Why C,
> not the Lisp package" below for what changed and why.
> Design source: [doc/WISHLIST.md](file:///work/doc/WISHLIST.md)
> ("dired-mode clone, or something even simpler if deemed too ambitious").
> Estimated size: ~1000–1200 added lines total (~450 of them in a new
> `src/dired.c`), scc ratchet roughly +170–240, six phases that each
> leave `make check` green.  Roughly 1.5–2x the git-rebase-todo mode.

## Implementation status

Phases 1–6 shipped; the plan is history now, kept for the design rationale.

- Phase 1 (listing and entry): `0a03fc1`
- Phases 2–3 (navigation, marks, flagged deletion): `d6eaf22`
- Phases 4–5 (`C-x C-f` on a directory, highlighting): `88d6ccd`
- Phase 6 (docs, help, `SCC_COMPLEXITY_MAX` 4116 → 4260): this change

Two details differ from the sketch below: the buffer name carries no
trailing slash (`*Dired: /abs/path*`, since it is a `realpath()`), and the
`x` confirmation was factored into `editor_confirm_yn()`, which the git
commit and rebase aborts now share.  The v1 scope cuts are recorded as
follow-ups in `doc/TODO.md`.

## What you are building

`M-x dired` (and `C-x d`, and `C-x C-f` on a directory) opens a
read-only buffer named `*Dired: /abs/path/*` listing that directory's
entries, one per line, with Emacs-like keys:

| Key | Command | Behavior |
|---|---|---|
| `RET` | `dired-find-file` | open the entry at point (directory → dired on it, file → normal buffer) |
| `^` | `dired-up-directory` | dired on the parent directory |
| `g` | `dired-revert` | re-read the directory in place |
| `m` | `dired-mark` | mark the entry at point (a `*` in column 0), move down |
| `d` | `dired-flag-file-deletion` | flag the entry at point (a `D` in column 0), move down |
| `u` | `dired-unmark` | clear the `*` mark or `D` flag, move down |
| `x` | `dired-do-flagged-delete` | delete every `D`-flagged entry after one y/n confirmation |
| `n` / `p` | — | next / previous line (plain cursor motion, Emacs habit keys) |
| `q` | — | already works: `q` closes any special `*...*` buffer (kbd.c) |

All seven named commands are ordinary `cmdtable[]` entries, so `M-x
dired-flag-file-deletion` works identically to pressing `d`, and the
Lisp layer can drive dired through the existing `(command-execute ...)`
path with zero dired-specific Lisp work.

The `*` mark and `D` flag are distinct, as in Emacs: `x` acts only on
`D` flags.  In v1 the `*` mark is inert — the commands that consume it
(copy, rename, `dired-do-delete` on marked files) are follow-ups — but
the marker semantics are laid down Emacs-faithfully now so those
commands slot in without re-teaching users.

Out of scope for v1 (record in `doc/TODO.md` as follow-ups; do not
build now): copy (`C`), rename (`R`), mkdir (`+`), `U` (unmark all),
`dired-do-delete` (capital `D`) on `*`-marked entries, sort toggles,
`ls -l`-style size/permission columns, recursive directory deletion
(non-empty directories fail loudly via `rmdir` + `strerror`), mark
persistence across `g` (marks are buffer text; revert rebuilds the
buffer and drops them — say so in the man page).

## Why C, not the Lisp package

The 2026-07-08 draft planned dired as `dired.fe` riding on ~8 new
generic Lisp primitives. Three things changed since:

1. **The precedent flipped.** Git commit mode was also once planned as
   a Lisp package (W7-B) and shipped as a C built-in; git rebase mode
   followed the same C pattern.  Modes in this codebase are C; the Lisp
   layer scripts them via named commands.
2. **The Lisp surface was rebuilt** (kg- prefix dropped, Emacs-shaped
   names, string natives).  The draft's primitive names and `(fn ...)`
   examples are all stale, and half its proposed primitives
   (`kg-shell`, `kg-switch-buffer`, `kg-clear-buffer`,
   `kg-set-readonly`, `kg-replace-line`) exist only to let Lisp do what
   `buf_open_special()` already does in C.
3. **The shell-out problem disappears.** A Lisp dired must build `ls`
   and `rm` command lines (the draft spends two sections on quoting
   footguns).  A C dired uses `opendir`/`stat`/`unlink`/`rmdir`
   directly: no quoting, no `/bin/sh` dependency, exact `errno`
   reporting, and it works under `WITH_LISP=0`, which the Lisp version
   never could.

What is *lost* is the "first shipped `.fe` package" milestone from the
wishlist.  That milestone is better served by a package that is
actually Lisp-shaped (e.g. a text-transforming or templating package)
than by dired, which is syscalls end to end.  If the packaging story
should still be exercised first, stop here and say so — this plan
deliberately does not hedge between the two designs.

## Architecture: ride the ibuffer pattern

`*Buffer List*` (bufmgr.c) already demonstrates every mechanism dired
needs:

- `buf_open_special(name, &syntax, populate, status)` creates or
  refreshes a named, read-only special buffer whose rows are rebuilt by
  a `populate` callback (bufmgr.c:1588).  It is currently `static`;
  export it (declare in `def.h`) rather than duplicating it.
- Mode identity is a static `struct editor_syntax` **outside** `HLDB[]`
  (`ibuffer_syntax`, bufmgr.c:21) — dired is never auto-selected by
  filename, so it does not belong in the filename-matching table.
  `syntax_is_dired()` compares `editor.syntax` against
  `&dired_syntax`, like `syntax_is_git_rebase()` compares highlighter
  pointers.
- Bare keys in a read-only buffer dispatch from the
  `if (editor.readonly)` block in `editor_process_keypress()`
  (kbd.c:522), where ibuffer's bare `ENTER` already lives.  This is the
  resolution to the bare-key problem the old draft identified: user
  Lisp bindings are C-c-only by design (keybind.c invariant in
  CLAUDE.md), so dired's keys are C built-ins, exactly like the
  commit/rebase C-c keys.  The dired branch must be checked **before**
  the existing `ENTER → buf_ibuffer_select()` line; add a comment
  saying the two are mutually exclusive by syntax pointer.

**State model — no state beyond the buffer itself:**

- The directory path lives in the buffer name: `*Dired: /abs/path/*`.
  A helper `dired_dir_of(const char *bufname, char *out, int size)`
  strips the `*Dired: ` prefix and trailing `*`.  The path is
  normalized (`realpath`) once at open time so `RET` into `sub/` and
  `^` compose without `..` accumulation.  Two dired buffers on
  different directories coexist for free (buffer names differ), and
  `populate` — which `buf_open_special` calls with no arguments after
  switching — recovers the directory from `editor.filename`.
- Row format: two-character gutter then the entry name, directories
  suffixed `/`.  Row 0 is a header line (`  /abs/path:`) that every
  command treats as unmarkable/unopenable.  Flags are the buffer text:
  `d` writes `D` into column 0 of the row (direct `row->chars` edit +
  `editor_update_row`, no undo record — `buf_open_special` already
  resets undo for special buffers).  `x` scans column 0.  Emacs
  semantics, zero side tables.
- Listing: `opendir` + `readdir`, skip `.` and `..` (the header and `^`
  cover them), `stat` for the directory suffix, sort with `strcoll`.
  Reuse `editor_path_expand_tilde` (path.c) for the prompt result; do
  **not** reuse `editor_path_complete_entries` — it is prefix-matching
  and capped for the completion popup, the wrong shape for a full
  listing.

## Phases

Each phase ends with `make check` green and is a natural commit point.

### Phase 1 — `src/dired.c`: listing and entry

1. New file `src/dired.c` (add to `SRCS` in `Makefile`): the static
   `dired_syntax` struct, `syntax_is_dired()`-equivalent
   (`dired_is_active()` or declare alongside the other
   `syntax_is_*` in def.h — pick one naming and note it), `dired_open(const char *dir)`
   (realpath → `buf_open_special` → status line with entry count), the
   `populate` callback, and `dired_dir_of()`.
2. Export `buf_open_special()` from bufmgr.c (drop `static`, declare in
   `def.h`).
3. `cmd.c`: `cmd_dired()` prompting via `editor_read_line_path()`
   (empty input → directory of the current file, else cwd), plus the
   `{ "dired", cmd_dired, CMD_NONE }` table entry.  **All dired
   commands must be `CMD_NONE`, not `CMD_EDITS_BUFFER`** — check what
   `CMD_EDITS_BUFFER` gates before assuming; the buffer is read-only by
   design and the commands self-guard on the syntax pointer, like
   `editor_rebase_set_action()` does.
4. `kbd.c`: `case 'd'` in the `C-x` prefix switch → same code path as
   `M-x dired` (`C-x d` is currently free — verified 2026-07-29).

Tests: native tests for the pure helpers (`dired_dir_of`, entry-name
parsing from a row) — put them in a new `test/test_dired.c` linking the
existing stubs only if the helpers are made stub-friendly; otherwise a
PTY case is the honest harness.  One PTY case: plant a small tree via
`config_files:`, `M-x dired` + type the path, tmux
`expected_screen_contains` the entries and `Dired` in the mode line.

### Phase 2 — bare keys and navigation

1. kbd.c readonly block: a fixed dispatch table
   `{ ENTER, "dired-find-file" }, { '^', "dired-up-directory" },
   { 'g', "dired-revert" }, { 'm', "dired-mark" },
   { 'd', "dired-flag-file-deletion" }, { 'u', "dired-unmark" },
   { 'x', "dired-do-flagged-delete" }`,
   gated on the dired syntax pointer, before the ibuffer `ENTER` line,
   calling `cmd_execute_named()`.  `n`/`p` map to plain
   `editor_move_cursor` calls in the same branch (no named command
   needed).
2. `dired-find-file`: parse the name from the current row (refuse
   header/blank), join with `dired_dir_of()`, `stat`; directory →
   `dired_open()`, file → the same open path `C-x C-f` uses (follow
   `editor_read_line_path`'s caller in bufmgr.c to find it).
3. `dired-up-directory`: `dired_open()` on the parent (truncate at last
   `/`; root's parent is root).
4. `dired-revert`: re-run `dired_open()` on the current dir; point goes
   to the first entry (documented mark-dropping behavior).

Tests: PTY cases — descend into a subdirectory via RET, come back with
`^`; open a file via RET and confirm its content on screen; `g` after
nothing changed is a no-op listing-wise.

### Phase 3 — marks, flags and deletion

1. `dired-mark` / `dired-flag-file-deletion` / `dired-unmark`: guard
   header row, write `*` / `D` / space into column 0,
   `editor_update_row`, move point down one line (Emacs behavior, makes
   marking runs fast).  `u` clears either marker; `x` acts on `D` only.
2. `dired-do-flagged-delete`: collect flagged names; zero flagged →
   status message, done.  Otherwise one prompt
   `Delete N entries? (y/n)` (reuse the `editor_git_abort`-style
   read-key prompt shape in kbd.c — but note those live in kbd.c and
   dired.c will need its own small confirm helper or an exported one;
   prefer exporting a `editor_confirm_yn(fd, prompt)` if kbd.c's
   pattern repeats a third time).  `unlink()` files, `rmdir()`
   directories; count successes, report the first failure with
   `strerror(errno)` and stop-or-continue **deliberately** (recommend:
   continue, report `"Deleted N, failed M (first: ...)"`), then
   revert the listing.
3. The confirm prompt needs the `fd` plumbed through — check how
   `cmd_execute_named` passes `fd` to commands (it does; rebase
   commands ignore it, dired's delete uses it).

Tests: PTY — flag two of three files, `x`, `y`: screen no longer shows
them after the auto-revert; flag a non-empty directory, `x`, `y`:
error surfaced, directory still listed.  Flag-then-unmark leaves the
file alone.  `x` with nothing flagged says so and does not prompt.

### Phase 4 — `C-x C-f` on a directory opens dired

`editor_open()` (fileio.c:409) currently fails on directories with
`Error opening ...: Is a directory`.  Before `load_file_transactional`,
`stat` the path: `S_ISDIR` → `dired_open(path)` and return success.
This also covers `kg somedir/` from the command line — decide whether
that is wanted at startup (recommend yes, it is what Emacs does; check
the startup path in main.c/bufmgr.c handles a buffer being replaced
during load).

Tests: PTY — `C-x C-f`, type a directory path, land in dired.

### Phase 5 — highlighting and mode line

`dired_syntax.highlight` highlighter (row-local, in dired.c —
note this is the first highlighter outside syntax.c; if that offends,
put the 25-line function in syntax.c and keep the struct in dired.c):
header row → `HL_COMMENT`; `D` flag byte → `HL_WARNING`; directory
entries (trailing `/`) → `HL_KEYWORD1`.  Mode line already shows the
syntax name (`Dired`) for free.

Tests: native — extend `test_syntax.c`-style hl assertions if the
highlighter lands in syntax.c; otherwise assert via a tmux PTY case
only that flagged rows render (color assertions aren't available in
the PTY harness; row-content assertions are enough).

### Phase 6 — docs, help, ratchet

1. `doc/kg.1`: `.Ss Dired Mode` after Git Rebase Mode — entry points,
   the key table, the v1 scope cuts (no copy/rename, no recursive
   delete, marks dropped on revert).
2. `README.md`: one bullet under the mode list.
3. `src/help.c`: one footer line
   (`"  Dired: RET open · ^ up · d/u flag · x delete · g revert"`),
   79-column checked.
4. `doc/TODO.md`: follow-ups entry (copy/rename/mkdir, mark-preserving
   revert, `ls -l` columns).
5. `doc/WISHLIST.md`: mark the dired line done; note the Lisp-package
   milestone was deliberately not consumed by it.
6. Settle `SCC_COMPLEXITY_MAX` by the measured amount, same-commit,
   comment updated (Makefile convention; see the git-rebase-todo
   ratchet bump for the current wording).  A new `.c` file also enters
   the per-file accounting — `dired.c` at ~450 lines will sit far under
   `SCC_FILE_COMPLEXITY_MAX` (520), no change needed there.

### Final verification

```
make clean && make && make check
make WITH_LISP=0 clean all && make check   # dired is Lisp-free; must be identical
.ci/run-ci-steps.sh --parallel             # full CI is the green light
```

Manual smoke test: `mkdir -p /tmp/dt/sub && touch /tmp/dt/{a,b}.txt`,
then `kg /tmp/dt` → listing; RET on `sub/`; `^`; `d` on `a.txt`, `x`,
`y` → gone from disk and listing; `q` → back to previous buffer.

## Edge cases checklist

- [ ] Empty directory: header-only buffer; RET/d/x on the header row
      refuse politely; `x` with no flags says "No files flagged".
- [ ] Filenames with spaces (no shell involved, so they just work —
      still add one PTY case with a space in a name as a regression
      guard; the row parser must take the whole rest-of-row, not the
      first word).
- [ ] Filename that is exactly `D` or starts with `/` — gutter parsing
      is positional (columns 0–1), never content-sniffing.
- [ ] Unreadable directory (`opendir` fails): status message with
      `strerror`, no buffer created / previous buffer intact.
- [ ] Entry deleted behind kg's back, then RET on it: open fails with
      the normal file-open error; `g` recovers.
- [ ] Symlinks: `stat` (not `lstat`) so a link to a directory descends;
      broken symlink must still list (stat fails → treat as file) —
      pick and test this deliberately.
- [ ] Two dired buffers on two directories, `C-x b` between them:
      flags and paths must not bleed (they cannot — both are buffer
      text — but the PTY case is cheap insurance).
- [ ] `q` from dired returns to the previous buffer (existing special-
      buffer behavior; just cover it in a PTY case).
- [ ] Very long filenames / deep paths: `PATH_MAX` truncation in
      `dired_dir_of` + join must fail loudly (status message), not
      silently operate on a truncated path.

## Files touched (summary)

| File | Change | Est. lines |
|---|---|---|
| `src/dired.c` (new) | listing, populate, open/find/up/revert/flag/delete, highlighter, helpers | ~400–450 |
| `src/cmd.c` | 7 thin `cmd_dired_*` wrappers + `cmdtable[]` rows | ~45 |
| `src/kbd.c` | readonly-block dispatch table, `C-x d`, maybe `editor_confirm_yn` export | ~30 |
| `src/bufmgr.c` | un-`static` `buf_open_special` | ~2 |
| `src/fileio.c` | `S_ISDIR` → dired in `editor_open` | ~12 |
| `src/def.h` | declarations | ~10 |
| `src/help.c` | one footer line | ~2 |
| `Makefile` | `SRCS += dired.c`, ratchet settle | ~3 |
| `test/pty/dired-*.yaml` (~8 cases) | acceptance | ~110 |
| `test/test_dired.c` or additions | pure-helper units | ~80–120 |
| `doc/kg.1`, `README.md`, `doc/TODO.md`, `doc/WISHLIST.md` | docs | ~70 |

Total ≈ 1000–1200 lines.  For calibration: the git-rebase-todo mode was
733 insertions and +120 scc; dired adds a translation unit and touches
the filesystem, hence the 1.5–2x estimate (~170–240 scc).
