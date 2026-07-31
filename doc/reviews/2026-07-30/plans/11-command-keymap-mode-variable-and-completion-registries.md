# Plan 11 — Commands, keymaps, modes, variables, and completion

## Goal

Replace duplicated command policy and hard-coded mode branches with staged,
testable registries. Consolidate minibuffer completion so new features and Lisp
packages compose without adding another switch loop.

## Verified baseline

Checked against the tree at `906e48f`. Re-read before implementing; line numbers
move.

| Thing | Where | Shape today |
| --- | --- | --- |
| Command table | `src/cmd.c:757` `static const struct named_cmd cmdtable[]` | 54 entries + `{NULL,...}` sentinel, alphabetical, linear `strcmp` |
| Command descriptor | `src/cmd.c:744-755` | `typedef void (*cmdfn)(int fd);` and 3 fields: `name`, `fn`, `unsigned flags` |
| Command flags | `src/cmd.c:746-749` | `CMD_NONE`, `CMD_EDITS_BUFFER` only; 21 of 54 entries flagged |
| Entry points | `src/def.h:812-818` | `cmd_execute_named`, `cmd_execute_named_with_prefix`, `cmd_static_exists` |
| M-x | `src/cmd.c:905 editor_named_command` | picker over `command_name_at` (`:882`): static table, then `kg_lisp_command_name` |
| Lisp allow-list | `src/lisp.c:1229 allowed_commands[]` | 11 entries, `{const char *name; bool mutates;}` |
| User bindings | `src/keybind.c:23 bindings[32]` | flat array, `int key` → `char command[64]` |
| Binding syntax | `src/keybind.c:35` | `strncmp(p, "C-c ", 4)`; second key printable ASCII or `C-<a-z>`, minus `C-c/C-g/C-x` |
| Key dispatch | `src/kbd.c:508 editor_process_keypress`, switch at 614-1110 | 91 case labels, ~497 lines |
| Prefix handlers | `src/kbd.c:311/334/368` | `handle_cc_prefix_key` (if/else), `handle_rect_prefix_key` (~7 cases), `handle_cx_prefix_key` (~25 cases) |
| Mode identity | `src/def.h:216-227 struct editor_syntax` | 8 fields, all highlighting; `editor.syntax` doubles as the major mode |
| Mode predicates | `src/syntax.c:934`, `:1013`, `src/dired.c:47` | `syntax_is_git_commit/git_rebase/dired`, pointer comparisons |
| Extension → mode | `src/syntax.c:2024 editor_select_syntax_highlight` over `HLDB[]` (`:602`) | 24 entries, `strstr` matching plus shebang fallback |
| Minor modes | `src/def.h:319-323`, duplicated at `:420-423` | loose `int` flags, plus globals `global_auto_revert`, `electric_pairs` |
| Local variables | `src/localvars.c` | three parsers, one result struct, merged in `src/bufmgr.c:1170` |
| Help text | `src/help.c:18 kg_help_lines[]` | hand-packed 79-column box drawing, decoupled from every table |

Corrections to the previous draft of this plan:

- There is **no `src/keybind.h`**; `keybind.c`'s four functions are declared in
  `src/def.h:820-824`. Decide deliberately whether new registry headers follow
  that convention or start splitting `def.h` (1009 lines) up.
- **`src/mode.c` already exists and is not a mode module.** All 259 lines are
  visual-line wrap geometry, and it is linked into `test_basic` and `test_lisp`.
  The mode registry must take another name — this plan uses `src/modereg.c` —
  or `mode.c` must first be renamed (suggest `visline.c`) in its own commit.
- The read-only policy is duplicated **three** ways, not two: `CMD_EDITS_BUFFER`
  (`src/cmd.c:844`), `allowed_commands[].mutates` (`src/lisp.c:1279`), and the
  per-keycode blocklist `readonly_blocked_keys[]` +
  `key_would_edit_readonly_buffer()` (`src/kbd.c:16-58`, 16 keycodes plus the
  printable and 0x80-0xFF ranges).
- The two command policy tables **agree today** — all 11 allow-list entries
  carry the same mutation verdict as `cmdtable`. The defect is drift risk, not a
  live bug; say so in the commit message rather than claiming a fix.
- Lisp-defined commands do bypass `CMD_EDITS_BUFFER`: `cmd_execute_named` falls
  through to `kg_lisp_run_command` (`src/cmd.c:854`) with no flags to consult.
  That **is** a live gap and deserves its own regression.
- Only `dired_keys[]` (`src/kbd.c:466-478`) and `keybind.c` map keys to command
  *names*; every other binding is a `case` calling a C function directly, so
  built-in keys never consult `cmdtable`.

## Dependencies

Plans 01-04 land first. Phases 1, 2, 6 and 7 then need **no** ownership
refactor — they touch command policy, local-variable value application and the
picker loops, none of which move buffer state. Phase 3 needs no ownership
refactor either but is large, and is ordered after phase 2 only because command
identity makes the parity tests cheaper. Phase 5 requires buffer-owned state
from Plan 09; phase 8 requires Plan 12's rooted-callable safety; Plan 10 markers
are needed only where a command stores a location.

## Ratchet budget

`SCC_COMPLEXITY_MAX` 4208, `SCC_FILE_COMPLEXITY_MAX` 520,
`PMCCABE_FUNCTION_COMPLEXITY_MAX` 120 (`Makefile:143-152`) were all lowered to
the measured value by `26ca20d`/`906e48f`, so there is **zero headroom** —
`editor_process_keypress` sits at exactly 120. No phase may add a case to the
`kbd.c` switch without removing more than it adds, which is why Plan 13's new
bindings are blocked on phase 3. Each phase states its measured before/after
`scc` total and worst `pmccabe`, and lowers the limits when it wins.

## Phase 1 — Make command descriptors authoritative

Files: `src/cmd.c`, `src/def.h`, `src/lisp.c`, `src/kbd.c`, `test/test_cmd.c`
(new), `Makefile` (`EXTRA_cmd`).

### Descriptor

Extend the existing struct rather than inventing a parallel one; keep the
`void (*)(int fd)` signature in phase 1 so no handler changes.

```c
enum command_flags {
	CMD_NONE = 0,
	CMD_EDITS_BUFFER = 1 << 0,
	CMD_LISP_CALLABLE = 1 << 1,
	CMD_MAY_PROMPT = 1 << 2,
	CMD_OK_READ_ONLY = 1 << 3,	/* allowed in special/read-only buffers */
	CMD_INTERNAL = 1 << 4,		/* hidden from M-x and generated help */
};

struct named_cmd {
	const char *name;
	cmdfn fn;
	unsigned flags;
	const char *summary;	/* one line, <= 60 columns, no trailing period */
};
```

Keep the table `const` and NULL-terminated; keep alphabetical order (the M-x
two-pass ranking at `src/cmd.c:934` depends on it).

### Central invocation

```c
enum command_origin { CMD_ORIGIN_KEY, CMD_ORIGIN_MX, CMD_ORIGIN_LISP,
                      CMD_ORIGIN_HOOK, CMD_ORIGIN_MACRO };

struct command_context {
	int fd;
	struct command_prefix prefix;	/* src/def.h:253 */
	enum command_origin origin;
};

/* 0 ran, 1 no such command, 2 refused by policy (status set). */
[[nodiscard]] int cmd_invoke(const char *name, struct command_context *ctx);
[[nodiscard]] const struct named_cmd *cmd_lookup(const char *name);
```

`cmd_execute_named*` become thin wrappers so the existing call sites are
unchanged in this commit. `cmd_invoke` owns read-only refusal, Lisp-callability,
prefix save/restore (open-coded today at `src/cmd.c:869-874`), and the status
message.

### Deletions

Delete `allowed_commands[]` (`src/lisp.c:1229`) and mark those 11 commands
`CMD_LISP_CALLABLE`; `native_command` becomes a `cmd_invoke` call with
`origin = CMD_ORIGIN_LISP` and an empty prefix. Preserve the exact error strings
`"command is not allowed: %s"` and `"buffer is read-only"`, asserted by a PTY
case. Give Lisp-defined commands a descriptor too, so `kg_lisp_run_command`
stops being an unpoliced fallthrough — minimally a runtime record with `name`
and `flags`, defaulting to `CMD_EDITS_BUFFER` until phase 8 lets Lisp declare
otherwise.

### Tests

Native `test/test_cmd.c` over the table (links `stubs.o`): names unique and
sorted; every non-`CMD_INTERNAL` entry has a summary under 60 columns; the
`CMD_LISP_CALLABLE` set is exactly the historical 11; `cmd_lookup` handles first,
last, `NULL` and `""`.

PTY, one file per assertion: an editing command refused identically from key,
M-x and `C-c` binding in a read-only buffer; `(command-execute 'upcase-word)`
refused in a read-only buffer (`requires_feature: lisp`, `config_files:`
planting `init.fe`); `(command-execute 'compile)` still rejected as not allowed;
and — separately labelled, because it is new behavior — a Lisp `defun`-defined
command refused in a read-only buffer.

## Phase 2 — Command identity and transient state

Files: `src/def.h` (session/command state), `src/cmd.c`, `src/kbd.c`,
`src/macro.c`, `src/yank.c`, `src/buffer.c`, tests.

Add stable `this_command`/`last_command` identifiers (index into the registry,
or a small interned ID for runtime commands) plus a command-owned transient
record cleared on: an unrelated command, `C-g`, buffer switch, and prompt entry.

`editor.last_key` (`src/def.h:312`) is the current stand-in and is already used
for two cycles at `src/kbd.c:605-611` (`CTRL_L` recenter, `ALT_R` window line).
Migrate those two first — they are the cheapest proof — then use identity for
kill coalescing, yank/yank-pop eligibility, and repeat.

Do not use the raw last key as identity: M-x, `C-c` binding, Lisp and macro
invocation of the same command must produce the same ID.

### Behavior change to schedule deliberately

`editor_kill_line` (`src/buffer.c:1366`) calls **`kill_ring_append`
unconditionally** (lines 1383, 1397) and never `kill_ring_set`, so today a
`C-k`, an unrelated motion and another `C-k` concatenate. Introducing
`last_command` changes that to Emacs semantics — a user-visible change needing
its own commit, a `README.md`/`doc/kg.1` note, and preservation of the
`C-u N C-k` invariant documented at `src/buffer.c:1362-1365`. Plan 13 bundle A
owns the details; do not land the identity change and the kill-ring change
together.

## Phase 3 — Normalized key events and a keymap trie

Files: `src/keybind.c` (extended), new `src/keymap.c`, `src/kbd.c`, `src/tty.c`,
`src/def.h`, tests.

### Why the representation must change

kg has no modifier bits: Meta combinations are **separate enumerators** in one
flat `enum` (`src/def.h:140-213`) decoded by a fixed `alt_keys[]` table
(`src/tty.c:31-68`). Adding one Meta binding today means editing `def.h`,
`tty.c` and the `kbd.c` switch — and the switch is at the complexity cap. There
is no `ALT_Y`, so even `M-y` is blocked on this phase.

```c
struct key_event {
	int32_t base;		/* codepoint, or KEY_FN_* for named keys */
	uint8_t mods;		/* KEY_MOD_CTRL | KEY_MOD_META | KEY_MOD_SHIFT */
};

[[nodiscard]] int key_parse(const char *text, struct key_event *out);
size_t key_format(struct key_event ev, char *dst, size_t size);
```

`key_parse`/`key_format` must round-trip and are the single source for config,
diagnostics, generated help and `describe-key`. `keybind_parse` becomes a
restricted front end over `key_parse` until phase 8 widens what is bindable;
its current gate (`src/keybind.c:35`) and the reserved `C-c/C-g/C-x` exclusion
stay until then.

### Keymap

A trie whose leaves hold command names (interned IDs once phase 2 lands), with
lookup precedence:

```text
transient > enabled minor modes, newest first > major mode > global
```

Prefix state stores the current node plus the full `{supplied, value}` prefix
(`struct command_prefix`, `src/def.h:253`). ESC/timeout semantics stay in the
input layer; do not move the 100 ms escape window or paste detection.

### Migration order

Each step is one commit: add the map, route dispatch through it, delete the
corresponding switch branches, and record the `pmccabe` drop.

1. global bindings from the main switch (`src/kbd.c:614-1110`), no behavior change;
2. `C-x` (`handle_cx_prefix_key`, ~25 cases);
3. `C-x r` (`handle_rect_prefix_key`);
4. Dired (`dired_keys[]` is already name-based — the cheapest conversion);
5. compilation (`C-c C-k`, gated on `strcmp(editor.filename, "*compilation*")` at `src/kbd.c:324`);
6. Git commit and rebase (`handle_cc_prefix_key`, `rebase_action_for_key` at `src/kbd.c:265`);
7. `C-c` user bindings (`bindings[32]` becomes a keymap layer).

Keep self-insert and `C-g` on a fast path if measurement demands it, and
document the exception where it lives. The `__attribute__((optimize("O0")))` and
`-Wanalyzer-out-of-bounds` suppression at `src/kbd.c:503-507` exist only because
the function is enormous; deleting them is part of this phase's acceptance.

### Tests

Native: `key_parse`/`key_format` round-trip over every current `ALT_*` and named
key; trie insert, lookup, shadowing, precedence, unbound. PTY: a parity case for
**every** binding touched by a migration step, added *before* the switch branch
is deleted — `test/pty/` already has 224 cases, so grep for the key first. Fuzz:
extend `test/fuzz_keypress.c` to drive the trie.

## Phase 4 — Generated introspection

Files: `src/help.c`, `src/cmd.c`, `src/keymap.c`, `README.md`, `doc/kg.1`, tests.

Implement `describe-command`, `describe-key`, `describe-bindings`, `where-is`,
and later `apropos-command`, rendered into a `*help*`-style special buffer
(`buf_open_help()` already loads `kg_help_lines[]` that way — `src/help.c:14-16`).

Scope the generation honestly: `kg_help_lines[]` is a hand-packed three-column
box-drawing table where each cell is "1 space + 9-char key + 15-char desc". Do
**not** try to generate that art from descriptors in one step. Instead:
generate the new `describe-*` buffers from the registries; add a test asserting
every key shown in `kg_help_lines[]` resolves to a bound command and every
summary matches its descriptor, which catches drift without rewriting the
layout; only then consider replacing the static art.

### Tests

Every keymap leaf resolves to a known command; every non-internal command has a
summary and internal ones are excluded; mode precedence is reported accurately
for a key bound at two layers; golden wrapping at 80 and at a narrow width
(PTY, `dimensions:`).

## Phase 5 — Explicit mode descriptors

Depends on buffer-owned state from Plan 09. Files: new `src/modereg.c`
(**not** `src/mode.c`, which is visual-line geometry), `src/syntax.c`,
`src/dired.c`, `src/kbd.c`, `src/word.c`, buffer struct, tests.

```c
enum mode_kind { MODE_MAJOR, MODE_MINOR };
struct editor_mode {
	const char *name;
	enum mode_kind kind;
	int (*detect)(const char *filename, const erow *first_row);
	struct editor_syntax *syntax;	/* may be NULL */
	struct keymap *keymap;		/* may be NULL */
	void (*activate)(void);
	void (*deactivate)(void);
	const struct mode_option_default *defaults;
};
```

This separates mode identity, highlighting, detection, keymap, local settings
and lifecycle — today all five are the single `struct editor_syntax *` at
`src/def.h:275`.

Migrate the five behavioral modes first, because they are the ones with
scattered `if (syntax_is_X())` tests: Dired, compilation, Git commit, Git
rebase, Lisp interaction. Call sites to convert: `src/kbd.c:313`, `:315`,
`:317`, `:319`, `:324`, `:485`, `:1007`; `src/word.c:1109`, `:1377`;
`src/dired.c:318`, `:353`. The 24 `HLDB[]` entries then share one generic
behavior and keep only their syntax field.

Keep `editor_select_syntax_highlight`'s resolution order intact when it becomes
`detect`: exact `git-rebase-todo`, then `gitcommit_basenames[]`, then
`filematch` substring matching with the trailing-dot refinement, then shebang.
The empty `filematch` arrays on both Git modes are a deliberate safety measure
(`src/syntax.c:589-592`) — preserve the comment with the code.

Major-mode change: deactivate old, clear major-mode-local state, install
syntax/keymap/defaults, activate, queue the mode hook at a safe point. Delete
the `syntax_is_*` predicates only once every caller is converted and a PTY case
covers each behavior.

## Phase 6 — Typed variable registry

Files: new `src/variable.c` and header, `src/localvars.c`, `src/localvars.h`,
`src/bufmgr.c`, tests.

### Verified starting point

All three envelope parsers live in `src/localvars.c` and support exactly **two**
variables — `compile-command` (string) and `buffer-read-only` (`t`/`nil`):

| Parser | Function | Input | Notes |
| --- | --- | --- | --- |
| `-*- … -*-` first line | `localvars_parse_modeline` (`:94`) | `erow` array | row 0, or row 1 after `#!`; quote/escape/paren-aware `;` splitting |
| `Local Variables:` block | `localvars_parse_footer` (`:978`) | last 3000 bytes, ≤512 lines | prefix/suffix envelope, form-feed rule, backslash continuation |
| `.dir-locals.el` | `dirlocals_find` (`:346`) + `dirlocals_parse` (`:838`) | file walked up to `/` | purpose-built non-evaluating reader; `eval` consumed, never run |

Merged in `buf_apply_local_settings` (`src/bufmgr.c:1170`), precedence weakest
to strongest: dir-locals → modeline → footer, with a user-typed
`compile-command` beating all three.

`localvars_parse_footer` has `pmccabe` 100 and `localvars_parse_modeline` 65 —
second and fourth worst in the tree. Extracting the shared value application is
the main complexity win available outside `kbd.c`.

```c
enum variable_type { VAR_STRING, VAR_BOOL, VAR_SYMBOL, VAR_INT };
enum variable_scope { VAR_GLOBAL, VAR_BUFFER_LOCAL, VAR_WINDOW_LOCAL };
struct variable_value { enum variable_type type;
	union { const char *s; bool b; long i; } as; };

struct editor_variable {
	const char *name;
	enum variable_type type;
	enum variable_scope scope;
	bool file_local_safe;
	int (*validate)(const struct variable_value *);
	int (*get)(struct variable_value *out);
	int (*set)(const struct variable_value *);
};

/* Single application point for all three envelopes and for Lisp. */
[[nodiscard]] int variable_apply(const char *name,
    const struct variable_value *value, bool from_file_local);
```

The three parsers keep their own **scanners** — the envelope grammars really are
different — and stop having their own **appliers**: each emits a name plus a
typed value (or `malformed`/`unsupported`) and calls `variable_apply`.

Initial registrations: `compile-command` and `buffer-read-only` (already
file-local-safe) plus, as `file_local_safe = false` until argued otherwise,
`auto-revert`, `visual-line-mode`, `overwrite-mode`, `electric-pair-mode`. The
last four are **new** as settable variables — they are `int` flags on
`struct editor_config` today (`src/def.h:319-323`, mirrored at `:420-423`) — so
each widens what a downloaded file can change and needs its own safety argument.

### Tests

Extend `test/test_localvars.c` (917 lines) with a matrix asserting that the same
variable, value and malformed input produce identical outcomes through all three
syntaxes, plus unknown and unsafe names, oversize values, mode defaults and
buffer switching. Keep `test/fuzz_localvars.c` and `test/fuzz_dirlocals.c`
running against the new applier.

## Phase 7 — Consolidate minibuffer and completion sessions

Files: `src/bufmgr.c` (prompt/history/buffer picker), `src/path.c`, `src/cmd.c`
(M-x), new `src/minibuffer.c`, tests.

### What already exists post-dedup

`26ca20d` made `editor_confirm_yn()` the only y/n prompt and lifted
`path_is_dir()`/`path_parent_dir()`/`buf_basename()` into `def.h`; `906e48f`
added `ALT_ENTER`. `editor_picker_render()`, `editor_picker_match_rank()`,
`editor_msg_appendf()` and `PICKER_MAX_ENTRIES` (64) are shared. The **loops**
are not: `editor_named_command` (`src/cmd.c:905`, `pmccabe` 50),
`editor_read_line_path` (`src/bufmgr.c:1014`, `pmccabe` 49) and
`buf_select_interactive` (`src/bufmgr.c:1308`) each re-implement the two-pass
rank filter, selection cycling, backspace, insertion, Tab LCP and the cancel
handshake. Only the first uses `minibuf_edit_key`; only
`editor_read_line_with_history` (`src/bufmgr.c:788`) has a history ring.

A candidate is `{display, value, annotation, rank}`; `value` is what gets
inserted, `rank` is `editor_picker_match_rank`'s verdict.

```c
struct minibuffer_session {
	const char *prompt;
	char *input;
	size_t input_len, input_cap;
	struct minibuf_history *history;
	size_t (*complete)(const char *text, struct completion_candidate *out,
	    size_t max, void *data);
	void *completion_data;
	enum require_match_policy require_match;
};

[[nodiscard]] int minibuffer_read(int fd, struct minibuffer_session *s);
```

Preserve today's ido-style behavior as the default strategy: prefix matches
ranked ahead of substring matches, Tab extending the longest common prefix over
the prefix group only, `M-p`/`M-n` history, and the empty-input default
(`last_extended_command`, `src/cmd.c:900`, disarmed by `explicit_selection`).

### Migration order

Generic read-line with history first, then M-x, then the buffer picker, and the
path picker last — it has the most special cases (`M-RET` literal accept,
`.`/`..`, directory descent) and the newest tests. `describe-*`, variable and
project completion then reuse the session.

Two divergences to resolve explicitly rather than silently: M-x accepts **ASCII
only** (`src/cmd.c:1069`) while the other two handle multi-byte input; and the
path picker deliberately gives `C-b`/`C-f` to cursor motion and cycles on
arrows, while M-x and the buffer picker cycle on `C-f`/`C-b` (comment at
`src/bufmgr.c:1091-1094`). The second is intentional and must survive.

### Tests

`test/test_complete.c` and `test/test_minibuf.c` already cover ranking, LCP,
history eviction and multibyte backspace — keep them green unchanged as the
parity gate. Add a session state fuzzer modelled on `test/fuzz_keypress.c`, and
PTY parity cases for each picker before its loop is deleted.

## Phase 8 — Runtime-defined commands, keymaps, and modes

Depends on Plan 12's rooted-callable and safe-point work.

Runtime commands already exist (`define-command`/`remove-command`,
`src/lisp.c:1462`/`:1504`, capped at `lisp_max_commands` 32). This phase gives
them descriptors — name, summary, flags, interactive spec — so they stop
bypassing command policy, makes removal invalidate bindings safely, and makes
both 32-entry ceilings (`lisp_max_commands` and `KEYBIND_MAX`,
`src/keybind.c:15`) configurable. Both already report their failure, so the
work is configurability, not diagnostics.

Widen `keybind_parse` past `C-c <key>` here, not earlier. Reserved keys (`C-g`
and the init-recovery route) stay unbindable, and `kg -Q` must remain a reliable
escape hatch. Interactive argument subset: prefix, point/region, string, file,
buffer, command. Do not expose raw C callback pointers to Lisp.

## Documentation

`README.md` gets keymap precedence, the mode concept, the variable registry and
any widened binding syntax; `doc/kg.1` gets the `KEY BINDINGS` sections and
`global-set-key`'s accepted syntax; `src/help.c` is required by `AGENTS.md`
whenever a binding changes, with phase 4's drift test keeping it honest; and
`AGENTS.md`'s "only `C-c <key>` sequences are bindable" line becomes wrong at
phase 8 and must be updated in the same commit.

## Commit sequence

1. command flags, summaries and `cmd_invoke`; delete the Lisp allow-list.
2. Lisp-defined commands get descriptors and a read-only verdict.
3. command identity; migrate the two `last_key` cycles.
4. kill coalescing on identity (user-visible; own commit and docs).
5. normalized key events and formatter.
6. global keymap, then one prefix/mode map per commit.
7. generated `describe-*` plus the help drift test.
8. mode registry; behavioral modes first.
9. variable registry; three scanners, one applier.
10. minibuffer sessions; one picker per commit.
11. runtime-defined commands and widened binding syntax.

## Acceptance

- one source of command mutation and Lisp-callability policy; no
  `readonly_blocked_keys[]`, no `allowed_commands[]`;
- no behavioral mode identified by a syntax pointer;
- a parity PTY case exists for every binding whose dispatch moved;
- every keymap leaf resolves to a known command, and help matches the registry;
- file-local variables remain non-evaluating and allowlisted;
- M-x, file and buffer completion behave as their existing tests assert;
- `scc` total and worst `pmccabe` are lower than at the start of the plan, with
  the `Makefile` limits lowered to match;
- both Lisp configurations pass full CI.

```sh
make check
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```

## Landed / deferred

### Landed

| Phase | State | Commits |
| --- | --- | --- |
| 7 — first slice: one two-pass rank filter for the name pickers | partial | `3f3238a` |
| 1 — authoritative command descriptors; allow-list deleted; the Lisp read-only gap closed | complete | `759c5c0` |
| 6 — one applier and one name table behind the three envelope scanners | partial (the value application; no `struct editor_variable`, no new settable variables) | `210ef56` |
| 2, 3, 4, 5, 8 | not started | — |

Counters at the close: `scc` 4276 at the start of the work, 4256 at the
end, with `SCC_COMPLEXITY_MAX` lowered to match at each step (4280 →
4271 → 4256, never raised).  `make check` is 20 unit binaries and 282
PTY cases, green in both Lisp configurations; the full twelve-step
runner is green at every one of the three commits.  Worst `pmccabe` is
`localvars_parse_footer` at 93, down from 100; `editor_named_command`
50 → 43, `buf_select_interactive` 33 → 28,
`localvars_parse_modeline` 65 → 56, `dlr_apply_pair` 38 → 35.

Self-funding arithmetic, in commit order:

| Commit | scc before → after | Note |
| --- | --- | --- |
| `3f3238a` picker filter | 4276 → 4268 | pure dedup; caps deliberately *not* lowered, the 12 points banked for the next commit |
| `759c5c0` command descriptors | 4268 → 4271 | the registry's net cost, paid by the line above; cap lowered to 4271 |
| `210ef56` variable appliers | 4271 → 4256 | pure dedup; cap lowered to 4256 |

### What phase 1 actually changed

- `struct named_cmd` is in `def.h` with `CMD_LISP_CALLABLE` and a
  one-line `summary`; `cmd_invoke(name, ctx)` is the only route into a
  command and owns the read-only refusal, the Lisp-callability verdict
  and the prefix argument.  `cmd_execute_named()` is a wrapper, so
  `kbd.c` is untouched; `cmd_execute_named_with_prefix()` and
  `cmd_static_exists()` are deleted along with their three stub copies.
- `allowed_commands[]` in `lisp.c` is deleted.  `(command-execute ...)`
  asks `cmd_invoke()` and translates the verdict, preserving both error
  strings exactly (`command is not allowed: %s`, `buffer is read-only`),
  which three PTY cases pin.
- **The live gap is closed.**  A Lisp-defined command used to fall
  through `cmd_execute_named()` into `kg_lisp_run_command()` with no
  descriptor, so a read-only buffer refused it only if the body happened
  to reach a native that guards itself — and then as a Lisp error,
  mid-command.  `test/pty/lisp-defined-command-readonly.yaml` fails at
  `3f3238a` (*missing screen text: 'Buffer is read-only'; unexpected
  screen text: 'Lisp error'*) and passes at `759c5c0`.
- Until phase 8, every Lisp-defined command counts as
  `CMD_EDITS_BUFFER`.  That is a user-visible restriction and is
  documented in `README.md` and `doc/kg.1`.
- `test/test_cmd.c` is new and links every translation unit except
  `main.c` (so `test/stubs_perf.c` is now `test/stubs_main.c`).  It
  asserts the table is sorted and unique, that every entry has a handler
  and a summary of at most 60 columns with no trailing period, and that
  the `CMD_LISP_CALLABLE` set is exactly the historical eleven with the
  same mutation verdicts the deleted `mutates` field carried.
- `test/test.h` gains `CHECKF`, a `CHECK` that can name the offending
  element.

### Deviations, all deliberate

- **`CMD_MAY_PROMPT`, `CMD_OK_READ_ONLY` and `CMD_INTERNAL` were not
  added.**  Nothing reads them: no command is hidden from M-x today, and
  a flag with no consumer is a claim the tests cannot check.  `test_cmd`
  rejects any flag bit outside the two that exist, so adding one is a
  deliberate act.
- **`CMD_ORIGIN_HOOK` and `CMD_ORIGIN_MACRO` were not added** for the
  same reason; the enum has the three origins that exist (key, M-x,
  Lisp).  Only the Lisp origin is policed differently, and it is the
  only one that reports refusal as an error rather than an echo-area
  message — which is the whole reason `origin` is in the context at all.
- **Phase 7 was started before phase 1, not after it.**  The command
  registry costs complexity and the ratchets had four points of
  headroom; the picker dedup is the plan's own phase 7 work and paid for
  it.  Only the two-pass rank filter moved — `minibuffer_session` does
  not exist, and the path picker is untouched, because it ranks through
  `editor_path_complete_entries()`, which sorts rather than filters.
- **Phase 6 landed the applier, not the registry.**  `localvars_kind()`
  is the one statement of which names exist and what kind of value each
  takes, and `localvars_apply_bool()`/`localvars_apply_string()` are the
  one statement of what a value means; the three scanners keep their
  grammars, as the plan requires.  What is *not* there is
  `struct editor_variable` with `scope`, `file_local_safe`, `validate`,
  `get` and `set`, and the four new settable variables (`auto-revert`,
  `visual-line-mode`, `overwrite-mode`, `electric-pair-mode`).  Those
  four would each widen what a downloaded file can change and the plan
  itself asks for a safety argument per variable; registering them as
  `file_local_safe = false` first would be a registry nothing can use.
  The cross-envelope matrix in `test_localvars` passes against the three
  old copies as well as the new applier, so it is characterization, not
  a bug fix — say so.
- **Two `localvars` questions were left as they are, on purpose**: the
  modeline still skips a `#!` first line before looking for the marker,
  and `dirlocals_find()` still walks to `/` rather than stopping at a
  project root.  Both are recorded here so they are decisions rather
  than oversights.

### Pick-up points

- **Phase 2 (`key_parse`/`key_format`) was skipped deliberately.**  It
  has no consumer until phase 3: nothing today formats a key, and
  `keybind_parse` already parses the one syntax that is bindable.
  Landing it alone is infrastructure the tests can only exercise
  directly.  Start it *with* phase 3.
- **Phase 3 is the next real unit of work and is large.**  The
  migration order in this document still holds.  Note that `P0` already
  spent the one sanctioned re-baseline on `editor_process_keypress`
  (120 → 85, and the `optimize("O0")`/`-Wanalyzer-out-of-bounds`
  workaround is already gone, so that part of phase 3's acceptance is
  met); the function measures 84 today against a cap of 110, so there is
  room to route dispatch through a map before deleting branches.
- **`readonly_blocked_keys[]` (`src/kbd.c:17`) is the last surviving
  second opinion on the read-only verdict** and is the acceptance
  criterion this work did not meet.  It cannot go until built-in keys
  resolve to command names, i.e. phase 3 step 1.
- **Phase 4's drift test is still blocked.**  `kg_help_lines[]` cells
  are a 9-column key field and a 15-column abbreviation, not command
  names, so "every key resolves to a bound command" needs the keymap and
  "every summary matches its descriptor" needs a name in the cell.  The
  `summary` field phase 1 added is the half that now exists.
- **Phase 7's remaining loops** are `editor_read_line_path`
  (`pmccabe` 49) and `editor_named_command` (43); both still own their
  cancel handshake, backspace and Tab handling.  The two divergences the
  plan names (M-x accepting ASCII only; the path picker deliberately
  giving `C-b`/`C-f` to cursor motion) are both still true and still
  unresolved.
