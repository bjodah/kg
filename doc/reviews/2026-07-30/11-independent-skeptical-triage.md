# Independent skeptical triage of reports 01–08

Date: 2026-07-30

Scope: cross-check the eight specialist reports against the current source,
challenge severity and architectural assumptions, collapse duplicated
recommendations, and produce one dependency-aware ordering. Product code was
not changed.

## Method and confidence labels

- **Reproduced here:** I executed a minimal case against the current binaries.
- **Reproduced by specialist:** the report gives an executed result that is
  consistent with the inspected source, but I did not repeat it.
- **Source-proved:** the control/data flow establishes the defect or
  asymptotic possibility without relying on timing.
- **Plausible, unmeasured:** the source exposes a cost or architectural hazard,
  but present user impact has not been demonstrated.
- **Overstated:** the underlying observation is sound but its severity,
  prerequisite claim, or proposed remedy is stronger than the evidence.

I read reports 01–08 in full and inspected the cited implementation. I also
re-ran the Fe macro and dotted-reader examples and the 300-repeat regex
example. This was not a new full CI or fuzz run.

## Findings that survive skeptical review

### 1. kg UTF-8 Backspace/Delete corruption is release-blocking

**Status:** source-proved. **Reports:** KG-C01 in 01; indirectly supported by
the mutation-gateway proposals in 04/06/08.

Backspace records and deletes exactly `chars[filecol - 1]`
(`src/buffer.c:1195-1198`), and forward delete does the same at `filecol`
(`src/buffer.c:1236-1239`). Cursor movement is glyph-aware, so those indices
can be the last byte of a multibyte character or its lead byte. The operation
therefore leaves invalid UTF-8 and records an incomplete undo payload.

No architectural work is needed first. Fix this locally with byte spans and
add native plus PTY tests. The later replace-range primitive should absorb the
implementation, but waiting for it would be the wrong dependency.

### 2. zero-width regexp replacement on UTF-8 can fail to progress

**Status:** source-proved. **Report:** R1 in 03; the fuzz gap is independently
identified by Q4/Q5 in 05.

After a zero-width match, query replacement advances one byte
(`src/search.c:948-953`). A subsequent match begun on a continuation byte is
snapped back to the containing glyph's start (`src/regex.c:41-53`). In `!`
mode no more input is read (`src/search.c:910-918`), so the cycle can continue
without a C-g polling point; a nonempty replacement may keep growing text.

This is stronger than a generic “UTF-8 semantic mismatch”: it is an
availability and memory-growth bug on an ordinary feature. Fix progress and
the wrapper's start-offset contract before regex architectural work.

### 3. `kill-line` at EOL records the wrong undo payload

**Status:** source-proved. **Report:** KG-C02 in 01.

The command removes a logical newline and places `"\n"` in the kill ring, but
records the next row's contents in `UNDO_KILL_TEXT`
(`src/buffer.c:1379-1391`). Undo reinserts exactly that recorded text
(`src/undo.c:219-224`). For `a\nb`, this yields `ab` then `abb`, not the
original buffer. This should be fixed independently and immediately.

### 4. compilation's advertised output cap has two real bypasses

**Status:** source-proved. **Report:** KG-C03 in 01; performance consequences
also appear in 07.

The pending line doubles without regard to `maximum_output`
(`src/compile.c:409-423`) and is not charged until newline commit. Newlines are
appended but only pending content advances `stored_output`
(`src/compile.c:431-447`). Thus one unbroken line can grow without the stated
cap, and newline-only output can grow the buffer without advancing the budget.

This must be corrected before generalizing compilation into a process service.
The larger process manager in 06/08 is not a prerequisite.

### 5. Fe destructive macro expansion is semantically broken

**Status:** reproduced here. **Report:** FE-1 in 02.

The macro path copies the returned object's two words over the call-site pair,
then evaluates the copied object (`fe/fe.c:1310-1317`). Nil identity is pointer
identity (`fe/fe.c:356-358`), and lexical symbol lookup is also identity-based
(`fe/fe.c:828-839`). I reproduced both the report's truthy fake-nil behavior
and its general mechanism against `fe/fe`.

The minimal remedy—evaluate the rooted expansion directly without destructive
copying—should precede macro caching, bytecode, or source-location work.

### 6. Fe printing cycles can hang and bypass the evaluation budget

**Status:** reproduced by specialist and source-confirmed. **Report:** FE-4 in
02; bounded callbacks are assumed by 06/08.

Lisp can construct cycles through `setcdr`, while pair writing walks/recurses
without cycle, byte, or step accounting. The specialist produced a cycle that
wrote tens of megabytes under a one-second timeout. The evaluator's budget
does not account for this native traversal. `%S` makes it relevant to kg, not
only standalone Fe.

The first requirement is bounded, cycle-safe output. A fully iterative general
graph serializer is desirable but not required for the first fix; an explicit
byte/depth bound and cycle detection can close the availability hole.

### 7. disk-change detection has a genuine overwrite-warning gap

**Status:** source-proved, with severity split. **Reports:** KG-C04 and KG-C05
in 01.

`file_state_differs()` maps every `stat()` error to unchanged and compares only
whole-second `st_mtime` plus size (`src/fileio.c:34-44`). Save trusts that
predicate (`src/fileio.c:476-484`). Missing files and same-size/same-second
rewrites can therefore evade a changed-on-disk warning.

KG-C04 remains high because it concerns silent overwrite of external edits.
KG-C05 is a separate, narrower collision: `write-file` adopts the destination
name before calling ordinary save (`src/fileio.c:517-540`), so an existing
destination is not unconditionally treated as a new-name collision. Keep it
medium; its no-prompt path requires metadata equality.

### 8. Fe's dotted-list reader silently rewrites invalid source

**Status:** reproduced here. **Report:** FE-2 in 02.

The reader assigns a dotted tail but continues reading list elements
(`fe/fe.c:912-933`). I reproduced `'(a . b c) => (a c)`. This is not merely a
dialect difference: accepting malformed source with discarded/rearranged data
is dangerous for configuration. Reject it before expanding the reader.

### 9. `FeToString(..., size == 0)` is a real API memory-safety defect, but
not currently a kg-critical path

**Status:** source-proved; severity ordering downgraded. **Report:** FE-3 in 02.

`size - 1` underflows and the terminator is written through `dst`
(`fe/fe.c:761-765`). The supported API therefore needs an immediate small fix
and sanitizer cases. However, all current kg calls found in `src/lisp.c` pass
real fixed buffers, and the in-tree Fe/Fex calls also pass nonzero buffers.
Calling this “critical” is defensible for the public C API's defect class, but
it should not outrank user-reachable kg corruption or hangs.

### 10. regex group repetition above 256 silently produces a false negative

**Status:** reproduced here. **Report:** R2 in 03.

The compiler accepts counts through 65,535, but execution returns `NULL` once
`done >= MAX_GROUP_REPEATS` (`fe/tiny-regex-c/re.c:1501-1513`) without setting
the too-complex status. I reproduced no-match for `\\(a\\)\\{300\\}` against
300 `a` bytes. The single-atom form's separate success in report 03 localizes
the defect.

The fix need not be an immediate complete regex VM. Returning an honest
resource-limit status is preferable to false no-match; removing the arbitrary
limit requires an explicit work representation.

### 11. matcher exhaustion is incorrectly collapsed into no-match

**Status:** source-proved. **Report:** R3 in 03; made an explicit invariant by
08.

The engine distinguishes `RE_STATUS_TOO_COMPLEX`
(`fe/tiny-regex-c/re.c:436-451`), but kg's forward and backward wrappers map
every non-OK execution status to `KG_REGEX_NOMATCH`
(`src/regex.c:114-130`, `src/regex.c:147-155`). This can silently miss a real
match. Thread the status through the UI before tuning limits.

### 12. the regex compiler's duplicated cursor has a concrete bookkeeping bug

**Status:** reproduced by specialist and source-confirmed. **Report:** item 1
in 04.

Invalid `\x` fallback emits multiple physical nodes while the logical `j`
counter advances once (`fe/tiny-regex-c/re.c:737-790`); closing-group lookup
uses `j` and repeated `getindex()` calls (`fe/tiny-regex-c/re.c:693-713`).
The specialist reproduced a resulting failed group match.

This deserves a local emitter abstraction, but its overall priority is medium:
the trigger is malformed-hex fallback behavior, not ordinary valid regex
syntax. Report 04's rank 1 is appropriate only within its
deduplication/ergonomics scope, not across the project.

### 13. row NUL termination is a real latent invariant violation

**Status:** source-proved, impact path plausible. **Report:** KG-C06 in 01.

`editor_insert_row()` copies `len + 1` bytes from a caller-supplied slice,
assuming the byte following the slice is NUL (`src/buffer.c:325-340`).
Undo paths can pass newline-delimited interior slices. This can leave
`chars[size]` non-NUL and makes later `strcmp` users such as sort-lines unsafe
semantically. The direct fix—copy `len`, write the terminator—is small and
should be taken even though a visible downstream failure was not reproduced.

### 14. hosted CI really omits the deep runner and standalone subproject suites

**Status:** source-proved. **Report:** Q1/Q2 in 05.

The only PR build workflow runs `make` and `make check`; it does not invoke
`.ci/run-ci-steps.sh`. Root builds Fe core and tiny-regex-c as dependencies but
does not run the full Fe/Fex or tiny-regex-c standalone suites. Connecting
existing gates is a high-value early investment.

The broader prescriptions in 05—an OCI tool image, SARIF, CodeQL, Mull,
FreeBSD, big-endian, and an 85% changed-line threshold—are options, not one
indivisible recommendation. Land the hosted deep runner and subproject tests
first; measure time and flakiness before adding the rest.

## Duplicates that should become single work streams

### A. One kg mutation seam, not four projects

Reports 03 R7, 04 item 10, 06 M1, 07's row/range findings, and 08 phase 2 all
describe the same underlying issue: commands directly coordinate storage,
undo, dirty state, render/syntax invalidation, and future markers/hooks.

Collapse them into:

1. a local, allocation-safe row/buffer replace-range primitive;
2. migrate existing high-cost/error-prone callers in small families;
3. after ownership is clarified, add compound undo, markers, and one change
   event at the transaction layer.

Do not require a complete transaction/event system before fixing UTF-8
deletion or query replacement. Conversely, do not add hooks to raw mutation
paths before a transaction exists.

### B. Buffer ownership must precede externally retained buffer state

Reports 04 item 7, 06 M2, and 08 phase 1 all identify the live `editor` /
`buflist` / `winlist` copy protocol (`src/bufmgr.c:45-140`,
`src/def.h:258-324`, `src/def.h:390-424`). This is source-proved coupling.

The claim that it is kg's highest current correctness problem is overstated;
no new corruption reproducer was established beyond the documented dired
fragility. It is nevertheless the right first large refactor. Stable buffer
handles, buffer-local modes/variables, markers, cross-buffer Lisp, and deferred
callbacks should wait for single ownership. Local command metadata and local
replace-range work need not wait.

### C. Command/key/mode unification is one staged program

Reports 04 item 2, 06 N1, and 08 phase 3 agree on duplicated command policy:
the C command table, Lisp allow-list, fixed Lisp command registry, flat C-c
bindings, hard-coded prefix dispatch, and hand-written help.

Do not start with the full keymap trie proposed by 06/08. First make the
existing command descriptor authoritative for Lisp-callability, mutation
policy, and documentation. Then convert one bounded mode map. Hierarchical
keymaps and generated help follow with parity tests. Mode descriptors require
buffer-owned mode state; command policy does not.

### D. Fe cleanup/unwind proposals overlap but are not identical

Report 02's evaluator completions, report 04's external-resource cleanup
stack, and report 08's error/cleanup frames solve adjacent problems:

- a C cleanup stack prevents native heap/fd leaks across the existing host
  `longjmp`;
- Lisp completions/unwind records enable `catch`, `condition-case`,
  `unwind-protect`, and quit propagation;
- host/editor state restoration needs a specified bridge between both.

Implementing two unrelated unwind stacks would create ordering and
double-cleanup hazards. Write one design first. A small C-resource cleanup
stack can land as an interim Fe hardening step, but it does not satisfy
language-level unwind semantics. kg does not compile the reviewed Fex I/O and
process extensions, so their leak risk should not be presented as a current kg
release blocker.

### E. Per-context Fe types are useful, but not an early prerequisite

Reports 02, 04, and 08 all recommend descriptors replacing the three global
Fex slots. The architectural direction is sound. It is not required to expose
the first stable editor handles: kg can retain `(ID, generation)` in a C handle
table and represent them conservatively until the type API is ready. Type
descriptors should follow the unwind/ownership contract, not block core editor
refactoring.

### F. Regex execution context, parser validation, and VM rewrite are three
scales of work

Moving global match counters into `re_ctx` (03 R5, 04 item 4, 08 phase 0) is a
small API-neutral correctness fix. Rejecting malformed groups/classes and
fixing the compiler cursor are bounded parser/compiler changes. Replacing the
recursive backtracker with an explicit VM is a separate architectural option.
Do not bundle them.

## Claims to downgrade or qualify

### Performance P0 labels are not release severities

Report 07 uses P0 for source-proved scalability, not for current product
failure. Preserve that distinction in the final report:

- File loading's exact row-array realloc on every line is a real worst-case
  `O(R^2)` metadata-copy pattern (`src/fileio.c:214-250`), but `realloc` may
  extend in place. Geometric capacity is still an easy win.
- `ab_append()` exact growth permits quadratic copying
  (`src/display.c:36-58`), but its output is screen-bounded and allocators
  often extend it in place. Add capacity; do not infer a need for a terminal
  diff renderer.
- Full-buffer visual-line geometry per refresh is source-proved
  (`src/display.c:444-454`, `src/display.c:514-529`,
  `src/mode.c:151-166`, `src/mode.c:247` onward), but user impact needs a
  large-buffer benchmark before a prefix tree.
- Fe linear symbol interning and full-arena sweep are source-proved
  asymptotics (`fe/fe.c:571-584`, `fe/fe.c:421-448`), but “P0 for richer
  Lisp” is conditional on package scale. Instrument before changing the GC or
  string representation.
- Backward regexp search can repeatedly rescan suffixes
  (`src/regex.c:135-185`), but preserving exact backward/overlap semantics
  makes the remedy nontrivial. Benchmark before designing a reverse VM.

The low-risk capacity/range changes are justified by code shape alone. Trees,
diff rendering, a new collector, and a new regex VM are not.

### Architecture schedules are too confident

Reports 02 and 04 attach week estimates to evaluator/type changes. Those
numbers are not supported by implementation experiments and omit compatibility
migration, submodule coordination, and fuzz stabilization. Retain relative
S/M/L risk, not calendar commitments.

Report 08 orders all state ownership before the mutation gateway; report 06
puts commands/keymaps first and transactions later. The contradiction is
resolved by separating local seams from retained state:

- local command-policy and replace-range refactors can start now;
- single buffer/view ownership must precede stable handles, markers,
  buffer-local modes, hooks, and cross-buffer Lisp;
- transaction/event semantics should be finalized after ownership, even if
  the underlying replace primitive already exists.

### Some affordance dependencies are softer than reported

- Basic `next-error` can visit immutable file/line diagnostics without
  adjusting markers; markers become necessary when locations must track edits.
- A bounded kill ring does not require a full command registry; a small
  command-identity state is sufficient.
- Backups/autosave recovery can land independently of keymaps, modes, or Fe,
  after disk identity/save semantics are corrected.
- A general multi-process manager should follow demand from a second
  concurrent client. Navigable compilation can first build on the existing
  single bounded process.

### Quality recommendations need staged baselines

Q3's measured coverage artifact is a useful snapshot, not a timeless project
fact. Q6's large warning list, Q8's Mull choice, and Q9's broad platform matrix
may produce valuable signal but are not proved to be the best next tools.
Connect existing gates, make missing tools fail visibly, add semantic
invariants, and gather stable baselines before enforcing arbitrary percentages
or a large portability fleet.

## Missing dependencies and design constraints

1. **Submodule ownership:** Fe and tiny-regex-c changes must land in their
   branches with standalone docs/tests, then kg must advance the pins. A root
   refactor cannot silently patch only the checked-out submodule.
2. **Compatibility mode:** strict unbound-variable and exact-arity semantics
   (FE-5/FE-6 in 02) can break existing scripts. They need a declared language
   version or migration plan, not an unconditional semantic flip.
3. **Failure atomicity:** a replace transaction must reserve undo and
   replacement storage before mutation. “One mutation gateway” without an OOM
   contract merely centralizes partial-edit bugs.
4. **Safe callback points:** markers and buffer ownership alone do not make
   hooks safe. Lisp callbacks must be queued until no row reallocation,
   renderer, minibuffer internal, or partial transaction is active.
5. **Handle lifetime:** `(slot, generation)` must be introduced before Lisp,
   jobs, diagnostics, or queued hooks retain buffer/process references.
6. **`WITH_LISP=0`:** command, mode, process, marker, and variable registries
   must be C-owned; Fe remains an adapter. Reports 06/08 are correct on this.
7. **Regex byte lengths:** before exposing regex broadly to Lisp, replace
   `strlen`-only subject APIs (`fe/tiny-regex-c/re.c:403-415`,
   `src/regex.c:114-145`) or explicitly reject embedded NUL.
8. **Resource accounting:** Fe steps do not bound writer/native work, regex
   steps do not count all glyph/helper work, and compilation's byte budget is
   incomplete. Each service needs counters at the actual unit of work.
9. **Mode migration:** syntax identity is currently used as behavioral mode
   identity. A mode registry needs an explicit compatibility mapping while
   dired/git/compilation behaviors migrate; it cannot be a flag-day pointer
   removal.
10. **Character policy:** buffer bytes, codepoint-visible Lisp positions,
    display columns, and regex byte spans need one documented conversion and
    malformed-UTF-8 policy before markers/search APIs are frozen.

## Recommended top 15

This ordering ranks concrete user harm first, then cheap correctness and
enforcement, then enabling architecture. Items on the same tier can run in
parallel.

1. **Fix kg UTF-8 Backspace/Delete and span-based undo** (01 KG-C01).
   Source-proved buffer corruption; no prerequisite.
2. **Fix zero-width regexp replacement progress and start-offset contracts**
   (03 R1). Source-proved hang/growth; add timeout PTY cases.
3. **Fix EOL `kill-line` undo** (01 KG-C02). Source-proved ordinary data
   corruption.
4. **Make compilation retention a true total byte budget** (01 KG-C03),
   including pending lines and newlines; add tiny-cap streaming tests.
5. **Fix Fe macro expansion and bounded/cycle-safe printing** (02 FE-1/FE-4).
   Macro behavior reproduced here; writer hang reproduced by specialist.
6. **Strengthen file identity/change states and `write-file` collision policy**
   (01 KG-C04/C05). Distinguish same/different/unknown; store nanoseconds and
   file identity where portable.
7. **Fix Fe's zero-size serializer and reject malformed dotted lists**
   (02 FE-2/FE-3). Both small; dotted rewrite reproduced here, serializer
   source-proved.
8. **Repair regex truthfulness:** distinguish too-complex, reject malformed
   structure, and report the 256-group-repeat ceiling honestly
   (03 R2/R3/R4). Then remove the ceiling with explicit repetition state.
9. **Fix the regex emitter cursor and move match counters into `re_ctx`**
   (04 items 1/4; 03 R5). Bounded, testable prerequisites for broader regex
   work.
10. **Restore the row NUL invariant and audit `(pointer,length)` constructors**
    (01 KG-C06). Small source-proved hardening.
11. **Put the existing deep runner and standalone Fe/tiny suites in hosted
    CI** (05 Q1/Q2). This makes the fixes above continuously enforceable.
12. **Land local mechanical scalability wins:** geometric staged-row and
    screen-buffer capacities, row storage reuse, an undo tail, and a checked
    replace-range primitive (07; 03 R7). Benchmark before deeper structures.
13. **Establish one buffer/view owner with stable IDs and generations**
    (04 item 7; 08 phases 0–1). Characterize multi-window behavior before
    migration; do not combine it with a feature.
14. **Make command descriptors authoritative, then stage keymaps/modes**
    (04 item 2; 06 N1; 08 phase 3). Start with Lisp-callability/read-only/docs,
    not a flag-day keyboard rewrite.
15. **Build transactions/markers/deferred hooks, then selective Fe semantics**
    (04 item 10; 06 M1–M3; 08 phases 2–6). Required sequence: failure-atomic
    transactions → marker relocation → queued safe-point hooks → versioned
    unbound/arity → unified unwind/cleanup → per-context types. Prove it with
    one small mode package before adding a general process/package ecosystem.

## Work that should remain conditional

- A rope, gap buffer, or balanced line/prefix tree: only after range splicing
  and visual-line counters show row movement/prefix scans dominate.
- A terminal diff renderer: only after geometric `abuf` growth and tty-byte
  measurements show full repaint is the remaining bottleneck.
- A moving/generational Fe collector, variable-sized object heap, or bytecode
  VM: only after allocation/GC/symbol/lookup counters on real packages.
- A Thompson/Pike regex VM: attractive for complexity guarantees, but first
  fix status, parser, reentrancy, compiler bookkeeping, and collect adversarial
  work counters.
- General concurrent process objects: after compilation caps are correct and
  a second concrete client requires concurrency.
- Full Emacs Lisp/ELPA compatibility, unrestricted text properties, dynamic
  scope by default, a C `dlopen` ABI, threads, or wholesale Fex import: the
  reports correctly reject these as poor fits for kg's stated size and
  `WITH_LISP=0` goals.

## Bottom line

The specialist reports are strongest where they give short execution traces:
ordinary kg editing has three high-priority corruption/progress bugs; Fe has
two reproduced semantic/availability faults plus two small parser/API faults;
and regex execution can silently lie about matches. Those should dominate the
first merges.

The architectural convergence is also real, but its ordering needs discipline:
land small command and mutation seams now, make buffer/view ownership singular
before retained extension state, then add failure-atomic transactions,
markers, and deferred hooks. Fe object-model and VM work is not the starting
point for kg extensibility. Likewise, the ambitious quality and performance
ideas should begin with connecting existing gates and adding counters, not
with a new tool fleet or data-structure rewrite.
