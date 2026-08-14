# kg Lisp API reference

**Document version 5.** Covers the whole Lisp surface Plan 06 (Phases
2-12) shipped: buffers, markers, editing, search, save-excursion /
with-current-buffer, hooks, keymaps, processes, the function/value
namespaces, provide / require / load-path, special variables and shallow
dynamic binding (Phase 11), and — since Phase 12 — `eval`, condition
handlers inside `unwind-protect` cleanups, and Emacs' `file-missing`
condition class. Bump this number when the
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
`LISP_MAX_OBJECTS` = 256 records shared across all three kinds). Only the
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
- **A record is released when its wrapper dies — or sooner, when the
  adapter owns the object.** The ordinary rule is fe's collector: a
  wrapper that nothing reaches any more is swept and its record goes
  back to the pool. That is enough for everything Lisp names, because a
  Lisp name is what keeps a wrapper reachable. It is *not* enough for
  the objects the adapter mints for its own use, because the pool has no
  back-pressure — a full pool raises rather than asking for a collection
  (fe publishes no collect-now entry point), and a loop that allocates
  no arena never provokes one by itself. `save-excursion`'s saved state
  is such an object: its restore releases the record on the spot, so
  the pool bounds how many excursions are *open at once*, never how
  many a run performs. Without that release the 65th `save-excursion`
  between two collections failed — the Phase 11 acceptance review's
  blocker, pinned now by `test_save_excursion_pool_bound` and two PTY
  cases. Since the pool went to 256 records (Phase 12), it no longer
  bounds nesting at all: fe's evaluation frame limit fires first.
  Re-measured at the let-binding-buffer-tag pin, nested `save-excursion`
  runs to **217** and the 218th raises `evaluation frame limit
  exceeded`; nested `with-current-buffer` over `(current-buffer)` runs
  to **155** and the 156th raises the same — both the arena partition's
  verdict (1087 frames here), not the pool's. `test/test_lisp.c`'s
  `test_save_excursion_pool_bound` pins its own probe's figures.
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
  reaches the host.
  **What an error READS as** is Emacs' own sentence, not the condition's
  name: `(error-message-string ERROR)` renders the `(SYMBOL . DATA)`
  object a handler is given, and kg's own diagnostic for an uncaught one
  is that rendering spliced over the name fe's message ends in. So
  `(goto-char "x")` reports `Wrong type argument: integer-or-marker-p,
  "x"`, and `(signal 'error '("custom msg"))` reports `custom msg`. Every
  condition symbol carries Emacs' `error-message` property — `(get
  'wrong-type-argument 'error-message)` is `"Wrong type argument"` — and
  a program may `put` its own over one. Three of those texts contain an
  apostrophe, and Emacs *curls* it when rendering
  (`text-quoting-style`); kg has no such variable and prints the property
  as stored, which is a recorded divergence.  A condition an editor native raises reaches a
  handler written lexically around it, passing through any intervening
  handler that does not name it: until Phase 13.2 those raises went
  through a nested evaluation whose completion transferred straight to
  the host, so no enclosing `condition-case` saw them at all.  Forms evaluated **before** the error remain
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
    about 2 frames per level (measured at the Phase 12 fix cycle:
    `(deep n)`-shaped recursion runs to 544 levels against the
    1095-frame arena of that pin), so in practice it stops such
    recursion a few hundred levels in. kg's default 1 MiB arena measures
    `frame_capacity` 1087; exceeding it raises
    `evaluation frame limit exceeded`. Macro expansion is bounded by the
    same limit, so a macro that expands into itself raises too.
  - **Native re-entry** (`FeEvalOptions.max_native_reentry`, 0 selecting
    `DefaultNativeReentry` = 32) counts nested evaluator runs a native
    starts synchronously, one below another — e.g. a `run-hooks` whose
    hook function calls a command with `command-execute`, whose command
    runs a `run-hooks` of its own. (The example here used to be
    `internal--with-current-buffer` calling `FeCall` on a body that
    itself called `internal--save-excursion`; Phase 11 deleted both
    natives — the two forms are prelude macros over `unwind-protect`
    now and re-enter nothing.) Unlike Lisp nesting, each
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
  **An error raised by a loaded file is catchable around the `load`**:
  since Phase 11 kg evaluates the file through Fe's protected string
  entry, unwinds the loader bookkeeping the frame owns, and re-raises
  into the enclosing run, so

  ```elisp
  (condition-case e (load "broken") (wrong-type-argument 'caught))
  ```

  runs its handler with the original condition, rather than the error
  transferring past every handler to kg's own outermost barrier as it
  did before. That covers read-time failures, run-time failures, the
  depth limit above and a missing file. `load` answers `t` on success,
  as Emacs' does.
  **A file that is not there raises Emacs' `file-missing`**, since
  Phase 12: `(file-missing "Cannot open load file" "No such file or
  directory" PATH)`, with the same condition and the same
  `(OPERATION STRERROR PATH)` data from `require` when nothing in the
  load-path matches — the feature's own name goes in the path slot
  there, because nothing resolved. `file-missing` is a `file-error`,
  which is an `error`, so a handler naming any of the three catches it:

  ```elisp
  (condition-case e (load "/nowhere/x.el") (file-missing (nth 3 e)))
  ```

  answers the path. A permission failure raises the parent class,
  `file-error`, where Emacs raises its third leaf `permission-denied`;
  that one is recorded rather than implemented, because it is
  measurable only unprivileged. Everything else the loader raises —
  the depth limit, out of memory, cyclic `require`, a file that does
  not `provide` — is a kg-policy error with no Emacs counterpart and
  stays a plain `error`. A read failure on an open file stays a plain
  `error` too, but that one *has* a counterpart — Emacs answers
  `(file-error "Cannot open load file" "Is a directory" PATH)` for
  `(load DIRECTORY)` — and is out of scope per 12A Decision 1.
  **A `throw` out of a loaded file reaches a `catch` around the
  `(load ...)`**, as it does in Emacs, since Phase 12's fix cycle:
  `load` is a prelude read-eval loop — each form read by an
  incremental reader that latches the form's own `path:LINE`, then
  `eval`ed in the caller's own run inside one fe input unit — so
  errors, throws and quits cross the `load` as if the loaded forms
  were written in place, and `unwind-protect` cleanups in the loading
  frame run as they cross. The flipped `load-throw-reachability` row
  records the history; `load-dynamic-extent` pins the consequences
  (incremental error timing, nested loads, cleanups) against the
  oracle.
  **`require` cycle detection** is separate again: it tracks feature
  *identity*, not nesting depth, in its own `LISP_MAX_REQUIRE_STACK` = 8
  entry stack, so `(require 'a)` from inside `(require 'a)`'s own load is
  an immediate "cyclic require" error naming the feature, not a
  depth-limit error and not a stack overflow — even if the actual nesting
  involved is well under 8.
- **A native's own bounded tables** refuse rather than silently drop or
  corrupt when full: `LISP_MAX_COMMANDS` = 32 Lisp-defined commands,
  `LISP_MAX_OBJECTS` = 256 pool records, `LISP_MAX_HOOKS` = 16 distinct
  hooks, `KG_PROCESS_TABLE_MAX` = 8 processes, `LISP_MAX_FEATURES` = 32
  provided features, `LISP_MAX_LOAD_PATH` = 8 load-path directories.
  Every one of these is a compile-time bound (`src/lisp_internal.h`,
  `src/lisp_obj.h`, `src/lisp_hooks.c`, `src/process_table.h`), not a
  heap allocation that grows; hitting one is an ordinary Lisp error, not
  a crash.
- **The object arena is fixed and exhaustible, and exhaustion is an
  ordinary catchable condition.** kg opens Fe with a 1 MiB arena that
  never grows (`KG_LISP_ARENA_SIZE`, `src/lisp_core.c`), measured at the
  current pin as 56147 object slots and a 1087-frame evaluator stack.
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

  That it allocates nothing is also its one caveat: slots return only
  when a collection runs, and Fe collects only when an allocation finds
  the free list empty. Asked immediately after an exhaustion it therefore
  reports `0 free` even when the exhausting data is already unreachable —
  measured, and the reason
  `test/pty/lisp-exhaustion-mid-command-recovers.yaml` evaluates `(+ 1 2)`
  between the failure and the question. Evaluate anything, then ask
  again, to see what came back; `0 free` on the *second* ask is the
  reading that means the arena is pinned.

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
| `(switch-to-buffer BUFFER-OR-NAME)` | Show it in the selected window and make it current, creating it when a string names no buffer. Answers the buffer object |
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
| `(forward-char &optional N)` / `(backward-char &optional N)` | Move point `N` characters; a line break is one character. Answers `nil`, or signals `end-of-buffer`/`beginning-of-buffer` for a count that runs off the end |
| `(forward-line &optional N)` | `N` lines forward, to the beginning of a line. Answers the *shortfall*: how many of `N` were not travelled |
| `(beginning-of-line &optional N)` / `(end-of-line &optional N)` | Start/end of the line `N - 1` lines on from this one, clamped. `N` defaults to 1, so 2 is the next line and 0 the previous |
| `(beginning-of-buffer)` / `(end-of-buffer)` | `(goto-char (point-min))` / `(goto-char (point-max))`, and nothing else — see below |
| `(move-beginning-of-line &optional N)` / `(move-end-of-line &optional N)` | Move point to the start/end of the line; `N` not nil or 1 first moves forward `N - 1` lines, clamped |
| `(skip-chars-forward SPEC &optional LIM)` / `(skip-chars-backward SPEC &optional LIM)` | Move point over characters in `SPEC`, no further than `LIM`. Answers the signed distance travelled |
| `(bounds-of-thing-at-point THING)` | Cons `(START . END)` for `'word` or `'line`, or `nil`; any other symbol raises |
| `(buffer-file-name &optional BUF)` | The file `BUF` visits, or `nil` for a buffer that visits none |
| `(buffer-modified-p &optional BUF)` | Whether `BUF` has unsaved changes |
| `(set-buffer-modified-p FLAG)` | Set that flag on the current buffer. Answers `nil`, which is what Emacs answers too |

`forward-char`, `backward-char` and `delete-char` **signal** at the ends
of the buffer, as Emacs does: point moves as far as it can and a count
that could not be spent raises `end-of-buffer` or `beginning-of-buffer`
with no data, and `delete-char` deletes nothing at all in that case.
Landing exactly *on* an end is not a signal. The condition names the end
that was reached rather than the function that was called, so a negative
count to `forward-char` can raise `beginning-of-buffer`. Both names are
ordinary children of `error`, so a generic handler catches either.

The rest of the motion family **clamps**: `forward-word`,
`backward-word`, `forward-line`, `beginning-of-line`, `end-of-line`,
`move-beginning-of-line` and `move-end-of-line` stop at the end and
answer, which is what Emacs' do too. The three that signal did not until
Phase 20, when fe's condition table gained the two names.

`beginning-of-buffer` and `end-of-buffer` do not push the mark, where
Emacs' — which are commands — do. kg's `set-mark` also lights the region
up, which a Lisp call has no business doing, and there is no unhighlighted
`push-mark` to use instead. The Emacs manual's own advice for Lisp code
is `(goto-char (point-min))`, which is exactly what these are.

`skip-chars-forward`'s `SPEC` is Emacs' character-set syntax: a leading
`^` negates it (so `""` skips nothing and `"^"` skips everything), `-`
between two characters is a range, and a backslash quotes the next
character. Named classes (`[:alpha:]` and its family) are **not**
understood and read as the ordinary characters they are made of.
Membership is tested over codepoints, so a `SPEC` naming a multi-byte
character skips whole glyphs.

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
| `(delete-char &optional N)` | Delete `N` characters after point, or `-N` before it. Clamps where Emacs signals, so a count past the end deletes what is there |
| `(erase-buffer)` | The whole buffer becomes empty |
| `(replace-region START END TEXT)` | The region becomes `TEXT`, as one edit — never delete-then-insert |
| `(search-forward STRING &optional BOUND)` | Literal search to `BOUND` (default `point-max`); moves point past the match, or `nil` |
| `(search-backward STRING &optional BOUND)` | Literal search to `BOUND` (default `point-min`); moves point to the match start, or `nil` |
| `(re-search-forward PATTERN &optional BOUND)` | Regexp search forward; error on a bad or too-complex pattern |
| `(re-search-backward PATTERN &optional BOUND)` | Regexp search backward; see `src/lisp_search.c` for the exact (non-Emacs) rule |
| `(match-beginning N)` / `(match-end N)` | Group `N`'s bounds from the last search, or `nil` |
| `(looking-at REGEXP)` | Anchored match at point; sets the match data, moves point nowhere |
| `(make-marker)` | A marker that points nowhere until something sets it |
| `(point-marker)` | A marker at point in the current buffer |
| `(copy-marker &optional POSITION TYPE)` | A marker at `POSITION` (a position or another marker); with no `POSITION` it points nowhere. Non-nil `TYPE` makes it advance ahead of text inserted at it |
| `(set-marker MARKER POS &optional BUF)` | Move `MARKER`; `POS` nil detaches it |
| `(marker-position MARKER)` / `(marker-buffer MARKER)` | Where `MARKER` points, or `nil` |

None of the search natives wrap around the buffer (unlike `C-s`/`C-r`),
none fold case, and a match cannot span two lines — the same limit
incremental search has, since both share `src/regex.h`'s
one-row-at-a-time engine. Match data outlives the search that set it,
the same way point outlives a `goto-char`: it is a piece of per-session
state (`struct kg_lisp_match_data`), not something scoped to one
top-level form.

## save-excursion, with-current-buffer, with-temp-buffer and unwind-protect

| Form | Result |
| ---- | ------ |
| `(save-excursion BODY...)` | Restores point and the current buffer on every exit, including an error, a `throw` or `C-g` |
| `(with-current-buffer BUF BODY...)` | Evaluates `BODY` with `BUF` current, then restores; never selects a window |
| `(with-temp-buffer BODY...)` | Evaluates `BODY` in a fresh, empty buffer that visits no file, then restores the caller's buffer and kills the temporary one |
| `(unwind-protect BODY CLEANUP...)` | Evaluates `BODY`, then `CLEANUP...` as an implicit `progn` on every exit — normal return, error, `throw`, `C-g`, or step-budget exhaustion; the value is `BODY`'s, the cleanups' are discarded |

`with-temp-buffer` is the other three composed, and three details of it
are kg's. Its buffer's name starts `" *temp*"` and carries a unique
suffix, because kg has no `generate-new-buffer` and a fixed name would
make a nested use reuse the buffer it is already inside. Its cleanup
clears the modified flag before killing, because kg's `kill-buffer`
refuses a modified buffer rather than asking. And the kill itself is
wrapped in `ignore-errors`, because a cleanup that raises *replaces* the
completion it is unwinding, and losing what the body computed is worse
than leaking a buffer. kg refuses a kill when the editor's lifecycle
event queue is full; that queue drains once per keystroke and each
temporary buffer costs three of its 64 slots, so a single command that
opens more than about twenty temp buffers starts leaking them.

All three are the same mechanism: Fe's cleanup registry
(`FeProtectWithCleanup`), which the first two reach from C and
`unwind-protect` (a core Fe special form, not a prelude definition)
exposes to Lisp directly. So "every exit" is not an approximation: a
raised error or an interrupt inside `BODY` still restores what was
saved, or runs `CLEANUP`. Nesting is fine; each restores exactly what it
saved, in the reverse order it was saved.

Since Phase 14 the first two bind their saved state to a `gensym`, so
nothing a body can write reaches it. Before that the binding was the
ordinary symbol `internal--excursion`, and a body that assigned that name
made the restoring cleanup raise — and a raising cleanup *replaces* the
completion it is unwinding, so the body's own error was lost.

**A cleanup may handle its own errors**, since Phase 12: a
`condition-case` or `ignore-errors` written *inside* `CLEANUP` catches
what `CLEANUP` raises, whether or not something is being unwound past
it, and the completion in flight — an error, a `throw`, a `C-g` — is
undisturbed.

```elisp
(unwind-protect 'body (ignore-errors (car 6)))                  ; body
(condition-case e (unwind-protect (/ 1 0) (ignore-errors (car 6)))
  (error e))                                                    ; (arith-error)
```

Before Phase 12 both of those escaped to the host: every raise inside a
running cleanup behaved as unhandled, because the replay to the cleanup
entry was tested before the handler search ever ran. What did **not**
change is the rule for a cleanup error nothing in the cleanup handles —
it still replaces the completion being unwound and is caught by a
handler written *around* the `unwind-protect`, which is Emacs' rule too
— and a cleanup that `throw`s still abandons the error it was unwinding.
The cleanups still pending after a raising cleanup run anyway. Two
corner shapes of *where* the replacement lands diverge from Emacs, both
pre-existing and found post-close: a `condition-case` in the already-
abandoned body can receive it before the enclosing one does, and a
cleanup's `throw` can reach a `catch` the exit had already left. The
`cleanup-raise-residuals` manifest row pins both probes with the
oracle's answers.

`save-excursion` and `with-current-buffer` are *transparent* to the
evaluation that encloses them, `throw` included. An error raised inside
either body reaches a `condition-case` written around the form carrying
its original condition symbol, and a `throw` reaches a `catch` written
around the form carrying the value it threw; the restore has already
run by the time either does. Since Phase 11 both forms are prelude
macros over Lisp `unwind-protect` rather than natives that run the body
as a thunk, which is what buys the `throw` half: Fe walls a throw search
at a native re-entry boundary by design, so while such a frame stood
between them a `throw` out of the body became `(no-catch TAG VALUE)`
instead.

**Every other native re-entry is still that wall.** A `throw` out of a
hook function, a process filter or sentinel, or a nested
`(command-execute …)` finds no catch outside the callback and becomes
`(no-catch TAG VALUE)` — an ordinary error an enclosing `condition-case`
handles, and one whose unwinding still runs cleanups, but not the value
the `catch` was waiting for. Those are callbacks kg invokes from its own
C, so there is no prelude expansion that removes the frame.
`test/lisp-compat/features.json` has a row for each.

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
`before-save-hook` and `after-save-hook` (called `(FN)`),
`kill-buffer-hook` (called `(FN)`) and `post-command-hook` (called
`(FN)`).

Every hook runs **in the buffer it is about**: the buffer the change
happened in, the one being opened or saved, the one being killed. A hook
whose buffer is not on screen therefore does not edit whatever is.

`kill-buffer-hook` runs while the buffer is still live and after every
refusal has been passed, which is Emacs' ordering: a hook body can still
read the text that is about to be lost, and a hook that ran never watches
the kill be refused afterwards. kg refuses a modified buffer's kill
outright rather than prompting, and that refusal comes *before* the hook.
Two bounds are kg's: a hook body may not kill the buffer it is being run
for (refused), and a kill it makes of some *other* buffer runs no hook of
its own — so `with-temp-buffer` inside one works.

`post-command-hook` runs once per keystroke the editor has finished
processing, not once per command: kg has no `self-insert-command`, so
"after each command" would leave ordinary typing out. A keystroke that
runs no command (a prefix key) fires it too. An empty
`post-command-hook` costs a keystroke nothing measurable — the seam does
not reach the evaluator at all until something is added to it.

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

### Rebinding the debugger's keys

The debugger's three maps — `dap-breakpoint` (`F9`, `C-F9`, live in any
buffer visiting a file), `dap` (everything else, live only while a session
exists) and `dap-info` (the six `*dap-*` panes) — are created in C before
`init.el` runs, so `define-key` finds them rather than creating a new map
of its own in the wrong layer:

```elisp
;; VS Code habits.
(define-key 'dap-mode-map "<f5>" 'dap-continue)
(define-key 'dap-mode-map "<f11>" 'dap-step-in)
(define-key 'dap-mode-map "S-<f11>" 'dap-step-out)
;; Or somewhere entirely your own.
(define-key 'dap-breakpoint-mode-map "C-c b" 'dap-breakpoint-toggle)
(define-key 'dap-info-mode-map "x" 'dap-info-delete-breakpoint)
```

The `-mode-map` suffix is `keymap-find`'s ordinary aliasing, so
`'dap-mode-map` and `'dap` name the same map. `global-set-key` cannot bind
an F-key by design; that these are real native maps is exactly what makes
`define-key` able to. A binding is a command *name*, so it may be a Lisp
command you defined yourself — a `defun` marked as a command is bindable
here like any built-in one.

Which map to put a binding in is the question worth a moment: a key in
`dap` is dead outside a session and cannot shadow anything you use while
editing, while a key in `dap-breakpoint` is live in every file buffer.

## Variables the editor reads

Most of what kg does is reached by calling a command, not by setting a
variable. The exceptions are listed here, and the list is short on
purpose. Editor modules use the narrow accessors in `src/lisp.h` rather
than evaluating Lisp themselves.

| Variable | Default | Read | Effect |
| ---- | ---- | ---- | ---- |
| `inhibit-startup-screen` | `nil` | once, after `init.el` has run | Non-nil draws no startup screen — the centred logo an empty buffer otherwise shows |
| `inhibit-startup-message` | `nil` | same | Emacs' other spelling of the above; either name suppresses the screen |
| `tab-width` | `8` | after Lisp evaluation and before repaint | Display columns between tab stops; an integer from 1 through 1000 |

Both are `defvar`'d by the prelude, so they are `boundp` and
`special-variable-p` before any init file runs, and `-Q` (no init file)
leaves both `nil`. They are read once rather than per frame: setting
either from `M-:` after startup changes nothing, which is Emacs'
behaviour too.

The alias is where kg diverges. In Emacs `inhibit-startup-message` is a
`defvaralias` of `inhibit-startup-screen` — one variable under two names,
so setting either reads back through both. kg has no variable aliases;
these are two ordinary variables, and what makes both spellings work is
that the startup path asks for both. Setting one leaves the other `nil`.
Emacs' third alias, `inhibit-splash-screen`, is not provided.

`tab-width` participates in the buffer-local machinery above. A plain
`setq` changes the default and therefore every buffer without a local
binding; `(setq-local tab-width N)` changes only the current buffer. kg
does not have Emacs' automatically-buffer-local variables, so unlike
Emacs a plain `setq` does not create a local binding. Changing the value
rebuilds rendered rows, syntax faces and visual-line geometry without
marking the file modified.

The `Press Ctrl-h for help` greeting in the status area is a separate
thing and neither variable suppresses it: Emacs gives that its own
`inhibit-startup-echo-area-message`, whose value must be your login name
spelled literally in the init file, and kg does not implement it.

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
| `(require FEATURE &optional FILENAME)` | No-op (returns `FEATURE`) if already provided; else resolves `FILENAME` (or `FEATURE`'s own name) through `load-path` — written with or without the `.el` suffix, the same rule `load` applies to a bare name — and evaluates it nested, the way `load` nests; a `FILENAME` containing `/` is a literal path, neither suffixed nor searched; errors if the feature is still not provided afterward |
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
| `(make-string N CHAR)` | `N` copies of one character. Emacs' third `MULTIBYTE` argument is not accepted — every kg string is UTF-8 |
| `(string-to-number S &optional BASE)` | `0` for anything that does not begin with a number, as in Emacs — including `"0x10"` and `"inf"`, which are not numbers to it either. `"1."` is the integer `1` and `"1.5"` is a float, the same split the reader makes. `BASE` is 2–16 |
| `(upcase X)` / `(downcase X)` / `(capitalize X)` | `X` is a string or a character, and the result has `X`'s type. **ASCII only** — see the differences section |

`length` is the Emacs spelling and works on both lists and strings;
`string-length` is a kg-only name for the string half of it, kept
because it is what the string natives are written against.

## Regular expressions from Lisp

The engine is kg's own (`src/regex.h`), the same one behind `C-s` and
`re-search-forward`; these two are the seam onto it for *strings*.
Everything about the pattern language, including which patterns are
rejected outright, is that engine's — Emacs' syntax, with `\\(`…`\\)`
groups and up to nine of them.

| Form | Result |
| ---- | ------ |
| `(string-match REGEXP STRING &optional START)` | 0-based **character** index of the match, or `nil`. Sets the match data. `START` counts back from the end when negative and is `args-out-of-range` past it |
| `(match-string N &optional STRING)` | The `N`th group's text, `nil` when that group did not participate. With `STRING` it reads the string the last `string-match` ran over; without it, the buffer |
| `(match-beginning N)` / `(match-end N)` | 0-based character indices after a `string-match`, 1-based buffer positions after a buffer search — the units of whichever subject was matched |
| `(replace-regexp-in-string REGEXP REP STRING &optional FIXEDCASE LITERAL)` | `REP` is a replacement string understanding `\\&`, `\\N` and `\\\\`, or a function of the matched text. `LITERAL` suppresses the escapes; `FIXEDCASE` is accepted and ignored (kg never case-adjusts, because it never case-folds) |
| `(regexp-quote S)` | `S` as a regexp matching itself |

There is **one** match-data register, shared by string and buffer
matches exactly as in Emacs, so a search of either kind replaces what
the other left. Three properties are inherited from the engine and are
recorded divergences rather than surprises: `^` and `$` anchor the whole
subject rather than each line of it, matching is always
case-**sensitive** (there is no `case-fold-search`), and the subject is
NUL-terminated, so a match stops at an embedded NUL.

## The string and list library

Ordinary Lisp over the natives above, in `lisp/prelude.el`. What is here
was chosen by measurement: `utils/forecast_audit.py` ranks the names a
corpus of *target* Lisp reaches for against the names kg has, and
`utils/forecast/AUDIT.md` is the checked-in result.

| Group | Forms |
| ---- | ---- |
| Splitting and joining | `split-string` (Emacs' `OMIT-NULLS` asymmetry included), `string-join` |
| Trimming and testing | `string-trim`, `string-trim-left`, `string-trim-right`, `string-prefix-p`, `string-suffix-p`, `string-empty-p` |
| Alists and plists | `alist-get`, `assq-delete-all`, `plist-get`, `plist-put` |
| List utilities | `elt`, `butlast`, `copy-sequence`, `number-sequence`, `nconc`, `mapcan`, `sort`, `cdar`, `caddr`, `cdddr`, `cadddr` |
| The `seq-` shim | `seq-map`, `seq-filter`, `seq-remove`, `seq-find`, `seq-some`, `seq-take` — **lists only** |
| Arithmetic | `abs`, `mod`, `%`, `ash` |

`string<` and `string>` are **not** in this table any more: they were
prelude Lisp until Phase 20 and are fe primitives now. Each takes a
string or a symbol on either side (a symbol compares by its name, and
`nil` compares as `"nil"`), and each is strictly binary. The move is
measurable rather than tidy-minded: the prelude spelling compared two
names a *character* at a time, and `apropos` -- kg's one heavy sorter --
could report 54 rows before an evaluation's step budget ran out, where
it now fits 158.

Three notes a caller will want:

* `sort` takes `(sort SEQ PREDICATE)` and is **destructive in Emacs 31's
  own way**: it moves values between the cells the list already has, so
  the result is `eq` to the input and a cell held separately sees a
  different value. Emacs 30's keyword convention (`(sort SEQ :lessp …)`)
  is not accepted.
* `nconc`, `plist-put` and `assq-delete-all` are destructive, as Emacs'
  are; `mapcan` is built on `nconc` and is therefore destructive in the
  same places.
* Where a form takes fewer optional arguments than Emacs' — `string-trim`'s
  `REGEXP`, `alist-get`'s `TESTFN`, `plist-get`'s `PREDICATE`,
  `split-string`'s `TRIM` — the extra argument is **refused by name**, not
  accepted and ignored.

## Symbols, property lists and the reader's escapes

Since Phase 14 fe carries the symbol surface itself, so these are core
forms rather than kg natives:

| Form | Result |
| ---- | ------ |
| `(intern NAME)` | The interned symbol named by the string `NAME`, creating it if there is none. `(intern "nil")` is `nil` |
| `(intern-soft NAME-OR-SYMBOL)` | The interned symbol, or **`nil` on a miss — and nothing is interned** |
| `(symbol-name SYMBOL)` | Its name, as a string. `(symbol-name nil)` is `"nil"` |
| `(make-symbol NAME)` | A fresh **uninterned** symbol: `eq` to nothing but itself, and in no obarray |
| `(gensym &optional PREFIX)` | `make-symbol` over a private counter — the way a macro gets a temporary nothing can capture |
| `(put SYMBOL PROPERTY VALUE)` | Stores `VALUE` and returns it. A new property appends at the tail; an existing one is overwritten in place |
| `(get SYMBOL PROPERTY)` | The stored value, or `nil` |
| `(symbol-plist SYMBOL)` | The whole `(PROPERTY VALUE ...)` list |
| `(error-message-string ERROR)` | Emacs' sentence for a condition object: `(error-message-string '(wrong-type-argument listp 6))` is `"Wrong type argument: listp, 6"` |

`intern-soft` is a probe and never a constructor. That matters more than
it looks: real code loops on it (`(while (setq x (intern-soft (format
...))) ...)`), and an implementation that interned on a miss would turn
such a loop into an arena exhaustion instead of a termination.

kg has one obarray and no way to name a second, so `intern` and
`intern-soft` do not take Emacs' optional `OBARRAY`; a second argument is
`wrong-number-of-arguments` rather than an argument accepted and ignored.
Symbol names are bounded at 63 bytes, which is the reader's own token
bound. `(put nil ...)` raises `(wrong-type-argument symbolp nil)` — kg's
`nil` is a distinct object with no storage — while `(get nil P)` answers
`nil` as Emacs does before anything was put.

An uninterned symbol prints as its **bare name**, which is what Emacs
does with `print-gensym` nil, its default; kg has no `print-gensym` and
no `#:` spelling, so printing does not distinguish two symbols of one
name and `eq` is the only thing that does. `(keywordp (make-symbol
":a"))` is `nil`: a keyword is an interned symbol whose name starts with
a colon.

**Reader escapes.** A backslash takes the next byte into a symbol's name
literally, so `a\ b` is the one symbol whose name is `a b`, `\1` is the
symbol named `1` rather than the integer, and one escape anywhere makes
the whole token a symbol. An escaped dot is an ordinary list element
where a bare one is the dotted-tail marker: `(a \. b)` has three
elements and `(a . b)` is a pair. `##` is the symbol with the empty name.
A backslash with nothing after it is a read error, and the strict-reader
policy is otherwise unchanged — vectors, `#:` and the other `#`
dispatches are still named read errors rather than misreadings.

The printer is the inverse, so a symbol always reads back as itself:
bytes at or below the space, and `"#'(),;[]\` and the backquote, escape
wherever they occur, and the first byte escapes when the whole name
reads as a number, when the name starts with `?`, or when it starts with
`.` and the next byte is not an ASCII letter. So `(intern "a b")` prints
`a\ b`, `(intern "1")` prints `\1`, `(intern ".")` prints `\.` — and
`(intern ".emacs")` prints `.emacs`.

kg evaluates a prelude (`lisp/prelude.el`, embedded into the binary as
`src/lisp_prelude_generated.inc`), written in Fe, at startup
before any init file runs — this is what makes `defun`, `let`, `cond`,
`dolist` and the rest available at all, since upstream Fe has only
`lambda`, one-binding `let`, `if` and `while`:

| Group | Forms |
| ---- | ------ |
| Definitions | `defun` `defmacro` `defvar` `defconst` `defcustom` `custom-set-variables` `declare` `interactive` `lambda` |
| Binding | `let` `let*` `setq` `progn` `special-variable-p` — `let`/`let*` bind a *marked* name dynamically and every other name lexically; `special-variable-p` answers whether `defvar`/`defconst` marked it |
| Control | `cond` `when` `unless` `prog1` `dolist` `dotimes` |
| Non-local exits | `catch` `throw` `condition-case` `signal` `error` `unwind-protect` `ignore-errors` — all core Fe forms except `ignore-errors`, which is the prelude's one-line macro over `condition-case` |
| Lists | `length` `nth` `nthcdr` `last` `reverse` `append` `mapcar` `mapc` `mapconcat` `assoc` `assq` `member` `memq` `push` `pop` `nreverse` `delq` `delete` `add-to-list` `caar` `cadr` `cddr` `1+` `1-` |
| Predicates | `null` `eq` `eql` `equal` `zerop` `integerp` `floatp` `listp` `type-of` `stringp` `symbolp` `numberp` `consp` `functionp` `commandp` `keywordp` `boundp` `special-variable-p` |
| Functions | `funcall` `apply` `eval` `function` (written `#'f`) `fboundp` `symbol-function` `symbol-value` `fset` `defalias` `fmakunbound` |
| Symbols | `intern` `intern-soft` `symbol-name` `make-symbol` `gensym` `put` `get` `symbol-plist` — core Fe primitives, described above |
| Numbers | `+` `-` `*` `/` and the comparators `=` `<` `<=` `>` `>=` `/=` |
| Quoting | `` ` `` / `,` / `,@` (quasiquote); `#'f` is `(function f)` |
| Editor | `string-empty-p` `thing-at-point` |
| Small library | `identity` `prog2` `max` `min` `documentation` `number-to-string` `string-to-list` `kbd` |
| Buffer-local | `setq-local` `setq-default` `set-default` `default-value` `make-local-variable` `kill-local-variable` `local-variable-p` `buffer-local-value` — see "Buffer-local variables" below |

The table is the whole startup surface, not only what the prelude adds:
the forms in `Functions` and `Numbers`, like `setq` in `Binding`, are
core Fe special forms and primitives rather than prelude definitions.

`(eval FORM)` — new in Phase 12 — evaluates `FORM` in **the caller's own
run**, not in a nested one, so a condition, a `throw` or a `C-g` out of
it reaches the handler, catch or recovery the caller is standing in:
`(catch 'tg (eval '(throw 'tg 99)))` is `99` and
`(condition-case e (eval '(car 6)) (error e))` binds the ordinary
`(wrong-type-argument listp 6)`. Its environment is the **global** one,
which is Emacs' answer and not an approximation of it: Emacs' optional
LEXICAL argument *selects* an environment rather than inheriting the
caller's, and `(let ((qq 1)) (eval 'qq))` is `(void-variable qq)` under
the pinned Emacs 31.0.90 exactly as it is here. A non-nil LEXICAL is
refused by name, the way `macroexpand`'s ENVIRONMENT is.

`load-path` remains a bounded C search-path
array; use kg's `add-to-load-path` native rather than modifying it with
`add-to-list`.

## Buffer-local variables

One name, one value per buffer, and a default for the buffers that have
no value of their own.

| Form | Result |
| ---- | ------ |
| `(setq-local SYM VAL ...)` | Give the current buffer a binding of its own and set it; answers the last VAL |
| `(setq-default SYM VAL ...)` | Set the value buffers without a binding of their own see |
| `(default-value 'SYM)` | Read that value, past whatever binding is in force |
| `(set-default 'SYM VAL)` | `setq-default`'s function form |
| `(make-local-variable 'SYM)` | Give this buffer a binding seeded from the default |
| `(kill-local-variable 'SYM)` | Drop this buffer's binding; the default becomes visible again |
| `(local-variable-p 'SYM &optional BUFFER)` | Whether BUFFER (default: the current one) has a binding |
| `(buffer-local-value 'SYM BUFFER)` | Read BUFFER's binding without selecting it |

An ordinary reference reads the current buffer's binding when it has one
and the default otherwise, and a plain `setq` writes whichever of the two
is in force. A binding dies with its buffer. None of this marks a name
special: `(setq-local x 1)` on a name no `defvar` declared is legal, and
`(special-variable-p 'x)` is still nil afterwards — a buffer-local
binding and a dynamic-binding mark are different things.

`let` over a name that is buffer-local in the current buffer binds
**that buffer's** binding, and the default is untouched: `default-value`
inside the form still reports it, and another buffer with a binding of
its own does not see the `let` at all. `let` over a name the current
buffer has no binding for binds the **default**, which is what
`default-value` reports inside the form. Both are Emacs' answers,
because kg's storage is Emacs' own: the symbol has one value cell
holding whichever binding is current, and the displaced one is kept
beside it until the buffer comes round again.

A `let` remembers **which storage it displaced**, as Emacs' specpdl does,
so the two shapes that get that wrong in a naive implementation get it
right here. A bare `set-buffer` inside such a form restores the buffer
the `let` was entered in rather than whichever one is current at exit;
and `setq-local` or `make-local-variable` *inside* a `let` over the same
name leaves the new local binding alone and restores the default into
the default. (Emacs warns about the second shape — `Making X
buffer-local while locally let-bound!` — rather than recommending it;
kg does not print the warning, but the values agree.) When the displaced
storage is gone by the time the form exits — its buffer killed, or its
binding dropped with `kill-local-variable` — the saved value is dropped
rather than written anywhere, which is Emacs' answer too.

One thing that follows from the representation is still a **divergence**,
recorded and tested:

* kg has no automatically-buffer-local variables:
  `make-variable-buffer-local` and `defvar-local` do not exist, and a
  plain `setq` never creates a binding. In Emacs `fill-column` is one of
  these, so where Emacs lets `setq` do it, kg needs `setq-local`.

`lisp/auto-fill.el` is the shipped consumer: `fill-column` is its
default, `(setq-local fill-column N)` gives one buffer a margin of its
own, and auto-fill reads the name inside the buffer the change happened
in.

`defun` recognises only an `(interactive ...)` form immediately after its
optional docstring. That declaration is removed from the body and registers
the closure as a command; a later form remains ordinary code. A docstring
followed by an empty declaration body gets an implicit `nil` body — but a
docstring that *is* the whole body is the body, and the function returns it,
as in Emacs. `(commandp OBJECT)` answers whether something is a command: a
name is asked of the command registry (a built-in row, or a `defun` whose
body carried an `(interactive ...)` declaration), and a function object is
asked by identity, so `(commandp (symbol-function 'my-command))` is `t`.
`(interactive-form COMMAND)` returns the declaration a command was defined
with — `(interactive "p")`, `(interactive (list "x"))` with the descriptor
form *unevaluated*, or `(interactive nil)` when there is no specification,
which is Emacs' own normalization — and `nil` for anything that is not a
command. Two recorded divergences are left. An anonymous
`(lambda () (interactive) 1)` is a command in Emacs and is not one here:
kg's `interactive` is an inert macro and a lambda carries no metadata, so a
function is a command exactly when it was registered as one. And a
**built-in** command answers `(interactive nil)` where Emacs answers that
primitive's own spec string — true rather than a placeholder, since a kg
built-in declares no interactive arguments and the handlers that need input
read the terminal themselves. Emacs' optional FOR-CALL-INTERACTIVELY
argument is not accepted.

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
the effective integer, not to the raw form. `P` is `eq` to that temporary value. The binding is made and unmade at
the command boundary by kg's C, not by `let` over a `defvar`'d name, and
`current-prefix-arg` is not marked special — so a `let` over that name
is an ordinary lexical binding: the forms lexically inside the `let`
read it, and a function *called* from inside still reads the
command-boundary value.
`(let ((current-prefix-arg 7)) (list current-prefix-arg (f)))` measures
`(7 nil)`, where under Emacs, whose `current-prefix-arg` is special, it
is `(7 7)`. (`defvar` marks, and marked names bind dynamically, since
Phase 11; this particular name is not one of them.)

`command-execute` uses the same metadata and evaluator, including nested
calls, and returns the command's value; an inner call inherits the active
command's prefix when there is one and uses none otherwise. A nested call
builds and runs inside the evaluator already running, so its error or quit
reaches a `condition-case` lexically around the `(command-execute ...)`.

**Which built-in commands it may reach** is `cmdtable`'s
`CMD_LISP_CALLABLE` flag and nothing else; Phase 17 audited all 153 rows
and 124 carry it. The 29 that do not are refused with *command is not
allowed*, for one of five reasons, each stated beside the table in
`src/cmd.c`: the command re-enters the evaluator (the `eval-*` family);
it is a modal loop that owns the keyboard until it ends (incremental
search, `query-replace`, keyboard macros); its argument *is* a keystroke
(`quoted-insert`, `zap-to-char`, `self-insert-command`, the four register
commands, `describe-key`); it ends or suspends the editor; or it is the
interactive command dispatcher itself (`execute-extended-command`), which
`command-execute` already is without the picker.

*Prompting* is not one of those reasons — a built-in command that opens a
minibuffer prompt is reachable, and prompts, exactly as a Lisp command
calling `read-string` does. What it needs is a descriptor to prompt on:
reached from a keystroke or from `M-x` there is one, and from an init
file, a hook, a process filter or inside `eval-expression` there is not,
where such a command raises *interactive prompt is not available here* —
the same words, and the same question, as the read functions below.

A built-in command knows the *window's* cursor, and the Lisp around it
knows the runtime point. `command-execute` hands one to the other in both
directions, so `(goto-char N)`, a `command-execute`, and an `insert`
read as one sequence of motions. Both halves are skipped when the exec
buffer is not the one on screen, because a built-in command reached from
Lisp acts on the window's buffer rather than on the buffer `set-buffer`
selected — a divergence, and not one this closes. `define-command` is the
kg-owned extension `(define-command NAME FUNCTION &optional SPEC DOC)`; its
spec is nil, a string, or a zero-argument function, and documentation is nil or
a string. Interactive definitions replace their function, spec and document
atomically; redefining without a declaration removes command status.
Prompting is refused outside a key/M-x command context or while another
prompt is active — the same rule the public read functions below inherit.
Prompt interpolation and deferred codes remain explicit divergences.

## Asking the user: the read functions

A command's argument list is not the only place it may ask a question.
These seven forms read the minibuffer from anywhere in a command's body,
through the same seam and with the same rules the interactive codes have:
prompting is refused outside a key/M-x command context and while another
prompt is already up, and `C-g` is Emacs' `quit`, catchable by a
`condition-case` naming `quit` and by nothing else.

| Form | Answer |
| ---- | ------ |
| `(read-string PROMPT &optional INITIAL-INPUT HISTORY DEFAULT-VALUE)` | The typed text. `INITIAL-INPUT` is the prompt's starting text; an empty answer is `DEFAULT-VALUE`, or `""` without one |
| `(read-number PROMPT &optional DEFAULT HISTORY)` | A number. With a `DEFAULT` the prompt reads `PROMPT(default N) ` and an empty answer takes it; without one, an empty answer re-prompts |
| `(read-file-name PROMPT &optional DIR DEFAULT-FILENAME MUSTMATCH INITIAL PREDICATE)` | A path, through kg's file picker. `DIR` and `INITIAL` together are the prompt's initial text; `MUSTMATCH` requires the path to exist |
| `(read-buffer PROMPT &optional DEFAULT REQUIRE-MATCH PREDICATE)` | A buffer name, through the `C-x b` picker. `REQUIRE-MATCH` accepts only an existing buffer |
| `(y-or-n-p PROMPT)` | `t` for `y` or `Y`, `nil` for any other key |
| `(yes-or-no-p PROMPT)` | `t` or `nil` for a typed `yes` / `no`, re-prompting until one of them arrives |
| `(completing-read PROMPT COLLECTION &optional PREDICATE REQUIRE-MATCH INITIAL-INPUT HISTORY DEFAULT)` | One of `COLLECTION`'s strings, or — without `REQUIRE-MATCH` — whatever was typed |

```lisp
(defun rename-thing ()
  (interactive)
  (let ((new (read-string "New name: " nil nil "untitled")))
    (when (y-or-n-p (concat "Rename to " new "? "))
      (insert new))))
```

`completing-read` shows kg's own pick-list, the `{apple | banana |
cherry}` `M-x`, `C-x b` and `C-x C-f` already show: typing filters,
`Left`/`Right` cycle the highlight, `Tab` completes the typed text to the
highlighted candidate, and `Enter` takes it. A query that is *exactly* one
of the candidates wins over the highlight, as Emacs' `completing-read`
does — so a short name is never shadowed by a longer one that sorts first
— unless the highlight was moved deliberately, which outranks both.
`Enter` on an untouched empty prompt answers with `DEFAULT` — `M-x`'s own
rule for its `(default ...)`, and the only shape in which a default can
survive a picker that always has something highlighted. `COLLECTION` is a list of strings and nothing else:
Emacs' alists, obarrays, hash tables and completion functions raise
`wrong-type-argument`, and more than 64 candidates is refused rather than
silently shortened, since that is how many the echo area can hold.

Four divergences, each recorded in `test/lisp-compat/features.json`:

* a `HISTORY` argument is **accepted and ignored**. kg's minibuffer
  histories are per-call-site rings (the shell command, the compile
  command), not values a symbol names, so there is nothing for the
  argument to select;
* a non-nil `PREDICATE` is **refused with an error** rather than dropped,
  in all three forms that take one. An ignored predicate would answer with
  candidates the caller excluded;
* `read-buffer`'s `DEFAULT` is accepted and ignored: kg's buffer picker
  answers a blank query with a default of its own — the current buffer
  under `REQUIRE-MATCH`, the next buffer in the ring otherwise — so it
  never returns the empty answer the argument would replace. The other
  three forms honour theirs;
* `y-or-n-p` answers `nil` for any key that is not `y`/`Y`, where Emacs
  re-asks. That is what every `(y/n)` question in the editor already does,
  and a Lisp form disagreeing with the editor around it would be the worse
  surprise. `yes-or-no-p` is the shape to use when the question deserves a
  typed word.

`read-file-name`'s `DEFAULT-FILENAME` answers an *empty* prompt, which on
kg's path picker means `M-RET` (its accept-the-typed-text-literally
escape): plain `Enter` on an empty prompt completes to the highlighted
directory entry instead, which is the picker's existing behaviour for
`C-x C-f`.

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
| `(function F)` / `#'F` | The function designator, without evaluating `F`: a symbol is returned as-is, a `(lambda ...)` form becomes the closure. `#'` is the reader's abbreviation for `(function ...)`, and the writer prints a `(function X)` form back as `#'X` — which is what `M-:` / `eval-expression` shows. Its sibling has done the same since Phase 11: `'X` reads as `(quote X)` and a two-element `(quote X)` prints back as `'X` |
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
  `(string-to-number "99999999999999999999")` answers the double the
  text rounds to, which is the same policy fe's reader takes for a
  literal.
- **Case conversion is ASCII only.** `upcase`, `downcase` and
  `capitalize` leave every byte ≥ 0x80 alone, because kg carries no
  Unicode case tables. A byte ≥ 0x80 *does* count as a word constituent
  for `capitalize`, so the ASCII letters of a non-ASCII word are not
  each read as the start of one.
- **A regexp never folds case, and its anchors are the subject's.**
  There is no `case-fold-search`: `string-match`, `re-search-forward`,
  `looking-at` and `replace-regexp-in-string` are all case-sensitive, and
  `replace-regexp-in-string` therefore never case-adjusts a replacement
  either (its `FIXEDCASE` argument is accepted and ignored). `^` and `$`
  match the start and end of the whole subject, not of each line in it —
  except in the buffer, where the subject *is* one line, which is why
  `looking-at`'s anchors behave exactly as Emacs' do and why no pattern
  it is given can match across a line break.
- **The writer does not re-escape a backslash inside a string.**
  `(format "%S" "a\\b")` is `"a\b"` here and `"a\\b"` in Emacs, so a
  printed string holding backslashes — anything `regexp-quote` returns,
  for instance — does not read back as itself. This is fe's writer, not
  kg's; `doc/TODO.md` carries the fix and its cost (one line in the
  writer, a re-measure of every golden that prints one, and a
  `FE_LANGUAGE_VERSION` move), which is why it is recorded here rather
  than closed by this phase.
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
  `end-of-buffer`, `beginning-of-buffer`
  and `no-catch` are all under `error`, and `file-missing` is under
  `file-error` (Phase 12), the one three-deep chain; `quit` is a separate branch not under `error` and
  is not catchable by `(error …)` handlers. `(signal 'ARITH-ERROR DATA)`
  raises a condition object `(ARITH-ERROR . DATA)`; `(error "fmt" ARGS)`
  formats at signal time and raises `(error "formatted-text")`.
  `throw` finds the innermost matching `catch` by `eq` tag and unwinds
  `unwind-protect` cleanups on the way; an uncaught `throw` signals
  `(no-catch TAG VALUE)`. `ignore-errors` is a one-line macro over
  `condition-case`. **kg's own editor natives raise Emacs' conditions
  with Emacs' data** since Phase 13: a type-check failure is
  `(wrong-type-argument PREDICATE VALUE)` with the predicate Emacs names
  — `(goto-char "x")` is `(wrong-type-argument integer-or-marker-p "x")`
  here as on Emacs 31.0.90 — and a range failure Emacs reports that way
  is `(args-out-of-range …)`. A handler naming the specific symbol
  catches them, and so does a generic `(error …)` one, since these are
  sub-conditions of `error`. Two things are deliberately *not* claimed.
  A native whose failure Emacs itself reports unstructured keeps a plain
  `error`: resource exhaustion, a dead buffer, a NaN position, and kg's
  own refusal of NUL and surrogate character codes, which Emacs accepts.
  An **uncaught** one reports Emacs' own sentence —
  `Wrong type argument: integer-or-marker-p, "x"` — since Phase 19 gave
  fe the `error-message` property and `error-message-string`; see below.
- **Dynamic binding is the Decision-2 subset, not `lexical-binding:
  nil`.** Variables are lexical by default and stay that way; a symbol
  becomes dynamic only by being *marked*, and only `defvar` and
  `defconst` mark. A `let` over a marked name swaps its global value
  cell and restores the old contents — or the fact that it had none — on
  every way out: return, error, `throw`, `C-g` and step-budget
  exhaustion. So the ordinary Emacs temporary-setting idiom works:

  ```elisp
  (progn (defvar dvs 1) (defun dvsf () dvs) (let ((dvs 2)) (dvsf)))
  ```

  is `2` here and `2` in the pinned Emacs 31. Three consequences follow
  with no further machinery, and all three are Emacs' measured answers:
  a function reading the name free sees the bound value wherever it was
  defined; `setq` inside the binding writes the binding rather than the
  value it hides; and a closure reads the value in force when it is
  *called*, not when it was made. The two arities differ as Emacs' do:
  `(defvar v VALUE)` and `(defconst v VALUE)` mark fully, so
  `(special-variable-p 'v)` is `t`, while a bare `(defvar v)` sets only
  the flag `let` consults — `special-variable-p` answers `nil` and `let`
  over it is dynamic anyway.

  **What stays lexical unconditionally is a *parameter*.** A `lambda` or
  `defun` parameter named after a marked symbol binds lexically, which
  is Emacs 31's own measured behaviour under `lexical-binding: t`; the
  flag is consulted at `let`'s binding paths and nowhere else.

  A one-argument `defvar`'s let-dynamic-only mark is scoped to the
  **input unit** it appears in — a `load`, a `require`, a batch file,
  an `M-:`, one `eval-buffer` — as Emacs scopes it to the evaluation
  unit, and Phase 12 measured the two agreeing on the canonical
  two-file probe: file A's own `let` over its bare `defvar` is
  dynamic, file B's `let` over the same name is lexical again, in both
  dialects. Two residuals are recorded rather than defended, and one
  runs **narrower** than Emacs, not broader: a `defun` written after
  the `defvar` in file A stays dynamic in Emacs when called from file
  B (the mark travels in the file's lexical environment), while kg —
  consulting the flag where the `let` runs — answers lexically there.
  The other is a widening: outside any input unit (hooks, command
  dispatch, process callbacks — host context), every mark is visible.
  A third residual, the generically named temporaries kg's prelude
  binds becoming dynamically capturable the moment a user `defvar`s
  such a name, is **closed**: of the 60 names the prelude `let`-binds,
  the 22 sites that are still bound while user code runs — the
  higher-order functions, the loader, and `add-to-list` — are bound by
  an immediately-applied lambda instead, whose parameter fe binds
  lexically unconditionally, and the rest cannot be observed.  The
  `prelude-temporary-hygiene` row carries the classification and the
  measurement that ruled `gensym` out for a function body.
  `test/lisp-compat/features.json`'s `prelude-defvar`,
  `phase11-one-arg-defvar-file-scope` and `prelude-let` rows carry the
  measurements, and fe's `one-arg-defvar-scope-carrier` row carries
  the probe grid.
- **The printer abbreviates `(quote X)` to `'X`, as Emacs' does.**
  `(format "%S" (list 'quote 'x))` is `"'x"` on both sides, recursively,
  so `(a 'b c)` prints that way and every `M-: ` echo of a quoted form
  reads as it does in Emacs. The discrimination is Emacs' measured one:
  exactly one element after the head and the form proper, so
  `(quote x y)`, `(quote)` and `(quote . x)` all keep printing as the
  pairs they are. The neighbouring **backquote** spelling is *not*
  closed and is different in kind: kg's reader expands `` ` ``/`,`/`,@`
  to the ordinary symbols `quasiquote`/`unquote`/`unquote-splicing`
  where Emacs uses distinct symbols its printer also abbreviates, so
  closing it means changing what the reader produces and breaking any
  Lisp that pattern-matches on `quasiquote`. Recorded as
  `phase8-reader-backquote-symbol-names`.
- **Buffer-local variables have one named gap**, not the three Phase 18
  left and not the wholesale absence this list used to record: there are
  no automatically-buffer-local variables, so where Emacs lets a plain
  `setq` create a binding (`fill-column` is one of its), kg needs
  `setq-local`. The two `let` interactions that were gaps are closed --
  a `let` now remembers which storage it displaced. See "Buffer-local
  variables" above; the manifest row is
  `phase18-automatically-buffer-local`, and the closed pair are
  `phase18-let-buffer-switched-out` and
  `phase18-make-local-while-let-bound`.
- **No vectors, no hash tables, no property lists.**
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
  string, and every public prelude definition has one since Phase 19
  (the `internal--` machinery deliberately does not). This is an
  alist-backed query, not a property list, and one divergence rides on
  that: the alist is a *definition registry*, so a `defun` with no
  docstring still records its name for `apropos` to find, which means
  `documentation` distinguishes "defined, undocumented" from "not
  defined" only by returning nil for both. `(documentation 'VARIABLE)`
  also answers here, where Emacs reserves `documentation` for functions
  and raises `void-function`. A BUILT-IN command's documentation is the
  one-line summary `cmdtable` carries for it, which is the same text
  `M-x` and the help screen show.
- A **variable's** docstring additionally lives where Emacs puts one,
  on the symbol's `variable-documentation` property, since Phase 20:
  `(get 'my-var 'variable-documentation)` answers it, a program can
  `put` its own over it, and `(documentation-property SYMBOL PROPERTY
  &optional RAW)` reads it back. `documentation-property` answers the
  property when it is a **string** and `nil` otherwise — Emacs' own
  answer for an integer, which there indexes a `DOC` file kg has no
  equivalent of. A re-`defvar` replaces the docstring and leaves the
  value alone, which is Emacs' rule for both halves. `describe-variable`
  reads through this rather than through the registry.
- The **describe surface is a package**, not a built-in: `(require
  'help-fns)` adds `describe-function`, `describe-variable` and
  `apropos`, each a command that writes into `*Help*`. What `apropos` can
  enumerate is fe's `(env)` — every interned symbol, so the primitives,
  kg's natives, the prelude and your own definitions — joined with
  `(internal--command-names)`, which is the only way to see a built-in
  command, whose name lives in a C table and is not a symbol until
  something writes one. It is capped at `apropos-max-results` matches per
  report (120 since Phase 20, 40 before it), because kg bounds every
  evaluation and a broad pattern is more sorting and formatting than one
  budget holds. The cap is measured against that budget rather than
  chosen: the broadest pattern in a stock session fits 158 rows and
  raises at 159.

## What is not here, and why

Things a reader coming from Emacs might expect are deliberately absent
from this surface, each recorded here rather than silently missing:

- **Hash tables, vectors and records.** Off-roadmap, and Phase 15's
  forecast audit is the instrument that re-answers it with kg-relevant
  data rather than intuition: across the whole corpus it measured
  **4 references to hash-table names** (one package sketch's word tally,
  which the same sketch also spells with an alist), **0 to vectors** and
  **0 to records**. `utils/forecast/AUDIT.md`'s watch-item table carries
  the number on every run, so reopening the question has evidence to
  start from.
- **`logand`, `logior`, `logxor`.** `ash` is here because it is exact in
  three lines of prelude Lisp over `expt` and `floor`; the three bitwise
  operations are not, and the forecast audit measured **zero** references
  to any bitwise operation in the corpus. When demand appears they belong
  beside fe's own `expt` and `floor` — fe owns the numeric tower and has
  `int64_t` in hand — not in a kg native and not in a 63-iteration
  prelude loop.
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
| Line/character motion, `skip-chars-forward`/`-backward` | `src/lisp_motion.c` |
| `load`, `require`/`provide`/`featurep`/load-path, XDG config resolution, `format`, `message`, `insert`, region edits | `src/lisp_io.c`, `src/lisp_require.c` |
| Command registry, `command-execute`, key bindings | `src/lisp_cmd.c` |
| Buffer/marker/process object pool, generation checks | `src/lisp_obj.[ch]` |
| Search, `looking-at`, `string-match`, `regexp-quote` and match data | `src/lisp_search.c` |
| Hooks and their event-drain subscriber | `src/lisp_hooks.[ch]` |
| Process objects, filters, sentinels | `src/lisp_process.[ch]`, `src/process_table.[ch]`, `src/process.[ch]` |
| String natives, including case conversion and `string-to-number` | `src/lisp_string.c` |
| The minibuffer reads — the interactive codes' four readers and the seven public `read-*`/`y-or-n-p`/`completing-read` forms | `src/lisp_prompt.c`, over `src/prompt.[ch]`'s candidate picker and y/n question |
| The forecast audit and its corpus | `utils/forecast_audit.py`, `utils/forecast/` |
| The public, Fe-free surface every editor module includes | `src/lisp.h` |

`src/lisp_internal.h` is the private surface shared only among
`src/lisp_*.c` (`make lisp-include-check` enforces the boundary); it is
not part of this document because nothing outside those files may
include it.
