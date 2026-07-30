# Performance and scalability review

Date: 2026-07-30

Scope: kg, its embedded Fe runtime, and the bundled tiny-regex-c engine. This is
a static, source-level review. Complexity claims below are derived from the
control flow and data structures; cache and user-visible impact claims are
identified as hypotheses until benchmarked.

## Executive triage

The highest-confidence scalability problems are not instruction-level
micro-optimizations. They are repeated whole-row, whole-buffer, or whole-file
work hidden behind common operations:

1. **Fix kg's exact-size growth patterns first.** File loading grows the row
   array one element at a time; screen construction grows the output buffer one
   append at a time; rows and undo records churn allocations. Geometric
   capacities and an undo tail/ring are local, low-risk changes.
2. **Make row mutation a single operation.** Insert, delete, replace, query
   replace, compilation output, and syntax highlighting repeatedly rebuild the
   same row. A range-replacement primitive plus capacity-bearing row storage
   gives several features one optimization and one correctness boundary.
3. **Measure and cache visual-line geometry.** Visual-line mode scans buffer
   prefixes or the whole buffer multiple times per repaint. This is the clearest
   editor-interaction scalability issue.
4. **Instrument Fe before enlarging it.** Full-arena GC, linear symbol
   interning, linear environment lookup, and cons-heavy strings/function calls
   will become architectural limits for a richer Lisp. Allocation/GC/lookup
   counters should precede a VM or GC rewrite.
5. **Compile regex structure once.** tiny-regex-c repeatedly walks its
   variable-sized instruction stream and kg's backward search repeatedly
   restarts the matcher. Resolved jumps and a continuation-based search API are
   more valuable than matcher micro-tuning.

The most important distinction is between throughput and latency. Full-screen
redraw is bounded by terminal size and may be acceptable; full-buffer visual
geometry, full-buffer paste reconstruction, and quadratic file/line growth are
not similarly bounded.

## Measurement foundation

There is no durable editor performance suite. `fe/bench.sh` checks out and
builds historical revisions, runs each ten times through `/usr/bin/time`, and
reports rounded averages (`fe/bench.sh:1-50`). tiny-regex-c's “Performance test”
prints match results rather than timing or enforcing a budget
(`fe/tiny-regex-c/tests/test2.c:2064-2105`); its README still lists a performance
test as TODO (`fe/tiny-regex-c/README.md:176-180`).

Add a non-gating benchmark runner first, with machine-readable results and a
later, deliberately loose regression gate. Record median, p95, peak RSS, and
input metadata. Do not gate on wall-clock microbenchmarks in ordinary shared CI
until variance is known.

Recommended counters:

- kg: bytes allocated/reallocated/copied; row render rebuilds; rows
  rehighlighted; syntax propagation distance; visual-width rows/bytes scanned;
  bytes sent to the terminal; full-buffer flatten/rebuild count; undo-node
  traversal count.
- Fe: objects allocated; GC count; slots scanned and objects marked per GC;
  symbol comparisons; environment links traversed; conses per evaluation;
  peak GC-root depth.
- regex: compiled nodes/bytes; structural-node scans; match attempts;
  `match_seq` steps; atom retry/backtrack count; capture snapshots; subject
  bytes revisited. The existing step budget is not a proxy for all work because
  helpers and glyph loops do work outside the increment site.

Use representative corpora, not only synthetic maxima: kg's source tree, a
large minified line, a million-line log, Unicode text, comment-heavy C, and
realistic Emacs-style Lisp packages.

## kg findings

### P0: file loading grows row metadata quadratically

**Source-proven.** `load_file_transactional()` calls `load_append_row()` once
per input line (`src/fileio.c:310-330`). Each call reallocates the row array to
exactly `numrows + 1` (`src/fileio.c:214-250`). Therefore loading `R` lines can
copy `1 + ... + R = O(R^2)` row records, even when total file bytes are fixed.
Each appended row also temporarily installs staged editor state, reselects
syntax, and renders/highlights the new row (`src/fileio.c:259-270`). On commit,
syntax is selected again and every row is highlighted again
(`src/fileio.c:362-382`).

Recommendation: give `temp_load_result` a geometric row capacity, select syntax
once before the loop, and avoid the second full highlight when staged state is
known complete. Preserve transactional failure behavior.

Benchmark: load equal-byte files with 10, 1,000, and 100,000 lines, with syntax
off and on. Count row-array reallocations/bytes copied and highlight calls.

Risk: low for capacity growth; medium for eliminating highlighting because
multiline syntax state and error cleanup must remain identical.

### P0: ordinary row edits repeatedly allocate and rebuild the whole row

**Source-proven.** `editor_update_row()` frees the render buffer, scans the
source row for tabs, allocates the exact rendered size, scans again to render,
and invokes syntax highlighting (`src/buffer.c:259-321`). Insertion reallocates
the character array to its exact new size, moves the suffix, and calls that
routine (`src/buffer.c:748-800`); deletion moves the suffix and calls it too
(`src/buffer.c:891-900`). Typing `N` bytes into an already long row is therefore
at least `O(N * row_length)` work and can be `O(N^2)` from an empty row, before
syntax propagation.

Query replacement magnifies this design: replacement is deleted and inserted
one byte at a time (`src/search.c:690-713`, `src/search.c:932-942`), so each
replacement can rebuild a long row `match_length + replacement_length` times.
Compilation output has the same shape when a process emits a very long line:
the pending fragment is repeatedly mirrored into a row, whose append path
reallocates and rerenders (`src/compile.c:409-423`,
`src/bufmgr.c:1761-1777`).

Recommendation:

- add `chars_capacity`, `render_capacity`, and geometric reserve helpers;
- introduce `editor_row_replace_range(row, at, delete_len, bytes, insert_len)`;
- update render and syntax once per logical mutation;
- route query replace and bulk append through the range primitive.

Benchmark: insert 1 MiB one byte at a time at start/end of one line; replace a
1 MiB match; consume a 10 MiB subprocess line at several read chunk sizes.
Record allocations, copied bytes, render rebuilds, syntax rows, and latency.

Risk: low-to-medium. Capacity fields add modest slack but leave the row-array
architecture intact. The central mutation primitive reduces duplicated
invariant maintenance and should be implemented before larger buffer changes.

### P0: visual-line mode performs whole-buffer geometry work per repaint

**Source-proven.** A refresh computes visual cursor position before drawing
(`src/display.c:437-454`), again for mode-line information
(`src/display.c:510-529`), and again to place the cursor
(`src/display.c:635-645`). `get_visual_row()` scans all preceding rows
(`src/mode.c:151-166`), while `get_total_visual_rows()` scans every row
(`src/mode.c:247` onward); row width computation scans row bytes
(`src/mode.c:92-125`). Consequently a cursor move in visual-line mode can scan
`O(buffer_bytes)` multiple times even when only a small viewport is visible.
Split windows repeat work and may have different widths.

Recommendation: first cache each row's display width/wrap count keyed by window
width. If profiles still show prefix scans, maintain prefix sums (a Fenwick tree
or balanced line tree) for wrap counts. Invalidate an edited row, all rows on
tab-width/display-width semantic changes, and a width-specific cache on resize.

Benchmark: cursor motion in 1 MiB, 100 MiB, and one-million-line buffers, with
visual-line mode off/on and one/two/four window widths. Count row bytes scanned,
not just wall time.

Risk: medium for a simple width cache; high for a prefix tree because edits,
window-width variants, tabs, and Unicode width must share one invalidation
model.

### P1: multiline paste reconstructs the entire buffer

**Source-proven.** `editor_insert_text_raw_bulk()` serializes all rows, allocates
a new whole-buffer string with the insertion, destroys all rows, and reconstructs
them (`src/buffer.c:663-743`). Any inserted text containing a newline selects
this path (`src/buffer.c:998-1010`). Its cost is `O(B + I)` time and transient
memory for buffer size `B` and insertion size `I`, independent of insertion
locality. Repeating such operations can be quadratic in total buffer size.

Recommendation: split only the insertion row, construct inserted rows directly,
and splice them into the row array. This should reuse the range-mutation
primitive above. A geometric row-array capacity removes reallocation but not
the `memmove` of rows after an insertion.

Benchmark: insert 1 KiB/1 MiB multiline text at start/middle/end of a 100 MiB
buffer; measure peak RSS, bytes copied, rows rerendered, and undo cost.

Risk: medium. A gap-buffer or balanced line tree would improve repeated
top-of-file line insertion but is an architectural change and should wait for a
trace showing row-array movement remains material after local splicing.

### P1: syntax work is full-row and can propagate to EOF

**Source-proven.** `editor_update_syntax()` reallocates highlighting for the
rendered row and scans it (`src/syntax.c:1789-1827`). When multiline state
changes, `syntax_propagate_downstream()` rehighlights following rows until state
stabilizes (`src/syntax.c:1829-1837`). An edit that opens or closes a comment or
string may therefore cost `O(bytes_to_EOF)`. Generic keyword matching also
walks the keyword table at candidate positions and recomputes keyword lengths
(`src/syntax.c:1745-1776`).

Recommendation: expose counters before redesign. Retain state convergence, but
cache/reuse highlight storage; precompute keyword lengths; consider a trie or
first-byte-indexed keyword lists only if keyword comparisons are material. For
long propagation, schedule highlighting in bounded visible-first slices while
retaining a conservative display state below the frontier.

Tradeoff: asynchronous or sliced highlighting improves latency but complicates
determinism, redraw, and tests. It is not justified merely by worst-case
asymptotics.

### P1: screen output construction uses exact-size reallocations

**Source-proven.** Every `ab_append()` reallocates to `len + append_len`
(`src/display.c:36-58`). A full refresh builds all visible windows and writes
the entire buffer to the tty (`src/display.c:437-662`). For output size `S`,
exact growth permits `O(S^2)` copying if the allocator cannot extend in place.

Recommendation: add a geometric capacity to `struct abuf`; this is a low-risk
win. Then measure terminal bytes and refresh CPU. A dirty-line/diff renderer is
only warranted if terminal bandwidth or repaint latency remains significant:
full repaint is simple, deterministic, and bounded by screen dimensions.

Benchmark: 24x80 through 200x500 terminals, one/four windows, syntax and visual
mode combinations. Count appends, reallocations, copied/output bytes, and p95
refresh latency.

### P2: position conversion and undo maintenance have avoidable scans

**Source-proven.** Character-offset conversion scans preceding rows and UTF-8
bytes in both directions (`src/buffer.c:591-635`). Repeated Lisp calls to
`point`, region functions, and movement can therefore turn a loop over a large
buffer into `O(iterations * buffer_prefix)`.

The undo stack allocates records/text separately and, once it exceeds
`MAX_UNDO_SIZE` (1,000), walks to the tail to evict the oldest record
(`src/undo.c:38-109`, `src/def.h:366`). This is bounded, but performs roughly a
thousand pointer traversals after every additional uncoalesced edit.

Recommendation: add a tail pointer or bounded deque for undo. For position
conversion, first cache per-row codepoint counts; add a prefix-sum tree only
when Lisp benchmarks demonstrate the need. Byte offsets as the internal editor
currency would be faster but create a broad compatibility and Unicode-audit
cost.

## Fe findings

### P0 for richer Lisp: linear symbol interning

**Source-proven.** `FeMakeSymbol()` linearly walks `symbol_list` and compares
names, then conses a symbol, string, and list entry on a miss
(`fe/fe.c:571-584`). Interning `N` distinct names is `O(N^2)` comparisons. The
current primitive set is small; packages, generated symbols, and richer
Emacs-Lisp compatibility will make this a startup and load-time limit.

Recommendation: add a hash interning table owned by `FeContext`, while
retaining the symbol list as a GC root or enumeration order if required.
Benchmark cold startup and parsing packages at 100/1,000/10,000 distinct
symbols, including hit-heavy workloads.

Tradeoff: table memory and resize latency versus predictable near-constant
lookup. Stable hash behavior is not semantically observable unless symbol
enumeration order becomes an API.

### P0 for richer Lisp: full-arena GC with no headroom policy

**Source-proven.** Allocation triggers collection only when the free list is
empty (`fe/fe.c:515-527`). Collection marks roots then sweeps every arena slot
(`fe/fe.c:421-448`), so each collection is `O(arena_capacity + live_graph)`.
When the live set is close to capacity, few allocations separate full sweeps.
kg reserves a fixed 1 MiB arena by default (`src/lisp.c:35-47`).

Recommendation: instrument before replacing. Track allocation rate, live count,
collection interval, and pause distribution on startup, editing hooks, package
load, and macro-heavy workloads. A larger fixed arena only delays the cliff.
Near-term options are an earlier configurable collection threshold plus arena
growth; longer-term options are generational/incremental GC or a page-based
nonmoving allocator.

Tradeoff: moving GC would invalidate the C API's raw `FeObject *` assumptions
and is therefore a major redesign. A nonmoving generational collector needs
write barriers. Arena growth is simpler but does not change full-sweep cost.

### P1: lexical lookup and call evaluation allocate linearly

**Source-proven.** `GetBound()` walks environment association lists one binding
at a time (`fe/fe.c:828-839`), making a lookup `O(lexical_depth)`. Evaluation
constructs evaluated argument lists and binding pairs on calls
(`fe/fe.c:1067-1106`, `fe/fe.c:1299-1307`), increasing GC pressure.

Recommendation: benchmark depth × lookup count and conses per function call.
For an interpreter evolution, represent environments as parent-linked frames
with indexed slots and resolve lexical references once. A bytecode compiler
could then use local-slot opcodes while preserving symbol lookup for dynamic
globals and reflective operations.

Tradeoff: this is an architectural project, not a preliminary optimization. It
affects closures, mutation, debugging, macros, and error reporting. Hashing
every small frame may be slower than a short linear scan; indexed lexical slots
avoid that tradeoff when resolution is possible.

### P1: strings are cache-unfriendly cons-like chains

**Source-proven layout; impact unmeasured.** A `FeObject` contains two value
words (`fe/fe.c:122-139`). String payload per object is
`sizeof(FeObject *) - 1` bytes (`fe/fe.c:108-120`), seven bytes on a typical
64-bit build, with the other word linking the next chunk. `FeMakeString()`
allocates chunks as it copies (`fe/fe.c:548-568`), and string byte copying first
walks for length then walks again (`fe/fe.c:803-813`). Thus large strings have
provable per-chunk object overhead and pointer traversal; the degree of cache
damage is a hypothesis until hardware-counter or wall-time measurement.

Recommendation: benchmark source loading, concatenation, equality, and C/Fe
bridge copies by string length. For richer Lisp, use variable-sized immutable
byte vectors (optionally a small-string inline form), with explicit length.
Ropes should be considered only for demonstrated large concatenation/edit
workloads.

Tradeoff: variable-sized objects complicate the current fixed-slot allocator
and GC. A separate string heap or external payloads preserve stable
`FeObject *` addresses but add lifetime accounting.

### Fe startup and memory layout

**Source-proven startup work; performance impact unmeasured.** `FeContext`
contains a fixed 4,096-entry GC-root stack (`fe/fe.c:149-176`), and
`FeOpenContext()` initializes the object arena/free list and registers all
primitives and aliases (`fe/fe.c:1636-1710`). Startup is therefore at least
linear in arena slot count, plus current linear symbol interning.

Benchmark context open, kg prelude evaluation, init-file load, first command,
resident arena bytes, and live objects. A separately allocated or dynamically
growing root stack could reduce every-context footprint, but this is a
secondary optimization unless multiple contexts are planned.

## tiny-regex-c findings

### P0: backward regex search can rescan suffixes quadratically

**Source-proven.** kg's backward wrapper repeatedly calls `re_exec()` from a
successively advanced byte offset to remember the last match before the limit
(`src/regex.c:135-185`). For a greedy pattern such as `a*` on `N` `a` bytes,
the first call scans about `N`, the next about `N-1`, and so on: `Theta(N^2)`
subject work.

Recommendation: extend the engine with an iterator/continuation API that
resumes after a reported match without restarting the same suffix, or an
explicit “last match ending before offset” API. Ensure zero-width progress and
UTF-8 byte/glyph semantics remain identical.

Benchmark: backward search for `a*`, `a+`, `x`, alternations, and zero-width
patterns over 1 KiB to 10 MiB subjects; count subject-byte visits and match
attempts.

### P1: regex compilation and matching repeatedly decode structure

**Source-proven.** The representation is a variable-sized instruction stream.
`getindex()` walks from the beginning to find an indexed node
(`fe/tiny-regex-c/re.c:253-291`), and closing groups calls it in a descending
loop (`fe/tiny-regex-c/re.c:693-713`), permitting `O(P^2)` compile work for
pattern structure size `P`. During matching, `group_end()` and `find_branch()`
walk the compiled structure (`fe/tiny-regex-c/re.c:1326-1360`), and branch
handling invokes those scans from sequence matching
(`fe/tiny-regex-c/re.c:1577-1615`).

Recommendation: resolve group-end and branch targets at compile time. Options
are offsets embedded in the compact stream or a fixed-width opcode/index array.
The former preserves footprint; the latter simplifies dispatch and is likely
more cache/predictor friendly, but that performance claim requires measurement.

Benchmark compile and execute patterns with increasing nested groups,
alternations, and character classes. Separate compile time from match time and
record structural bytes scanned.

### P1: unanchored search multiplies backtracking cost

**Source-proven asymptotic possibility.** `re_matchp_internal()` starts a match
at each subject glyph for unanchored patterns (`fe/tiny-regex-c/re.c:1656-1694`).
The atom matcher greedily consumes repetitions and retries the continuation
while backing up (`fe/tiny-regex-c/re.c:1430-1460`). Thus worst-case work is the
cost of a backtracking attempt multiplied by `O(subject_length)` starting
positions. The engine caps `match_seq` calls and recursion depth
(`fe/tiny-regex-c/re.c:80-101`, `fe/tiny-regex-c/re.c:1625-1631`), which bounds
some denial-of-service cases but does not count every helper scan or glyph loop.

Recommendation:

- compile a nullable flag, first-byte/first-codepoint set, and required literal
  prefix where possible;
- use `memchr`/substring search to skip impossible starting positions;
- count all major work categories before changing the safety budget;
- retain explicit resource limits even after optimization.

Do not promise linear time without replacing the backtracking engine. A Pike VM
or Thompson NFA gives stronger complexity guarantees for the supported regular
subset, but capture priority and Emacs-compatible semantics require careful
differential tests and make this a major rewrite.

## Ranked implementation roadmap

### Low-risk, high-confidence

1. Add benchmark/counter scaffolding and capture a baseline.
2. Give `abuf` and staged file rows geometric capacities.
3. Add an undo tail/ring.
4. Add capacity fields to rows and reuse render/highlight allocations.
5. Introduce one range-replacement primitive and migrate query replace.
6. Select load syntax once; measure before removing the second highlight pass.
7. Precompute syntax keyword lengths.

### Medium changes justified by scaling benchmarks

1. Splice multiline insertions instead of serializing/rebuilding the buffer.
2. Cache visual width/wrap counts per row and window width.
3. Add Fe symbol hashing and GC/lookup instrumentation.
4. Compile regex branch/group targets and add start-position prefilters.
5. Add a resumable regex match iterator for backward search.
6. Keep subprocess pending-line storage separate from repeatedly rendered rows.

### Architectural projects

1. A balanced line/prefix structure for visual geometry, character positions,
   and repeated line insertion.
2. Fe lexical frames plus bytecode/indexed locals.
3. Fe variable-sized strings and an allocator/GC model that supports them.
4. Incremental/generational Fe GC with a C-API-compatible object-address
   policy.
5. A non-backtracking regex VM if adversarial-pattern latency and stronger
   guarantees outweigh the compatibility and implementation cost.
6. A terminal diff renderer, only after tty-byte and refresh profiles show that
   full repaint—not whole-buffer geometry—is the remaining bottleneck.

## Suggested acceptance criteria

- Loading time at fixed bytes should grow near-linearly as line count rises;
  row-array reallocations should be logarithmic.
- A logical range replacement should trigger at most one row render/syntax
  update plus necessary downstream syntax convergence.
- Cursor-motion cost in visual-line mode should be independent of total buffer
  size for an unchanged viewport, apart from logarithmic prefix queries.
- Fe package-load symbol comparisons should grow near-linearly, not
  quadratically, after interning changes.
- GC telemetry should make pause time and allocation headroom visible; no
  architecture should be selected without workloads that reproduce the limit.
- Regex backward-search subject visits should be linear or near-linear on the
  greedy benchmark, and resource limits must remain effective on adversarial
  patterns.

These targets are more useful than a single “editor operations per second”
number: each is tied to a specific invariant and will distinguish a real
algorithmic improvement from allocator or machine noise.
