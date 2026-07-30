# Master implementation roadmap for the 2026-07-30 review

## Purpose

This directory turns the kg / Fe / tiny-regex-c review into implementable,
reviewable work. It is written for an engineer who is new to the codebase but
comfortable with C and tests.

Do not treat this as one branch or one pull request. The plans deliberately
separate:

- release-blocking local fixes;
- submodule fixes;
- verification enforcement;
- low-risk performance work;
- large ownership/transaction/registry migrations;
- product features built on those foundations.

The source review is [../FINAL.md](../FINAL.md). Each specialist report is
linked from that document.

## Mandatory workflow for every implementation

1. Read root `CLAUDE.md` (and its mirror `AGENTS.md`) and `README.md`.
2. For Fe/tiny work, also read the nested contributor guide and docs.
3. Re-read the cited functions; line numbers in reports may move.
4. Add a failing focused test before changing behavior where practical.
5. Keep one semantic change per commit.
6. Run the smallest focused test while iterating.
7. Run `make check` before handing off any root change.
8. Run `WITH_LISP=0` whenever core/editor behavior changes.
9. End a completed workstream with `.ci/run-ci-steps.sh --parallel`.
10. Update README/man/help for user-visible commands, keys, modes, or policy.
11. Never raise a complexity/coverage ratchet just to make routine growth pass.
12. Preserve unrelated worktree changes.

For submodules:

1. implement/test/document inside the submodule branch;
2. commit there;
3. update the parent pin in a separate kg commit;
4. run both standalone and kg CI.

## Global constraints every plan must budget for

These were measured during plan verification (2026-07-30) and bind *all*
plans simultaneously; each plan discovering them independently wastes a
review cycle.

- **Complexity ratchets have zero headroom.** `scc` over `src/` measures
  exactly `SCC_COMPLEXITY_MAX` (4208); the worst function
  (`editor_process_keypress`) measures exactly
  `PMCCABE_FUNCTION_COMPLEXITY_MAX` (120); Fe's total is 171 against a cap
  of 172. Adding one branch anywhere can fail `.ci/ci-01`. Plan 0 owns the
  one deliberate re-baseline; after that, phases must be complexity-neutral
  or pay for growth with extraction in the same commit. Never raise a
  ratchet inside an unrelated change.
- **kg ships `fe/tiny-regex-c/re.c` directly** (`Makefile` compiles it into
  the editor). Regex submodule changes are not "at arm's length": the pin
  chain is kg → fe (`analyzers-etc`) → tiny-regex-c (`adapt-to-fe`), and a
  tiny change reaches users through both pins.
- **Hosted CI cannot currently be trusted as a safety net.**
  `utils/pty_accept.py` pins `/opt-3/emacs-31-lucid/bin/emacs`, 17 PTY
  cases use `oracle: emacs` (missing oracle is an ERROR, not a skip), 71
  cases need tmux, and the GitHub workflow installs none of this. Land
  Plan 0 / Plan 07 phase 1a before relying on hosted results.
- **`fe/` is checked out detached** at `origin/analyzers-etc`; submodule
  work starts with `git -C fe checkout <branch>`, and every Fe divergence
  adds a row to `doc/fe-upstream.md`.

## Plan index

| Plan | Primary outcome | Relative size | Start when |
| --- | --- | --- | --- |
| [P0 — Evidence, baselines, and budgets](p0-evidence-baselines-and-budgets.md) | Characterization tests, truthful hosted `make check`, ratchet re-baseline, fresh coverage baseline | S/M | First |
| [01 — Terminal and platform hardening](01-terminal-and-platform-hardening.md) | Renderer-wide escaping, reliable TTY writes, safe geometry/ctype/input | M | After P0 |
| [02 — Editing, undo, and prefix invariants](02-editing-undo-and-prefix-invariants.md) | UTF-8 delete, C-k undo, clean history, row NUL, prefix zero | S/M | After P0 |
| [03 — Regex search/replace integration](03-regex-search-and-replace-integration.md) | Empty-match progress, truthful status, buffer-text search, one-shot replace | M | After P0 |
| [04 — Compilation, file, and Dired safety](04-compilation-file-and-dired-safety.md) | True output cap, disk identity, guarded save/delete | M/L | Immediately |
| [05 — Fe correctness and extension safety](05-fe-correctness-and-extension-safety.md) | Macro/reader/writer/API fixes, arity/unbound, Fex ownership, unwind/types | L, many small phases | Immediately in Fe |
| [06 — tiny-regex-c hardening](06-tiny-regex-engine-hardening.md) | Alignment, emitter/parser, reentrancy, honest repeats, work stack | L, many small phases | Immediately in tiny |
| [07 — Verification and invariant testing](07-verification-ci-and-invariant-testing.md) | Hosted deep CI, subproject gates, semantic fuzz/model/differential ratchets | L, incremental | CI phases immediately |
| [08 — Performance quick wins](08-performance-quick-wins-and-benchmarks.md) | Counters, capacities, row replace/splice, measured conditional work | M/L | Counters and capacities after P0 fixes |
| [09 — Session/buffer/view ownership](09-session-buffer-view-ownership.md) | One state owner, stable handles, no swap/copy protocols | L | After Plan 02 and P0 characterization tests |
| [10 — Transactions, markers, and hooks](10-edit-transactions-markers-and-deferred-hooks.md) | Failure-atomic edits, undo groups, markers, decorations, safe events | L | After buffer ownership |
| [11 — Command/keymap/mode/etc. registries](11-command-keymap-mode-variable-and-completion-registries.md) | Authoritative commands, layered keys, modes, variables, completion | L | Command metadata now; modes after ownership |
| [12 — Runtime/process/Lisp extensibility](12-runtime-process-and-lisp-extensibility.md) | Stable optional Fe adapter, callbacks, buffers, processes, packages | L | After handles/transactions/registries |
| [13 — Emacs affordances](13-emacs-affordances-delivery.md) | Kill ring, recovery, registers, diagnostics, project navigation, packages | Incremental | Per feature dependencies |
| [14 — Coordinate-space invariants](14-coordinate-space-invariants.md) | One audited chars/render/columns policy; fix render-indexed state | M | After Plan 03; feeds 09/10 |
| [15 — Structural and toolchain hygiene](15-structural-and-toolchain-hygiene.md) | Header decomposition, docs coherence, tool discoverability, dead-infra cleanup | S/M, incremental | Anytime; before Plans 09–13 add modules |

Relative size is risk/surface, not a calendar estimate.

## Issue coverage matrix

Each finding has one primary plan. Related plans may consume its result.

| Review finding | Primary plan/phase | Required regression |
| --- | --- | --- |
| Terminal control injection | 01 phases 1–2 | Raw OSC/CSI absent from buffer, mode line, Dired, status |
| `HL_NONPRINT` caret renderer emits raw bytes (incl. ESC) for non-ASCII | 01 phases 1–2, 5 | C-syntax buffer with UTF-8 renders as text, no raw control bytes |
| Short/EINTR TTY writes | 01 phase 3 (route `tty_write` through existing `write_all`, do not duplicate it) | Injected short write/EINTR completes exact frame |
| Invalid/tiny geometry | 01 phase 4 | parser unit + 1x1/tiny resize PTYs |
| Negative-char ctype UB | 01 phase 5 | `-fsigned-char` vs `-funsigned-char` syntax parity (kg never calls `setlocale`; locale lanes test nothing) |
| Malformed UTF-8 consumes next key | 01 phase 6 | `E2 41` preserves `A` |
| UTF-8 Backspace/Delete corruption | 02 phase 1 | 2/3/4-byte delete + one undo |
| EOL C-k wrong undo | 02 phase 2 | `a\nb -> ab -> undo -> a\nb` |
| Undo eviction false clean | 02 phase 3 | max-size-2 saved snapshot sequence |
| Row slice not NUL-terminated | 02 phase 4 | rectangle/reflow undo + row assertions |
| Explicit prefix zero lost | 02 phase 5 (fix is *removing* `cx_prefix_arg`, not adding a struct) | `C-u 0` across C-x/C-c/M-x/macro |
| Zero-width regexp loop/growth | 03 phases 1–2 | `a*` before `å`, `!`/`n`, timeout |
| Matcher exhaustion swallowed | 03 phase 3 | UI/wrapper exposes too-complex |
| Regexp isearch uses rendered tabs | 03 phase 4 | tab vs eight spaces |
| Regexp isearch leaves point in render coordinates | 03 phase 4 | tab-prefixed line: search, then type at match |
| Byte-at-a-time query replace | 08 phase 5 owns the primitive; 03 phase 5 is its first consumer | one update/one undo per match |
| Compilation cap bypasses | 04 phases 1–2 | tiny cap, newline-only, huge line, retained newlines charged |
| Weak disk change detection | 04 phases 3–5 | same metadata, delete, replace-before-rename |
| Write-file overwrite collision | 04 phase 4 | *any* existing destination prompts with destination-exists wording before the name is adopted |
| Insert-file size/error types | 04 phase 6 | EINTR/short/error/overflow injection |
| Dired deletion TOCTOU | 04 phase 7 | replace target after confirm |
| FeToString size zero | 05 phase 1 | ASan `(NULL,0)`/redzone |
| Fe malformed dotted lists | 05 phase 2 | valid and malformed reader matrix |
| Fe destructive macro expansion | 05 phase 3 | nil truth, lexical symbol, redefinition |
| Fe cyclic/unbounded writer | 05 phase 4 | cdr/car cycles, limits, C-g |
| Fe lax arity | 05 phase 5 (hard prerequisite: Fe binder learns `&optional`/`&rest` and the pin lands, or strict arity breaks kg's `internal--arglist` lowering and zero-arg command calls) | required/optional/rest too few/many |
| Fe unbound == nil | 05 phase 6 | boundp/defvar/void-variable |
| Fex FILE lifecycle | 05 phase 7 | double close, post-close, GC/fd limit |
| Fex truncation/process gaps | 05 phase 8 | path/argv/write boundaries and EINTR |
| Fe cleanup/unwind/types | 05 phases 9–10 | error-injection and multi-context types |
| Regex alignment UB | 06 phase 1 | misaligned caller under UBSan |
| Regex compiler cursor drift | 06 phase 2 | invalid-hex + following/nested group |
| Malformed regex accepted | 06 phase 3 | exact supported-subset acceptance |
| Regex global execution state | 06 phase 4 | TSan/reentrant independent budgets |
| >256 grouped repeat false no-match | 06 phase 5 | 255/256/257/300 + too-complex, plus empty-body `\(a*\)\{300\}` non-regression (matches today; Emacs agrees) |
| Regex stack/work accounting | 06 phase 6 | small stack/adversarial bounded exit |
| Hosted `make check` is not truthful (pinned `/opt-3` emacs, missing tmux/pexpect) | P0 / 07 phase 1a | oracle/tool discovery with loud failure |
| Hosted CI omits deep runner | 07 phase 1 | deliberate gate failures caught |
| Root omits subproject suites | 07 phase 2 | deliberate Fex/tiny failures caught |
| Coverage has no ratchet and an unrepresentative file set; complexity ratchets are exact but aggregate and per-symbol-blind | 07 phases 3–5 | generated exact baselines |
| Regex differential is narrow (first forward match only), fuzz smoke shallow/unseeded | 07 phases 6–8 | stateful invariants/full traces |
| Missing mutation/portability tracking | 07 phases 9–11 | pilot baseline + verified lanes |
| File-load row O(R²) growth | 08 phase 2 | logarithmic realloc counter |
| Live row-array growth (`editor_insert_row`) on compile/shell output | 08 phase 2b | logarithmic realloc counter under appended output |
| Compilation mirroring/truncation cost | 08 phase 5b | bounded work per poll |
| Screen abuf exact growth | 08 phase 3 | growth/copy counters, identical frame |
| Row per-byte rebuilds | 08 phases 4–5 | one update per logical replacement |
| Whole-buffer multiline insert | 08 phase 6 | local splice + OOM atomicity |
| Undo tail walk | 08 phase 7 | O(1) eviction counters |
| Visual-line full scans | 08 phase 8 | row cache counters; tree conditional |
| Syntax propagation/allocation | 08 phase 9 | reuse/counters; slicing conditional |
| Fe/regex conditional performance | 08 phases 10–11 | real workloads before redesign |
| Ambiguous editor/buffer/window ownership | 09 | native state model + no copy helpers |
| Distributed mutation invariants | 10 | reference model/OOM injection |
| Duplicate command/key/mode policy (three copies: `CMD_EDITS_BUFFER`, Lisp allow-list, `readonly_blocked_keys[]`; plus a live gap — Lisp-defined commands bypass read-only checks) | 11 phases 1–5 | binding parity and table invariants |
| Duplicated local-variable semantics | 11 phase 6 | same typed-value matrix across syntaxes |
| Duplicated picker/minibuffer loops | 11 phase 7 | picker parity + state fuzzer |
| Narrow kg Lisp surface | 12 phases 1–8 | stable handles, safe callbacks, API tests |
| Three duplicated `fork`/`execl` sites; only compilation sets `setpgid` | 12 phases 9–10 (dedup + `setpgid` first, concurrency only with a second client) | two concurrent/reaped/capped processes |
| Missing Emacs affordances | 13 | per-bundle unit/PTY/docs/full CI |
| Mixed chars/render/display coordinate producers and render-indexed state (`saved_hl`, reveal offsets) | 14 | audit table + `RESTORE_HL` bounds regression |
| `def.h` monolith, `/opt-3` tool pins, doc drift, rotted tiny-regex infra (`make verify`, unbuilt tests) | 15 | headers compile standalone; tools discoverable; `make -C fe/tiny-regex-c verify` builds |

## Dependency graph

```text
Release fixes: 01, 02, 03, 04
        │
        ├──────────────> 07 hosted enforcement/invariant tests
        │
        ├──────────────> 08 counters + local capacity/range wins
        │
        └──────────────> 09 single state ownership
                                  │
                                  v
                         10 edit transactions/markers/events
                                  │
                   ┌──────────────┴──────────────┐
                   v                             v
        11 commands/keymaps/modes       12 runtime/process adapter
                   │                             │
                   └──────────────┬──────────────┘
                                  v
                         13 Emacs affordances

Fe 05 and regex 06 run in their submodules in parallel.
Their pinned results feed 03 and 12.
```

Refinements to the graph:

- Plan 09 waits for Plan 02 (it relocates undo clean-state and `C-k`
  records that Plan 02 changes) as well as P0 characterization tests.
- The `09 → 10` edge is really `09 phases 2–4 → 10`; Plan 09 phases 5–7
  (view, session nesting, lifecycle) can run concurrently with Plan 10
  phases 1–3.
- Plan 10 benefits from Plan 08 phase 5 (`editor_row_replace_range`)
  landing first; Plan 08 phase 7 (undo deque) is parallel-safe.
- Kill ring/`M-y` and `transpose-words` (Plan 13 bundle A) are blocked on
  Plan 11 phase 3 (keymap trie): the flat key enum has no `ALT_Y`/`ALT_T`
  and `editor_process_keypress` sits exactly at the pmccabe ratchet, so
  new `case` arms cannot be added to the current dispatcher.

## Recommended delivery waves

### Wave 0 — Preserve evidence (now owned by [Plan P0](p0-evidence-baselines-and-budgets.md))

Before fixes:

- add minimal regressions for the P0 findings;
- add buffer/window characterization PTY cases (`C-x b`, `C-x C-b`,
  `C-x k`, `C-x 0`, `C-x 1`, two windows on one buffer) — today the suite
  has one case each for `C-x 2`/`C-x 3`/`C-x o` and none for the rest,
  which is the only net under the Plan 09 flag days;
- make hosted `make check` truthful (oracle/tool discovery) before adding
  a deep-CI job on top of it;
- capture fresh coverage/complexity/test-time baselines (the checked-in
  coverage artifact covers 20 of 28 `src` files — regenerate, do not quote);
- take the one deliberate complexity-ratchet re-baseline with headroom;
- do not add broad new tools yet.

Parallel tracks:

- kg tests/fixes;
- Fe API/reader/macro tests;
- tiny alignment/emitter/status tests;
- CI integration.

### Wave 1 — Release hardening

Suggested root commit order:

1. terminal escaping;
2. UTF-8 Backspace/Delete;
3. zero-width regexp progress;
4. EOL C-k undo;
5. compilation total budget;
6. undo clean checkpoint;
7. disk snapshot/change enum;
8. write-file collision;
9. row NUL/prefix zero/ctype/read/tty focused fixes.

Items 7 and 8 ship together (or 8 first): the three-state disk enum
changes what `autorevert_poll()` decides, and shipping it without the
destination-exists prompt widens the write-file window it is meant to
close.

Do not bundle them into one “review fixes” commit.

### Wave 2 — Submodule hardening

Fe (ranked by kg exposure — phases with zero kg consumers come last):

1. serializer zero-size;
2. dotted reader;
3. macro expansion;
4. bounded writer — **promoted**: `(setcdr x x)` then `M-:` is a
   reachable, un-interruptible kg hang today, not a standalone-Fe wart;
5. arity/unbound design and compatibility — binder support for
   `&optional`/`&rest` must land and be pinned *before* strict arity;
6. Fex ownership/truncation (standalone Fe only; kg compiles just `fe.c`);
7. unwind/type design (standalone Fe today; enables kg callbacks later).

tiny:

1. storage alignment;
2. emitter;
3. parser states;
4. per-execution context;
5. honest group ceiling;
6. explicit work stack.

Advance pins only after standalone green.

### Wave 3 — Local seams and performance

- performance counters;
- geometric file/screen/row capacities;
- row range replacement;
- undo deque;
- authoritative command metadata (Plan 11 phases 1, 2, 6, 7 need no
  ownership refactor and belong here);
- typed local-variable application;
- coordinate-space audit (Plan 14) and header/docs hygiene (Plan 15).

These changes reduce risk/complexity before large ownership work.

### Wave 4 — Ownership

Execute Plan 09 alone:

- characterization harness;
- handles/generations;
- buffer ownership;
- view ownership;
- delete partial/full swapping;
- buffer-targeted background updates.

No user feature in this wave.

### Wave 5 — Transactions and registries

Parallel only after interfaces stabilize:

- Plan 10 transaction/marker core;
- Plan 11 command/keymap groundwork.

Modes/variables need buffer ownership. Hooks need transactions and safe event
points.

### Wave 6 — Runtime and product

- Fe controlled callbacks/unwind;
- stable editor objects;
- buffer/edit/search/minibuffer APIs;
- process manager only with a second client;
- Emacs feature bundles in Plan 13.

Prove the runtime with first-party packages before expanding object types or
package surface further.

## Cross-plan design decisions

### Text and positions

- internal editor mutation/regex spans: bytes;
- Lisp API: 1-based codepoint positions;
- display: terminal columns;
- newline counts as one logical position;
- malformed UTF-8 policy must be documented before marker/search APIs freeze.

### Failure atomicity

- allocate undo/replacement/row state before publishing edits;
- save temp files before guarded rename;
- runtime callbacks occur after commit;
- submodule resource cleanup is exactly once;
- “unknown” state is never treated as safe/same.

### Boundedness

Bound:

- compilation retained/pending bytes;
- kill ring entries/bytes;
- diagnostics/results/candidates;
- event queues;
- runtime evaluation;
- regex work;
- process output;
- autosave artifacts.

Limits must report truncation/exhaustion distinctly from empty/no-match.

### Optional Lisp

- core owns buffers, modes, variables, keymaps, markers, processes;
- Fe adapter owns Fe roots and conversions;
- no Fe types in core headers;
- no core behavior implemented only in Lisp unless explicitly optional;
- every architecture phase tests `WITH_LISP=0`.

## Work that remains conditional

Do not schedule these until counters/prototypes justify them:

- rope/gap/balanced text store;
- balanced visual prefix tree;
- terminal diff renderer;
- moving/generational Fe GC;
- variable-size Fe object heap;
- Fe bytecode VM;
- Thompson/Pike regex VM;
- threads;
- C `dlopen` ABI;
- full Emacs Lisp/ELPA compatibility.

Each needs a separate design document with measured motivation.

Identified during plan verification, also needing their own design
documents before scheduling (none is covered by Plans P0–15):

- buffer-slot lifecycle policy (`MAX_BUFFERS` 20 fixed slots, no reuse
  policy, retained indices in `compilation_state` racy against slot
  reuse — Plan 09's handles make failure safe, not good);
- rendering/damage design (`editor_refresh_screen()` redraws everything;
  visual-line mode mutates `editor.cx`/`coloff` *during* rendering);
- asynchronous buffer mutation ownership (`autorevert_poll()` and
  `compilation_poll()` both mutate buffers from inside the input loop,
  including while minibuffer prompts are open);
- iterative Fe GC marking (`FeMark`'s recursive `car` walk is the same
  stack-exhaustion shape as the writer's, in the collector, where it can
  corrupt state rather than merely hang — documented as a known Fe issue).

## Pull request template for these plans

Every PR description should answer:

1. Which plan and phase?
2. Which invariant changes?
3. What failing test existed first?
4. What files/functions changed?
5. What is explicitly out of scope?
6. What OOM/error/cancel path was tested?
7. Does it affect `WITH_LISP=0`?
8. Does it require a Fe/tiny pin?
9. Did user-visible docs/help change?
10. Which focused and full checks passed?
11. Did complexity/coverage improve, stay level, or need reviewed exception?
12. What follow-up is now unblocked?

## Definition of complete

The review program is complete when:

- all P0/P1 confirmed defects have focused regressions and fixes;
- hosted CI runs the deep and standalone suites;
- editor state and mutation have one owner/gateway;
- command/mode/variable/keymap metadata is authoritative;
- runtime callbacks use stable handles and safe points;
- at least two first-party Lisp packages prove the extension surface;
- the first feature bundles ship with both Lisp configurations green;
- larger rewrites remain supported by measured evidence, not assumption.
