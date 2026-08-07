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
- **A callback that errors is contained to itself; a callback that is
  cancelled is not.** Those are two different completions and the seam
  treats them differently on purpose. An *error* — and a `throw` that
  names a `catch` outside the callback, which arrives as `(no-catch TAG
  VALUE)`, since the containment boundary is a catch wall too — is
  reported through the status line (`Hook error (NAME): ...` / a process
  callback's own labelled message) and does not stop the remaining
  subscribers for that event, other events in the same drain, other
  processes' callbacks, or any later delivery. A *`quit`* (`C-g`) or a
  *budget exhaustion* is re-signalled instead of swallowed: it abandons
  the rest of the hook list or the rest of that event's delivery and
  cancels the enclosing evaluation, reporting as `Quit` or as the
  budget's own message. Containment must never be able to eat a `C-g`.
  The containment itself is Fe's protected call
  (`FeTryCallWithOptions`), whose `setjmp` lives inside Fe in a frame
  that is live for the length of the call; the host `setjmp` this
  replaced was a longjmp target Fe had already unwound past, which is
  undefined and in practice corrupted the GC stack. `src/lisp_hooks.c`
  and `src/lisp_process.c` each keep exactly one guarded frame, at the
  event-subscriber level, for what a protected call deliberately does
  not contain: a raise from kg's own bookkeeping around the call, and
  the re-signalled quit.
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
  callback, a process callback — shares. The budget is what bounds
  `equal` on a long list, and 05E's type-honest leaf comparison — `eql`
  rather than fe's broad `is` — made that bound tighter: measured on the
  default budget, `(equal l l)`
  succeeds on a flat 7489-element list and exhausts at 7490, where the
  pre-05E `is`-tailed definition reached 9361 — 20% less reachable list
  length for the same budget. Nesting depth is unaffected, since `equal`
  walks the spine iteratively and only recurses per element.
- **A raised error unwinds the whole top-level call** unless caught by
  `condition-case`: `(condition-case VAR BODY (CONDITION HANDLER...) …)`
  catches errors of the named condition (or its sub-conditions in the
  static hierarchy) and runs the first matching handler as an implicit
  `progn`; unmatched conditions re-signal unchanged. `unwind-protect`
  runs its cleanup forms when any non-local exit — error, `throw`, or
  `quit` — passes through, innermost-first. Recovery at the C boundary
  (`kg_lisp_eval_string`, `kg_lisp_load_file`, `kg_lisp_run_command`, or
  a hook/process callback's own frame) frees whatever it was holding
  (load buffers, scratch allocations, the require cycle-detection stack)
  and reports the labelled diagnostic.   A `quit` (from `C-g` or
  `(signal 'quit nil)`) reports as `Quit`; a budget exhaustion keeps its
  explicit message; an ordinary error keeps today's format verbatim.
  `quit` is catchable by `(quit …)` and `(t)` handlers but not by
  `(error …)`; budget exhaustion is catchable by nothing — it always
  reaches the host.  Forms evaluated **before** the error remain
  applied — an init file or package that fails partway through still has
  its earlier `defun`s and `setq`s in effect. Errors raised while loading a
  file include its source label and the 1-based line of the top-level form,
  for example `init.el:7: ...`; missing files name the resolved path tried.
  Sub-form columns are intentionally not reported.
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
    `frame_capacity` 1096; exceeding it raises
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
- **The object arena is fixed and exhaustible, and exhaustion is an
  ordinary catchable condition.** kg opens Fe with a 1 MiB arena that
  never grows (`KG_LISP_ARENA_SIZE`, `src/lisp_core.c`), measured at the
  current pin as 56222 object slots and a 1096-frame evaluator stack.
  A program that consumes all of them raises `out of memory` under the
  condition `arena-exhaustion`, and a program that fills Fe's GC root
  stack raises `GC stack overflow` under `evaluation-stack-exhaustion`.
  Both sit under `error` in the hierarchy, so all three of these catch
  them:

  ```elisp
  (condition-case e (make-a-lot) (t 'caught))
  (condition-case e (make-a-lot) (error 'caught))
  (condition-case e (make-a-lot) (arena-exhaustion 'caught))
  ```

  Fe pre-builds both condition objects when the context opens and keeps
  them rooted, so signalling one allocates nothing — which is what makes
  them catchable when there is no memory left to build a condition with.
  A *named* raise that happens while the arena is full arrives as
  `arena-exhaustion` rather than as its own name, because its condition
  could not be built; a `quit` is unaffected, since it is matched by
  completion kind rather than by object shape. Budget exhaustion (steps,
  frames, native re-entry) remains catchable by nothing.

  **Recovery depends on what the exhausting data was reachable from.**
  Data held in a `let` local or an argument is unreachable the moment the
  handler runs, so the next collection returns it and the session is
  normal again. Data assigned to a *global* keeps the arena pinned: the
  session stays alive and keeps reporting truthfully, but the space does
  not come back. kg has no arena-reset command (`doc/TODO.md` records it
  as debt); restarting is the recovery.
- **`(command-execute "lisp-arena-stats")`** reports the arena's state to
  the echo area: total slots, free slots, peak live objects, collections,
  peak GC root depth, peak frames against `frame_capacity`, and the count
  of failed allocations. It allocates nothing and mutates nothing, so it
  is safe to call from inside a handler that has just caught an
  `arena-exhaustion` — it is the one way for a program to see how much of
  the arena came back. `M-x lisp-arena-stats` is the same command.

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
| `(save-excursion BODY...)` | Restores point and the current buffer on every exit, including an error, a `throw` or `C-g` |
| `(with-current-buffer BUF BODY...)` | Evaluates `BODY` with `BUF` current, then restores; never selects a window |
| `(unwind-protect BODY CLEANUP...)` | Evaluates `BODY`, then `CLEANUP...` as an implicit `progn` on every exit — normal return, error, `throw`, `C-g`, or step-budget exhaustion; the value is `BODY`'s, the cleanups' are discarded |

All three are the same mechanism: Fe's cleanup registry
(`FeProtectWithCleanup`), which the first two reach from C and
`unwind-protect` (a core Fe special form, not a prelude definition)
exposes to Lisp directly. So "every exit" is not an approximation: a
raised error or an interrupt inside `BODY` still restores what was
saved, or runs `CLEANUP`. Nesting is fine; each restores exactly what it
saved, in the reverse order it was saved. A cleanup form that itself
raises is reported directly rather than replacing the error already
unwinding, and the cleanups still pending after it run anyway.

`save-excursion` and `with-current-buffer` are *transparent* to the
evaluation that encloses them: an error raised inside either body
reaches a `condition-case` written around the form, carrying its
original condition symbol, and the restore has already run by the time
the handler does. (They are natives that run the body as a thunk, and
they get that transparency from Fe's protected call plus `FeResignal` —
`lisp_call_body` in `src/lisp_core.c` — which puts the completion back
in flight in the enclosing run instead of transferring it straight to
kg's outermost barrier.) `throw` is the one exception, and a recorded
divergence from Emacs: Fe walls the throw search at a native re-entry
boundary, so a `throw` out of either body finds no catch, becomes
`(no-catch TAG VALUE)` — which an enclosing `condition-case` still
handles, and which still runs the restore — and does *not* reach the
`catch` that names the tag. `test/lisp-compat/features.json`'s
`catch-throw-reachability` row is where that is written down.

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
| `(format FORMAT ARG ...)` | `%s`/`%S`/`%d`/`%e`/`%f`/`%g`/`%c`/`%x`/`%X`/`%o`/`%%`; `-`/`0`, widths and precision are supported for numeric and string/character conversions; `%c` writes a UTF-8 codepoint, and refuses 0 where Emacs writes a NUL byte; Emacs' `+`, ` ` and `#` flags and its `N$` field numbers raise `invalid format operation`; extra arguments ignored, a missing one or an unknown specifier raises |

kg evaluates a prelude (`lisp/prelude.el`, embedded into the binary as
`src/lisp_prelude_generated.inc`), written in Fe, at startup
before any init file runs — this is what makes `defun`, `let`, `cond`,
`dolist` and the rest available at all, since upstream Fe has only
`lambda`, one-binding `let`, `if` and `while`:

| Group | Forms |
| ---- | ------ |
| Definitions | `defun` `defmacro` `defvar` `defconst` `defcustom` `custom-set-variables` `declare` `interactive` `lambda` |
| Binding | `let` `let*` `setq` `progn` |
| Control | `cond` `when` `unless` `prog1` `dolist` `dotimes` |
| Non-local exits | `catch` `throw` `condition-case` `signal` `error` `unwind-protect` `ignore-errors` — all core Fe forms except `ignore-errors`, which is the prelude's one-line macro over `condition-case` |
| Lists | `length` `nth` `nthcdr` `last` `reverse` `append` `mapcar` `mapc` `mapconcat` `assoc` `assq` `member` `memq` `push` `pop` `nreverse` `delq` `delete` `add-to-list` `caar` `cadr` `cddr` `1+` `1-` |
| Predicates | `null` `eq` `eql` `equal` `zerop` `integerp` `floatp` `listp` `type-of` `stringp` `symbolp` `numberp` `consp` `functionp` `commandp` `keywordp` `boundp` |
| Functions | `funcall` `apply` `function` (written `#'f`) `fboundp` `symbol-function` `symbol-value` `fset` `defalias` `fmakunbound` |
| Numbers | `+` `-` `*` `/` and the comparators `=` `<` `<=` `>` `>=` `/=` |
| Quoting | `` ` `` / `,` / `,@` (quasiquote); `#'f` is `(function f)` |
| Editor | `string-empty-p` `thing-at-point` |
| Small library | `identity` `prog2` `max` `min` `documentation` `number-to-string` `string-to-list` `setq-default` `setq-local` `kbd` |

The table is the whole startup surface, not only what the prelude adds:
the forms in `Functions` and `Numbers`, like `setq` in `Binding`, are
core Fe special forms and primitives rather than prelude definitions.

`setq-default` and `setq-local` are aliases of `setq` because kg has no
buffer-local variable namespace. `load-path` remains a bounded C search-path
array; use kg's `add-to-load-path` native rather than modifying it with
`add-to-list`.

`defun` recognises only an `(interactive ...)` form immediately after its
optional docstring. That declaration is removed from the body and registers
the closure as a command; a later form remains ordinary code. A docstring
followed by an empty declaration body gets an implicit `nil` body — but a
docstring that *is* the whole body is the body, and the function returns it,
as in Emacs. `(commandp NAME)` answers whether a name is a command. It asks
the command registry, which is the only place kg can answer from: 07D adds no
interactive-form reflection, so unlike Emacs it says `nil` for an anonymous
lambda that carries an interactive form, and it does not accept Emacs'
optional FOR-CALL-INTERACTIVELY argument. Both are recorded divergences.

The declaration's nil/empty spec supplies no arguments; string specs split
newline-delimited clauses and support `p`, `P`, `r`, `s`, `n`, `N`, `f`, `F`,
`b`, and `B`. An interior or trailing empty clause is an invalid
specification, not a skipped one, and each clause's prompt is its own tail —
it stops at the newline that ends the clause. An interactive command receives
at most 16 arguments; the cap is a recorded divergence, not a silent
truncation. `p` receives `prefix-numeric-value`, `P` receives the raw prefix,
and `r` receives sorted region bounds. `s` reads literal text; `N` uses the
prefix when supplied and otherwise runs exactly the `n` path. `f`/`F` read
paths without visiting them, with `f` requiring an existing entry, while
`b`/`B` read buffer names without selecting them (`B` permits a new name, and
`M-RET` accepts typed text literally even when a completion exists).
Cancellation is a quit and overflow is an error.

Instead of a specification string the declaration may carry a single **form**,
as in `(interactive (list 1 2))`. The `defun` macro wraps it in a
zero-argument thunk in the command closure's own lexical environment —
creating the thunk does not evaluate the form — and calls that thunk once at
invocation, after `current-prefix-arg` is bound and before the body. It must
return a proper list; an improper one raises `(wrong-type-argument listp
VALUE)` naming the tail. `define-command`'s spec argument takes the same
thunk, but cannot take detached raw form syntax, whose lexical environment it
would have to guess. Emacs' additional `interactive` MODES arguments are
accepted and ignored — a recorded divergence.

`n` and `N` accept one decimal token: an optional sign, digits, an optional
fraction and an optional exponent, with nothing but ASCII whitespace around
it. That is fe's own reader grammar minus its `1e+INF`/`1e+NaN` spellings, so
`inf`, `nan`, `0x10`, `1e`, trailing junk, a second token and an empty answer
all re-prompt. Classification happens before any conversion and evaluates no
Lisp. An integer past `int64` becomes a float, as an integer literal does in
fe's reader; kg has no bignums.

Prompts are literal. Emacs passes a prompt containing `%` through `format`
with the earlier interactive arguments; kg does not, and records that
interpolation as a divergence rather than treating status text as a format
string. A valid Emacs code kg has not implemented, and the deferred modifiers
`*`, `@` and `^`, report `unsupported interactive code CODE`; a byte outside
Emacs' measured set reports `invalid interactive code CODE`. In both cases the
command body does not run.

`current-prefix-arg` is temporarily bound during a command. Its raw values
are nil, an integer, a one-element list for a universal prefix, or the symbol
`-`; `(prefix-numeric-value X)` converts these forms, and a malformed one
raises a real `wrong-type-argument` condition carrying the value, which a
handler naming `wrong-type-argument` catches. A run of bare `C-u` produces the
uncapped `(4)`, `(16)`, `(64)`, ... Emacs produces; the 1000 cap belongs to
the effective integer, not to the raw form. `P` is `eq` to that temporary value. This is a command-boundary value
binding, not general dynamic binding, so a lexical variable named
`current-prefix-arg` shadows it.

`command-execute` uses the same metadata and evaluator, including nested
calls, and returns the command's value; an inner call inherits the active
command's prefix when there is one and uses none otherwise. A nested call
builds and runs inside the evaluator already running, so its error or quit
reaches a `condition-case` lexically around the `(command-execute ...)`. `define-command` is the
kg-owned extension `(define-command NAME FUNCTION &optional SPEC DOC)`; its
spec is nil, a string, or a zero-argument function, and documentation is nil or
a string. Interactive definitions replace their function, spec and document
atomically; redefining without a declaration removes command status.
Prompting is refused outside a key/M-x command context or while another
prompt is active. kg has no public `read-*` APIs or `completing-read`; prompt
interpolation and deferred codes remain explicit divergences.

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
primitive aliases (`progn` and `null`) capture the primitive's
own function cell with `(defalias 'progn (symbol-function 'do))`, and
`internal--let` is pinned *before* the Emacs `let` macro overwrites the
primitive's function cell.

## Explicit differences from Emacs Lisp

- **Function arity is strict.** Calls with too few or too many arguments raise
  `wrong-number-of-arguments` with data `(FUNCTION NARGS)`. `&optional`
  parameters bind `nil` when omitted, and `&rest` collects the remaining
  arguments into a fresh list. Native helper checks use the same condition
  while preserving their existing rendered error text.
- `eq` is Emacs' `eq`: `(eq 3 3)` is `t` (fixnum equality — integers
  compare by value) but two separately-read equal strings, and two
  float objects, are `nil`. Fe's own broad comparator remains available
  as `is` — doubles approximately by value (its own epsilon), integers
  exactly, strings by content, everything else by identity — but `is`
  is fe-native, not an Emacs form.
- Numbers are signed 64-bit integers or doubles — **no bignums**. Integer
  arithmetic that overflows, and integer division by zero, raise an
  `arith-error` message rather than promoting or wrapping. Character
  literals such as `?a` read as their codepoint numbers.
- `t`, `nil` and keyword symbols are protected constants: `setq`, `set`,
  a `let`/`let*` binding name, `fset` and `defalias` all refuse them with
  the `setting-constant` condition. Keywords are self-evaluating, and
  `(keywordp X)` answers whether a symbol is one — `:` alone included. A
  lambda parameter may still shadow `t`, matching the measured lexical
  oracle; unlike Emacs, one named `nil` or a keyword is refused rather
  than bound (a divergence recorded in fe's own compat corpus).
- **`condition-case` exists; `catch`/`throw` exist; `signal`/`error`
  exist.** Conditions have a static hierarchy: `wrong-type-argument`,
  `wrong-number-of-arguments`, `void-function`, `void-variable`,
  `arith-error`, `args-out-of-range`, `file-error`, `setting-constant`,
  and `no-catch` are all under `error`; `quit` is a separate branch not under `error` and
  is not catchable by `(error …)` handlers. `(signal 'ARITH-ERROR DATA)`
  raises a condition object `(ARITH-ERROR . DATA)`; `(error "fmt" ARGS)`
  formats at signal time and raises `(error "formatted-text")`.
  `throw` finds the innermost matching `catch` by `eq` tag and unwinds
  `unwind-protect` cleanups on the way; an uncaught `throw` signals
  `(no-catch TAG VALUE)`. `ignore-errors` is a one-line macro over
  `condition-case`. **kg's own editor natives still signal a plain
  `error`** whose message happens to read like a condition name, so
  `(condition-case e (goto-char "x") (error …))` catches while
  `(… (wrong-type-argument …))` does not; classifying kg's ~81 natives
  is the follow-up sub-plan 06A's Decision 2 deferred, and Fe's own
  natives are already classified, which is why `(car 1)` *does* match a
  `wrong-type-argument` handler.
- **No dynamic binding, no vectors, no hash tables, no property lists.**
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
- Source positions are per top-level form, not per sub-form. An error
  raised while loading a file carries its source label and the 1-based
  line of the top-level form that was running (`init.el:7: ...`), which
  is what "Error handling and budget limits" above describes; it does not
  narrow to the sub-form, the column, or the position inside a function
  called from that form. A read error reports a byte offset rather than a
  line, since `FeReadString` has no label to count lines against. An
  expression evaluated interactively (`M-:`, `C-j`) carries no position
  at all — there is no file to name.
- Docstrings are retained by `defun`, `defmacro`, `defvar` and
  `defconst`; the prelude's `(documentation 'NAME)` returns the captured
  string. This is an alist-backed query, not a property-list or
  `describe-function` UI. One divergence rides on that: because the store
  is a single name-keyed alist, `(documentation 'VARIABLE)` answers a
  variable's docstring here, where Emacs reserves `documentation` for
  functions and answers a variable through
  `(documentation-property 'VARIABLE 'variable-documentation)`, which kg
  does not have.

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
