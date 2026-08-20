# Phase 28's remeasurement: Phase 21's battery, re-run at today's pin

Status: the measurement half of Phase 28 of
`doc/plans/2026-08-18-elisp-data-model.md`, whose gate is "re-run the
Phase 21 measurements and capability table, then choose one separately
planned branch".  This document is the RE-RUN and the evidence table.
**It selects nothing.**  The selection among the five branches is the
owner's, made from what is below; there is deliberately no
recommendation section, and no branch is ordered ahead of another
anywhere in this file.

Nothing in this document changed a source, test, Makefile, ratchet,
census or baseline file.  Where a measurement would have needed one, the
limitation is recorded instead.

## The pin these numbers describe

  ..                   Phase 21's pin                today's pin
  superproject         059dd8e .. 20df729            0bfef81 (`more-elisp-work`)
  fe                   dd35a2b                       e1d4fbd
  FE_API_VERSION       12                            15
  FE_LANGUAGE_VERSION  14                            18
  fe_version           --                            22.0
  kg arena             1 MiB = 56147 cells           10 MiB = 440101 cells
                                                     + 2350944 payload bytes
                                                     + 10909 frames

84 kg commits and 38 fe commits separate the two (718 kg files, +23120
/ -7426; 527 fe files, +9731 / -1195).  Between them lie the payload
substrate (23), vectors (24), length-bearing strings and match data
(25), the symbol index (26), the frontier demand phase, and the R1/R2
repair tranche.

Artifacts, stated the way `make bench` states them so a number can be
attached to the binary that produced it:

  kg counting binary   `kg 1.1.0 +lisp -tree-sitter +lsp +dap`,
                       sha256 ee5f7fc7091ed78497bed4f389baa7ccb80bf584dea00617d9e3e8c59e1b146d,
                       `git describe` 0bfef81 / fe e1d4fbd
  fe battery binary    sha256 fdefdf87cca5be29bac423734d9d7d5cc2a048403593ab7a37e515865897429a,
                       schema `fe-perf-workloads/4`, 22 workloads
  compilers            gcc (Debian 14.2.0-19) 14.2.0 for kg,
                       clang 22.1.8 for fe's battery
  Emacs oracle         GNU Emacs 31.0.91 (`/opt-3/emacs-31-lucid`)
  box                  32 cores; `/proc/loadavg` 1.48 .. 1.72 across
                       every timed run below, so no number here was
                       taken under load

## Method

Phase 21's two instruments, unchanged, and its rule that a counter is
the evidence and a clock is the corroboration.

* `make -C fe perf-workloads` for fe's own battery
  (`fe/perfobj/workloads.json`).
* `utils/bench.py` driving `test/perfobj/kg` in a real pty for kg's own
  workloads, with `test/kgbatch` checking each case's answer.  fe's
  counters ride inside kg's record under fe's own names, as Phase 21's
  closed follow-up arranged.
* `test/kgbatch` for terminal-free arena and load probes.
* For package demand, the capabilities report's methodology finding is
  followed exactly: **plain `kgbatch FILE` is the load-shaped
  read-eval-per-form loop, and `-r`/`-p` wrap the whole file in one
  expression and therefore answer a different question.**  Every "which
  blocker stops the load" answer below is plain mode.  Every ELPA file
  is read IN PLACE under `/root/.emacs.d/elpa/`; nothing was copied into
  this tree and nothing is committed from it.  Scratch shim files live
  under the session scratchpad and are named where they are used.

### What could NOT be re-run, and what replaced it

**`make bench` does not complete at this pin.**  `make perf-baseline`
exits 2:

    RuntimeError: bench case 'lisp-arithmetic-loop': lisp_gc_count was 1,
    expected > 1 -- this case is measuring nothing beyond the startup
    constant (see bench_case()'s assert_gt docstring)

That assertion is Phase 21's own Finding 6 repair: it was raised from
`> 0` to `> 1` so the case proved its 20000 iterations of boxed-integer
garbage forced a collection ON TOP of `kg_lisp_init()`'s post-prelude
one.  At a 10 MiB arena they no longer do -- 40004 boxed integers do not
exhaust 440101 cells -- so the case is now non-discriminating in the
opposite direction from the one Phase 21 repaired, and it takes the
whole report down with it.  **Recorded, not fixed: this is a measurement
wave.**  Two things stand in for it:

1. every other Lisp case run at the default arena (`--case` naming the
   twelve that pass), and
2. the whole set re-run with `KG_LISP_ARENA_BYTES=1048576`, i.e. at
   Phase 21's own 1 MiB arena, which is both the only way to get
   `lisp-arithmetic-loop`'s numbers with the unmodified tool and an
   honest control that separates "the arena grew" from "the
   representation changed".

The `fe script corpus` row of Phase 21's Finding 2 (6686624 cells,
23.1% integers) came from fe's script suite rather than from the
battery and has no `make` target; it is not re-measured here.

## 1. The arena report

Phase 21 and the capabilities report both record this exact line, so it
is the first thing to re-take.

    $ ./test/kgbatch -a /dev/null
    /dev/null: nil
    census: total=440101 free=430468 peak-live=10800 collections=1 failures=0

    $ ./test/kgbatch -g /dev/null
    /dev/null: nil
    arena: collections=1 peak-live=10800 failures=0 bytes=10485760
           payload-capacity=2350944 payload-live=46120 payload-peak=85344
           payload-compactions=1 payload-failures=0

  field                       Phase 21    today       delta
  total_slots                    56147     440101     x7.84
  free_slots                     50188     430468     x8.58
  peak_live_objects               6819      10800     +58.4%
  collections                        1          1     --
  failures                           0          0     --
  payload_capacity_bytes            --    2350944     new (FE_API_VERSION 13)
  payload_live_bytes                --      46120     new
  payload_peak_bytes                --      85344     new
  payload_compactions               --          1     new

The same line at Phase 21's arena SIZE, which is what separates the two
causes:

    $ KG_LISP_ARENA_BYTES=1048576 ./test/kgbatch -a -g /dev/null
    arena: ... bytes=1048576 payload-capacity=227576 payload-live=46120
           payload-peak=85344 payload-compactions=1 payload-failures=0
    census: total=41970 free=32337 peak-live=10800 collections=1 failures=0

So at one megabyte the CELL count fell 56147 -> 41970 (the payload carve
took 14177 of them) while the prelude's own retention rose 5959 -> 9633.
The margin at 1 MiB is 9633/41970 = **23.0%**, against Phase 21's
5959/56147 = 10.6%.  That is what `d878ea3` ("Raise the compiled Lisp
arena default from 1 MiB to 10 MiB") answered; at the shipped 10 MiB the
margin is 9633/440101 = **2.2%**.

### The prelude census, both pins

`.ci/prelude-startup-census.json`, `git show <pin>:` on each side:

  ceiling                     Phase 21 (20df729)   today (0bfef81)
  peak_live_objects                         6819             10800
  reachable_live_objects                    5959              9633
  embedded_bytes                           73730             88866
  definition_count                           122               139
  payload_capacity_bytes                      --           2350944
  payload_live_bytes                          --             46120
  payload_peak_bytes                          --             85344

    $ ./test/prelude_gc_probe
    after kg_lisp_init(): total=440101 free=430468 peak-live=10800 collections=1 failures=0
    reachable-live (total - free) = 9633
    payload: capacity=2350944 live=46120 peak=85344 compactions=1 failures=0

**One drift found and left alone.**  `src/lisp_core.c`'s arena-floor
comment cites "`.ci/prelude-startup-census.json`,
`reachable_live_objects` 9336".  9336 was the real figure when that
comment was written (`94694e6`); the census has since been re-banked to
9633 through several commits and the comment was not re-derived.  The
claim the comment supports still holds -- 3 x 9633 = 28899 is still
under the 30911 slots a 768 KiB arena opens -- and
`test_arena_floor_matches_census()` re-derives both halves from the
census file rather than from the comment, so nothing is measured wrong.
It is a stale number in prose, recorded here because the census ratchet
cannot see it.

## 2. fe's battery: Findings 1 to 5, re-checked

19 workloads at Phase 21's pin, **22** today: `vector-8` and
`vector-8192` are new (Phase 24) and the five `string-*` rows now
measure a different representation.  Whole-battery wall time is
**0.055895 s**.

### Finding 1 -- interning is no longer quadratic.  This is the largest delta in the document.

Phase 21's table, and the same probes today (`--json`'s per-workload
`extra` block):

  tier    obarray/indexed   candidates examined by ONE miss   hit on `car`
  ..      P21     today     P21              today            P21    today
  128     236     247       22 388           10               199    11
  1024    1132    1143      636 596          0                1095   1
  8192    8300    8311      34 451 636       0                8263   1

The probe names changed with the index (`miss_candidates`,
`miss_probes`, `hit_head_candidates`, `hit_core_candidates`); the
column above is `miss_candidates`, and `miss_probes` reads 11 / 1 / 1
across the three tiers.  Whole-workload totals for `intern-8192`:

  counter          Phase 21          today       ratio
  name_byte        442 042 413       135 135     3272x fewer
  intern_candidate 34 451 636 (miss)  12 719     --
  seconds          0.67              0.004718    142x faster
  share of battery 89%               8.4%        --

A bare context open paid **5947** candidate examinations to intern its
own 108 symbols at Phase 21's pin.  Today it interns 119 symbols for
**50** (`context-open`, `intern-cd` column).

The 128-symbol tier is the one that still walks: 10 candidates and 11
probes for a miss.  That is a small table's collision rate, not a
length-proportional scan, and it does not grow with the symbol count --
which is the whole shape of the finding.

**Finding 1 is closed, by Phase 26, and it was Phase 21's single
largest number.**

### Finding 2 -- scalar boxing: the fe-side integer shares are UNCHANGED, to the digit

  workload            cells P21/today   integers P21/today   share P21/today
  arithmetic-loop     80043 / 80043     40004 / 40004        49.98% / 49.98%
  deep-call-chain      2150 / 2150        605 / 605          28.1%  / 28.14%
  gc-sparse-garbage   80030 / 80030      20003 / 20003       25.0%  / 24.99%
  macro-heavy         18072 / 18072       4005 / 4005        22.2%  / 22.16%
  list-walk            2651 / 2651         305 / 305         11.5%  / 11.51%

Nothing between the pins touched how fe boxes an integer, and the
battery says so exactly.  `env-width-8`'s 59.43% scaffolding artifact,
excluded by name from Phase 22's Design C ceiling, is also unchanged.

New rows worth naming because they change what "second by type" means:
`vector-8192` allocates 8193 cells of which 8192 are the boxed integers
it stores (99.99%) plus one vector header, against 73976 payload bytes;
and every `string-*` row now allocates exactly **1** cell whatever the
length, where a 8192-byte string was a chain of ~1171 seven-byte cells
at Phase 21's pin.

### Finding 3 -- the sweep is still arena-proportional, asserted and holding

`gc_sweep_examined == gc_collection * total_slots` exactly, in every
collecting workload at this pin:

  gc-sparse-garbage   81 collections x 1881 slots  = 152 361 examined,
                      73 143 marked
  gc-dense-live        3 collections x 11557 slots =  34 671 examined,
                      24 042 marked
  kg's prelude         1 collection  x 440101      = 440 101 examined,
                      9635 marked, 1167 reclaimed

`gc-sparse-garbage` ran 45 collections in a 2694-cell arena at Phase
21's pin and 81 in an 1881-cell one now: the arena is the same byte
count and the payload carve took the difference, so the same garbage
meets the ceiling more often.  The invariant is the finding, and it
holds.

### Finding 4 -- `IsNamedSymbol` against the literal "t" -- STILL STANDS, and is now a larger share of what is left

  arithmetic-loop      Phase 21    today
  name_compare         80 837      80 047
  intern_candidate     817         27

The intern half of that line fell by 30x; the 80 020 `IsNamedSymbol`
comparisons did not move, because nothing between the pins touched
them.  Both sites Phase 21 named are still there and still compare a
name chain rather than a pointer: `fe/fe_eval.c:61` (`BindLambda`) and
`fe/fe_eval.c:85` (`ValidateSetqTarget`).

Phase 21 said this belongs to "Phase 28's evaluator optimisation branch
rather than to the storage work", and forbade fixing it in a
measurement commit.  Both halves of that still apply, including to this
document.

### Finding 5 -- frames are still the pool that saturates; the ceiling moved with the arena

Re-measured with the bench case's own `lw` shape:

  n                     150   300   400   600   1200   2400   3600
  peak_frame_depth      305   605   805  1205   2405   4805   7205
  peak_gc_stack_depth    67    67    67    67     67     67     67
  frame_capacity      10909 10909 10909 10909  10909  10909  10909

Two frames per level, exactly as Phase 21 measured; the GC root stack
still does not grow with this shape at all (it reads 67 now rather than
107 -- a smaller constant, not a different behaviour).  The ceiling,
bisected under `kgbatch`:

    n = 5450  fits
    n = 5451  eval:1: evaluation frame limit exceeded

Phase 21: 520 fits, 540 raises.  The ceiling moved 10.1x and
`frame_capacity` moved 10.03x (1087 -> 10909), so the shape is
unchanged and the pool simply got bigger with the arena.

### The two representation facts Phase 21 pinned -- both unchanged

    env-width-64   env_lookup 64, env_cell 4096, alloc_fn  1
    env-depth-64   env_lookup 64, env_cell 4096, alloc_fn 64
    env-width-8    env_lookup 64, env_cell  512, alloc_fn  1
    env-depth-8    env_lookup 64, env_cell  512, alloc_fn  8

Width and depth still cost the same, because the environment is still
one flat alist that a nested `let` extends rather than framing; and a
`let` still allocates one `FeTFn` apiece.  **Phase 21 pinned these
precisely so that Phase 28's first-class environments would break them
loudly.  Nothing has broken them.**

### Where the battery's wall time now goes

  arithmetic-loop     0.019822  35.5%
  gc-sparse-garbage   0.018231  32.6%
  gc-dense-live       0.007112  12.7%
  intern-8192         0.004718   8.4%
  macro-heavy         0.003040   5.4%
  vector-8192         0.001070   1.9%
  everything else     0.001902   3.4%
  TOTAL               0.055895

At Phase 21's pin `intern-8192` alone was 89% of the battery at 0.67 s,
which implies a battery of roughly 0.75 s.  Today's is 0.0559 s.

## 3. kg's own workloads

Every row's answer is checked, per Phase 21.2's rule.  `peak-live` is a
HIGH-WATER MARK since `kg_lisp_init()`, not a current-live figure
(Phase 21's Finding 7; still true and still the reason two cases are
sized as they are).

  workload                        answer      peak-live      free   gc  frame
  lisp-arena-prelude              --              10800    430468    1      8
  lisp-arena-auto-fill            t               10800    429754    1     26
  lisp-arena-grep-buffer          t               11242    428859    1     26
  lisp-arena-help-fns             t               11291    428810    1     26
  lisp-arena-pipeline             16              10800    429395    1     25
  lisp-arena-pipeline-text        t               11535    428566    1     41
  lisp-arena-representative-init  (1..25)         10800    429767    1     54
  lisp-list-walk (n=150)          150             11243    428858    1    305
  lisp-arithmetic-loop            NOT MEASURABLE AT THIS ARENA -- see below
  lisp-macro-heavy                2000            27711    412390    1      8
  lisp-deep-call-chain            300             11825    428276    1    904
  lisp-command-latency            3               10800    430462    1      8
  lisp-interactive-command x100   100             10999    429102    1     10

Phase 21's same table, for reading beside it:

  lisp-arena-prelude              --               6819     50188    1      8
  lisp-arena-auto-fill            t                7182     48965    1     33
  lisp-arena-grep-buffer          t                7988     48159    1     33
  lisp-arena-help-fns             t                8265     47882    1     33
  lisp-arena-pipeline             16               7367     48780    1     32
  lisp-arena-pipeline-text        t                8349     47798    1     48
  lisp-arena-representative-init  (1..25)          6819     49342    1     54
  lisp-list-walk (n=150)          150              7914     48233    1    305
  lisp-arithmetic-loop            199990000       56147     20297    2      8
  lisp-macro-heavy                2000            24101     32046    1     10
  lisp-deep-call-chain            300              8262     47885    1    904
  lisp-command-latency            3                6819     50182    1      8
  lisp-interactive-command x100   100              7456     48691    1     13

`peak_gc_stack_depth` is 67 and `peak_native_reentry` is 1 in every row
(Phase 21: 107 and 1); `frame_capacity` is 10909 in every row (Phase
21: 1087).  The payload pool moves per workload and did not exist at
Phase 21's pin at all: 46120 bytes live after the bare prelude, rising
to 60392 at `lisp-arena-help-fns`, against a 2350944-byte capacity --
**2.6% at the most expensive shipped package**, with the peak
(85344 bytes) set during prelude construction and reclaimed by the one
post-prelude compaction.

### Which workloads collect, and why

At Phase 21's pin: every kg workload once, from `kg_lisp_init()`'s
deliberate post-prelude collect, and `lisp-arithmetic-loop` a second
time from its own boxed-integer garbage.

**At this pin, at the shipped arena: every kg workload once and NOTHING
a second time.**  That is the whole content of the `make bench` failure
above.  Run at Phase 21's arena size the second collection comes back
and then some:

  KG_LISP_ARENA_BYTES=1048576     answer      peak-live      free   gc  frame
  lisp-arena-prelude              --              10800     32337    1      8
  lisp-arena-representative-init  (1..25)         10800     31636    1     54
  lisp-list-walk                  150             11243     30727    1    305
  lisp-arithmetic-loop            199990000       41970     16913    3      8
  lisp-macro-heavy                2000            27711     14259    1      8
  lisp-deep-call-chain            300             11825     30145    1    904

`lisp-arithmetic-loop` collects three times in 41970 cells where it
collected twice in 56147.  Its answer is unchanged (199990000).

### Arena margins, as the gate asks

56147 cells with 5959 retained (10.6%) at Phase 21's pin; **440101
cells with 9633 retained (2.2%)** here, or 23.0% if the arena is held
at Phase 21's size.  The most expensive shipped package leaves 428810
free.  Frame capacity 10909, saturated by the list-walk shape at
n = 5451.  Payload capacity 2350944 bytes, 46120 live after the
prelude.

## 4. Allocation by type, kg's own workloads

The table Phase 21's closed follow-up produced, re-taken.  Shares of
each workload's own `alloc_object`; the by-type block sums to
`alloc_object` exactly in every row; "other" is
fn + macro + primitive + native_fn + double.

  workload                        alloc_obj   pairs   integers  strings  symbols  vectors  other
  lisp-arena-prelude                  10800  84.08%     0.87%    6.61%    5.02%    0.00%  3.42%
  lisp-arena-representative-init      11501  84.58%     1.07%    6.33%    4.77%    0.00%  3.25%
  lisp-arena-auto-fill                11514  84.70%     0.83%    6.36%    4.81%    0.00%  3.30%
  lisp-arena-grep-buffer              12409  85.39%     0.79%    6.13%    4.54%    0.00%  3.15%
  lisp-arena-help-fns                 12458  85.23%     0.79%    6.34%    4.53%    0.00%  3.11%
  lisp-arena-pipeline                 11873  85.00%     0.82%    6.21%    4.73%    0.00%  3.25%
  lisp-arena-pipeline-text            12702  85.62%     0.83%    5.92%    4.47%    0.00%  3.16%
  lisp-interactive-command x100       12166  84.93%     1.61%    5.92%    4.49%    0.00%  3.05%

Phase 21's same eight rows (pairs / integers / strings / symbols /
other):

  lisp-arena-prelude              63.40%   0.01%  25.71%  5.87%  5.02%
  lisp-arena-representative-init  66.48%   0.39%  23.21%  5.34%  4.58%
  lisp-arena-auto-fill            66.68%   0.05%  23.52%  5.30%  4.45%
  lisp-arena-grep-buffer          69.10%   0.06%  21.81%  4.90%  4.14%
  lisp-arena-help-fns             67.40%   0.05%  23.77%  4.78%  4.00%
  lisp-arena-pipeline             68.11%   0.05%  22.18%  5.24%  4.42%
  lisp-arena-pipeline-text        69.59%   0.14%  21.39%  4.76%  4.13%
  lisp-interactive-command x100   68.52%   1.24%  21.19%  4.87%  4.18%

**The second-place type changed, and the reason is a representation
change rather than different behaviour.**  Phase 25 made a string's
bytes payload and a string one OBJECT, so `alloc_string` counts string
objects where it used to count seven-byte string CELLS.  The prelude's
1753 string cells became **714 string objects carrying 12572 string
bytes inside 85344 payload bytes**.  Strings fell 25.71% -> 6.61% of
allocation without any string disappearing; pairs rose to 84% because
the denominator shrank in exactly that place.

Integers rose from 0.01-1.24% to 0.74-2.00% of allocation in kg's real
workloads -- the same absolute counts against a smaller string share --
and are still two orders of magnitude under `arithmetic-loop`'s 49.98%.

## 5. Two workloads Phase 21 could not run at all

At Phase 21's pin `s.el` did not load, so nothing measured a real
third-party package.  It does now.  Both rows below were produced with
`utils/bench.py`'s own `bench_case()` -- the same instrument, the same
pty, the same counters -- against the ELPA file read in place, with the
init file planted in the case's throwaway HOME.  Neither is a new tree
fixture and neither is checked in.

  workload                      alloc_obj   pairs  integers strings symbols vectors
  s.el load (ELPA, in place)        19699  88.26%    0.74%   5.12%   3.60%   0.01%
  s.el load + 73 entry points       33660  89.27%    1.95%   4.23%   2.28%   0.00%

  counter                     s.el load   + 73 calls
  peak_live_objects              18532        32493
  payload_live_bytes            113104       131456
  peak_frame_depth                  19           44
  gc_collection                      1            1
  eval_step                      28584        75982
  env_lookup                      2793         8568
  env_cell                        8178        22247
  macro_expansion                   83          298
  intern_lookup                   6227         7002
  intern_candidate                9093        10226
  name_compare                   18318        30979
  function_resolve                6349        14905
  vector_element                     3            3
  string_object                   1008         1424
  string_copy                        0         3242

The three `vector_element`s are the single vector value in 793 lines of
`s.el`: the Edebug spec `[&or (function &rest form) fboundp]` at
s.el:455 that Phase 21's capabilities report named as the blocker
stopping the whole file.  It is now read, allocated, and never used
again.

## 6. Demand, measured today

### 6.1 The forecast audit

`make forecast-audit` regenerates `utils/forecast/AUDIT.md`
byte-identically at this tree (`make forecast-check` agrees), so the
checked-in report is today's measurement:

    MISSING (4 names, 4 references)
      1  gethash          forecast-wordcount.el
      1  make-hash-table  forecast-wordcount.el
      1  maphash          forecast-wordcount.el
      1  puthash          forecast-wordcount.el

    Watch item          References   Names seen
      hash-tables            4       gethash, make-hash-table, maphash, puthash
      vectors                9       vectorp x4, aref x3, vconcat x2
      records                0       --

**Records: zero references, still.**  Vectors' nine references are all
COVERED now.  The corpus's entire residual demand is the hash-table
family, from one sketch file, on a code path that sketch's own alist
sibling avoids -- which is the same "not on an unconditional load path"
verdict the capabilities report gave it, and the reason Phase 27 is
adjudicated dormant.

### 6.2 s.el: it loads, and where the frontier is now

    $ ./test/kgbatch -a /root/.emacs.d/elpa/s-20260522.135/s.el
    /root/.emacs.d/elpa/s-20260522.135/s.el: s
    census: total=440101 free=421897 peak-live=18204 collections=1 failures=0

Unmodified, in place, plain mode: the file loads to completion and
answers its own `(provide 's)`.  At Phase 21's pin the same command
answered `eval:34: void-function autoload`.
(`external/elpa/s.el`, the vendored testing copy, is byte-identical to
the ELPA file, so the tree's own capability case and this probe are
about the same bytes.)

**The NEXT-blocker probe, widened.**  The frontier phase's own probe
called 51 entry points and its six-name NEXT list is asserted equal to
nil in `test/test_lisp.c`.  Re-run here against **73 of s.el's 74
public entry points** (every `^(defun s-...` except
`s-lex-fmt|expand`), each called with plausible arguments inside a
`condition-case` collecting the name of anything that raises:

    $ ./test/kgbatch -p <scratch probe>
    (73 nil)

Seventy-three called, **zero raised**.  The 74th,
`s-lex-fmt|expand`, answers too -- and answers WRONGLY, which is the
first of two things found past the probe that are not `void-function`
and that no `fboundp` probe would ever see:

1. **`s-lex-format` (a `defmacro`, so not among the 74 `defun`s) is
   silently wrong, then hard-fails.**  Its expander
   builds the binding list with
   `(s-match-strings-all "${\\([^}]+\\)}" fmt)`, and

       (string-match "${\\([^}]+\\)}" "${a}")   kg nil      Emacs 0
       (string-match "a$b" "a$b")               kg nil      Emacs 0

   kg's engine treats `$` as an anchor everywhere; Emacs treats it as a
   literal unless it is at the end of the regexp or before `\|` or
   `\)`.  So `s-lex-fmt|expand` produces `(s-format "${a}" 'aget
   (list))` -- an EMPTY binding list -- and evaluating that then raises.
   Regex semantics, on the same engine seam R2 opened.

2. **A condition symbol declared through the symbol plist is not a
   condition.**  s.el:627 does `(put 's-format-resolve
   'error-conditions '(error s-format s-format-resolve))` and then
   signals it.  `put` and `get` and `symbol-plist` all work; `signal`
   does not consult the plist and kg has no `define-error`:

       (progn (put 'my-err 'error-conditions '(error my-err))
              (condition-case e (signal 'my-err nil) (error (list (car e) (cdr e)))))
       kg    (error ("Invalid error symbol" my-err))
       Emacs (my-err nil)

Both verified against GNU Emacs 31.0.91 on this box.

### 6.3 The rest of the ELPA tree, plain mode

112 package directories, 876 `.el` files.  Every package's own main
file (110 of them have one) probed with plain `kgbatch`, and then again
behind a scratch shim file (`eval-when-compile`, `eval-and-compile`,
`declare-function`, `defgroup`, `defcustom`, `defface`,
`gv-define-setter` as inert macros) with `s`, `dash`, `f` and `ht`
added to the load-path in place:

  first blocker                                   plain   shimmed
  `require` of a library absent from the tree         91        95
  void-function eval-when-compile                      9         0
  void-function defgroup                               2         0
  Wrong number of arguments: defalias, 3               0         7
  Wrong number of arguments: require, 3                1         1
  Wrong number of arguments: regexp-opt, 2             0         1
  void-function expand-file-name                       1         1
  void-function file-name-sans-extension               1         1
  void-function make-syntax-table                      1         1
  void-function define-obsolete-function-alias         0         1
  void-function eval-and-compile / declare-function    2         0
  unsupported read syntax: unknown escape              1         1
  LOADS TO COMPLETION                                  1         1

The one that loads is `s`.  The absent libraries, counted: **cl-lib 35,
compat 12, subr-x 4**, then singletons.  Neither `cl-lib` nor `compat`
exists anywhere under `/root/.emacs.d/elpa`, and this box's Emacs ships
only `.elc` for `cl-lib`/`cl-macs`/`subr-x`, so there is no source to
point kg at -- the cl-lib class is measurable as a COUNT of packages and
not as a load probe.

**No package in the tree has a vector, record, hash-table or
`cl-defstruct` blocker as its own first blocker, in either column.**
That is the same result the capabilities report got for records at
Phase 21's pin, now measured over the whole tree and for all four
families at once.

Two gaps this census turned up that are worth writing down because
nothing else in the tree names them:

* **`"\("` in a string is an unknown escape.**  `cond-let.el:319` has
  the standard `\(fn FORM FORM...)` docstring convention; kg's reader
  rejects it and Emacs reads an unknown escape as the character itself
  (`"a\(b)"` -> `"a(b)"`).  A reader-substrate gap on an unconditional
  load path.
* **Three arity gaps**: `defalias/3` (the DOCSTRING argument; it is
  `dash.el:603`, and it is the first blocker for 7 packages behind the
  shim), `require/3`, and `regexp-opt/2` (the PAREN argument -- a name
  this campaign landed three weeks ago).

### 6.4 The named refusals kg raises today

Measured, not read out of source.  These are the places kg has already
decided to say no in a sentence, which is the demand signal a branch
would close:

    (eval '(+ 1 2) t)                  -> "unsupported feature: eval lexical argument"
    (macroexpand FORM NON-NIL-ENV)     -> "unsupported feature: macroexpand environment"
    (macroexpand FORM nil)             -> works
    lexical-binding                    -> void-variable  (Emacs: t)
    defvar-local                       -> unbound        (Emacs: bound)
    define-error                       -> unbound
    cl-defstruct / record / recordp / make-record -> all unbound
    make-hash-table / gethash          -> unbound
    vectorp / aref / vconcat / make-vector / vector / type-of -> ALL BOUND

And what kg already gets right, so a branch is not sold on a bug:

    (defun make-adder (n) (lambda (x) (+ x n)))
    (list (funcall (make-adder 3) 10) (funcall (make-adder 5) 10)
          (funcall (let ((dyn 'inner)) (lambda () (read-dyn)))))
    kg    (13 15 global)
    Emacs (13 15 global)

Closure capture and dynamic/lexical interaction agree with Emacs on
these shapes.  kg's environment is a flat alist that a closure captures
whole; it is not broken, it is unstructured.

### 6.5 Macro re-expansion, measured against a control

fe re-expands a macro on every invocation.  Phase 21 measured that only
through fe's synthetic `macro-heavy`.  kg's prelude has **27 macros
among its 139 definitions** -- `let`, `let*`, `cond`, `when`, `unless`,
`dolist`, `dotimes`, `push`, `pop`, `save-excursion`,
`with-temp-buffer`, `save-match-data` among them -- so the cost is paid
by ordinary Lisp, not by a benchmark.  Four M-: workloads through
`bench_case()`, each 2000 iterations, with a hand-written control that
does the same work without the macro:

  case              macro_expansion  eval_step  alloc_object  alloc_fn
  let-2000                     2005     316063         90890      2109
  no-let-2000 (control)           5      54063         28889       109
  dolist-1x2000                2005     374066        108920      2109
  while-1x2000 (control)          5     154066         30926       109
  when-2000                    2005     110063         42888        --
  no-macro-2000 (control)         5      62063         26889        --

Per evaluation of the macro, against its control: **`let` +31 cells and
+131 eval steps; `dolist` +39 cells and +110 steps; `when` +8 cells and
+24 steps.**

**The honest limitation on those three numbers**: the delta is the
whole cost of spelling the form as a prelude macro -- the expansion
itself PLUS whatever the expansion evaluates to (`let` expands to
`internal--let` and allocates the `FeTFn` the `alloc_fn` column shows).
An expansion cache would recover only the first half, and no counter
here separates them.  Sizing that split is work the branch owes, not
something this document can hand it.

By contrast, real code at load time expands very little: 4 expansions
for the bare prelude, 10-28 for kg's five shipped packages, **83 for
the whole `s.el` load**, and 298 for `s.el` plus 73 entry-point calls
(0.39% of that run's 75982 eval steps).

## 7. The five-branch evidence table

Cost classes use this wave's own measured sizes as the scale, from
`git`: Phase 23 was 13 kg commits / ~6100 inserted lines under
`src lisp test` plus 2 fe commits; Phase 24, 4 kg commits / ~5966 lines
(including the 793-line vendored `s.el`) over ~10 fe commits / ~3536
lines; Phase 25, 8 kg commits / ~3718 lines over 6 fe commits / ~316;
Phase 26, 13 kg commits / ~3531 lines over 5 fe commits / ~528; the
frontier phase plus the R1/R2 repairs, 25 kg commits / ~9750 lines.
kg's scc total is 10767 and fe's 1063 at this pin.  **S** = under a
Phase 25/26 (one fe commit series + a kg surface, ~500 fe lines);
**M** = a Phase 23/26 (a substrate plus its Lisp surface); **L** =
larger than any phase in this wave.

| # | Branch | Demand measured TODAY | Depends on | What 21/22 already said | Review's sketch | Cost class |
|---|---|---|---|---|---|---|
| 1 | **First-class lexical environments** | **Two named refusals kg raises itself**: `(eval FORM t)` answers "unsupported feature: eval lexical argument" and `(macroexpand FORM ENV)` answers "unsupported feature: macroexpand environment". `lexical-binding` is void-variable (Emacs: t). Neither has an outside consumer NAMED yet: 765 of 876 ELPA files carry the `lexical-binding: t` cookie and 16 files use `eval`'s second argument, but **no package's first blocker is either refusal** -- dash.el's `static-if` polyfill reaches `(eval condition lexical-binding)`, but its first USE is at dash.el:1067, behind four earlier blockers measured here (`eval-when-compile` :46, `require 'cl` :59, `defgroup` :69, `defalias/3` :603). Closure capture and dynamic shadowing already agree with Emacs on the shapes probed. | Nothing among the other four. Would consume Phase 23's payload region if a frame becomes an object. | Phase 21 pinned the two facts this would break: env width == env depth in lookup cost (4096 env cells for 64 lookups either way) and one `FeTFn` per `let`. **Both re-measured unchanged at this pin.** The plan calls it "the strongest likely next semantic investment"; that is the plan's word, not a measurement. | Sketch Phase 30, "conditional on Phase 28's measurements"; asks for an environment object and closure contract with dynamic/special interaction frozen first. | **M-L.** Touches fe's evaluator core (`fe_eval.c` is fe's largest file at scc 402 against a 520 per-file cap) plus a kg-side surface. Larger than Phase 26; the only branch here that changes how every form is evaluated. |
| 2 | **Records** | **No consumer named yet, and this is now a stronger negative than at Phase 21.** Forecast audit records row: **0 references** (hash-tables 4, vectors 9). Tree-wide, `#s(` appears in **1** of 876 ELPA files (`lsp-mode/lsp-rust.el:945`, an empty hash table's printed form as a defcustom default -- unchanged from Phase 21). **No package in the 110-file first-blocker census stops on a record, in either the plain or the shimmed column.** `record`, `recordp`, `make-record` are all unbound and nothing has asked. | Phase 23's payload substrate (landed) and Phase 24's vectors (landed) -- a record is a typed vector-like object over the same payload block. Nothing else. | Phase 21's capabilities report hunted for a record exhibit deliberately and found none: "no measured pressure, and excluded ... the plan's Phase 28 position arrived at from evidence rather than from taste". The capabilities report also called it the weakest-motivated of kg's three named future uses, buying ergonomics over performance. | Sketch Phase 32: records first as a typed vector-like object, `cl-defstruct` as a separate later project, and explicitly "do not let reader `#s(...)`, printer support and the entire cl-lib surface arrive as one type constructor". | **S.** The substrate exists. A typed header over a payload block, a `type-of` answer, accessors, a printer, oracle rows. Comparable to Phase 25's smaller half. |
| 3 | **`cl-defstruct`** | **No consumer named yet as a first blocker**, but the largest single blocker CLASS in the tree sits in front of it: **35 of 110 packages stop at `(require 'cl-lib)`** and `cl-lib` is absent from the ELPA tree and ships only as `.elc` with this box's Emacs. 68 of 876 `.el` files call `cl-defstruct`, always behind a `cl-lib`/`eval-when-compile` blocker, so not one of them attributes cleanly to this family. Forecast audit: 0 references. | **Branch 2 (records) is a hard prerequisite** -- the plan and the review both sequence it that way. Also needs a stated `cl-` neighbourhood, i.e. a decision about `cl-lib` that no other branch needs. | The plan: "a library/macro compatibility project after records, not part of the record representation. It does not commit kg to all of `cl-lib`, but the supported `cl-` neighbourhood must be stated before advertising it." Phase 21: `cl-defstruct` calls are macro expansions, never reader literals, so they are always gated behind something else. | Sketch Phase 32's second half, explicitly "a bounded `cl-defstruct` project" with an explicit supported neighbourhood. | **L**, and it is the only branch here whose size is set by a compatibility surface rather than by a mechanism. Records (S) plus a macro library plus a policy on how much of `cl-lib` kg claims. |
| 4 | **Macro-expansion caching** | **No outside consumer can name this -- it is an internal cost, and it is now measured.** Per evaluation, against a hand-written control: `let` +31 cells / +131 eval steps, `dolist` +39 / +110, `when` +8 / +24, with 2005 expansions per 2000 calls in each. **27 of kg's 139 prelude definitions are macros**, `let`/`let*`/`cond`/`when`/`dolist` among them, so this is paid by ordinary Lisp. Against that: real LOAD-time code expands very little -- 83 expansions for the entire `s.el` load, 298 for load plus 73 calls (0.39% of eval steps). fe's `macro-heavy` is 5.4% of the battery's wall time. | Nothing. It is the only branch that adds no type and no representation. Interacts with branch 1 if environments become objects (an expansion cache keyed by form identity must survive that). | Phase 21 did not measure this beyond fe's synthetic `macro-heavy` (22.16% integers, 18072 cells, 2000 expansions -- all unchanged at this pin). The plan's condition is explicit: "take this branch if Phase 21 still shows it dominating realistic code after the storage work"; the load-time numbers above are what that condition has to be judged against. **The three per-call deltas above conflate expansion with what the expansion evaluates to; no counter separates them today.** | Sketch Phase 34, "measured evaluator optimization ... only if the repeated Phase 21 counters select it", with "cache invalidation must preserve macro redefinition semantics" named as the hard part. | **S-M.** A cache and an invalidation rule inside fe's evaluator, plus redefinition-semantics tests. No public type, no arena change. The sizing risk is invalidation, not volume. |
| 5 | **Tagged `FeValue`** | **The gate it failed in Phase 22 it fails by more here.** Integers are **0.74-2.00%** of allocation in every kg real-session workload (0.74% for the `s.el` load, 1.95% for load-plus-calls), against Phase 22's 50% floor. 49.98% on fe's synthetic `arithmetic-loop` is unchanged to the digit, and that shape is not a kg workload -- its kg-side case cannot even be measured at the shipped arena because its garbage no longer forces a collection. `nil` and `t` are already singletons, so "integers removed" IS the whole projection. | Would touch every other branch, since every one of them stores values. | Phase 22's ADR: "ELIMINATED BY MEASUREMENT before any code was written ... C is not a near miss for kg. It is the wrong instrument for kg's shapes." Phase 21 additionally warned that Finding 4's 80020 name comparisons would be misread as evidence for it, and **Finding 4 still stands unchanged at this pin**, so that warning is still live. | Sketch Phase 34's second half: reopen "only if the repeated Phase 21 counters select it", and only if it "clear[s] the migration thresholds the Phase 22 ADR used to reject Design C". | **L**, and the ADR already priced the alternative to it. Every stored value's representation, in fe and in every `src/lisp_*.c` adapter. |

## 8. What changed since Phase 21 that bears on the choice

Seven things, in the order they change a reading of the old evidence.

1. **Phase 21's largest number is gone.**  Quadratic interning --
   34 451 636 candidate examinations for one miss, 89% of the battery's
   wall time -- is 0 candidates and 1 probe.  Phase 22's ADR was
   partly argued from "a material lookup constraint"; that constraint
   no longer exists, and the remaining lookup costs are Finding 4's
   name comparisons and the flat-alist environment.

2. **The second-place allocation type changed for a representation
   reason, not a behavioural one.**  Strings were 21-26% of every real
   kg workload and are 5.9-6.6% now, because a string is one object
   with payload bytes rather than a chain of seven-byte cells.  Pairs
   went 63-69% -> 84-85% in the same move.  Any argument of the form
   "strings are second, so fund string work" has to be re-derived: the
   string BYTES did not go away, they left the cell census.

3. **The arena is ten times bigger, and that quietly disarmed a
   gate.**  `make bench` does not run to completion at this pin
   because `lisp-arithmetic-loop`'s garbage no longer forces a second
   collection in 440101 cells.  This is the same failure mode Phase
   21's Finding 6 repaired in the other direction, and it is now the
   second time an assertion in this file stopped discriminating
   because a baseline moved under it.  The margin picture changed with
   it: 10.6% of the arena retained at Phase 21's pin, 2.2% now -- but
   23.0% if the arena is held at Phase 21's size, because the prelude's
   own retention rose 5959 -> 9633 and the payload carve took 14177
   cells.

4. **s.el loads, so the capability shortlist's first entry is
   spent.**  Vectors landed; the one vector literal in 793 lines is
   read and never used.  73 of 74 public entry points answer.  **The
   whole ELPA tree now yields exactly one loadable package, and zero
   first blockers of any data-model kind** -- the gating classes are
   `require` of absent libraries (cl-lib 35, compat 12), arity gaps
   (`defalias/3`, `require/3`, `regexp-opt/2`), a handful of missing
   built-ins, and one reader escape.  A branch chosen for
   package-loading reach has to be argued against that distribution,
   not against a family census.

5. **The next real s.el blockers are not data-model shaped.**  They
   are `$`-as-literal in the regex engine (silently wrong, which is
   how it survived the frontier phase's own probe) and
   `error-conditions` on the symbol plist.  Both sit on seams the
   repair tranche opened rather than on any of the five branches.

6. **Everything Phase 21 pinned about environments and frames is
   still true.**  Env width and env depth still cost the same; a `let`
   still allocates one `FeTFn`; the list-walk shape still spends two
   frames a level and still meets `frame_capacity` rather than the
   cell pool or the GC root stack -- now at n = 5451 rather than
   n = 540, because the pool grew 10.03x with the arena.  Phase 21
   pinned those assertions precisely so first-class environments would
   break them loudly; nothing has.

7. **Design C's gate is further from clearing, and Finding 4 is still
   the trap in front of it.**  Integers are 0.74-2.00% of real
   allocation.  fe's `arithmetic-loop` is bit-identical at 49.98%, and
   its kg-side sibling is the one workload that cannot be measured at
   the shipped arena at all.  Meanwhile 80 020 of that workload's
   80 047 name comparisons are still `IsNamedSymbol` testing for `"t"`
   at `fe_eval.c:61` and `fe_eval.c:85` -- a lookup cost inside the
   most boxing-looking workload in the battery, which no
   representation change fixes, and which Phase 21 recorded precisely
   so it would not be misread as evidence for tagging.

## Verified

    $ git rev-parse HEAD                       0bfef81...
    $ git -C fe rev-parse HEAD                 e1d4fbd...
    $ ./test/kgbatch -a /dev/null
      census: total=440101 free=430468 peak-live=10800 collections=1 failures=0
    $ ./test/prelude_gc_probe
      reachable-live (total - free) = 9633
    $ make perf-baseline                       EXIT=2, lisp-arithmetic-loop's
                                               lisp_gc_count assertion; recorded
                                               above, not repaired
    $ python3 utils/bench.py --case ... (12 Lisp cases + startup)   EXIT=0
    $ KG_LISP_ARENA_BYTES=1048576 python3 utils/bench.py --case ... (6 cases)  EXIT=0
    $ make -C fe perf-workloads                perf_workloads: 22 workload(s) ok
    $ make forecast-audit && make forecast-check
      regenerated byte-identically; git status clean
    $ make complexity-check                    EXIT=0, scc total 10767 (limit 10767)
    $ make pmccabe-check                       EXIT=0, max function 67 (limit 110)
    $ make -C fe complexity-check              scc total 1063 (limit 1063)
    $ make -C fe pmccabe-check                 1468/482 symbols (limit 1468)

`/proc/loadavg` was 1.48 .. 1.72 across every timed run.  No ratchet
moved and none was re-baselined.  `git status --porcelain` shows only
this file, in kg and in fe.  Nothing under `/root/.emacs.d/elpa/` was
copied, modified, or committed; the scratch shim and probe files used to
see past one blocker at a time lived in the session scratchpad and are
named where they are used.
