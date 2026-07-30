# kg / Fe / tiny-regex-c review: final triage

Date: 2026-07-30
Reviewed tree: kg `906e48f`, Fe `0ca1229`, tiny-regex-c `de5f96a`
Scope: correctness, deduplication, architecture, ergonomics, verification,
Emacs affordances, portability/security, and performance.

## Executive verdict

kg is already much more disciplined than its size suggests. The native and
PTY suites are broad, the local ten-stage CI runner is serious, Fe has an
unusually explicit embedding/GC contract, and the regex engine has sanitizer,
fuzz, and Emacs differential infrastructure.

The review nevertheless found several ordinary, user-reachable defects that
should outrank new features:

1. untrusted buffer/filename bytes can be emitted as terminal control
   sequences;
2. Backspace and forward-delete corrupt multibyte UTF-8;
3. zero-width regexp replace-all can loop forever or grow memory on UTF-8;
4. undo after `C-k` at end-of-line restores the wrong bytes;
5. asynchronous compilation's advertised output cap is bypassable;
6. undo-stack eviction can later mark modified contents clean;
7. disk-change detection can silently miss replacement/deletion;
8. Fe has reproduced macro, reader, serializer, and cyclic-writer defects;
9. tiny-regex-c can silently report no-match for valid patterns or exhausted
   matching.

My architectural recommendation is deliberately narrower than “rewrite kg
around Lisp”:

- keep the editor core independent of Fe and preserve `WITH_LISP=0`;
- give session, buffer, and view/window state exactly one owner;
- establish one failure-atomic replace-range/edit transaction;
- unify command policy before building hierarchical keymaps and real modes;
- build markers and deferred hooks on the transaction boundary;
- generalize the existing poll-based compilation process only when a second
  client needs it;
- improve Fe selectively: correct macro expansion, unbound values, arity,
  cleanup/unwind, then per-context extension types.

Do not start with a rope, bytecode VM, moving GC, terminal diff renderer, full
regex VM, `dlopen` ABI, or broad Emacs Lisp compatibility. The evidence does
not justify those investments yet.

## Method and confidence

Eleven bounded specialist passes ran in parallel waves. Each produced a
separate Markdown report; a final skeptical pass cross-checked reports 01–08
against the code and downgraded unsupported urgency claims.

Findings below use these confidence levels:

- **Reproduced:** executed against the current tree.
- **Source-proved:** the data/control flow establishes the failure.
- **Confirmed gap:** an observable API/test/CI omission, without claiming a
  current failure.
- **Conditional:** plausible architectural or performance work that needs
  measurement.

No product code was changed. Only review reports were added.

Detailed implementation work is broken down in the
[implementation-plan master roadmap](plans/00-master-roadmap.md).

## Priority 0: fix before ambitious feature work

| Order | Finding | Confidence | Why it is first |
| ---: | --- | --- | --- |
| 1 | Escape all untrusted terminal text | Source-proved | Extensionless contents, filenames, Dired entries, and status messages can inject CSI/OSC, including clipboard-affecting OSC 52. |
| 2 | Make Backspace/Delete glyph-granular | Source-proved | Normal editing deletes one byte from a multibyte glyph and records incomplete undo. |
| 3 | Guarantee regexp iterator progress | Source-proved | Empty matches advance one byte, then span snapping can move back to the same UTF-8 glyph; `!` reads no more input. |
| 4 | Correct EOL `kill-line` undo | Source-proved | `a\nb` becomes `ab`, then undo produces `abb`. |
| 5 | Enforce one compilation-output byte budget | Source-proved | A long unterminated line and newline-only output bypass the 8 MiB retention cap. |
| 6 | Invalidate unreachable undo clean checkpoints | Source-proved | Eviction retains a stale count; a later undo can call modified text clean. |
| 7 | Strengthen disk identity/change checks | Source-proved | Missing files, stat errors, and same-size/same-second rewrites are treated as unchanged. |

### Recommended patch shape

Keep these fixes small and independent:

1. Make terminal escaping a renderer-wide invariant. Ordinary strings are
   always escaped; trusted styling uses typed internal markup rather than raw
   ANSI embedded in status text. Cover contents, mode lines, Dired, and echo
   messages.
2. Use `utf8_glyph_start_before()` / `utf8_glyph_span_at()` for deletion and
   record the full span in undo. Do not wait for the transaction refactor.
3. Normalize start offsets to glyph boundaries and share one “advance after
   empty match” helper across forward, backward, and replacement iteration.
   Add timeout-backed PTY tests for empty and nonempty replacements.
4. Record `"\n"` for the EOL `C-k` undo half-step—the same bytes removed and
   put in the kill ring.
5. Charge pending bytes, committed bytes, and newlines to one compilation
   budget; stop retaining while continuing to drain/reap safely.
6. Set the undo clean checkpoint unreachable when its history is evicted.
   Ultimately replace count equality with a stable history/checkpoint identity.
7. Represent disk comparison as `same`, `different`, or `unknown`; store
   nanosecond mtime plus device/inode/size, and treat disappearance/errors as
   conflicts. Check an existing `write-file` destination before adopting its
   name.

## Priority 1: subproject correctness and bounded hardening

### Fe core

Fix in this order:

1. **`FeToString(..., size == 0)`**: `size - 1` underflows and the function
   writes through `dst`. This is an immediate public-API fix, although current
   kg call sites pass nonzero buffers.
2. **Malformed dotted lists**: reject leading, repeated, missing-tail, and
   trailing forms. Current Fe rewrites `'(a . b c)` as `(a c)`.
3. **Destructive macro expansion**: evaluate the rooted expansion directly.
   Shallow-copying an atom into the call-site pair creates a truthy fake
   `nil` and breaks lexical symbol identity.
4. **Bounded, cycle-safe printing**: cyclic conses currently produce unbounded
   output and bypass Fe's evaluation budget; recursive `car` printing also
   risks the C stack.
5. **Exact lambda/macro arity and an internal unbound sentinel**: these unlock
   correct `defvar`, `boundp`, useful `void-variable` errors, optional/rest
   arguments, and better diagnostics. Treat this as a versioned semantic
   migration because existing scripts may rely on lax arity or nil-for-unbound.

### Fe extensions

Standalone Fe's Fex layer needs its own hardening even though kg does not
compile it:

- replace raw `FILE *` payloads with owned/closed handle objects;
- prevent double-close/use-after-close and close unreachable owned files;
- distinguish standard streams from owned files;
- stop silently truncating 1024-byte process arguments, 31+ argument lists,
  paths, and 4 MiB writes;
- retry `waitpid()` on `EINTR` and distinguish EOF from I/O error.

### tiny-regex-c and kg's wrapper

1. Expose matcher exhaustion as “regexp too complex,” never “no match.”
2. Report the consuming-group repetition ceiling honestly, then replace the
   256-frame limit with explicit repetition state. The engine accepts
   `\(a\)\{300\}` but currently returns no-match on 300 `a` bytes.
3. Reject malformed groups/classes and define the supported Emacs subset
   precisely.
4. Replace the compiler's split physical/logical output cursors with one
   emitter. The invalid-`\x` fallback already desynchronizes them.
5. Move step/depth counters into the per-execution context; deprecate or make
   explicit the process-global `re_compile()` buffer.
6. Fix the public storage alignment contract. `unsigned char *` is cast to a
   typed program containing `unsigned short`; unaligned valid callers invoke
   undefined behavior.
7. Keep compiled programs immutable and add byte-length APIs before exposing
   regex broadly to Lisp.

### Small kg hardening

- Make `editor_insert_row()` copy exactly `len` bytes and write its own NUL.
- Preserve `{supplied, value}` across `C-x`; explicit prefix zero is currently
  confused with no prefix.
- Cast every `<ctype.h>` input to `unsigned char`, or use locale-independent
  ASCII predicates for syntax.
- Use `ssize_t` for `read()` results and checked `size_t` growth.
- Clamp terminal dimensions at the input boundary and loop on short/EINTR tty
  writes.
- Revalidate Dired deletion targets in shared writable directories.

## Architecture: the smallest useful overhaul

The current coupling is visible in three manual copies of overlapping state:
`editor_config`, `editor_buffer`, and `editor_window`. Text mutations also
independently coordinate storage, undo, dirty state, syntax, and rendering
across many modules.

The recommended dependency direction is:

```text
                         kg_session
             ┌──────────────┼───────────────┐
             v              v               v
      command registry   event loop    runtime adapter
             │              │          ┌────┴────┐
             v              v          │ no Lisp │
       layered keymaps   processes     │ Fe      │
             │                         └────┬────┘
             v                              │
       mode registry                    stable handles
             │                              │
          kg_view ───────> kg_buffer <──────┘
                                │
                     replace-range transaction
                       ┌────────┼─────────┐
                       v        v         v
                     undo    markers   invalidation
                                          │
                                   syntax/display/hooks

      search ──> kg regex service ──> private tiny-regex-c
```

### Phase A: local seams now

- Add a checked row/buffer replace-range primitive that reserves storage
  before mutation.
- Make the existing command descriptor authoritative for edit/read-only
  policy, Lisp-callability, prefix behavior, and short documentation.
- Give staged file rows, row buffers, render/highlight buffers, and screen
  `abuf` geometric capacity.
- Give undo a tail/deque rather than walking ~1000 nodes for every eviction.
- Centralize local-variable value decoding/application while keeping the
  three envelope parsers separate.

These are useful without committing to the full architecture.

### Phase B: one state owner

Split:

- `kg_session`: terminal, minibuffer, command loop, global options, kill ring,
  registries, jobs, runtime adapter;
- `kg_buffer`: stable ID/generation, text, filename, dirty/undo, marks, local
  variables, modes and future decorations;
- `kg_view`: point, goal column, selection presentation, viewport;
- `kg_window`: layout plus one view.

Eliminate save/restore ownership copies. Two windows on one buffer share text
and undo but retain independent point/scroll. Introduce stable IDs/generations
before Lisp, diagnostics, jobs, or queued hooks retain references.

### Phase C: failure-atomic edit transactions

Every edit—ordinary typing, undo, rectangle, reflow, query replace, Lisp,
shell replacement, and process output—must go through one transaction that:

1. validates bounds/read-only;
2. reserves undo and replacement storage;
3. commits text atomically;
4. relocates markers;
5. invalidates syntax/render/decorations;
6. changes dirty/content generation once;
7. queues one typed change event.

Do not call Lisp inside a partial edit, renderer, signal handler, or minibuffer
internal. Drain bounded callbacks at documented top-level safe points.

### Phase D: commands, keymaps, modes, variables

After command metadata is authoritative:

- use layered keymaps with explicit precedence:
  transient > minor modes > major mode > global;
- separate mode identity/activation/keymap/options from syntax highlighting;
- migrate Dired, compilation, Git commit/rebase, and Lisp interaction first;
- use a typed C variable registry for global/buffer-local/file-local-safe
  options;
- keep emergency recovery keys and init bypass reliable.

### Phase E: markers, hooks, processes, runtime

- Build markers and compact decorations on edit transactions.
- Add typed deferred hooks with evaluation budgets and C-g cancellation.
- Generalize the existing poll-based compilation lifecycle into a bounded
  process table only when a second concrete client needs concurrency.
- Keep tiny-regex-c private behind kg's normalized, byte-counted service.
- Fe remains an adapter; only `src/lisp.c` includes `fe.h`, and all C-owned
  editor facilities continue to work under `WITH_LISP=0`.

### Phase F: selective Fe evolution

After the editor surface is stable:

1. add a controlled-call API for rooted callbacks;
2. introduce a unified completion/unwind design covering Lisp conditions and
   C-resource cleanup;
3. add `error`, `unwind-protect`, `condition-case`, `catch`/`throw`, and quit;
4. replace global `FeTFexN` slots with per-context registered type/native
   descriptors;
5. add vectors, hash tables, `provide`/`require`, and richer metadata only
   when first-party packages demonstrate the need.

Do not implement two unrelated cleanup/unwind stacks without specifying their
ordering and ownership.

## Deduplication and developer ergonomics

Highest-value concrete reductions:

1. one regex compiler emitter instead of physical pointer + logical index;
2. one command policy source instead of C table + Lisp allow-list + key/help
   metadata drift;
3. one local-variable typed-value decoder/application policy;
4. composable test doubles instead of four broad stub variants and weak no-op
   dependencies;
5. table-driven uniform Fe numeric adapters with exact arity;
6. one buffer state owner instead of full and partial swap protocols;
7. one edit transaction instead of roughly 45 direct undo call sites and
   independent dirty/render maintenance.

The first five can be incremental. The last two are architectural migrations
and should not be mixed into an unrelated feature.

## Performance: proven wins versus conditional rewrites

### Low-risk, source-justified

- Staged file loading reallocates the row array exactly once per line:
  worst-case quadratic metadata copying. Use geometric capacity.
- Row insertion/deletion reallocates exact character storage and rebuilds the
  entire render/syntax row. Add capacities and one range replacement.
- Query replacement performs byte-at-a-time delete/insert and repeated row
  rebuilds. Replace once per logical match.
- `ab_append()` reallocates to exact size for every screen fragment. Add
  capacity; keep full repaint until measurement says otherwise.
- Multiline insertion serializes and reconstructs the whole buffer. Splice
  rows locally after the mutation seam exists.
- Undo eviction traverses the whole bounded list. Add a tail/deque.
- Precompute syntax keyword lengths and reuse highlight storage.

### Measure before redesign

- Visual-line mode scans prefixes/full buffers several times per repaint.
  Start with counters and per-row wrap-width caching; add prefix trees only if
  large-buffer latency remains.
- Fe symbol interning is quadratic in distinct symbols and GC sweeps the full
  fixed arena. Add allocation/GC/symbol/environment counters and benchmark
  real packages before changing the collector or object layout.
- Backward regex search can rescan suffixes quadratically and the compiled
  stream repeatedly resolves structure. Benchmark subject/node visits, then
  consider resolved jumps and a resumable iterator.
- Only consider a balanced line tree, terminal diff renderer, Fe bytecode or
  generational GC, variable-sized object heap, or Thompson/Pike regex VM after
  simpler changes and stable profiles identify the limit.

## Verification roadmap

The largest immediate quality gap is enforcement, not absence of tools.
GitHub Actions currently runs only `make` and `make check`; the local
ten-stage runner is not invoked. Root CI also does not run standalone Fe/Fex
and tiny-regex-c suites.

### First

1. Add a hosted quality job for `.ci/run-ci-steps.sh --parallel`.
2. Add a numbered root stage for `make -C fe check` and
   `make -C fe/tiny-regex-c check`.
3. Pin and print tool/compiler versions; upload per-stage logs on failure.
4. Make missing tools fail visibly rather than silently weakening a gate.

### Then

- Establish line/function/branch and per-file coverage baselines from fresh
  hosted runs. The existing local artifact reports 55.3% lines and 68.2%
  functions for 20 `src` files, but treat that as a snapshot, not a timeless
  threshold.
- Replace aggregate complexity ceilings with per-symbol no-increase
  manifests; the current largest functions deserve monotonic reduction rather
  than merely a ceiling of 120.
- Change fuzz smoke from tiny run counts to short time budgets.
- Add invariant-bearing stateful targets for:
  - search/replace iteration and cancellation;
  - buffer/undo and UTF-8 edit sequences;
  - buffer/window ownership transitions;
  - Fe tiny-arena forced GC, roots, recovery, cyclic graphs, and cleanup;
  - regex forward/backward/empty-match operation traces.
- Compare regex compile status and complete operation sequences against Emacs,
  not only the first valid forward match.
- Add exhaustive small-state/property models before adopting another random
  framework.
- After stable baselines, add changed-function mutation testing, scheduled
  long fuzz/differential campaigns, and targeted portability lanes
  (musl/FreeBSD/32-bit/big-endian/locale/terminal canaries).

Track test duration/flakiness, coverage, complexity, analyzer fingerprints,
fuzzer corpus/features/timeouts, mutation score, binary size, and selected
benchmarks in machine-readable artifacts. Do not begin by adding a long list
of fashionable analyzers.

## Productive Emacs affordances

### Can land with limited architectural dependency

1. multi-entry bounded kill ring and `M-y`, after adding small command identity;
2. backup-on-save and autosave recovery, after fixing disk identity/conflict
   semantics;
3. `transpose-words`;
4. `display-line-numbers-mode`;
5. basic immutable `next-error` navigation on current compilation output.

### Become cheap after command/keymap work

- generated `describe-key`, `describe-command`, `describe-bindings`, and
  `where-is`;
- composable major/minor mode maps;
- reliable Lisp/user rebinding beyond the current fixed `C-c <key>` table;
- generated help and command documentation.

### Depend on edit transactions and markers

- position registers and persistent bookmarks;
- locations that track edits;
- compact decorations/diagnostics;
- `save-excursion`;
- `occur` result navigation;
- completion-at-point annotations.

### Depend on completion and process services

- navigable compilation with next/previous error;
- project-find-file;
- project-grep with `rg`/grep fallback;
- bounded async formatters/linters;
- small xref- and VC-shaped workflows.

### Prove the Lisp surface with first-party packages

After modes, typed options, hooks, buffers, minibuffer entry points, and safe
callbacks exist, ship two or three small packages such as whitespace mode,
auto-fill mode, a declarative config mode, or dabbrev. If a package requires
private C knowledge, improve the seam rather than adding a package-specific
native.

Intentionally omit full ELPA compatibility, dynamic scope by default,
unrestricted text properties, terminal emulation, TRAMP, a package.el clone,
full Org/Magit/LSP in core, and freely rebindable emergency keys.

## Dependency-aware delivery order

### Release-hardening wave

1. terminal escaping;
2. UTF-8 deletion;
3. zero-width regexp progress;
4. EOL kill-line undo;
5. compilation byte budget;
6. undo clean-checkpoint eviction;
7. disk identity/write-file collision;
8. row NUL, prefix zero, ctype/read/tty hardening.

### Submodule-hardening wave

1. Fe zero-size serializer and dotted reader;
2. Fe macro expansion and bounded/cycle-safe writer;
3. Fe arity/unbound compatibility design;
4. Fex file/resource and truncation fixes;
5. regex status/group ceiling/parser/emitter/alignment/reentrancy fixes;
6. advance submodule pins only after standalone docs/tests/CI pass.

### Architecture-enabling wave

1. local replace-range and authoritative command metadata;
2. capacity/reuse performance wins and benchmark counters;
3. single session/buffer/view ownership with stable IDs;
4. failure-atomic transactions and marker relocation;
5. variable/mode/keymap registries;
6. deferred hooks and runtime handles;
7. bounded process service when demanded;
8. selective Fe unwind/type/package semantics.

### Product wave

1. kill ring/yank-pop, save recovery, transpose words, line numbers;
2. command introspection and generated help;
3. registers/bookmarks and navigable diagnostics;
4. completion consolidation, occur, project find/grep;
5. first-party Lisp modes/packages;
6. decorations, process-backed packages, xref/VC only after measurement and
   real use.

## Validation performed

All existing tests remain green:

- `make check`: 17/17 native suites and 224/224 PTY cases passed.
- `make -C fe check`: core/header/API/example and all script goldens passed
  with GCC/Clang checks.
- `make -C fe/tiny-regex-c check`: hand-picked, randomized Python comparison,
  compiler, API, and pathological cases passed; three pre-existing private
  layout checks remain expected failures.
- A specialist ran 20,000 seeded in-grammar regex/Emacs differential cases:
  zero divergences. The report explains why that grammar cannot reach the
  confirmed group-count, malformed-pattern, offset, and iteration failures.
- The root reviewer independently reproduced:
  - Fe macro expansion yielding a truthy fake `nil`;
  - Fe malformed dotted-list rewriting;
  - no-match for a valid 300-repeat consuming group;
  - the invalid-`\x`/following-group compiler failure.

The green baseline is not contradictory. The strongest findings are precisely
state combinations and trust boundaries the current suite does not model.

## Specialist reports

1. [kg correctness](01-kg-correctness.md)
2. [Fe architecture](02-fe-architecture.md)
3. [regex correctness and performance](03-regex-correctness-performance.md)
4. [deduplication and developer ergonomics](04-dedup-developer-ergonomics.md)
5. [quality, static analysis, and fuzzing](05-quality-static-analysis-fuzzing.md)
6. [Emacs affordances roadmap](06-emacs-affordances-roadmap.md)
7. [performance and scalability](07-performance-scalability.md)
8. [cross-component extensibility architecture](08-cross-component-extensibility-architecture.md)
9. [portability, security, and reliability](09-portability-security-reliability.md)
10. [test invariants and gap analysis](10-test-invariants-and-gap-analysis.md)
11. [independent skeptical triage](11-independent-skeptical-triage.md)

## Bottom line

The first work should be boring in the best sense: escape terminal data, stop
corrupting UTF-8 and undo state, make regex iteration progress, enforce output
and file-conflict contracts, and repair Fe/regex truthfulness. Connect the
excellent local gates to hosted CI while those fixes land.

Then spend architecture once on ownership, transactions, registries, markers,
and safe callback points. That path improves the C editor immediately, keeps
`WITH_LISP=0` healthy, and gives Fe a stable editor API to extend. It also buys
the most productive Emacs affordances without committing kg to becoming GNU
Emacs, and it leaves large data-structure/runtime rewrites conditional on
evidence rather than ambition alone.
