# Cross-component extensibility architecture review

Date: 2026-07-30

Scope: kg, its embedded Fe interpreter, and tiny-regex-c. This is an
architecture review, not a proposal to turn kg into a clone of GNU Emacs.

## Executive conclusion

The right next architecture is **a small, Lisp-independent editor core with one
owner for each piece of state, one mutation gateway, and registries for
commands, variables, modes, keymaps, hooks, processes, markers, and overlays**.
Fe should remain an optional adapter over those services. tiny-regex-c should
remain a bounded leaf library behind kg's wrapper.

Do not begin by adding more Lisp natives to the current `editor` global. That
would make a larger API out of the least stable boundary. The highest-leverage
work, in order, is:

1. separate session, buffer, and window/view state;
2. funnel every text change through one replace-range transaction;
3. unify static and Lisp commands, then add layered keymaps and real mode
   descriptors;
4. build markers, overlays, variables, and deferred hooks on that mutation
   transaction;
5. generalize the existing compilation subprocess into a small process manager;
6. only then extend Fe where host-side emulation is inadequate.

This preserves kg's minimalism better than either a rewrite or continued
feature-by-feature wiring. It also preserves `WITH_LISP=0`: every editor
facility exists in C, while the Fe adapter registers callbacks and converts
values when compiled in.

## What the code is telling us

### State ownership is the first constraint

`struct editor_config` mixes terminal/session state, active-window viewport,
buffer text, mark, local options, and mode state
(`src/def.h:258-324`). `struct editor_buffer` then duplicates most of the
buffer and viewport fields (`src/def.h:390-424`). Switching buffers manually
copies them in both directions (`src/bufmgr.c:45-86`,
`src/bufmgr.c:88-139`), while windows copy viewport state again
(`src/winmgr.c:13-32`, `src/winmgr.c:35-58`).

That arrangement is already difficult to reason about with multiple windows.
It is not a sound base for buffer-local Lisp variables, markers, hooks, or
callbacks holding buffer references: an extension cannot know whether the
authoritative value is in `editor`, `buflist`, or `winlist`.

Text mutation is similarly distributed. Row insertion/deletion directly
updates storage, dirty state, and highlighting (`src/buffer.c:323-366`,
`src/buffer.c:390-409`); ordinary insertion separately records undo and dirty
state (`src/buffer.c:903-952`); newline insertion performs more direct row
changes (`src/buffer.c:961-994`, `src/buffer.c:1055-1134`); kill-line mutates
rows through another path (`src/buffer.c:1360-1406`). There is no single seam
at which markers, overlays, before/after-change hooks, syntax invalidation, and
undo grouping can all be made correct.

### “Mode” currently means several unrelated things

`editor_syntax` contains a display name, filename patterns, lexical syntax, and
a highlighter (`src/def.h:215-227`). The global syntax database is a static
array (`src/syntax.c:602-650`), but behavioral modes are inferred from
highlighter or descriptor pointer identity (`src/syntax.c:933-937`,
`src/syntax.c:1012-1016`, `src/dired.c:17-47`). `kbd.c` then hard-codes mode
bindings before user bindings (`src/kbd.c:306-328`) and has another special
table for dired (`src/kbd.c:463-498`).

This works for a handful of C modes, but syntax highlighting, keymaps, mode
identity, mode state, and activation are different concepts. Treating one
pointer as all of them prevents Lisp-defined modes and makes composition of
minor modes impractical.

### Commands and keys have promising seams, but duplicate policy

The named-command table already records an edit flag and centrally enforces
read-only buffers (`src/cmd.c:742-818`, `src/cmd.c:835-855`). Static and
Lisp-defined command names already share M-x completion
(`src/cmd.c:878-895`). Those are good seams to retain.

However, Lisp's `command-execute` has a second allow-list with its own mutation
booleans (`src/lisp.c:1224-1241`) instead of using the command table's policy
(`src/lisp.c:1252-1297`). Lisp commands live in a fixed 32-entry private
registry (`src/lisp.c:50-60`, `src/lisp.c:1416-1500`), and user keys live in a
fixed 32-entry C-c-only flat table (`src/keybind.c:15-24`,
`src/keybind.c:25-58`). Command prefix state is ambient and temporarily
save/restored around named execution (`src/cmd.c:857-875`).

The next step is not a larger allow-list. It is one command registry and an
explicit command invocation context.

### The Lisp boundary is disciplined but too monolithic

There is one useful existing invariant: only `src/lisp.c` includes `fe.h`, and
kg deliberately compiles only Fe's core (`doc/fe-upstream.md:18-22`). The
`WITH_LISP=0` implementation provides the same outward functions as stubs
(`src/lisp.c:2179-2230`). Keep both properties.

Inside that boundary, however, `src/lisp.c` owns evaluation recovery, package
loading, string utilities, position conversion, editor natives, command roots,
key bindings, and a large prelude. Natives reach straight into the active
global buffer—for example, `insert` reads `editor.readonly`, pushes undo, and
calls a raw insertion helper (`src/lisp.c:385-413`), while point conversion
directly traverses global rows (`src/lisp.c:425-478`). This is an adapter over
implementation details, not yet an editor API.

Fe itself is capable of persistent callback values through `FeRoot`
(`fe/fe.h:127-135`, `fe/fe.c:1345-1368`) and bounded/cancellable evaluation
through `FeEvalOptions` (`fe/fe.h:25-30`, `fe/fe.c:1395-1452`). Those are good
building blocks. Current limits that matter for richer packages are:

- unbound and globally bound-to-`nil` are indistinguishable because global
  lookup simply returns the symbol value (`fe/fe.c:828-843`); kg's `defvar`
  consequently reinitializes a nil-valued variable
  (`src/lisp.c:1894-1901`);
- recovery is host `longjmp`; Fe explicitly cannot unwind host resources or
  its GC stack (`fe/doc/implementation.md:129-157`);
- the fixed GC protection stack bounds useful recursion, and the collector
  recursively marks `car` (`fe/fe.c:108-120`, `fe/fe.c:379-419`);
- custom pointer-backed types are three process-global legacy slots, not
  per-context registrations (`fe/fe.h:45-62`,
  `fe/doc/c-api.md:333-346`);
- kg rejects nested top-level evaluation in its host frame
  (`src/lisp.c:2002-2016`, `src/lisp.c:2111-2127`).

These are reasons for selective Fe work, not for moving editor state into Fe.

### Processes and regexes already suggest general services

Compilation has a careful asynchronous lifecycle, but it is one global process
(`src/compile.c:13-16`) polled specially from both the main loop
(`src/main.c:160-170`) and terminal input waits (`src/tty.c:413-437`). Its
state already contains most of a reusable process record
(`src/compile.h:11-51`). Generalizing this is substantially safer than adding
another independent subprocess subsystem for Lisp.

kg's regex wrapper is also the correct public boundary: it owns checked
compiled storage and normalized spans (`src/regex.h:13-35`,
`src/regex.c:57-112`). tiny-regex-c is intentionally bounded to ten spans and
8192 compiled bytes (`fe/tiny-regex-c/re.h:99-115`) and bounds backtracking and
stack depth (`fe/tiny-regex-c/re.c:80-101`). Two facts must be fixed before it
becomes a broadly callable extension service:

- matching uses process-global step/depth counters
  (`fe/tiny-regex-c/re.c:294-299`), so the matcher is not re-entrant;
- both the engine and wrapper derive subject length with `strlen`
  (`fe/tiny-regex-c/re.c:403-415`, `src/regex.c:114-145`), so the API cannot
  represent embedded NUL.

The former is a small correctness fix: put both counters in the existing match
context. The latter should become an explicit, documented byte-length API
before Lisp regex functions or syntax packages depend on it.

## Dependency diagrams

Current effective dependencies:

```text
 terminal input ──────────────┐
 M-x / hard-coded mode keys ──┼──> global `editor`
 buffer/window switch copies ─┤      ⇅ manual snapshots
 display/syntax/undo ─────────┤   buflist[] + winlist[]
 Fe natives/commands ─────────┘
          │
          ├── directly calls editor implementation helpers
          └── Fe core (optional)

 search ──> kg regex wrapper ──> tiny-regex-c
 main/tty ──> one global compilation process
```

Recommended target:

```text
                         kg_session
          ┌─────────────────┼──────────────────┐
          v                 v                  v
   command registry   event/process loop   runtime adapter
          │                 │                  │
          v                 v            ┌─────┴─────┐
   layered keymaps      processes         │ no-op     │
          │                                 WITH_LISP=0
          v                              │ Fe adapter │
   mode registry                           WITH_LISP=1
          │                              └─────┬─────┘
          v                                    │ callbacks/roots
   kg_view ──points to──> kg_buffer <──────────┘
                              │
                    replace-range transaction
                       ┌──────┼────────┐
                       v      v        v
                    undo   markers  invalidation
                              │        │
                           overlays  syntax/display

   core regexp service ──> kg regex wrapper ──> tiny-regex-c
```

Dependency direction is the important part: editor facilities do not depend on
Fe objects; the Fe adapter depends on stable editor facilities.

## Target core, kept deliberately small

### 1. One owner for state

Introduce:

- `kg_session`: terminal, minibuffer, command loop, global options,
  registries, kill ring, process manager, runtime adapter;
- `kg_buffer`: stable `(id, generation)`, rows/text, filename, dirty state,
  undo, mark ring, major/minor modes, local variables, markers/overlays;
- `kg_view`: buffer handle, point, viewport, goal column, rectangle/selection
  presentation;
- `kg_window`: layout plus one `kg_view`.

A buffer's content and properties must never be copied into a session-global
active snapshot. Accessors such as `kg_current_buffer(session)` can keep call
sites concise. A window owns point/scroll; the buffer may retain a “last point”
used when a new view visits it. This resolves the current ambiguous
buffer-versus-window cursor copies without forcing an Emacs-perfect window
model.

Use stable integer IDs plus generations at every externally retained boundary.
Never give Lisp an `erow *`, `editor_buffer *`, slot index, or raw process PID
as identity.

### 2. One edit transaction

Make the primitive operation conceptually:

```c
kg_edit_result kg_buffer_replace(kg_buffer *buffer,
    size_t begin_byte, size_t end_byte,
    const char *replacement, size_t replacement_len,
    const kg_edit_options *options);
```

Byte offsets are the appropriate internal unit because rows and regex spans
already use them. Lisp continues to see 1-based codepoint positions; conversion
belongs at the adapter boundary. The transaction must:

1. validate read-only and bounds;
2. open or join an undo group;
3. update row storage;
4. relocate markers according to insertion gravity;
5. invalidate affected overlays and syntax/render caches;
6. update dirty/change generation;
7. enqueue one typed change event;
8. leave the buffer and undo stack valid on allocation failure.

Higher operations—insert, delete, newline, yank, rectangle edits, query
replace, undo, and process output—compose this primitive. Raw row helpers may
remain private implementation details. This does **not** require replacing the
row representation with a rope or gap buffer.

### 3. Commands, keymaps, and modes as registries

Use one command record for both C and runtime callbacks:

```text
name, invoke(context), flags, interactive/prefix policy, documentation
```

The invocation context carries session, selected view/buffer, input fd, prefix,
and origin (`key`, `M-x`, `Lisp`, `hook`). Read-only policy and completion then
come from the same record. Remove the Lisp allow-list; commands whose flags say
they are safe may be invoked uniformly.

A keymap is a trie of normalized key events/sequences whose leaves reference
command names/IDs. Resolution order should be explicit and testable:

```text
transient map > enabled minor modes (newest first) > major mode > global map
```

Keep C-c as the initially supported user prefix if desired, but do not bake it
into the representation. The current parser can become the first textual
front-end to the trie.

A major-mode descriptor should contain identity/name, detection predicates,
syntax/highlighter, local keymap, activation/deactivation functions, and
mode-line contributions. A minor-mode descriptor contributes a keymap,
variables, hooks, and mode-line text. Dired, git commit, git rebase, Lisp
interaction, and compilation should migrate first because they exercise
behavior beyond highlighting. Syntax becomes one field of a mode, not mode
identity.

### 4. Variables, markers, overlays, and hooks

Create a C variable registry with name, type, default, scope, safety policy,
getter/setter, and optional validation. Start with the existing
`compile-command`, `buffer-read-only`, `auto-revert`, `visual-line-mode`, and
`overwrite-mode`. The current local-variable parser is hard-coded to two fields
(`src/localvars.h:18-26`, `src/localvars.c:272-310`); it should parse into the
registry while still accepting only entries marked file-local-safe. This keeps
non-evaluating local variables independent of Lisp.

For Lisp, provide `buffer-local-value`, `set`, and `setq-local` over this
registry. Arbitrary Lisp-only buffer-local values can be stored by the runtime
adapter as Fe roots keyed by buffer ID; buffer destruction tells the adapter to
release them. Do not make Fe the storage for C-owned options.

A marker is `(buffer ID/generation, byte offset, insertion gravity)`. Every
replace transaction relocates it. An overlay is two markers plus a small set of
display properties (initially face/highlight, priority, and optional
evaporate). The renderer asks the buffer for overlay runs for the visible
range; it must not call Lisp while drawing.

Hooks are typed events, not arbitrary string notifications. Start with:

- before/after change;
- buffer opened/killed;
- before/after save;
- major mode changed;
- pre/post command;
- process output/exit.

Crucially, enqueue runtime hooks and drain them only at safe points after the
outer command/transaction completes. Never call Lisp while row arrays are being
reallocated, while rendering, or beneath a minibuffer prompt. This avoids the
current nested-evaluation restriction becoming a reentrancy bug and gives hook
ordering a deterministic contract. Bound every callback with the same step and
C-g policy as interactive evaluation.

### 5. Processes as a core service

Extract the lifecycle already present in compilation into a bounded process
table. Each process owns stable ID/generation, pid/process group, fds, state,
output cap, associated buffer, filter callback, sentinel callback, and cleanup
policy. The event loop polls all input/process fds and queues filter/sentinel
events. Compilation becomes the first client.

Initially allow Lisp to start shell commands only through an explicit,
documented API and deliver callbacks at safe points. Lisp is trusted code
already, so this is not a sandbox; bounds are for responsiveness and resource
control. Always reap children, make kill/exit idempotent, and invalidate stale
handles before invoking sentinels.

### 6. Regex as a leaf service

Keep tiny-regex-c private. Add byte-counted compile/execute wrapper functions,
move match budgets into per-call context, and expose kg-level compiled regex
handles. A generation-checked C handle table is adequate initially and works in
`WITH_LISP=0`; per-context Fe custom types can later make the Lisp value nicer.

Do not compile Fe's whole Fex layer merely to get regexes. kg intentionally
excludes its I/O/process/regex extensions (`doc/fe-upstream.md:18-22`), and the
kg wrapper contains UTF-8 span normalization and editor-specific status
mapping that a direct Fex binding would bypass.

## Selective Fe roadmap

Most useful extensibility does not require an evaluator rewrite. Prioritize:

1. **`FeCallWithOptions`** (or equivalent) so callbacks need no source-string
   trampoline; kg already documents this gap (`doc/TODO.md:214-218`).
2. **A real unbound state** distinct from nil, plus `boundp`/`makunbound`.
   This fixes `defvar` semantics and makes package feature detection reliable.
3. **Per-context custom type registration** with mark/finalize callbacks.
   This replaces the three global Fex slots and supports safe opaque editor
   handles.
4. **Internal error/cleanup frames** sufficient for `error`,
   `unwind-protect`, and `condition-case`. This is the largest justified Fe
   change: hooks and process callbacks need cleanup and recoverable package
   errors. Specify native-resource rules first; `longjmp` must not silently
   skip C ownership.
5. Only after real package demand: vectors and hash tables, documentation
   metadata, `provide`/`require`, and better source locations/backtraces.

Do not promise general Emacs Lisp compatibility. Fe is lexically scoped,
arena-bounded, has doubles rather than the Emacs numeric tower, and lacks much
of the Emacs object model. Call the supported language “Fe with an
Emacs-shaped editor API,” publish a compatibility matrix, and grow it from
useful packages. Dynamic binding, advice, text properties, byte compilation,
and an exact Emacs reader are not prerequisites for productive kg extensions.

Keep the fixed caller-owned arena initially. Add arena usage/high-water
telemetry and a user-configurable size before considering a moving or
multi-arena collector. Replacing Fe's compact object model or GC now would
consume the project without unlocking the editor seams above.

## Prioritized migration phases

### Phase 0 — contracts and characterization

- Document ownership, handle, callback, edit, and hook-order invariants.
- Add stable buffer IDs/generations without exposing them yet.
- Characterize multiple windows visiting one buffer, undo grouping,
  read-only rejection, mode key precedence, and `WITH_LISP=0` parity.
- Move tiny-regex-c match counters into call context.

Exit criterion: no behavior change; both builds and full CI green.

### Phase 1 — state ownership

- Split session, buffer, and view/window structs.
- Replace save/restore struct copies with selected-buffer/view accessors.
- Keep row representation and existing public command behavior.

Exit criterion: every buffer property has one owner; two windows on one buffer
retain independent point/scroll state.

### Phase 2 — mutation and command cores

- Introduce replace-range and compound undo transactions.
- Migrate all mutating commands and special-buffer append paths.
- Replace static-plus-Lisp lookup and the Lisp allow-list with one command
  registry and explicit invocation context.

Exit criterion: a debug build can assert that content generations change only
inside the mutation gateway.

### Phase 3 — variables, modes, and keymaps

- Add variable registry and route safe file/directory locals through it.
- Add keymap trie and explicit precedence.
- Add mode descriptors; migrate dired, git modes, Lisp interaction, and
  compilation before ordinary syntax-only modes.

Exit criterion: no behavioral mode is recognized by syntax-pointer identity;
a C test can define a temporary mode/keymap without editing `kbd.c`.

### Phase 4 — markers, overlays, and deferred hooks

- Implement marker relocation and convert mark/mark-ring to markers.
- Add overlay range queries to rendering.
- Add typed event queue and C callbacks; then expose Fe callback roots.

Exit criterion: randomized edits preserve marker/overlay invariants, and Lisp
cannot be entered from inside a half-completed edit or redraw.

### Phase 5 — process manager and runtime packages

- Generalize compilation into the process table/event loop.
- Expose bounded process APIs, filters, and sentinels.
- Add byte-counted regex handles and useful buffer/search natives.
- Ship two small first-party packages—one minor mode and one process-backed
  mode—as architectural acceptance tests.

Exit criterion: compilation still passes its PTY suite, two concurrent
processes are independently cancellable/reaped, and the same core works with
`WITH_LISP=0`.

### Phase 6 — justified Fe semantics

- Land `FeCallWithOptions`, unbound values, per-context types, then condition
  cleanup support in separate reviewed changes.
- Add vectors/hashes or package loading features only against demonstrated
  package needs.

Each Fe change must update Fe's own API/implementation documentation and pass
Fe's standalone CI before kg advances the submodule pin.

## Invariants worth making non-negotiable

- `WITH_LISP=0` is a first-class product, not a stub-filled afterthought.
- Only the Fe adapter includes `fe.h`; core headers contain no `FeObject *`.
- Buffer text/properties and view position are each owned in exactly one place.
- Every text edit is one validated transaction, including undo and process
  output.
- External handles are `(ID, generation)` and fail cleanly after deletion.
- Renderer, signal handlers, raw mutation, and minibuffer internals never call
  Lisp.
- Runtime callbacks are queued, bounded, cancellable, and run at documented
  safe points.
- Command flags, read-only policy, completion, and documentation come from one
  registry.
- Mode identity never depends on a syntax/highlighter pointer.
- File-local variables remain non-evaluating and explicitly allow-listed by
  variable metadata.
- Every child is reaped exactly once; every fd and Fe root has a clear owner.
- Regex compile/match remains bounded and reports “too complex” distinctly
  from “no match.”

## Test strategy

1. **Ownership/unit tests:** multiple views of one buffer, killing a buffer
   with live handles, slot reuse/generation rejection, local-variable lifetime.
2. **Model-based edit tests:** compare random replace sequences with a flat
   reference string; simultaneously check point, markers with both gravities,
   overlays, undo/redo, UTF-8 boundaries, dirty state, and syntax invalidation.
3. **Command/keymap tests:** all precedence layers, prefixes, read-only flags,
   disabled commands, mode activation/deactivation, command removal while
   bound.
4. **Hook tests:** exact ordering, error/cancellation isolation, buffer killed
   before queued callback, recursive edits, callback-created callbacks, and
   budget exhaustion.
5. **Process tests:** fake pipes/wait results for deterministic units plus PTY
   cases for two concurrent children, output truncation, cancellation,
   callback errors, and editor shutdown.
6. **Fe tests:** GC stress with many rooted callbacks, root release on buffer
   kill/redefinition, unbound-versus-nil, cleanup during error/cancellation,
   and nested callback accounting.
7. **Regex tests:** keep fuzzing and the Emacs differential oracle; add
   reentrancy, explicit-length/NUL, stale-handle, and per-call budget cases.
8. **Configuration matrix:** run focused unit and PTY cases with both
   `WITH_LISP=1` and `0`; end each phase with `.ci/run-ci-steps.sh`.

Add debug-only counters/assertions for live buffers, views, markers, overlays,
processes, fds, Fe roots, queued events, and edits attempted outside the
gateway. These are cheaper and more actionable than a large generic plugin
ABI.

## Overhauls to avoid

- **No editor rewrite and no rope/gap-buffer prerequisite.** Hide rows behind
  replace-range first; replace storage only after profiling proves a need.
- **No Fe-owned editor core.** It breaks `WITH_LISP=0`, complicates recovery,
  and makes GC lifetime the editor's lifetime model.
- **No “full Emacs Lisp compatibility” project.** Port productive concepts and
  packages against an explicit subset.
- **No synchronous Lisp hooks from low-level edits, drawing, signals, or
  process polling.** Queue them to top-level safe points.
- **No raw C pointers encoded as Lisp numbers and no permanent slot indices.**
- **No `dlopen` C plugin ABI yet.** A versioned native ABI, allocator
  boundaries, and crash isolation are far costlier than trusted Fe packages.
- **No threads for process output.** A bounded `poll`-based event loop fits the
  existing terminal architecture and avoids making Fe/thread safety a problem.
- **No wholesale Fex import.** Bind the small editor-owned services kg intends
  to support.
- **No regex-driven replacement of every highlighter.** Keep incremental C
  highlighters for hot paths; later allow bounded declarative rules where they
  are expressive enough.

## Honest ordering of outcomes

The first three phases are architectural debt repayment with immediate
correctness and ergonomics value even if richer Lisp work stops. Markers and
the edit transaction are the inflection point: once they exist, overlays,
save-excursion, robust mark rings, bookmarks, match navigation, diagnostics,
and many mode affordances become small compositions rather than special cases.

Modes/keymaps/variables come next because they let kg itself stop adding
behavior in `kbd.c` and syntax-pointer checks. Deferred hooks then make Lisp
extensions useful without making the core re-entrant. Processes are highly
valuable, but should follow safe callback scheduling. Fe evaluator/object-model
work is last because most user-visible extensibility can be delivered without
it, and its error and GC invariants make casual changes disproportionately
risky.

If only one ambitious program is funded, fund Phases 0–4. They produce a
cleaner, safer editor in both build configurations and establish the stable
surface on which richer Fe, productive Emacs affordances, and performance work
can proceed without repeatedly rebuilding the foundation.
