# Plan 13 — Productive Emacs affordances

## Goal

Deliver high-leverage Emacs habits in dependency order, using the registries,
transactions and services from Plans 09-12 instead of adding more one-off
branches.

This is a product plan, not an Emacs compatibility promise.

## Verified baseline

Checked against the tree at `906e48f`. What already exists, and what does not:

| Affordance | Status | Evidence |
| --- | --- | --- |
| Kill ring | **single slot** | `struct kill_ring { char *text; int len; }`, `src/def.h:335` |
| Kill coalescing | partial and quirky | `kill_ring_append` has one caller, `editor_kill_line` (`src/buffer.c:1383`, `:1397`); no `last_command` |
| `M-y` / yank-pop | absent | no `ALT_Y` token anywhere |
| Rectangle kill | separate single slot | `src/rect.c:12-14`, `rect_kill_ring_set/free` |
| Minibuffer kill | third separate slot | `static char minibuf_kill[1024]`, `src/bufmgr.c:507` |
| Mark ring | **exists**, 16 entries/buffer | `MARK_RING_MAX`, `src/def.h:250`; `editor_pop_to_mark`, `src/yank.c:196` |
| Backup on save | absent | no `~`-suffix write in `src/fileio.c` |
| Autosave | absent | — but **auto-revert exists** (`autorevert_poll`, `src/def.h:503`, polled at `src/main.c:163` and `src/tty.c:428`) |
| `transpose-chars` | **exists**, UTF-8 aware | `src/buffer.c:1412`, bound `C-t` at `src/kbd.c:730` |
| `transpose-words` | absent | no `ALT_T`; `doc/TODO.md:138` |
| Line numbers | absent | no gutter in `src/display.c`; `doc/TODO.md:156` |
| `next-error` | absent | `*compilation*` is plain ANSI-stripped text; nothing parses `file:line` |
| `describe-*` | absent | `src/help.c` is a static 79-column box-art array |
| Registers / bookmarks | absent | `C-x r` is rectangles only (`handle_rect_prefix_key`, `src/kbd.c:334`, handles `k/C-k d c y/C-y t C-g` then reports `"C-x r %c is undefined"`) |
| `occur` | absent | only the English word "occurrence" in `src/search.c` |
| `dabbrev` | absent | `src/autocomplete.c` (99 lines) is electric-pair bracket closing only |
| Project find/grep | absent | — |

## Delivery principles

- User-visible behavior or key changes update `README.md`, `doc/kg.1`, and the
  built-in help table in `src/help.c` (`AGENTS.md` requires all three).
- Every command is a named registry command and behaves identically from key,
  M-x, `C-c` binding, and Lisp.
- Interactive behavior uses PTY cases; pure transforms use native tests.
- `WITH_LISP=0` retains built-in functionality.
- Prefer bounded memory and results; report truncation distinctly from empty.
- Do not block independent small features on the full architecture — but do
  respect the two gates below, which are real.

## Two gates that apply to every bundle

### The keymap gate

`editor_process_keypress` (`src/kbd.c:508`) has `pmccabe` **120**, exactly
`PMCCABE_FUNCTION_COMPLEXITY_MAX` (`Makefile:152`), and the `scc` total is at
its measured limit too (4208, `Makefile:143`). Both were lowered to the measured
value by `26ca20d`/`906e48f`.

Adding a single `case` to that switch fails `.ci/ci-01-complexity.sh`.

kg also has no modifier bits: Meta keys are individual enumerators in
`src/def.h:140-213` decoded by `alt_keys[]` at `src/tty.c:31-68`, so a new
`M-<key>` costs an edit in `def.h`, `tty.c` **and** the capped switch.

Therefore any bundle needing a new key binding is gated on **Plan 11 phase 3**
(normalized key events and the keymap trie), not merely on "command identity".
This corrects the previous draft and FINAL.md's "can land with limited
architectural dependency" grouping: `M-y` and `M-t` cannot land before phase 3
unless the same commit removes more complexity than it adds. A `C-c <key>`
binding is the only escape hatch that works today, and it is the wrong final
home for `M-y`.

### The registry gate

Commands must be added to `cmdtable` (`src/cmd.c:757`) with a summary and
policy flags, per Plan 11 phase 1. Adding a command without a descriptor
re-creates the drift this program exists to remove.

## Bundle A — Kill ring and yank-pop

### Dependencies

- command identity, Plan 11 phase 2;
- keymap, Plan 11 phase 3 (for `M-y`);
- range replacement and undo grouping, Plan 10 (a local checked helper may
  suffice for a first cut).

### Files

`src/yank.c`, `src/def.h`, `src/buffer.c`, `src/cmd.c`, keymap layer,
`test/test_yank.c`, PTY cases, `README.md`, `doc/kg.1`, `src/help.c`.

### Ring

```c
#define KILL_RING_MAX_ENTRIES 16
#define KILL_RING_MAX_BYTES   (8 * 1024 * 1024)

struct kill_entry { char *text; size_t len; };

struct kill_ring {
	struct kill_entry entries[KILL_RING_MAX_ENTRIES];
	size_t count;		/* live entries */
	size_t current;		/* index yank/yank-pop reads */
	size_t total_bytes;
};
```

`MARK_RING_MAX` 16 (`src/def.h:250`) is the existing precedent for a bounded
per-feature ring; match its size and its eviction style so the two read alike.

On cap: evict oldest entries. kg buffers hold arbitrary bytes, so do **not**
silently truncate mid-glyph — either reject an oversize entry with a status
message or document exact byte truncation. Pick one and test it.

### Coalescing — this changes existing behavior

Today `editor_kill_line` calls `kill_ring_append` **unconditionally**
(`src/buffer.c:1383`, `:1397`) and nothing ever marks a kill boundary. A `C-k`,
an unrelated motion, and another `C-k` concatenate; only `C-w` / `M-w` / `M-d` /
`M-z` (which all call `kill_ring_set`) reset. Emacs would start a new entry.

Fixing this is a **user-visible change**, so:

- give it its own commit and its own `README.md` / `doc/kg.1` note;
- preserve the invariant documented at `src/buffer.c:1361-1366` — the `C-u N C-k`
  batch at `src/kbd.c:677-707` detects EOF by watching `killring.len` stop
  growing and pushes one `UNDO_KILL_TEXT` over `killring.text + prev_kill_len`.
  A reset in mid-batch silently corrupts undo replay. Either keep the batch
  reading a single entry's length, or rewrite the batch first;
- add PTY cases pinning `C-k C-k C-y`, `C-k C-n C-k C-y`, and `C-u 3 C-k C-y`
  before and after the change.

Target semantics: consecutive forward kills append; consecutive backward kills
prepend; copy starts a new entry; any unrelated command ends coalescing.

### `yank-pop`

Yank stores the inserted range (marker-backed once Plan 10 lands) plus the ring
index. `yank-pop` is valid only immediately after `yank`/`yank-pop`, atomically
replaces the previous span with the next entry, remains eligible after itself,
and produces one undo per pop (or a documented group). An intervening command
makes it refuse without modifying text.

### Scope decision to record

There are three independent kill stores: `killring`, the rectangle slot
(`src/rect.c:12-14`), and the minibuffer slot (`src/bufmgr.c:507`). Emacs keeps
`killed-rectangle` separate from the kill ring, and minibuffer killing shares
the ring. Decide explicitly; the default recommendation is: leave rectangles
separate, and leave the minibuffer slot alone in this bundle.

### Tests

Native `test/test_yank.c`: ring insert/evict/wrap, byte cap, append vs prepend,
`kill_ring_get` at each index, UTF-8 and embedded NUL/arbitrary bytes. The
existing `test_append_*` cases assert today's unconditional append and will need
rewriting — do that in the same commit as the behavior change, not silently.

PTY: direction, eviction, multiple buffers, read-only refusal, undo, invocation
from key vs M-x vs Lisp, prefix selection.

### Docs

`README.md:25` says "Multiple buffers with shared kill ring", and `doc/kg.1`
says "a kill ring" in `DESCRIPTION` and "paste from the kill ring" in
`Copy and Paste`. Those are already misleading for a one-entry store; this
bundle makes them true and should say so.

## Bundle B — Backup on save and autosave recovery

### Dependencies

File identity and conflict semantics from Plan 04.

### Files

`src/fileio.c`, `src/bufmgr.c`, `src/main.c` (periodic work), variable registry
(Plan 11 phase 6), tests, docs.

### Backup

A safe, file-local-settable variable; one backup before the first overwrite in a
save session; atomic creation in the same directory; explicitly defined symlink,
permission and metadata behavior; never blindly overwrite a newer backup.

### Autosave

Only dirty buffers visiting real files; periodic by time and/or edit count;
atomic recovery file carrying enough metadata to compare against the source;
never replaces the source automatically; prompts on open when the recovery is
newer or different; a successful save or clean exit removes only kg-owned
recovery files; a crash leaves them.

Interaction to design deliberately: **auto-revert already exists** and reloads
the buffer when the file changes on disk (`autorevert_poll`, driven from
`src/main.c:163` and `src/tty.c:428`). Autosave writing must not look like a
disk change to auto-revert, and recovery-prompt-on-open must not race it.

### Tests

Use the existing injection seams — `editor_write_fn`, `editor_fsync_fn`,
`editor_close_fn`, `editor_rename_fn` (`src/fileio.c:48-51`) — for permission
errors, short writes and rename failures. Plus: source replaced under us,
multiple buffers, privacy mode, stale/malformed recovery, cleanup, simulated
crash, `WITH_LISP=0`.

## Bundle C — Small editing and display wins

### `transpose-words`

Files: `src/word.c`, command table, keymap, tests, docs. Needs `M-t`, so it is
behind the keymap gate.

Obtain the two word ranges around point with one documented constituent
predicate, replace in one transaction, preserve the separating text, produce one
undo, and define behavior at BOB/EOB, across punctuation, and for Unicode.
`editor_transpose_chars` (`src/buffer.c:1412`) is the model for UTF-8 handling.

### Unicode word consistency

`src/word.c` is 1514 lines and `editor_reflow_paragraph` already sits at
`pmccabe` 60. Unify interactive word motion with the Lisp thing bounds
(`native_forward_word` `src/lisp.c:792`, `native_backward_word` `:801`,
`native_bounds_of_thing` `:874`) onto **one** codepoint-aware constituent
helper. Decide the categories explicitly (ASCII alnum plus underscore, and a
stated policy for non-ASCII), keep malformed-byte behavior deterministic, and
check the decision against Emacs with cases for accented Latin, CJK and
punctuation.

### `display-line-numbers-mode`

Files: `src/display.c`, `src/winmgr.c`, `src/mode.c`, window/view options, mode
and variable registries, tests, docs.

This is larger than it looks and should not be treated as a quick win:

- `draw_window_rows` (`src/display.c:92`) is at `pmccabe` 66, third worst in the
  tree — a gutter added inline will not pass the complexity gate;
- a gutter narrows the text area, and **every** wrap computation in
  `src/mode.c` is parameterised on `win_w` (`visual_line_width`,
  `visual_segments`, `render_offset_at_visual`, `goto_visual_row_col`). The
  gutter width must be subtracted before those are called, or visual-line mode
  and line numbers will disagree about where lines wrap;
- gutter width depends on the largest visible line number and must be stable
  enough not to shimmer during scrolling;
- splits, horizontal scroll and 1-column terminals all need cases
  (`dimensions:` in PTY YAML).

Do not alter buffer text. Cache the formatted numbers.

## Bundle D — Registers and bookmarks

### Dependencies

Stable handles (Plan 09), markers (Plan 10), `C-x r` keymap (Plan 11 phase 3).

`C-x r SPC` and `C-x r j` are free today: `handle_rect_prefix_key`
(`src/kbd.c:334`) handles only `k`, `C-k`, `d`, `c`, `y`, `C-y`, `t`, `C-g` and
reports `"C-x r %c is undefined"` for everything else. So registers extend an
existing prefix rather than claiming a new one.

### Position registers

`point-to-register` (`C-x r SPC`) and `jump-to-register` (`C-x r j`), over a
fixed bounded table keyed by register character, storing a marker and an
optional window-restoration policy. Handle a killed buffer or stale marker with
a clear message, not a jump to a wrong place.

### Text registers

Later: `copy-to-register`, `insert-register`; bounded memory; exact bytes.

### Persistent bookmarks

Stored atomically under kg's config directory: name, canonical filename, 1-based
codepoint position, a surrounding-text fingerprint, optional timestamp. On jump:
open the file, try exact then fingerprint-nearby relocation, and report stale
ambiguity rather than guessing.

Tests: edits before and through a marker, buffer slot reuse (`MAX_BUFFERS` is
20), file rename and deletion, UTF-8 positions, malformed bookmark file, atomic
persistence under injected write failure.

## Bundle E — Command introspection

### Dependency

Plan 11 phases 1-4.

`describe-key`, `describe-command`, `describe-bindings`, `where-is`,
`apropos-command`, rendered into `*help*`-style special buffers. `buf_open_help()`
already loads `kg_help_lines[]` into a buffer and renders it through the normal
display pipeline (`src/help.c:14-16`), so the presentation layer exists.

Completion uses the shared session from Plan 11 phase 7 — note that M-x today
accepts **ASCII only** (`src/cmd.c:1069`) while the other pickers handle
multi-byte input, so command names must stay ASCII until that is unified.

Tests: global/mode/minor/transient precedence, unbound key, a runtime command
removed between binding and describe, width wrapping, read-only help navigation.

## Bundle F — Navigable compilation

### Dependencies

- truthful compilation cap (Plan 04);
- stable handles;
- markers optional — an immutable first cut is fine;
- the process service is **not** required.

### Files

`src/compile.c`, a new diagnostic parser module, commands, keymap, mode, tests,
docs.

```c
struct diagnostic {
	char *path;
	unsigned line;
	unsigned column;	/* 0 when absent */
	enum severity severity;
	/* marker once Plan 10 lands; row/col snapshot before that */
	struct diag_location location;
};
```

Parse `file:line[:column]: message` conservatively into bounded records, and
preserve the raw output — `*compilation*` already strips ANSI in
`compilation_process_bytes` (`src/compile.c:474`), so the parser sees clean
text, but it must still cope with arbitrary chunk boundaries because output
arrives in 4 KiB reads.

Commands: `next-error`, `previous-error`, visit source in the other window,
recenter and highlight the location, skip or report stale and missing files.
Resolve relative paths against the compilation's own directory
(`compilation_resolve_directory`, `src/compile.h:53`), not the editor's cwd.

Parser tests are pure and belong in the native harness: chunk boundaries,
Windows-style `C:\` paths, colons in messages, huge lines, malformed numbers,
line numbers exceeding `int`.

## Bundle G — Occur and project navigation

### Dependencies

Completion sessions (Plan 11 phase 7); regex status and progress fixes
(Plan 03); special-buffer mode and keymap (Plan 11 phase 5); the process
manager (Plan 12 phase 9) for async grep only.

### `occur`

Prompt for literal or regexp; search the current buffer; bounded result count
and output; each row stores a stable source location; `RET` visits; a refresh
command; too-complex reported explicitly rather than as no-match.

### Project root and files

Nearest parent containing `.git`, with an explicit override variable; bounded
traversal; ignore `.git` and configured patterns; symlink-loop protection;
candidate cap and cancellation. Note that `dirlocals_find` (`src/localvars.c:346`)
already walks upward to `/` — reuse its traversal shape, but stop at the project
root rather than `/`. `project-find-file` uses the completion session.

### Project grep

Prefer an `rg` argv invocation with a POSIX `grep` fallback, explicit cwd,
bounded async process and output, results parsed into selectable locations,
cancel and restart, and no shell unless explicitly chosen. All three current
spawn sites hardcode `/bin/sh -c` (`src/compile.c:190`, `src/shell.c:251`,
`src/shell.c:379`); this is the first argv-based child and the reason Plan 12
phase 10 splits `start-process` from `start-shell-command`.

Tests use temporary trees: no-`rg` fallback, ignored files, spaces in names,
result cap, cancellation.

## Bundle H — First Lisp modes and packages

### Dependencies

Plan 12 phases 2-8 (callbacks, buffers, variables, modes, keymaps, hooks,
packages). Note that kg has **no hook mechanism at all** today, so every package
below is blocked on Plan 12 phase 7.

Packages: whitespace mode; auto-fill mode; a derived config/make mode; dabbrev
expand; optionally a project-grep UI.

Acceptance per package: no private C assumptions; uses only documented hooks,
options, keymaps and transactions; a failure or `C-g` leaves the editor usable;
bounded work on large buffers; load/`provide`/`require` covered by tests. Ship
each as a PTY case with `requires_feature: lisp` and `config_files:` planting
the package under the isolated HOME.

## Bundle I — Decorations and completion-at-point

### Dependencies

Markers and decorations (Plan 10), deferred hooks, completion sessions.

A small `completion-at-point-functions`-shaped interface: bounds, candidate
producer, annotation, replacement transaction. First producers: words from open
buffers, filesystem paths, Lisp symbols.

`src/autocomplete.c` is *not* a starting point — its 99 lines are electric-pair
bracket closing (`autopairs[]`, `editor_insert_char_auto_complete`), unrelated
to completion. Path candidates should come from `editor_path_complete_entries`
(`src/path.c:117`).

Display candidates in the echo area or a temporary window; no GUI popup
prerequisite. Cap candidates and producer work; the session already caps at
`PICKER_MAX_ENTRIES` 64.

## Bundle J — Conditional structural and VCS work

Only after the prior APIs prove stable: show-paren and balanced-expression
motion; small Lisp structural editing; tags- or grep-backed xref; a compact Git
status/diff/stage/commit workflow (kg already has Git commit and rebase modes
with seven `git-rebase-*` commands, so this extends rather than starts).

Do not attempt full Magit, Org, or LSP clients in core.

## Intentional exclusions

Full Emacs Lisp / ELPA compatibility; dynamic scope by default; unrestricted
text properties or overlays; a terminal emulator; TRAMP; a `package.el` clone
with network install; daemon/server/GUI frames; full Org/Magit/LSP; a freely
rebindable `C-g` or init-recovery route.

## Recommended order

Revised from the previous draft, because `M-y` and `M-t` are behind the keymap
gate and line numbers are behind display work:

1. **before the keymap lands** — the pure-logic halves that need no new key:
   the kill ring data structure and its native tests; the unified word
   constituent predicate; the compilation diagnostic parser as a pure module;
2. kill ring wiring, coalescing change and `M-y` (after Plan 11 phase 3);
3. backup and autosave recovery;
4. `transpose-words`;
5. command introspection (`describe-*`);
6. `next-error` / `previous-error` on the parser from step 1;
7. registers, then bookmarks;
8. `display-line-numbers-mode`;
9. `occur`, project find, project grep;
10. first Lisp packages;
11. decorations and completion-at-point;
12. structural, xref and VC work, only from demonstrated demand.

Steps 1, 3 and part of 6 are the only ones that can start before Plan 11
phase 3.

## Per-bundle completion checklist

- [ ] named commands with registry metadata and a summary
- [ ] keymap binding, and introspection shows it
- [ ] native tests for pure logic
- [ ] PTY tests for interaction and saved-file outcomes
- [ ] UTF-8, read-only, undo, and error cases
- [ ] bounded memory and work, with truncation reported distinctly
- [ ] `README.md`, `doc/kg.1`, and `src/help.c` updated
- [ ] `WITH_LISP=0` and `WITH_LISP=1` both green
- [ ] `scc` total and worst `pmccabe` measured; `Makefile` limits lowered, never raised
- [ ] full CI

## Final verification

```sh
make check
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```
