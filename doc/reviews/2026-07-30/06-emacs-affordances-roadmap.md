# Emacs affordances roadmap for kg and Fe

Date: 2026-07-30
Scope: user-facing behavior, command/key architecture, modes, minibuffers,
buffer/window state, tests/docs, and kg's Fe bridge. This review made no product
code changes.

## Executive recommendation

kg should aim to become a **small editor with an Emacs-shaped extension
kernel**, not a miniature reimplementation of GNU Emacs. The highest-leverage
next step is not another set of hard-coded keys. It is to unify commands,
keymaps, mode-local behavior, documentation, and Lisp invocation around one
registry. Today those concepts are split among:

- the named-command table (`src/cmd.c:742-895`);
- large built-in dispatch switches and special-mode branches
  (`src/kbd.c:264-483`, `src/kbd.c:580-1069`);
- the fixed 32-entry, `C-c <key>`-only user binding table
  (`src/keybind.c:15-123`);
- the separate Lisp command registry and built-in allow-list
  (`src/lisp.c:80`, `src/lisp.c:1229-1294`, `src/lisp.c:1459-1565`);
- the hand-maintained help screen (`src/help.c:18-96`).

That split makes every new mode and binding cost more than it should and keeps
Lisp packages from composing with built-ins. A unified command/keymap layer,
followed by buffer-local mode state, edit transactions/markers, and a generic
minibuffer completion API, is the enabling architecture. It should then be
used to ship a deliberately small set of daily wins:

1. multi-entry kill ring and `M-y`;
2. generated help/introspection (`describe-key`, `describe-command`,
   `describe-bindings`, `where-is`);
3. position registers and persistent bookmarks;
4. backups/autosave recovery;
5. navigable compilation and project grep (`next-error` / `previous-error`);
6. project file finding;
7. a real Lisp mode/package API with hooks and buffer-local settings.

Do not postpone all features for a rewrite. The kill ring, save safety, line
numbers, and basic registers can land against the current core. But do not add
more mode-specific branches to `kbd.c`; mode growth should wait for keymaps.

## Current baseline

kg already covers much of the high-frequency Emacs editing vocabulary:
character/word/sentence/paragraph motion, mark and mark ring, active regions,
shift selection, rectangles, incremental literal and regexp search,
query-replace, smart case, multi-level undo, keyboard macros, prefix
arguments, multiple buffers/windows, Dired, compilation, shell filters,
visual-line and overwrite modes, file/directory local settings, Lisp
evaluation, and an ido-style picker. The PTY suite exercises these features
extensively, including UTF-8, search history, Lisp init/packages, compilation,
Dired, Git commit/rebase modes, and special terminal dimensions.

The extension surface is much narrower than that feature list suggests:

- Lisp can inspect/move point and mark, read text, insert text, manipulate
  strings, show a message, load a file, and call eleven explicitly allowed
  built-ins (`src/lisp.c:1229-1294`, `src/lisp.c:1588-1631`).
- Lisp cannot delete/replace a region directly, enumerate/switch buffers,
  create special buffers, save excursion, use markers, define modes or
  syntax, run hooks, prompt through the minibuffer, or launch an async job.
- A Lisp command has no doc record or interactive argument specification.
  `(interactive)` is stripped and used only as a registration marker
  (`src/lisp.c:1862-1903`).
- “Mode” is principally an `editor_syntax *`; special behavior is often
  inferred from pointer/name/filename and dispatched in C. The modeline reads
  `syntax->name` and separate booleans (`src/display.c:375-427`).
- Buffer state is duplicated between the live `editor` object and fixed
  `MAX_BUFFERS` slots by large save/restore copies
  (`src/bufmgr.c:45-140`, `src/def.h:258-324`, `src/def.h:390-424`).
- The minibuffer has good editing and history primitives, but path, buffer,
  and command pickers each own substantial policy/loops rather than consuming
  one completion protocol (`src/bufmgr.c:435-1138`,
  `src/bufmgr.c:1310-1436`, `src/cmd.c:903-1068`).
- The event loop already polls asynchronous compilation and auto-revert
  (`src/main.c:160-170`), so a bounded job abstraction can evolve from proven
  machinery rather than introducing threads.

## Correctness gate

The roadmap must begin with the high-severity editor and interpreter findings
in the companion correctness reviews, especially UTF-8 deletion, kill-line
undo corruption, compilation output bounds, disk-change identity, Fe macro
expansion, malformed dotted lists, bounded/cycle-safe printing, exact function
arity, and an unbound sentinel. Extensibility multiplies the reach of core
invariants; it should not multiply known corruption or hang paths.

This is a gate on exposing more primitives, not a requirement to perfect all
of Fe before shipping independent editor affordances.

## Dependency graph

```text
correctness gate
   |
   +--> unified command descriptors --> hierarchical keymaps --> modes/hooks
   |             |                           |                  |
   |             +--> generated help        |                  +--> Lisp modes
   |             +--> safe Lisp dispatch     +--> transient maps
   |
   +--> edit transactions --> markers --> registers/bookmarks/save-excursion
   |          |                    |
   |          +--> undo grouping   +--> diagnostic/navigation locations
   |
   +--> generic minibuffer/completion --> project files / apropos / M-x
   |
   +--> bounded async jobs --> compilation parser --> next-error / project-grep
   |
   +--> Fe unbound+arity+unwind --> errors/cleanup/hooks --> richer packages
```

The first three foundation tracks can proceed independently. Modes should
depend on command/keymap unification; cross-buffer Lisp should depend on stable
buffer handles and unwind-safe temporary selection; navigable diagnostics
should depend on markers and a reusable job/output abstraction.

## Effort notation

- **S**: focused implementation, a native unit case and roughly 1–3 PTY
  cases; README/man/help touch if user-visible.
- **M**: a module or shared protocol, native matrix and roughly 4–10 PTY
  cases, `WITH_LISP=0` coverage, and a dedicated manual subsection.
- **L**: cross-cutting state model, fuzz/state-machine testing plus broad PTY
  coverage, migration documentation, and compatibility tests.

These are relative costs, not schedule estimates.

## Near term: daily leverage and enabling architecture

### N1. Unify command metadata and key dispatch

**Minimum semantics:** one command descriptor contains name, implementation
(C or rooted Lisp function), edit/read-only policy, short documentation,
prefix policy, and optional interactive argument description. One bounded
key-sequence parser handles existing keys and prefix maps. Lookup precedence
is transient map, minor-mode maps, major-mode map, then global map. Initially
populate maps with exactly today's bindings; preserve `C-c` as the recommended
user namespace, but allow explicit rebinding elsewhere.

Replace `cx_prefix`, `cc_prefix`, and `rect_prefix` special state gradually
with a generic prefix-map state. Convert the C-x, rectangle, Dired,
compilation, Git commit, and Git rebase branches to named commands in maps.
Keep raw self-insert and terminal decoding in C.

**Integration points:** `src/cmd.c:742-895`, `src/keybind.c`,
`src/kbd.c:264-483`, `src/kbd.c:580-1069`, Lisp registration at
`src/lisp.c:1459-1631`, and the static help table in `src/help.c`.

**Immediate payoff:** `describe-key`, `describe-command`,
`describe-bindings`, `where-is`, accurate M-x docs, mode-local maps, and one
read-only policy. It also removes the brittle second Lisp allow-list:
descriptors can explicitly say whether a command is callable from Lisp and
whether it may prompt or edit.

**Test/docs cost:** **L**. Add table invariants (unique names, valid bindings,
known targets), key parser/property tests, PTY parity for every existing
prefix family, Lisp rebind/unbind tests, and generated help snapshots.
Document precedence and reserved emergency keys (`C-g`, quit/recovery).

### N2. Add command identity and transient command state

**Minimum semantics:** maintain `this-command`, `last-command`, and one small
command-owned transient record cleared when an unrelated command runs. This
is not a general event system.

**Payoff:** correct yank-pop eligibility, repeated kill coalescing based on
command identity instead of incidental keystrokes, repeatable commands, and
clean replacement for current one-off `last_key`, recenter, and window-line
cycle fields (`src/def.h:312-316`).

**Test/docs cost:** **M**. Test keyboard, M-x, Lisp, and macro invocation
produce identical command identity; cancellation must clear transient state.

### N3. Ship a bounded kill ring and `M-y`

**Minimum semantics:** 16 entries, byte/entry and total-memory caps;
consecutive kill commands append/prepend to the current entry as appropriate;
copy starts a new entry; `C-y` remembers the exact inserted span; `M-y`
immediately after yank/yank-pop atomically replaces that span with the next
entry and remains one undo step per replacement. Any intervening command makes
`M-y` refuse rather than edit surprising text.

**Integration points:** single-entry storage at `src/yank.c:10-87`, yank at
`src/yank.c:560-581`, kill producers throughout `src/yank.c`, `src/word.c`,
and `src/buffer.c`, and command identity from N2.

**Test/docs cost:** **M**. Native tests for direction/coalescing/caps/UTF-8 and
undo; PTY tests for eligibility, intervening commands, prefix selection,
buffer switching, and read-only refusal. Update README, man page, and generated
help.

### N4. Backups and autosave recovery

**Minimum semantics:** optional backup before the first overwrite of an
existing file in a save session, and periodic crash-recovery files for dirty
real-file buffers. Recovery files must be written atomically, must never
replace the source automatically, and should be offered on open with clear
source/recovery metadata. Clean exit/save removes only kg-owned recovery
artifacts.

**Integration points:** the atomic save path in `src/fileio.c`, dirty/clean
transitions in buffer and undo code, multi-buffer polling in
`src/main.c:160-170`, and buffer slots in `src/bufmgr.c`.

**Test/docs cost:** **M/L** because failure injection matters more than UI.
Test permission errors, stale recovery, symlinks, multiple buffers, crashes,
and cleanup. Document exact filenames, privacy implications, opt-out, and
recovery policy.

### N5. Lightweight registers and bookmarks

**Minimum semantics:** position registers (`C-x r SPC`, `C-x r j`) first,
using stable buffer identity plus an adjusting marker. Add text registers only
after the position form is solid. Bookmarks persist filename plus codepoint
position and a short surrounding-text fingerprint; reopening relocates near
the saved text or reports staleness.

**Integration points:** extend the existing C-x-r map currently dedicated to
rectangles (`src/kbd.c:331-365`); do not overload raw row/column pairs because
edits invalidate them. Marker adjustment belongs in centralized insertion and
deletion primitives.

**Test/docs cost:** position registers **M**, persistence **M**. Cover edits
before/through markers, killed buffers, renamed files, UTF-8, rectangle map
coexistence, malformed bookmark files, and atomic bookmark save.

### N6. Small independent wins

- `transpose-words` (`M-t`): **S**, reuse word bounds and one replace
  transaction; test punctuation, UTF-8 policy, undo.
- `display-line-numbers-mode`: **M**, per-window gutter width and resize;
  test splits, wrapping, horizontal scroll, large line counts, and narrow
  terminals. Do not call it `linum-mode`; use Emacs' current name.
- Make word motion use one documented Unicode-aware constituent predicate:
  **M**, because current Lisp bounds and interactive word movement disagree
  (documented in README). Differential PTY cases against Emacs should cover
  accented, CJK, underscore, and punctuation examples.

## Mid term: project navigation and a real mode/package layer

### M1. Edit transactions and adjusting markers

**Minimum semantics:** all mutations pass through insert/delete/replace
operations inside an optional transaction. A transaction produces one undo
unit, adjusts registered markers, sets dirty state once, and emits one bounded
after-change notification. Keep the existing row storage; this is not a text
buffer rewrite.

**Integration points:** mutation primitives in `src/buffer.c`, composite edits
in `src/yank.c`, `src/rect.c`, and `src/word.c`, and the undo record family in
`src/undo.c` / `src/def.h:340-374`.

**Payoff:** reliable yank-pop, registers, compilation locations, Lisp
`save-excursion`, hooks, and simpler compound-command undo.

**Test/docs cost:** **L**. Property-test marker adjustment over random edits,
fuzz transaction nesting/cancellation, and retain PTY outcomes for every
composite editing command.

### M2. Buffer-local mode state and hooks

**Minimum semantics:** each buffer owns a major-mode identifier/keymap,
zero or more minor-mode identifiers/keymaps, and a small typed set of local
options. Supply `before-save-hook`, `after-save-hook`, `find-file-hook`,
`after-change-functions`, and `post-command-hook`, but start with explicit
lists and budgets. Mode activation resets major-mode-local state, installs its
map/syntax/options, then runs its hook.

Do not make every Lisp symbol magically buffer-local. Expose explicit
`buffer-local-set/get` over supported option names. Do not run
`after-change-functions` once per byte; transactions define notification
granularity.

**Integration points:** move ownership out of the live/saved duplication in
`src/bufmgr.c:45-140`; separate syntax from mode identity at
`src/def.h:209-226` and `src/syntax.c:2024-2063`; replace special-mode checks
and key branches in `src/kbd.c`.

**Test/docs cost:** **L**. Mode precedence, buffer switches, multiple windows
on one buffer, hook order/error isolation/budget cancellation, and
`WITH_LISP=0` defaults all need coverage.

### M3. Richer Fe editor bridge

Build this in dependency order:

1. Fe correctness foundations: unbound sentinel, exact arity, safe macro
   expansion/printing.
2. `error` and an internal unwind/completion mechanism; then
   `unwind-protect`, `condition-case`, and `save-excursion`.
3. Editing natives: `delete-region`, `replace-region`, `looking-at`,
   `re-search-forward`, and transaction grouping.
4. Stable buffer handles/names: `current-buffer`, `get-buffer`,
   `buffer-list`, `set-buffer`, `with-current-buffer`,
   `get-buffer-create`, `kill-buffer`.
5. Minibuffer entry points: `read-string`, `completing-read`, `yes-or-no-p`.
6. Mode/key APIs: `make-keymap`, `keymap-set`, `define-derived-mode`,
   explicit buffer-local options, and hook add/remove.
7. Package hygiene: `load-path`, `provide`, `require`, `featurep`, docstrings,
   and `documentation`.

Every native that holds C resources across evaluation must wait for unwind
support. Opaque editor objects should use stable IDs with generation checks,
not pointers into `buflist`; the current 20-slot table can reuse indices.

**Test/docs cost:** **L** across Fe unit tests, kg native bridge tests, PTY
package cases, evaluation budgets/C-g, GC rooting, stale handles, and
`WITH_LISP=0`. Publish a versioned “kg Lisp API” rather than implying broad
Emacs Lisp compatibility.

### M4. One completion protocol

**Minimum semantics:** a minibuffer session owns prompt, editable UTF-8 text,
history, completion callback, candidate metadata, require-match policy, and
accept/cancel callbacks. Preserve current ido-style substring behavior as one
completion style. M-x, file, buffer, and future project pickers consume this
protocol.

**Integration points:** shared minibuffer editing/history in
`src/bufmgr.c:435-866`; path picker at `src/bufmgr.c:875-1138`; buffer picker
at `src/bufmgr.c:1310-1436`; M-x picker at `src/cmd.c:903-1068`.

**Test/docs cost:** **L** initially, then each new completion consumer becomes
**S/M**. Fuzz the session state machine; cover UTF-8 cursor edits, large
candidate counts, candidate refresh, cancellation, histories, and narrow
screens.

### M5. Navigable compilation, grep, occur, and projects

**Minimum semantics:**

- Parse common `file:line[:column]: message` output into bounded diagnostic
  records while preserving raw text.
- `next-error`/`previous-error` visit the file/location, opening another
  window when appropriate; stale locations fail clearly.
- `occur` lists literal or regexp matches from the current buffer and visits
  them.
- Project root is the nearest parent containing `.git` (with a small explicit
  override); `project-find-file` enumerates regular files with ignore and
  result caps; `project-grep` invokes a configured external grep (`rg` when
  available, POSIX grep fallback) through the bounded job layer.

**Integration points:** evolve the existing nonblocking subprocess and
polling machinery in `src/compile.c` and `src/main.c:160-170`; special buffers
already have safe append/display helpers in `src/bufmgr.c`. Locations depend
on markers/stable buffer IDs; UI depends on the completion protocol and mode
maps.

**Test/docs cost:** **L** for jobs/navigation, **M** per consumer. Use fake
process streams and temporary project trees; cover chunk boundaries, huge
output, deleted/renamed files, spaces/colons in paths, cancellation, buffer
reuse, and no-`rg` fallback. PTY screen assertions should verify selectable
results without depending on timing.

### M6. First genuinely Lisp-shaped packages

Once M1–M4 exist, deliberately prove the API with small packages:

- `whitespace-mode`: highlight/report trailing whitespace and tabs;
- `auto-fill-mode`: post-self-insert fill at a buffer-local column;
- a simple `conf-mode` or `makefile-mode` customization using declarative
  syntax, comment settings, hooks, and a local keymap;
- `dabbrev-expand`: scan open buffers for word-prefix candidates.

Each package should be possible without private C knowledge. If it is not,
fix the API seam rather than adding a one-package native. Keep syntax
highlighting declarative; do not call Lisp once per displayed byte.

## Long term: ambitious, conditional work

### L1. Decorations, diagnostics, and completion-at-point

Add a compact decoration interval API—face category, optional message, and
evaporation/stickiness—rather than Emacs' unrestricted text properties and
overlay zoo. This enables compiler/linter diagnostics, match highlighting,
whitespace display, and completion-at-point annotations. Decorations must not
be persisted as buffer text and must adjust via markers/transactions.

Then add a `completion-at-point-functions`-like hook and a small candidate
popup in the echo area or a temporary window. Start with buffer words,
filesystem paths, and Lisp symbols; language servers are not the first target.

Cost: **L** plus performance budgets and display fuzzing.

### L2. General bounded process objects

If multiple packages need asynchronous tools, generalize compilation into
process objects with command/argv, cwd, output buffer/filter, sentinel,
cancel, and byte/time budgets. Prefer direct `execvp` argv APIs; make shell
interpretation explicit. Integrate file descriptors into the single-threaded
poll loop. Do not embed a terminal emulator.

This can support async formatters, linters, VCS status, and project indexing.
Cost: **L**, with lifecycle, signal, output-cap, reentrancy, and cleanup fuzzing.

### L3. Productive structural and VCS packages

Candidates, in order:

1. balanced-expression motion and `show-paren-mode`;
2. a small Lisp structural-editing package;
3. `xref`-shaped find-definition/references backed by tags/grep;
4. a compact VC status/log/diff workflow using external Git.

Do not attempt Magit's full porcelain. kg can capture most remote-terminal
value with status, diff, visit, stage/unstage, and commit, while delegating
complex history surgery to Git.

### L4. Fe performance work only after profiling

Richer packages may expose symbol lookup, cons allocation, and repeated
string conversion costs. Measure startup, M-x completion, hook dispatch, and
large-buffer package workloads. Likely targets are symbol interning, indexed
command lookup, reusable conversion buffers, and reduced full-GC frequency.
Do not begin with bytecode or a VM rewrite. Preserve Fe's caller-owned arena
and understandable embedding model unless measurements show they are the
actual limit.

## Affordances kg should intentionally not copy

- **Full Emacs Lisp compatibility.** Keep familiar names and semantics where
  practical, but document a versioned kg Lisp dialect. Do not promise existing
  ELPA packages will run.
- **Dynamic scope as the default.** Fe's lexical closures are the better
  foundation. Add explicit buffer-local settings and unwind-safe temporary
  bindings instead.
- **The complete interactive-spec language.** A small typed subset—prefix,
  point, region, buffer, file, string, command—covers kg's needs.
- **Every variable being arbitrarily buffer-local.** Use a bounded typed option
  registry; it is easier to validate, display, and support in `WITH_LISP=0`.
- **Unrestricted text properties/overlays.** Use compact decorations with
  explicit lifetime and adjustment rules.
- **A terminal emulator, TRAMP, daemon/server ecosystem, GUI frames, package.el
  clone, byte compiler, dumping, or full Customize.** They are poor fits for a
  dependency-free remote quick editor.
- **Full Org, Magit, or LSP clients in core.** Offer composable primitives and
  small workflows; let external tools do protocol- and policy-heavy work.
- **Emacs' unbounded configurability on emergency keys.** `C-g` and a
  documented init-bypass/recovery route must remain reliable.
- **Copying historical names when current Emacs terminology is clearer.** For
  example, use `display-line-numbers-mode`, not obsolete `linum-mode`.

## Honest priority order

1. Fix the confirmed kg/Fe correctness issues that threaten buffer integrity
   or availability.
2. Build unified command descriptors, hierarchical keymaps, and generated
   introspection.
3. Add command identity, then ship the multi-entry kill ring and `M-y`.
4. Add backup/autosave recovery.
5. Centralize edit transactions and adjusting markers.
6. Add position registers/bookmarks and `transpose-words`; line numbers can
   land independently if kept display-local.
7. Consolidate minibuffer/completion.
8. Introduce buffer-owned major/minor modes, typed local options, and bounded
   hooks.
9. Strengthen Fe semantics/unwinding and expose editing, buffers, minibuffer,
   keymaps, and package loading in that order.
10. Generalize compilation just enough for diagnostic records and
    next/previous-error, then add occur, project-find-file, and project-grep.
11. Prove the extension API with two or three small Lisp packages.
12. Add decorations, completion-at-point, generalized jobs, xref/VCS, or Fe
    performance changes only when the earlier packages demonstrate the need.

The central architectural judgment is that kg should spend complexity once on
**registries, maps, transactions, markers, completion sessions, and bounded
jobs**, then buy many Emacs affordances cheaply on top. Continuing to encode
each feature as another filename/syntax check and switch branch would preserve
short-term line-count minimalism while making genuine extensibility
progressively less attainable.
