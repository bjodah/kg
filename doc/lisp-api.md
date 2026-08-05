# kg Lisp API reference

**Document version 2.** Covers the whole Lisp surface Plan 06 (Phases
2-8) shipped: buffers, markers, editing, search, save-excursion /
with-current-buffer, hooks, keymaps, processes, the function/value
namespaces, and provide / require / load-path. Bump this number when the
surface changes materially (a new native, a changed contract, a changed
limit); a wording fix does not need a bump. `README.md`'s "Lisp" section
is the narrative introduction and worked examples; this document is the
reference: object lifetimes, position units, ordering rules, error
handling, and every difference from Emacs Lisp in one place. Where the
two disagree on a detail, this document is authoritative — it is closer
to the code.

## Trust model — read this first

**Lisp in kg is trusted code, not a sandbox.** An init file and every
package it loads run with the full privileges of the editor process: no
capability restriction, no filesystem confinement, no resource limit
beyond the step budget and `C-g` cancellation described below. A
malicious or merely buggy init file can read, write or delete anything
the user running kg can. This is a deliberate design choice, the same
one Emacs makes for `.emacs`, and it is not scoped to change: do not
present kg's Lisp as sandboxed in documentation, error messages or
review, because it is not one.

## Position units

A **position** is a 1-based codepoint offset into a buffer, exactly as
Emacs numbers them: `(point-min)` is 1, `(point-max)` is one past the
last character, and every line break counts as one character regardless
of how many bytes it or any neighboring multi-byte character occupies.
This is the unit every native that takes or returns "a position" uses:
`point`, `goto-char`, `mark`, `region-beginning`/`region-end`,
`buffer-substring`, `char-after`, `search-forward`/`search-backward`,
`re-search-forward`/`re-search-backward`, `match-beginning`/`match-end`,
`marker-position`, and the `start`/`end` arguments
`after-change-functions` hands its callbacks.

Internally, buffer storage and the edit gateway (`kg_buffer_replace`,
`struct kg_edit`) address text as **flat bytes**: one buffer-wide space
with one `\n` between rows, the same "buffer byte" space
`doc/coordinates.md` describes for the editor core generally. Every
native that crosses this seam converts explicitly (`lisp_byte_of_char_offset`,
`lisp_char_offset_of` in `src/lisp_buffer.c`); nothing in the public
Lisp surface ever hands a byte offset to Lisp code or expects one back.
A result is that a position argument outside a native call is safe to
treat as "the same kind of number Emacs would print here" — but nothing
about kg's internal byte addressing is Lisp-visible, and no native
signature depends on the two staying numerically related for any given
buffer content (they diverge as soon as a buffer holds one multi-byte
character).

## Object lifetimes and generation checking

Buffers, markers and processes reach Lisp as opaque `FeTFex0` wrapper
objects over records in a bounded pool (`src/lisp_obj.[ch]`,
`LISP_MAX_OBJECTS` = 64 records shared across all three kinds). Only the
adapter creates these wrappers, so a Lisp value cannot be forged into
looking like a buffer or a process; every native that consumes one
checks both the wrapper's kind and the identity of what it names before
trusting it.

- **Buffer objects** are deduplicated: two calls that resolve to the
  same live buffer answer with the same `(eq ...)`-identical object.
  Killing a buffer does not un-mint its Lisp object, but every native
  that resolves one checks the buffer is still alive first and raises
  "buffer is dead" (or answers `nil`, for the handful of natives —
  `buffer-live-p`, `marker-position`/`marker-buffer` for a
  buffer-that-died-under-a-marker — documented to do that instead of
  raising) rather than reading freed memory.
- **Marker objects** are never deduplicated: `(eq (make-marker)
  (make-marker))` is `nil`, matching two calls to Emacs' `make-marker`.
  A marker's underlying position is mutable — `set-marker` can move the
  same Lisp-visible marker to a different buffer's marker store — so the
  pool record, not merely what it currently points at, is what identity
  has to track. A detached marker (never set, or its buffer died)
  resolves as "points nowhere": `marker-position`/`marker-buffer` answer
  `nil`, not an error, the same as Emacs.
- **Process objects** are deduplicated like buffer objects (one object
  per live table entry) and, like a buffer object, never change handle
  once minted. **The handle is the only identity a process ever
  exposes; no native takes or returns a PID.** It is a
  `{slot, generation}` pair into `KG_PROCESS_TABLE_MAX` = 8 bounded
  table entries: a handle to a slot that has since been reclaimed by a
  different process never resolves to that new occupant, because the
  generation numbers differ. A finished process keeps its terminal
  status queryable until `delete-process` releases the slot or the table
  needs room for a new spawn; only the *oldest finished* entry is ever
  reclaimed automatically, and a table full of *running* processes
  refuses a new spawn outright rather than reclaiming one.

Every one of these checks is "identity, not liveness of the C pointer":
a stale wrapper is always safe to hold and pass around from Lisp, and
every native that dereferences what it names re-checks generation and
kind on every call rather than trusting a value handed to it earlier.

## Safe points and callback ordering

kg has no event loop Lisp reaches into directly. Two callback families —
hooks (`add-hook`) and process filters/sentinels (`set-process-filter`,
`set-process-sentinel`) — are both delivered from the same place:
`kg_event_drain_safe()`, called only from the three points `main.c`
names safe (the top of the main loop and the two points named in
`src/event.h`'s own contract). A callback therefore never runs in the
middle of the edit, keystroke or process-output read that produced the
event it is reacting to; it always sees a buffer (and the rest of editor
state) in a fully-settled condition.

Ordering rules that hold across every subscriber:

- **Registration order** decides which of several callbacks on the same
  hook runs first, except where a specific contract overrides it (next
  bullet). A hook list holds at most 16 functions
  (`LISP_MAX_HOOK_ENTRIES_PER_HOOK`); adding the same function twice
  runs it twice.
- **A process's filter always sees every byte written before its
  sentinel fires**, even though the exit event and the last output event
  for one process can both be sitting in the queue at once, and even
  though the exit is detected (and its event published) as soon as the
  child is reaped — potentially before the last chunk of its output has
  been delivered. The drain flushes a process's queued output to its
  filter (or appends it to `process-buffer`) before invoking its
  sentinel, regardless of subscriber registration order. This is the one
  place ordering is enforced structurally rather than left to
  registration sequence, because it is also Emacs' contract.
- **A callback that errors is contained to itself.** The error is
  reported through the status line (`Hook error: ...` / a process
  callback's own labelled message) and does not stop the remaining
  subscribers for that event, other events in the same drain, other
  processes' callbacks, or any later delivery. Each callback gets its
  own saved/restored Lisp frame (`state.frame`, the same discipline
  `src/lisp_hooks.c`'s `run_one_hook_function` established and
  `src/lisp_process.c`'s process callbacks copy verbatim) so one bad
  callback cannot corrupt the frame the next one runs in.
- **Point is per-buffer, not per-callback.** Every top-level Lisp
  evaluation — including each individual hook or process-callback
  invocation — re-syncs its notion of "point" for the active buffer from
  that buffer's own remembered point at entry (`lisp_exec_enter`), and
  syncs it back to the window cursor on a successful exit
  (`lisp_exec_leave`) only when the buffer a callback worked in is still
  the one the active window shows. A callback that inspects or moves
  point at a position other than where it means to leave it needs its
  own `save-excursion`; nothing restores point automatically just
  because a callback returned. (`lisp/auto-fill.el`'s
  `auto-fill--maybe-break` is a worked example of getting this wrong and
  then right: its `auto-fill--column-at` helper moves point on purpose
  to answer "what column would this position be at", and every caller of
  it wraps that query in `save-excursion` — the *condition* of a `when`
  is not exempt, since evaluating it still runs the query's side
  effect.)
- **Process output is capped.** `KG_PROCESS_OUTPUT_MAX` = 256 KiB of
  undelivered output per process; the oldest bytes are dropped past that,
  with a `kg: output truncated` marker appended exactly once per overflow
  run, so a filter is never told nothing happened.

## Error handling and budget limits

- **Every top-level evaluation runs under a step budget**
  (`KG_LISP_STEP_LIMIT`, default 2^20 evaluation steps) and polls a host
  interrupt callback (`kg_lisp_set_interrupt_check`) so `C-g` can cancel
  a runaway evaluation. Both are enforced by the same `FeEvalOptions`
  (`eval_options` in `src/lisp_core.c`) every entry point —
  `eval-expression`, `eval-buffer`, `C-j`, a Lisp-defined command, a hook
  callback, a process callback — shares.
- **A raised error unwinds the whole top-level call**, not just the
  innermost form. kg's Lisp has no `condition-case`: nothing in Lisp
  catches an error partway, and `unwind-protect` (below) runs its cleanup
  forms as the error passes through without stopping it. Recovery
  is entirely the adapter's: `setjmp`/`longjmp` back to whichever C entry
  point (`kg_lisp_eval_string`, `kg_lisp_load_file`,
  `kg_lisp_run_command`, or a hook/process callback's own frame) started
  the evaluation, which then frees whatever it was holding (load buffers,
  scratch allocations, the require cycle-detection stack) and reports
  the labelled diagnostic. Forms evaluated **before** the error remain
  applied — an init file or package that fails partway through still has
  its earlier `defun`s and `setq`s in effect.
- **The interpreter's own recursion limit is two separate bounds**, not
  one. Fe's frame machine (sub-plan 03F) roots Lisp nesting — nested
  calls, nested special forms, self-expanding macros, deep argument
  lists — in the context-owned arena rather than in C recursion, so it
  costs a constant amount of C stack no matter how deep it goes:
  - **Lisp nesting** (`FeEvalOptions.max_frames`, 0 selecting the arena's
    own `frame_capacity`) counts live evaluator frames, not Lisp call
    frames one-for-one — an ordinary self-recursive function costs
    roughly 3 frames per level (`if`, the arithmetic, and the recursive
    call each open one), so in practice it stops `(deep n)`-shaped
    recursion a few hundred levels in. kg's default 1 MiB arena measures
    `frame_capacity` 1098; exceeding it raises
    `evaluation frame limit exceeded`. Macro expansion is bounded by the
    same limit, so a macro that expands into itself raises too.
  - **Native re-entry** (`FeEvalOptions.max_native_reentry`, 0 selecting
    `DefaultNativeReentry` = 32) counts nested evaluator runs a native
    starts synchronously, one below another — e.g.
    `internal--with-current-buffer` calling `FeCall` on a body that
    itself calls `internal--save-excursion`. Unlike Lisp nesting, each
    level here *is* a real C-stack bound (the native's own C activation,
    `FeCall`, and the nested run's barrier), so the default is a small
    number derived from kg's own corpus rather than a large one.
    Exceeding it raises `native evaluation re-entry limit exceeded`.
    Calling a native from Lisp is not itself re-entry; only that native
    synchronously starting another evaluation is, so ordinary Lisp
    nesting through natives never counts against this bound.

  Both are caught in every build, including every sanitizer lane: the
  frame machine keeps the real C stack flat, so it is never what fires
  first for Lisp nesting the way Fe's GC stack (`GcStackSize` = 4096) or
  the real C stack could before sub-plan 03F.

  Write list-walking code with `while`, not recursion — every list helper
  the prelude defines (`length`, `reverse`, `mapcar`, `assoc`, ...) does.
- **`load` nesting** (`(load ...)`, and the file `(require ...)`
  evaluates when a feature is not yet provided) is capped at
  `LISP_MAX_LOAD_DEPTH` = 8 levels, independent of the step budget.
  **`require` cycle detection** is separate again: it tracks feature
  *identity*, not nesting depth, in its own `LISP_MAX_REQUIRE_STACK` = 8
  entry stack, so `(require 'a)` from inside `(require 'a)`'s own load is
  an immediate "cyclic require" error naming the feature, not a
  depth-limit error and not a stack overflow — even if the actual nesting
  involved is well under 8.
- **A native's own bounded tables** refuse rather than silently drop or
  corrupt when full: `LISP_MAX_COMMANDS` = 32 Lisp-defined commands,
  `LISP_MAX_OBJECTS` = 64 pool records, `LISP_MAX_HOOKS` = 16 distinct
  hooks, `KG_PROCESS_TABLE_MAX` = 8 processes, `LISP_MAX_FEATURES` = 32
  provided features, `LISP_MAX_LOAD_PATH` = 8 load-path directories.
  Every one of these is a compile-time bound (`src/lisp_internal.h`,
  `src/lisp_obj.h`, `src/lisp_hooks.c`, `src/process_table.h`), not a
  heap allocation that grows; hitting one is an ordinary Lisp error, not
  a crash.

## Buffers, point and marks

| Form | Result |
| ---- | ------ |
| `(current-buffer)` | The exec buffer's object |
| `(buffer-list)` | All live buffers, as objects |
| `(get-buffer NAME)` | The buffer named `NAME`, or `nil` |
| `(get-buffer-create NAME)` | The buffer named `NAME`, creating it if needed |
| `(buffer-live-p OBJ)` | `t` for a live buffer object, else `nil` (never raises) |
| `(buffer-name &optional BUF)` | Display name of `BUF`, or the current buffer |
| `(set-buffer BUF)` | Make `BUF` current for the rest of this top-level form only |
| `(kill-buffer &optional BUF)` | Kill `BUF` (or current); raises on a modified buffer with no confirmation path from Lisp |
| `(point)` / `(point-min)` / `(point-max)` | Point and buffer bounds, 1-based |
| `(goto-char N)` | Move point to `N`, clamped to the buffer |
| `(goto-line N)` | Move point to the start of line `N`, clamped; takes no column |
| `(line-number-at-pos)` | 1-based line of point |
| `(current-column)` | Display column of point (tabs expand, wide characters count two) |
| `(mark)` / `(set-mark N)` / `(deactivate-mark)` | Mark and the region highlight; `set-mark` activates the region |
| `(region-beginning)` / `(region-end)` | Region bounds; error with no mark |
| `(buffer-substring BEG END)` | Text between two positions, order-insensitive |
| `(char-after &optional POS)` | Codepoint at `POS` (default point) as a number, `nil` at end of buffer |
| `(forward-word &optional N)` / `(backward-word &optional N)` | Move point over `N` words (ASCII word constituents only) |
| `(bounds-of-thing-at-point THING)` | Cons `(START . END)` for `'word` or `'line`, or `nil`; any other symbol raises |

`set-buffer` is a top-level-form-scoped selection: the *next* command or
evaluation starts again in the active window's buffer. Use
`with-current-buffer` (below) to scope a buffer selection explicitly to
one piece of code.

## Editing and search

Every one of these is one call through the edit gateway
(`kg_buffer_replace`, `KG_EDIT_INTERNAL`/`KG_EDIT_USER` as appropriate)
and therefore one undo step:

| Form | Result |
| ---- | ------ |
| `(insert TEXT)` | Insert `TEXT` at point |
| `(delete-region START END)` | Delete the region; positions in either order |
| `(replace-region START END TEXT)` | The region becomes `TEXT`, as one edit — never delete-then-insert |
| `(search-forward STRING &optional BOUND)` | Literal search to `BOUND` (default `point-max`); moves point past the match, or `nil` |
| `(search-backward STRING &optional BOUND)` | Literal search to `BOUND` (default `point-min`); moves point to the match start, or `nil` |
| `(re-search-forward PATTERN &optional BOUND)` | Regexp search forward; error on a bad or too-complex pattern |
| `(re-search-backward PATTERN &optional BOUND)` | Regexp search backward; see `src/lisp_search.c` for the exact (non-Emacs) rule |
| `(match-beginning N)` / `(match-end N)` | Group `N`'s bounds from the last search, or `nil` |
| `(make-marker)` | A marker at point in the current buffer |
| `(set-marker MARKER POS &optional BUF)` | Move `MARKER`; `POS` nil detaches it |
| `(marker-position MARKER)` / `(marker-buffer MARKER)` | Where `MARKER` points, or `nil` |

None of the search natives wrap around the buffer (unlike `C-s`/`C-r`),
none fold case, and a match cannot span two lines — the same limit
incremental search has, since both share `src/regex.h`'s
one-row-at-a-time engine. Match data outlives the search that set it,
the same way point outlives a `goto-char`: it is a piece of per-session
state (`struct kg_lisp_match_data`), not something scoped to one
top-level form.

## save-excursion, with-current-buffer and unwind-protect

| Form | Result |
| ---- | ------ |
| `(save-excursion BODY...)` | Restores point and the current buffer on every exit, including an error or `C-g` |
| `(with-current-buffer BUF BODY...)` | Evaluates `BODY` with `BUF` current, then restores; never selects a window |
| `(unwind-protect BODY CLEANUP...)` | Evaluates `BODY`, then `CLEANUP...` as an implicit `progn` on every exit — normal return, error, `C-g`, or step-budget exhaustion; the value is `BODY`'s, the cleanups' are discarded |

All three are the same mechanism: Fe's cleanup registry
(`FeProtectWithCleanup`), which the first two reach from C and
`unwind-protect` (a core Fe special form, not a prelude definition)
exposes to Lisp directly. So "every exit" is not an approximation: a
raised error or an interrupt inside `BODY` still restores what was
saved, or runs `CLEANUP`. Nesting is fine; each restores exactly what it
saved, in the reverse order it was saved. A cleanup form that itself
raises is reported directly rather than replacing the error already
unwinding, and the cleanups still pending after it run anyway.

`save-excursion` restoring "point" means restoring the *buffer's*
remembered point (see "Point is per-buffer" above) to what it was when
`save-excursion` was entered — not to some frame-global value. A
`save-excursion` that switches buffers via `set-buffer` inside its body
and then exits normally still restores the original buffer's point
correctly, because point is looked up and stored per buffer handle, not
per frame.

## Hooks

| Form | Result |
| ---- | ------ |
| `(add-hook 'HOOK FN &optional LOCAL)` | Add `FN`; `LOCAL` restricts it to the current buffer |
| `(remove-hook 'HOOK FN)` | Remove `FN` from `HOOK` |
| `(run-hooks 'HOOK)` | Run `HOOK`'s functions now, synchronously in the calling frame |

The hooks that exist: `after-change-functions` (called `(FN BUFFER START
END OLD-LEN)` — `START`/`END` are 1-based positions of the *new* text,
`OLD-LEN` is how many characters the edit replaced), `find-file-hook`
(called `(FN)`, no arguments, once a buffer has finished opening),
`before-save-hook` and `after-save-hook` (called `(FN)`). There is
deliberately no `post-command-hook`; its per-keystroke cost has not been
measured.

**`FN` may be a function value or a quoted symbol naming one.**
`(add-hook 'my-hook 'my-fn)` is the Emacs idiom and works here too. A
symbol is resolved through the *function namespace* when the hook runs
rather than when it is added, so redefining `my-fn` afterwards takes
effect, and a symbol whose function cell is empty is reported as an
ordinary contained hook error naming it rather than crashing the hook
run.

## Key bindings

| Form | Result |
| ---- | ------ |
| `(global-set-key "C-c <key>" NAME)` | Bind a `C-c` sequence to command `NAME` (string or symbol) |
| `(global-unset-key "C-c <key>")` | Unbind it |
| `(define-key MAP KEY COMMAND)` | Bind `KEY` in `MAP`; `nil` `COMMAND` unbinds; `KEY` may be any sequence, not just `C-c` |
| `(lookup-key MAP KEY)` | What `MAP` alone says `KEY` means, regardless of whether `MAP` is currently active |
| `(current-local-map)` | The active major-mode map, or `nil` |

Map names are kg's own (`global`, `dired`, `compilation`, ...); the
Emacs spellings `global-map` and `dired-mode-map` resolve to them.
`global-set-key`/`global-unset-key` only ever accept `C-c <key>` —
`C-c` is reserved for user bindings so they can never shadow a built-in
key — while `define-key` takes any sequence the built-in maps could
hold, so it *can* shadow a built-in binding; `C-g` and `C-x C-c` are the
keys to leave alone.

## Processes

| Form | Result |
| ---- | ------ |
| `(start-process NAME BUFFER PROGRAM &rest ARGS)` | Exec `PROGRAM` with `ARGS` directly — no shell, so no word splitting, globbing or redirection touches an argument |
| `(start-shell-command NAME BUFFER COMMAND)` | Run `COMMAND` through `/bin/sh -c`, the way `M-!` does |
| `(process-live-p PROC)` | `t` while `PROC` is running (generation-checked), else `nil` |
| `(delete-process PROC)` | `SIGTERM` to the process group, a bounded wait, then `SIGKILL`; releases the slot |
| `(process-buffer PROC)` | `PROC`'s target buffer object, or `nil` once it is gone |
| `(set-process-filter PROC FN)` | `FN` is called `(FN PROC STRING)` per delivered chunk; `nil` restores appending to `process-buffer` |
| `(set-process-sentinel PROC FN)` | `FN` is called `(FN PROC EVENT-STRING)` once the process exits |
| `(process-status PROC)` | `'run`, `'exit` or `'signal` |

`BUFFER` is a buffer object, a name (resolved exactly as
`get-buffer-create` resolves one), or `nil` to discard output entirely.
Setting a filter stops output from landing in `process-buffer` — the
filter owns it, exactly as in Emacs — and `(set-process-filter PROC
nil)` resumes auto-append. Children get pipes, not a pty, and
`/dev/null` on stdin; there is no `process-send-string`. Every tracked
process is killed (its whole process group, so a grandchild dies too)
and reaped when kg exits — `kg_process_table_shutdown()`, wired into the
same `atexit` teardown as everything else — so nothing outlives the
editor.

## provide / require / featurep / load-path

| Form | Result |
| ---- | ------ |
| `(provide FEATURE)` | Register `FEATURE` (a symbol or a string) in the bounded feature table; returns `FEATURE` |
| `(require FEATURE &optional FILENAME)` | No-op (returns `FEATURE`) if already provided; else resolves `FILENAME` (or `FEATURE`'s own name) through `load-path` and evaluates it nested, the way `load` nests; errors if the feature is still not provided afterward |
| `(featurep FEATURE)` | `t`/`nil`, without loading anything |
| `(add-to-load-path DIR)` | Prepend `DIR` to the bounded load-path, so it is searched before every directory already in it |

`load-path` itself is not Lisp-visible as a list — it is a bounded
C-side array of `LISP_MAX_LOAD_PATH` = 8 directories, each up to
`PATH_MAX` bytes, which is why it has its own mutator native
(`add-to-load-path`) instead of the Emacs spelling `(push DIR
load-path)`: a Fe list the adapter had to re-validate (bounds, that
every element is actually a directory-shaped string) on every `require`
would cost more than it is worth for something only ever read
directory-at-a-time. It defaults to one entry,
`$XDG_CONFIG_HOME/kg/lisp` (falling back to `~/.config/kg/lisp`), seeded
the first time either `require` or `add-to-load-path` runs — whichever
comes first, so calling `add-to-load-path` before the first `require`
still leaves the default directory searched *after* whatever was just
added, never skipped. A directory that does not exist is not a
`load-path` error by itself; it just never matches anything, exactly
like a missing file would.

`require`'s `FILENAME` argument, like a bare `load` name, is a literal
path when it contains `/` and otherwise a stem resolved to
`DIR/FILENAME.el` in each `load-path` directory in order, first match
wins — which is what makes "load-path order decides which of two
same-named files wins" a real, testable property
(`test/test_lisp.c`'s `test_load_path_order`) rather than an
accident of directory listing order.

`require`'s cycle detection and `load`'s depth limit are different
bounds answering different questions; see "Error handling and budget
limits" above.

## Strings and the prelude

Fe has no string operations of its own; every one of these is a kg
native (`src/lisp_string.c`), indexed by codepoint like the position API
so no result is ever cut mid-glyph:

| Form | Result |
| ---- | ------ |
| `(string-length S)` | Length of `S` in characters |
| `(substring S FROM &optional TO)` | 0-based character indices; negative counts from the end; clamps out of range; `TO` before `FROM` yields `""` |
| `(concat A B ...)` | Joins any number of strings; `(concat)` is `""` |
| `(string= A B)` | `t` when equal |
| `(char-to-string N)` | One-character string for codepoint `N`; rejects 0, surrogates, values above `U+10FFFF` |
| `(string-to-char S)` | First codepoint of `S`, `nil` for `""` |
| `(format FORMAT ARG ...)` | `%s`/`%S`/`%d`/`%e`/`%f`/`%g`/`%%`; no field widths, no `%c`/`%x`/`%o`; extra arguments ignored, a missing one or an unknown specifier raises |

kg evaluates a prelude (`lisp/prelude.el`, embedded into the binary as
`src/lisp_prelude_generated.inc`), written in Fe, at startup
before any init file runs — this is what makes `defun`, `let`, `cond`,
`dolist` and the rest available at all, since upstream Fe has only
`lambda`, one-binding `let`, `if` and `while`:

| Group | Forms |
| ---- | ------ |
| Definitions | `defun` `defmacro` `defvar` `defconst` `interactive` `lambda` |
| Binding | `let` `let*` `setq` `progn` |
| Control | `cond` `when` `unless` `prog1` `dolist` `dotimes` |
| Lists | `length` `nth` `nthcdr` `last` `reverse` `append` `mapcar` `assoc` `member` `memq` `push` `pop` `caar` `cadr` `cddr` `1+` `1-` |
| Predicates | `null` `eq` `equal` `listp` `type-of` `stringp` `symbolp` `numberp` `consp` `functionp` `boundp` |
| Functions | `funcall` `apply` `function` (written `#'f`) `fboundp` `symbol-function` `symbol-value` `fset` `defalias` `fmakunbound` |
| Quoting | `` ` `` / `,` / `,@` (quasiquote); `#'f` is `(function f)` |
| Editor | `string-empty-p` `thing-at-point` |

The table is the whole startup surface, not only what the prelude adds:
the nine forms in `Functions`, like `setq` in `Binding`, are core Fe
special forms and primitives rather than prelude definitions.

`defun` strips a body `(interactive)` form and registers the function as
a command under its own name (`define-command` underneath), the same as
Emacs' `defun` plus `(interactive)` making a command. `command-execute`
(and `M-x`) can then run it, subject to the same `CMD_LISP_CALLABLE` /
`CMD_EDITS_BUFFER` verdicts as every built-in command.

## Namespaces: function and value cells

Fe is Lisp-2, like Emacs: every symbol has two independent cells — a
*value cell*, which bare-symbol evaluation reads and `setq`/`defvar`/`set`
write, and a *function cell*, which call position resolves and
`defun`/`defalias`/`fset` write. The two never shadow each other:

```lisp
(setq f 7)
(defun f () 9)
(list f (f))                ;; => (7 9)
```

A name in call position resolves its function cell only: calling a name
whose function cell is empty is `void-function NAME`, even when the name
has a value. Bare-symbol evaluation reads the value cell only, so a name
that has never been assigned a value is `void-variable NAME` regardless
of its function cell. Ask which namespace a name has with
`(fboundp 'NAME)` (function cell) and `(boundp 'NAME)` (value cell);
remove from either with `(fmakunbound 'NAME)` and `(makunbound 'NAME)`;
read the two cells directly with `(symbol-function 'NAME)` and
`(symbol-value 'NAME)`.

| Form | Result |
| ---- | ------ |
| `(function F)` / `#'F` | The function designator, without evaluating `F`: a symbol is returned as-is, a `(lambda ...)` form becomes the closure. `#'` is the reader's abbreviation for `(function ...)`, and the writer prints a `(function X)` form back as `#'X` — which is what `M-:` / `eval-expression` shows |
| `(funcall F &rest ARGS)` | Call function object or designator `F` with `ARGS` |
| `(apply F &rest ARGS LIST)` | Like `funcall`, with the final operand a list whose elements are appended as arguments |
| `(fset 'NAME FN)` | Write `FN` into `NAME`'s function cell |
| `(defalias 'NAME FN)` | Emacs' spelling for installing `FN` in `NAME`'s function cell; the cell may hold a symbol, which is resolved at call time, so a `defalias` chain is late-bound |
| `(fboundp 'NAME)` | `t` if `NAME`'s function cell holds anything, else `nil` — never consults the value cell, never errors |
| `(symbol-function 'NAME)` | `NAME`'s function cell, without resolving a designator (`void-function` when empty) |
| `(symbol-value 'NAME)` | `NAME`'s value cell, without evaluating it (`void-variable` when empty) |
| `(fmakunbound 'NAME)` | Empty `NAME`'s function cell |

Because call position reads only the function cell, a function held in a
*variable* must be called with `funcall`:

```lisp
(mapcar #'car lst)          ;; head-position designator: fine
(let ((f #'car)) (funcall f lst))   ;; a value is not callable
```

`(functionp F)` asks about what `F` resolves to, not about `F` itself: a
symbol is followed through its function cell, so `(functionp 'car)` is
`t` and a name bound only as a value is `nil`. **Special forms and macros
are not functions**, as in Emacs — `(functionp 'if)`, `(functionp 'let)`
and `(functionp 'when)` are all `nil`, however callable those names look
in head position — while a closure, an editor native and a
function-shaped primitive are all `t`. A *cyclic* alias chain
(`(fset 'x 'x)`) is answered rather than raised at: `(functionp 'x)` is
`nil` and `(fboundp 'x)` is `t`, since the cell does hold something.
*Calling* the name is what raises `cyclic-function-indirection` —
`(x)`, `(funcall 'x)` — and a cyclic name used as a hook or a process
callback is reported like any other unresolvable one, as
`void-function x`. (Emacs reaches the same `nil` by a different route:
its `fset` refuses to build the cycle at all.)

kg's own prelude is written against these rules: its top-level
definitions are installed with `defalias` into function cells, the
primitive aliases (`progn`, `null`, `eq`, ...) capture the primitive's
own function cell with `(defalias 'progn (symbol-function 'do))`, and
`internal--let` is pinned *before* the Emacs `let` macro overwrites the
primitive's function cell.

## Explicit differences from Emacs Lisp

- **There is no `>` or `>=`.** Fe defines `<` and `<=` only; write `(>
  a b)` as `(< b a)`.
- `eq` compares numbers and strings **by value**, so `(eq "a" "a")` is
  `t` where Emacs says `nil`; only pairs are compared by identity (`is`).
- Every number is a double; there is no character type — write
  `(string-to-char "a")` rather than `?a`.
- `t` is an ordinary assignable global, not a self-evaluating constant.
- **No `condition-case`, no dynamic binding, no vectors, no hash tables,
  no property lists.** (`unwind-protect` does exist — see above — but it
  runs cleanups rather than catching. No property lists is
  why there are no docstring-backed `describe-function`-style natives
  yet — see below.) The namespace diagnostics `void-function`,
  `void-variable` and `cyclic-function-indirection` are names carried in
  the error *message*, not catchable condition objects — there is no
  `condition-case` to catch them with (Phase 6).
- **A macro's function cell holds fe's own macro object**, not Emacs'
  `(macro . FUNCTION)` cons: `(symbol-function 'a-macro)` prints
  `(macro (args) ...)` rather than Emacs' `(macro . FUNCTION)`. A
  recorded, tested representation divergence (fe's manifest pins it as a
  `kg-policy` entry, `lisp2-macro-representation`), observable only
  through `symbol-function` of a macro.
- Recursion is bounded by the interpreter's two frame limits, not by
  Fe's GC stack — see "Error handling and budget limits" above; walk long
  lists with `while`.
- A self-referential structure prints as far as the cycle, then
  `#<cycle>`, rather than looping forever.
- No source line numbers in error messages: a raised error names what
  failed, not where in the source it was written. Fe's reader does not
  carry position information through to evaluation; adding it is a
  `fe/` submodule change with its own pin move, not a kg-side one.
- No docstring introspection: `defun`'s optional docstring string
  literal is accepted syntactically (as an ordinary, ignored body form
  before the code) but nothing stores it anywhere, and there is no
  `describe-function`. `src/describe.c` today describes keys, commands
  and bindings, not functions; adding this needs a bounded
  symbol-to-docstring table, which is not part of the surface this
  document describes.

## What is not here, and why

Three things a reader coming from Emacs might expect are deliberately
absent from this surface, each recorded here rather than silently
missing:

- **Mode hooks / a mode registry** (`define-derived-mode`, per-mode
  `defvar`s). kg's major modes are still a fixed C table
  (`src/mode.[ch]`); there is no Lisp-visible way to define one.
- **Decoration natives** (anything a package would use to add
  `whitespace-mode`-style highlighting). `src/decor.[ch]` exists and
  backs kg's own syntax highlighting, but nothing in it is reachable
  from Lisp yet.
- **`process-send-string`** and PTY allocation for child processes —
  processes get pipes and `/dev/null` on stdin only; see "Processes"
  above.

## Where each piece lives

| Concern | Module |
| ---- | ---- |
| Interpreter lifecycle, `WITH_LISP=0` stubs | `src/lisp_core.c` |
| Natives bound at startup | `src/lisp_prelude.c` |
| The Fe-written prelude itself | `lisp/prelude.el`, embedded as `src/lisp_prelude_generated.inc` |
| Position/codepoint conversions, buffer/mark/point natives | `src/lisp_buffer.c` |
| Word motion, `bounds-of-thing-at-point` | `src/lisp_word.c` |
| `load`, `require`/`provide`/`featurep`/load-path, XDG config resolution, `format`, `message`, `insert`, region edits | `src/lisp_io.c`, `src/lisp_require.c` |
| Command registry, `command-execute`, key bindings | `src/lisp_cmd.c` |
| Buffer/marker/process object pool, generation checks | `src/lisp_obj.[ch]` |
| Search and match data | `src/lisp_search.c` |
| Hooks and their event-drain subscriber | `src/lisp_hooks.[ch]` |
| Process objects, filters, sentinels | `src/lisp_process.[ch]`, `src/process_table.[ch]`, `src/process.[ch]` |
| String natives | `src/lisp_string.c` |
| The public, Fe-free surface every editor module includes | `src/lisp.h` |

`src/lisp_internal.h` is the private surface shared only among
`src/lisp_*.c` (`make lisp-include-check` enforces the boundary); it is
not part of this document because nothing outside those files may
include it.
