# Plan 08 — Performance counters and low-risk scalability work

## Goal

Fix source-proven allocation/copying pathologies with local changes, establish
benchmarks, and make larger data-structure/runtime work conditional on
evidence.

This plan covers: exact-size row-array growth while loading and inserting;
exact-size screen `abuf` growth; per-byte row realloc/render/syntax work;
whole-buffer multiline insertion; visual-line whole-buffer scans; undo tail
traversal; syntax allocation and keyword work; Fe interning/GC/environment/
string costs; and regex backward/structural rescans.

Correctness plans take precedence where paths overlap.

**Rule for every phase below: no optimization commit lands before the counter
or benchmark named in its "Gate" line exists, is checked in, and has a
before/after reading in the commit message.** Phases 1 and the "Measurement
foundation" are therefore blocking prerequisites, not preamble.

## Measurement foundation

### Files

- new `utils/bench.py` (or `utils/bench/`) and a Makefile `bench` target
- new `src/perf.h` plus counter sites in `src/`
- optional counters in `fe/` and `fe/tiny-regex-c/re.c`
- `.gitignore`; `quality.json` integration from Plan 07 Phase 3

### Mechanism (consistent with what the tree already does)

- Compile counters behind `KG_PERF_COUNTERS`, following the existing
  `-DKG_FUZZ=1` and `-DKG_SHOW_TILDE=$(KG_SHOW_TILDE)` pattern
  (`Makefile:69,127-129`). `src/perf.h` defines `KG_PERF_INC(name)` etc. as
  empty macros when the flag is off, so the default build is byte-identical.
- Dump at exit as JSON to `$KG_PERF_OUT`. PTY cases can already pass `args:`
  (`Case.editor_args`, `utils/pty_accept.py:64,278`) but have no per-case
  `env:`; either add `env:` support (the harness already assembles a per-case
  environment for `HOME` around `utils/pty_accept.py:404`) or export
  `KG_PERF_OUT` for a whole dedicated bench run.
- **Prefer counter assertions in native tests over wall-clock gates.**
  `test/test.h`'s `CHECK()` can assert shapes — reallocations ≤ `c·log2(rows)`,
  one `editor_update_row()` per logical replacement — and those run inside
  `make check` on every lane, deterministically. `test/test_buffer.c` already
  links `fileio.o` and `bufmgr.o` (`EXTRA_buffer`, `Makefile:333`), so loader
  and row counters have a home without new infrastructure.
- Wall-clock benchmarks stay non-gating in their own `.ci/ci-12-bench.sh`
  (the runner's step list is a glob, so it joins with no runner change) that
  publishes JSON rather than enforcing thresholds.

### Requirements

Record median, p95 and peak RSS; input size/shape; compiler and build flags;
the counters specific to the work performed; JSON output. Note that the default
build is `-Os` (`Makefile:7`) — a counter build is a different binary, so never
compare its wall times against a release build.

Corpora: the kg source tree; 100k/1M-line logs at fixed byte sizes; a 1 MiB
single minified line; Unicode/combining/wide text; multiline comment-heavy C;
representative Fe packages/prelude (`fe/scripts/*.fe` already exists and
`fe/bench.sh` already drives it via `make -C fe bench` — reuse it instead of
writing a second Fe benchmark); adversarial and ordinary regex patterns.

Avoid shared-CI nanosecond microbenchmarks. Prefer counters and millisecond-plus
workloads. Never benchmark inside a sanitizer lane: `.ci/ci-env.sh` already
doubles `PTY_TIMEOUT` to 40 s under `--parallel` because those lanes distort
timing. And a bench step cannot run while a full runner holds the `.ci/.run`
lock in the same tree.

## Phase 1 — Instrument before optimizing

### kg counters

- allocations/reallocations, with requested and copied bytes;
- `editor_update_row()` calls (32 call sites across `src/*.c`);
- syntax rows/bytes highlighted and downstream propagation distance;
- visual-line rows and bytes scanned;
- full-buffer flatten/rebuild count;
- screen `ab_append()` calls, reallocations and output bytes;
- undo eviction traversals.

### Fe counters

Objects allocated; GC count, slots scanned, objects marked, pause duration when
a clock is available; live/free high water; symbol probes and bytes compared;
environment links traversed; conses per call; max GC stack/evaluator depth.
Prefer per-context counters behind a public debug snapshot API, compiled out by
default.

### regex counters

Compiled nodes/bytes/structural scans; match start attempts; glyph comparisons;
steps/backtracks/capture snapshots; subject bytes revisited. Use the execution
context from Plan 06.

Gate: this phase *is* the gate for every phase that follows.

## Phase 2 — Geometric staged file-row capacity

### Evidence

`load_append_row()` (`src/fileio.c:214`) reallocs `res->row` to exactly
`numrows + 1` records for every line (`src/fileio.c:228-250`), so loading R
lines copies O(R²) row records. Two further per-row costs sit in the same
function: `editor_select_syntax_highlight(res->filename)` is called once *per
row* (`src/fileio.c:269`), and `editor_update_row()` renders and highlights
each staged row (`:270`) — then `commit_load_result()` selects syntax again and
runs `editor_update_syntax()` over every row a second time
(`src/fileio.c:379-382`). The only caller is the open path in
`src/bufmgr.c:217-229`.

### Changes

Add `row_capacity` to `struct temp_load_result` (`src/def.h:782`). In
`load_append_row()`: grow 0 → small initial, then ×2 with the existing checked
arithmetic helpers; cap at the representable row count and
`SIZE_MAX / sizeof(erow)`; keep allocating row chars independently; preserve
transactional cleanup on partial failure (`free_load_result()`,
`src/fileio.c:389`); shrink on commit only if the waste is material — normally
do not pay another copy.

Hoist syntax selection out of the per-row path. Measure the staged-highlight +
commit-highlight duplication before deleting either pass, and if the staged
pass is removed, prove the multiline (`hl_oc`) end state is identical.

Gate: realloc counter per load, plus load wall time at a fixed byte count for
10k/100k/1M lines.

Acceptance: row-array reallocations O(log rows); fixed-byte loading scales
near-linearly as line count grows; a failed load leaves the live buffer
untouched.

## Phase 2b — The same growth bug on the live row array

### Evidence

`editor_insert_row()` (`src/buffer.c:325`) reallocs `editor.row` to exactly
`numrows + 1` on every insertion (`:341`). This is the growth path for *all*
non-load insertion, including subprocess output: `buf_append_special_text()`
(`src/bufmgr.c:1815`) calls `editor_insert_row()` once per output line, which
is how `M-x compile` and `M-x shell-command` fill a buffer
(`src/compile.c:431,459`; `src/shell.c:628`). A long build log therefore pays
the same O(R²) copying as a file load, on a path users watch in real time.

### Changes

Give `editor.row` a capacity field alongside `numrows`, grown geometrically by
one reserve helper shared with Phase 2, and used by `editor_insert_row()` and
`editor_del_row()` (`src/buffer.c:392`). Do not shrink on delete.

Gate: reallocation counter over a 200k-line `make`-style log captured through
`buf_append_special_text()`; compare against Phase 2's loader counter.

## Phase 3 — Give the screen append buffer capacity

### Evidence

`struct abuf` is `{char *b; int len; int oom;}` (`src/def.h:327-332`);
`ab_append()` reallocs to exactly `len + n` on every append
(`src/display.c:36-58`); `ab_free()` frees `ab->b` without resetting `b`, `len`
or `oom` (`src/display.c:60`), which is a latent double-use hazard rather than
a live bug today. `ab_fill()` splits fills into ≤256-byte chunks
(`src/display.c:71-80`), so a single blank line is several appends, and one
frame is hundreds of them.

### Changes

Add `capacity`. `ab_append()`: checked-add the required length; grow
geometrically with a minimum chunk; preserve the sticky `oom` behavior; append
without reallocating when capacity suffices. `ab_free()` resets all fields.
Do **not** add a terminal diff renderer; measure full repaint after this change
first.

Tests: growth boundaries, OOM, a large terminal frame, byte-identical output.

Gate: `ab_append` call/realloc/copied-byte counters per refresh at 24×80 and at
200×600, before and after.

## Phase 4 — Add row source/render/highlight capacity

### Evidence

`editor_update_row()` (`src/buffer.c:259`) frees `row->render` and mallocs a
fresh worst-case buffer on every call (`:267,285`), then calls
`editor_update_syntax()` (`:320`). Highlight storage is *already* reused via
`realloc(row->hl, row->rsize)` (`src/syntax.c:1808`) — the earlier draft's
"reuse the `hl` allocation" is half-done; what remains is the exact-size
realloc and the `memset` of the whole row. `erow` has no capacity fields
(`src/def.h:230-240`).

### Changes

Add `chars_capacity`, `render_capacity` and `hl_capacity` to `erow`, with
checked reserve helpers. Update every constructor and consumer: `editor_init_row()`
(`src/buffer.c:648`), `editor_insert_row()`, `editor_free_row()` (`:369`),
staged loads (`src/fileio.c`), undo snapshots, and the special-buffer swap in
`src/bufmgr.c`.

`editor_update_row()` then reuses render storage, computes the required render
length, reserves once, renders once, and reuses highlight storage when
sufficient. Maintain `chars[size] == '\0'` and the render terminator.

Tests: capacity growth/shrink behavior; pointer stability when no growth is
needed; OOM leaves the old row valid; every buffer switch/free path; ASan and
MSan lanes.

Gate: bytes allocated and copied per `editor_update_row()` on a 1 MiB single
line, before and after.

## Phase 5 — One row range replacement

### Evidence — corrected caller list

The byte-at-a-time pattern is `editor_row_del_char()` then repeated
`editor_row_insert_char()`, and each of those calls `editor_update_row()`
(`src/buffer.c:798,899`), i.e. a full render + highlight rebuild per byte. Real
callers:

- `src/search.c:706-713` (query-replace) and `src/search.c:935-941`
  (replace-string/regexp), plus `query_replace_newline_at()`
  (`src/search.c:157-176`);
- `src/rect.c:284` and `src/rect.c:446` (rectangle kill/replace column loops);
- `src/undo.c:136,146` (undo replay).

Not `src/compile.c` and not `src/lisp.c`, as the earlier draft claimed: Lisp
and shell insertion go through `editor_insert_text_raw()`
(`src/lisp.c:410`, `src/shell.c:499`), and compilation output goes through
`buf_append_special_text()`/`buf_truncate_last_row()` — see Phase 5b.

### API

```c
int editor_row_replace_range(int row, int at, int delete_len,
    const char *insert, int insert_len,
    const struct edit_options *options);
```

This is the local precursor to Plan 10's transaction. Implementation: validate
and check the final size; reserve before mutating; copy the undo payload before
the overlapping `memmove`; one `memmove` plus one insert; one row update; one
dirty/undo action; return enough coordinate delta for callers.

Migrate query replace first, then `src/rect.c`'s column loops, then undo
replay. Do not migrate every command in one commit.

Gate: `editor_update_row()` call counter — exactly one per logical replacement
— plus a 1 MiB row insert/delete/replace benchmark.

## Phase 5b — Compilation/shell output mirroring

### Evidence

Each read chunk calls `buf_truncate_last_row()` (`src/bufmgr.c:1950`, one
`editor_update_row()`) and then re-mirrors the partial line with
`buf_append_special_text()` (`src/bufmgr.c:1815`), which appends per line via
`editor_row_append_raw()` (`src/bufmgr.c:1761`) and `editor_insert_row()`;
the drivers are `compilation_commit_line()`/`compilation_mirror_pending()`
(`src/compile.c:431,459`). A long line arriving in many small reads is
re-rendered once per read.

### Changes

Only after Phase 2b and Phase 4 land: keep the mirrored-tail bookkeeping but
skip the redundant re-render when the appended bytes do not change the rendered
prefix, and coalesce truncate+append into one row update.

Gate: `editor_update_row()` calls per KiB of subprocess output.

## Phase 6 — Splice multiline insertion locally

### Evidence

`editor_insert_text_raw_bulk()` (`src/buffer.c:700`, static) flattens the whole
buffer with `editor_rows_to_string()` (`:415`), builds a new copy, and rebuilds
every row with `editor_replace_rows_from_text()` (`:663`). It is reached from
`editor_insert_text_raw()` (`:998`) whenever `!editor.rect_mode` and the text
contains a newline — i.e. multiline yank, `src/lisp.c:410`, `src/shell.c:499`.

### Changes

Replace the flatten with a local splice: split only the current row into
prefix/suffix; parse inserted newlines into new row slices; reserve the row
array once (Phase 2b's helper); `memmove` trailing row records once; build the
new rows; append the original suffix to the final inserted row; update only the
affected rows and downstream syntax; and fail atomically by allocating every
new row before publishing. Keep a temporary owner structure for cleanup.

Tests: start/middle/end of buffer, empty buffer, final-newline policy, huge
insertion, OOM at each allocation, undo, and marks after Plan 10.

Gate: flatten/rebuild counter must go to zero for this path; yank of a 10 MiB
region benchmarked before and after.

## Phase 7 — Add an undo tail or deque

### Evidence

`undo_push()` (`src/undo.c:38`) pushes at the head, then, once
`size > max_size`, walks `max_size - 1` links to find the eviction point
(`src/undo.c:83-105`). `MAX_UNDO_SIZE` is 1000 (`src/def.h:366`) and
`struct undo_stack` is `{head, size, max_size, clean_size}` (`src/def.h:369`).

Be honest about the size of this: ~999 pointer hops per edit past the
thousandth is microseconds, not a stall. This is a tidiness and
predictability fix that should be justified by the counter, not sold as a
latency win.

### Changes

Maintain head and tail, or a bounded deque: O(1) push and oldest eviction, O(1)
size. `clean_size` is a count-from-top marker consumed by `undo_mark_clean()`,
so preserve the clean-state identity Plan 02 depends on, and keep per-buffer
ownership copies (`src/bufmgr.c`) copying every pointer safely.

Tests: max sizes 0/1/2/1000, eviction, clean checkpoints, free.

Gate: links-traversed-per-push counter reaching O(1); no change to any undo
test outcome.

## Phase 8 — Cache visual-line row geometry

### Evidence

With `editor.visual_line_mode` on, one repaint does all of the following:
`find_visual_row()` (`src/mode.c:169`) is called once per screen row from
`draw_window_rows()` (`src/display.c:141`) and rescans from row 0 each time —
O(win_h × numrows); `get_visual_row()` (`src/mode.c:151`) is called for scroll
adjustment (`src/display.c:448`), the mode line (`:515`) and cursor placement
(`:641`); `get_total_visual_rows()` (`src/mode.c:247`) scans the whole buffer
for the mode line (`src/display.c:524,527`). Each `visual_segments()` call
walks the row's bytes through `visual_line_width()` (`src/mode.c:92,117`).

Priority note: `visual_line_mode` is off by default and toggled by
`M-x visual-line-mode` (`src/cmd.c:303`), so this is not on the default repaint
path. It ranks below Phases 2–6.

### First step

Cache each row's display width and wrap count, keyed by row content
generation, window width, and tab/width-table semantics generation. Reuse the
cache within one refresh so cursor, mode line and placement do not rescan.
Invalidate the edited row, and the width-dependent cache on resize.

### Conditional second step

If prefix scans still dominate after row caching, add a Fenwick tree or
balanced prefix-sum structure per width/cache generation. This is genuinely
complex with multiple window widths; consider caching only active widths and
invalidating lazily. Do not start it without benchmarks showing meaningful
latency on large files in visual-line mode.

Gate: rows-and-bytes-scanned counter per repaint on a 1M-line buffer in
visual-line mode, before and after.

## Phase 9 — Bound syntax work without losing correctness

### Evidence

`editor_update_syntax_row_only()` (`src/syntax.c:1789`) reallocs `hl` to
exactly `rsize` and `memset`s it, then dispatches to a per-language callback or
`generic_keyword_scan()` (`src/syntax.c:1577`, pmccabe complexity 50).
`syntax_propagate_downstream()` (`src/syntax.c:1829`) walks forward only while
`hl_oc` keeps flipping, so unbounded propagation needs an open/close comment
change — worth measuring before assuming it is common.
`editor_rehighlight_all()` (`src/syntax.c:1868`) is a genuine whole-buffer pass
and runs on every `editor_set_syntax()`.

### Low-risk

Precompute keyword lengths and group keywords by first ASCII byte in the
`HLDB` tables; reuse the `hl` allocation via Phase 4's `hl_capacity` instead of
the exact-size realloc; count downstream propagation distance.

### Conditional

Only if measured propagation stalls appear: highlight visible rows first;
maintain a frontier with conservative state below it; process bounded slices in
the event loop; never display stale text as terminal control (Plan 01 is
independent); preserve deterministic tests with an explicit "finish syntax
work" hook that PTY cases can call.

Gate: rows highlighted and propagation distance per edit, on comment-heavy C.

## Phase 10 — Fe measured improvements

### First: symbol interning

If counters show material probe counts: add per-context hash buckets; retain
symbol identity and the enumeration list; define a stable hash over string
bytes; resize within the fixed-arena constraint or derive a fixed bucket count
from arena size; benchmark 100/1k/10k symbols and hit-heavy loads via
`make -C fe bench`.

### Then: call and environment allocation

Measure lexical depth and conses per call. Parent-linked validated frames and
indexed slots are a semantic/architecture project to be coordinated with the Fe
arity/unwind work in Plan 05, not a quick win.

### GC

Do not start with a moving or generational GC. First expose live/headroom/pause
telemetry and test a configurable arena size; a larger arena delays exhaustion
but increases full-sweep work.

### Strings

Measure large-string construction and copying. Variable-sized strings require a
new allocator/object layout and an API review; keep it conditional.

Gate: Fe counters plus `fe/bench.sh` medians on `fe/scripts/*.fe`.

## Phase 11 — Regex measured improvements

After Plan 06: resolve group/branch jumps at compile time; compile
nullable/anchored/first-set/required-prefix metadata; skip impossible
unanchored starts; add resumable iteration for backward search if benchmarks
show quadratic rescans. Do not build a new VM until explicit-stack and
resolved-jump results plus adversarial counters are known.

Gate: subject bytes revisited and start attempts per search, on both ordinary
and adversarial patterns.

## Ratchets and acceptance

After several stable runs, add loose budgets for: row reallocations versus
input lines; row updates per logical replacement; visual rows scanned per
cursor move; Fe symbol probes on a reference package; regex subject visits on
reference patterns; binary size.

Budgets tolerate machine noise by preferring counters; wall-time gates use
medians and generous thresholds, and stay out of sanitizer lanes.

## What landed, and what was deferred (2026-07-31)

Implemented, each with its counter before/after in the commit message:
measurement foundation and phase 1's kg counters (`src/perf.h`,
`test/test_perf.c`, `utils/bench.py`, `make bench`); phase 2 (staged
loader growth, and the duplicated syntax selection and highlight pass a
load paid); phase 2b (live row array growth); phase 3 (frame buffer
capacity); phase 4 (render and highlight reuse); phase 5 (the options
parameter, rect.c's column loops, the mid-glyph refusal advance); phase 6
(local multiline splice).

Everything below was measured first and then *not* done. The measurement
is the reason, and it is recorded here so the next person does not have
to take it again. Every number comes from the counting build
(`-DKG_PERF_COUNTERS=1`), either `test/test_perf.c` or `make bench`.

### Phase 4, `chars_capacity` — deferred

Only `render_capacity` and `hl_capacity` landed. `row->chars` is already
reallocated only when a row grows (`editor_row_replace_range()` does not
touch it for a shrink), and phase 5 removed the callers that grew it a
byte at a time. Adding a third capacity means every site that mallocs
`row->chars` -- `editor_insert_row()`, `editor_init_row()`,
`load_append_row()`, the four `editor_row_*` primitives,
`editor_row_append_raw()` in bufmgr.c, the splice -- has to set it, which
is a wide blast radius for storage that phase 4's own gate (bytes per
`editor_update_row()` on a 1 MiB line) does not measure.

### Phase 5, undo replay — deferred

`src/undo.c:153,163` replay `UNDO_INSERT_CHAR` and `UNDO_DELETE_CHAR`.
Each is one call and one `editor_update_row()` already, so there is
nothing to save, and replay must not push a record -- routing it through
a primitive that does adds a way to corrupt the stack for no measured
gain. The rectangle and query-replace loops, which did pay per byte, were
migrated.

### Phase 5b, compilation mirroring — deferred

Measured by `test_compilation_mirror_updates_per_read()`: a 4 KiB line
arriving in 64-byte reads costs 128 `editor_update_row()` calls, two per
read (one to truncate the displayed pending line off the last row, one to
re-mirror it). That is bounded work per poll -- proportional to reads and
to the length of the *pending line*, not to the buffer -- and the buffer
it mirrors into no longer pays O(R^2) for its row array (phase 2b) or a
fresh render allocation per update (phase 4). The remaining win is
coalescing truncate+append into one update, halving a number that is
already small; the counter test is checked in so the claim can be
re-checked when it stops being small.

### Phase 7, undo tail or deque — skipped

Measured by `test_undo_eviction_walk()`: with `max_size` 1000, trimming
keeps `max_size - 1` records, so a full stack evicts on every *second*
push -- 4 walks of 999 links per 8 pushes past the limit, about 500 links
amortised per push. At roughly a nanosecond a link that is under a
microsecond per keystroke, three orders of magnitude below the 30 ms
paste threshold in `kbd.c` and four below a repaint. The reviewer's
verdict ("microseconds, tidiness") is what the counter says. Not worth
destabilising the `clean_size` marker plan 02 depends on; the counter
stays checked in so the claim is re-checkable.

### Phase 8, visual-line geometry — deferred, but the gate is met

Measured by `make bench --case visual-line-100k`: 24 repaints of a
100k-line buffer in visual-line mode scanned 13,999,212 rows and
713,958,945 row bytes -- about 583,000 rows and 30 MB per repaint, and
6.7 s for a session that costs 0.18 s with the mode off. This is a real
pathology and phase 8's gate is satisfied; it is deferred only because
the plan ranks it below phases 2-6 (the mode is off by default and
reached through `M-x visual-line-mode`) and because the per-row wrap
cache it needs -- a content generation on `erow`, invalidation on resize
and on tab-width change -- does not fit in what is left of the
`SCC_COMPLEXITY_MAX` budget without an extraction pass of its own.

There is no cheap subset: the scans come from `get_visual_row()` twice,
`get_total_visual_rows()` once and `find_visual_row()` once per screen
row, and the two `get_total_visual_rows()` calls in `src/display.c` are
exclusive branches of one ternary, not a duplicate to hoist.

### Phase 9, syntax — partly landed, the rest deferred

The `hl` allocation reuse landed with phase 4. Downstream propagation was
measured and is not a problem: opening a block comment at the top of a
40k-line comment-heavy C file propagated **4 rows**
(`syntax_propagate` 4, `make bench --case open-comment-c-40k-edit`),
because the next comment terminator closes it again. Neither the
visible-rows-first frontier nor bounded slicing has a workload behind it.
Keyword grouping by first ASCII byte in `HLDB` is untouched for the same
reason: `generic_keyword_scan()` is 51 pmccabe and the corpora that
exercise it (40k-line C) load in 158 ms.

### Phases 10 and 11, and phase 1's Fe and regex counters — skipped

No kg workload measured here is Fe-bound or regex-bound. The Fe and regex
counter sub-phases of phase 1 were therefore not implemented: they are
submodule changes, and a counter in `fe/` or `fe/tiny-regex-c/` has to
travel the whole pin chain (tiny `adapt-to-fe` -> fe `analyzers-etc` ->
kg) to reach this tree, which is a disproportionate price for
instrumentation whose consumers (phases 10 and 11) are themselves waiting
on evidence. `kg_regex_match_backward()`'s quadratic rescan is the one
candidate with a known shape -- an anchored entry point in tiny-regex-c
would fix it -- and it stays a separate, submodule-touching piece of work
with its own differential run.

### The bench step

The plan allowed a non-gating `.ci/ci-NN-bench.sh`; `make bench` is a
Makefile target only, and no CI step runs it. The runner's whole point is
that its steps run concurrently, and five of them drive PTYs under
sanitizers -- `.ci/ci-env.sh` already doubles `PTY_TIMEOUT` because of
it. A benchmark taken in that company measures the box. What CI does hold
is the counter assertions, which are the same number under any lane.

### Ratchets

No loose budgets were added on top of the counter assertions. The
assertions in `test/test_perf.c` *are* the ratchet: they are bounds
(`c*log2(rows)`, exactly one row update per logical replacement) rather
than recorded numbers, so they catch a pathology coming back without
failing on every unrelated refactor. Wall-time budgets were deliberately
not added -- see the note at the top of `utils/bench.py`.

## Final commands

```sh
make check
make bench
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```

Each optimization commit must include before/after counter output and must not
change saved-file or PTY behavior.
