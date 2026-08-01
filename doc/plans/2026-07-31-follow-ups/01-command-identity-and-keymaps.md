# Plan 01 — Command identity and layered keymaps

## Status (2026-07-31, after the first implementation campaign)

Phases 0–6 are complete on `stricter-emacs-adherence`

## Outcome

Every complete interactive action has one stable command identity and is
invoked through `cmd_invoke()`.  Terminal input is represented as a normalized
base key plus modifiers.  Prefix and mode bindings live in layered keymaps, not
branches in `kbd.c`.  The same registry answers dispatch, read-only policy,
Lisp callability, `where-is`, and help drift checks.

This is the first enabling plan: it unlocks `M-y`, `M-t`, registers, generated
help, runtime keymaps, and deletion of the last read-only policy duplicate.

## Starting point and non-negotiable behavior

- `struct named_cmd`, flags, summaries, `command_context`, and `cmd_invoke()`
  live in `src/cmd.h`/`src/cmd.c` and are authoritative for named invocation.
- `editor_process_keypress()` still dispatches most keys directly.  Newline,
  movement, deletion, mark, yank, prefixes, and self-insert mostly lack table
  rows suitable for keymap leaves.
- `src/keybind.c` accepts only `C-c <key>` and stores 32 integer-key bindings.
- Meta keys are separate `ALT_*` enumerators decoded by `alt_keys[]` in
  `src/tty.c`; `M-y` and `M-t` do not exist.
- `readonly_blocked_keys[]` in `src/kbd.c` remains the second read-only verdict.
- Preserve the 30 ms paste classification, 100 ms bare-ESC distinction, quoted
  raw-byte input, macro recording, explicit prefix zero, and emergency `C-g`.

## Phase 0 — Inventory and characterization

Before changing dispatch, create a checked inventory with one row per current
built-in binding:

```text
sequence | current handler | proposed command name | policy | prefix behavior
         | active layer/global-special-mode | existing test
```

Generate the inventory from the proposed keymap declarations if practical;
do not maintain a prose table and a C table indefinitely.  Classify:

- complete commands that need descriptors/wrappers;
- prefix nodes (`C-x`, `C-x r`, `C-c`, numeric argument);
- input operations that legitimately remain outside ordinary maps (`C-q`'s
  next raw byte, terminal resize, paste classification);
- the self-insert fallback;
- emergency quit.

Add missing parity cases before moving a binding.  Use a native structural test
for table completeness and focused PTYs for branches with stateful semantics;
do not create one slow PTY per printable key.

Acceptance: every current switch/prefix branch is classified and every editing
leaf has the intended `CMD_EDITS_BUFFER` verdict recorded.

## Phase 1 — Stable command identity and transient state

Add a small `command_id` owned by the command registry.  Static commands get a
stable ID derived from their table slot; runtime commands get monotonically
assigned IDs.  Redefining an existing runtime command preserves its ID, while
remove-and-recreate gets a new one.  Do not use handler pointers or borrowed
name pointers as identity.

Add session command state:

```c
struct command_state {
	command_id this_command;
	command_id last_command;
	unsigned invocation_depth;
	struct command_transient transient;
};
```

Only a completed top-level command advances `last_command`.  Prefix nodes,
decoder activity, prompt keystrokes, and refused commands are not commands.
Nested `(command-execute ...)` may expose `this_command` but must not overwrite
the outer top-level history.  Clear command-owned transient state on an
unrelated command, `C-g`, buffer kill/switch, prompt entry, and runtime command
removal.

First consumers are the `C-l` recenter and `M-r` window-line cycles currently
keyed by `editor.last_key`.  Preserve their exact cycle and reset PTYs, then
delete those uses of raw key identity.  Do not change kill coalescing in this
phase; Plan 05 gives that user-visible change its own commit.

Native tests cover identity through key/M-x/Lisp origins, nested invocation,
runtime redefinition/removal, refusal, and transient clearing.

## Phase 2 — Normalized key events

Create a self-contained `src/keyevent.h` and implementation.  The public value
has one base (Unicode scalar/raw byte or named terminal key) and modifier bits:

```c
struct key_event {
	int32_t base;
	uint8_t mods; /* CTRL, META, SHIFT */
};
```

Implement checked `key_event_equal`, `key_parse`, and `key_format`.  Formatting
is canonical; parsing may accept only documented aliases.  Cover printable
ASCII, UTF-8 input, control letters, `M-RET`, arrows, modified Home/End,
Backspace/Delete, function keys already supported, and every current `ALT_*`.
Reject duplicate/impossible modifiers and trailing input.

Initially add a lossless adapter between the legacy integer returned by the
TTY decoder and `key_event`; this keeps the terminal flag day separate from map
migration.  Then make `src/tty.c` emit normalized events and delete `alt_keys[]`
and the individual Meta enumerators once no consumer needs them.  ESC timing
and macro recording remain in the input layer.

Tests:

- parse/format round trips over all supported events;
- every legacy key maps to the expected normalized event during transition;
- unknown escape sequences and malformed UTF-8 preserve current unread-byte
  behavior;
- the keypress fuzzer drives normalized events and prefix sequences.

## Phase 3 — Keymap core and precedence

Create self-contained `src/keymap.h`/`src/keymap.c`.  Use a sparse trie (or an
equally bounded prefix structure) whose leaves contain an owned/interned command
name reference, never a C handler pointer.  Resolve it to the current
`command_id` at dispatch: user configuration may bind a name before a runtime
command is defined, and remove/redefine must not leave a pointer dangling.
Define these lookup results explicitly: no match, prefix, complete command, and
ambiguous configuration error.

Precedence is:

```text
transient > enabled minor maps (newest first) > major/special map > global
```

Only global and existing special-mode layers need consumers initially.  Do not
invent the future mode registry merely to hold empty layers.  Adapt existing
Dired, compilation, git commit/rebase, buffer-list, and user `C-c` predicates
into named layers; replace those adapters when a real mode registry lands.

Keymap storage and runtime mutations are bounded.  A failed bind leaves the map
unchanged.  Built-in declarations must resolve when installed; user/runtime
bindings may be temporarily unresolved and report that state.  Command removal
leaves a detectable name binding that can resolve again after definition,
never an accidental call through a reused pointer/slot.  Reserved emergency
keys cannot be shadowed.

Lookup must advance all candidate layers in parallel.  A major map containing
the prefix `C-c` may shadow its own `C-c C-k` leaf while an unrelated `C-c u`
still falls through to the user/global layer; selecting one winning layer at
the first prefix would swallow that valid fallback.

Native tests cover insert/rebind/unbind, prefix conflict, precedence, stale
runtime IDs, table exhaustion, and allocation failure if maps allocate.

## Phase 4 — Global dispatch migration

Move one coherent family per commit.  In the same commit:

1. add missing named descriptor and a short wrapper that owns prefix repetition;
2. add the global binding;
3. route it through `cmd_invoke()`;
4. delete the corresponding switch branch and any key-policy duplicate;
5. add/update parity tests and record scc/pmccabe before and after.

Recommended families:

1. motions and recenter/window-line cycles;
2. mark/region commands;
3. insert newline/open line/self-insert and overwrite behavior;
4. delete/kill/yank/undo;
5. search, word, paragraph, case, transpose, and whitespace commands;
6. help, suspend, macros, and remaining global commands.

Handlers, not key dispatch, own numeric prefix behavior so key, M-x, macro, and
Lisp invocation agree.  Self-insert may remain a measured fast fallback, but it
must perform the same read-only policy check and publish the same command
identity as a descriptor-backed invocation.  `C-g`, raw quoted input, and
universal-argument collection may stay explicit fast paths with comments that
name why they are outside the map.

Update `key_finish_keypress()` to use command identity/metadata rather than raw
key keep-lists for goal-column and shift-selection teardown.  Add only metadata
with an immediate consumer and invariant test.

Delete `readonly_blocked_keys[]` only when all editing paths in this phase have
moved.  The removal commit must prove read-only parity for printable input,
newline, backspace/delete, kill/yank, a mode binding, M-x, and Lisp.

## Phase 5 — Prefix and special-mode maps

Migrate in this order, one layer per commit:

1. `C-x` global prefix;
2. `C-x r` rectangles (leaving space for registers);
3. Dired and buffer-list keys;
4. compilation;
5. git commit and rebase;
6. `C-c` user bindings.

Prefix traversal state replaces `editor.cx_prefix`, `rect_prefix`, and
`cc_prefix`; it carries the full `command_prefix` without losing explicit zero.
An undefined sequence formats the whole canonical sequence in its diagnostic.
Mode maps must shadow global keys only while their existing predicates are true.

Once `C-c` uses the same trie, make `keybind_parse()` a restricted facade over
`key_parse()` and keep today's bindable subset until runtime keymaps are ready.
Remove the fixed 32-entry array only in the same commit that replaces its
bounded-storage policy and error behavior.

## Phase 6 — Introspection and generated drift checks

Add `describe-key`, `describe-command`, `describe-bindings`, and `where-is` over
the command/keymap registries.  Render into a normal read-only help buffer.
Report effective precedence and shadowed bindings.

Do not immediately regenerate the packed built-in help art.  First add a drift
test proving that every key it names parses and resolves in the expected layer.
The table cells have roughly 15-column descriptions while command summaries may
use 60 columns, so add an explicit width-checked short/help summary before
claiming textual equality or generating the art.  Replace the static table only
if generated output remains legible at its narrow width.

PTY cases cover a global binding, a shadowed mode binding, a prefix, an unbound
key, a removed runtime command, and narrow wrapping.  Update README/man/help for
new commands.

## Completion gate

- All built-in bindings resolve to command IDs, except documented input-layer
  fast paths.
- `readonly_blocked_keys[]`, `alt_keys[]`, and integer user binding storage are
  gone.
- No new command-policy or mode-key switch has appeared.
- `editor.last_key` and the three prefix booleans are gone.
- Every keymap leaf resolves or reports a stale runtime binding.
- Full tests pass in both Lisp configurations; keypress fuzz seeds replay; the
  full CI runner is green.
- Total scc and worst per-symbol complexity do not rise; durable reductions are
  banked in the ratchets.
