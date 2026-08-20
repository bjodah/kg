#!/usr/bin/env python3
"""Phase 13.5: prove fe's FE_GC_STRESS knob does something, through kg.

Fe's `MakeObject()` collects before *every* allocation when fe.c is built
with -DFE_GC_STRESS=1, instead of only when the free list is empty.  An
object that is live only through an unrooted C local survives an ordinary
run whenever nothing happens to collect between its creation and its last
use; under stress it dies at the next allocation.  That is the bug class
this exists to turn into a first-run failure -- and, separately, the
"collector that is never actually invoked" one, which is why the ordinary
build is asserted here too and not only the stress build.

The check runs ONE heavy-allocation script through kg's own evaluator
twice: an ordinary `test/kgbatch` and a `test/kgbatch-gcstress` linked
against the stress-built fe object.  It requires

  * the same answer from both -- a stress collection that took something
    still live would change it, or crash;
  * a collection count from the stress build far above the number of
    loop iterations, which is what says the knob is compiled in rather
    than silently defined to 0;
  * a nonzero collection count from the ORDINARY build too, read off the
    arena-stats surface kgbatch's `-g` prints and `M-x lisp-arena-stats`
    renders.  That is the separate "collector that is never invoked"
    assertion, and it needs its own, larger script: kg's arena holds
    tens of thousands of objects, so the loop the stress build can
    afford (every allocation is O(arena) there) never fills it.

Three kg processes, and what they cost is the build's, not the check's:
about a second all told on a plain build, minutes under a sanitizer,
because a stress collection is O(arena) and there are ~15600 of them.
STRESS_TIMEOUT_RATIO below is where that is measured and sized.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

# Conses hard and keeps almost nothing: the garbage is what drives the
# collector, and the small retained list is what a mistaken collection
# would corrupt.  The answer is checked, not just the exit status.
# Phase 14 added the symbol half: `make-symbol` and `gensym` manufacture
# live-then-dead SYMBOL objects, which are the first symbols fe has that
# the collector may reclaim (an interned one is on `symbol_list`, a root),
# and `put` appends two fresh pairs onto a property list whose symbol is
# reachable only through the caller's operand list.  Both are exactly the
# unrooted-intermediate seam this lane exists for.  `keeper` is retained
# across the churn and its name read back afterwards; `gcprobe`'s property
# is written and read at every hundredth iteration.
# Phase 15 added the string-and-list half, which is the same seam again
# in its most ordinary shape: `split-string`, `string-join`,
# `replace-regexp-in-string`, `seq-filter` and `sort` all build
# intermediate strings and cons cells that nothing roots until the next
# form takes them, and `replace-regexp-in-string` rebuilds its
# accumulator once per match.  A mistaken collection among them changes
# `shaped`, which is retained across the churn and reported.
SCRIPT = """
(setq kept nil)
(setq keeper (make-symbol "kept-name"))
(setq shaped nil)
(setq n 0)
;; Phase 18: two buffer-local bindings, made once and read every
;; iteration.  Their values are stashed in the value cells of hidden
;; INTERNED symbols rather than behind roots of kg's own, which is the
;; claim this lane exists to test -- a stash the collector could not see
;; would come back as something else, or as garbage, long before the
;; loop ends.  Made outside the loop on purpose: this lane's cost is
;; per collection, and creating a binding once adds nothing per
;; iteration.  Phase 18's follow-up added the `let' over one of them
;; that is LEFT in the other buffer, on the periodic branch for the same
;; cost reason.  Its restore does not go through the value cell: it goes
;; to the stash of the buffer the binding displaced, chosen by a tag fe
;; carried across the form.  It reports nothing new, and that is the
;; point -- the two buffer-local values at the end of the list are what
;; a restore landing in the wrong buffer would change.
(defvar gcprobe-local 'the-default)
(setq gcprobe-one (get-buffer-create "gc-stress-one"))
(setq gcprobe-two (get-buffer-create "gc-stress-two"))
(with-current-buffer gcprobe-one
  (setq-local gcprobe-local (list 'one (concat "held" "-by-one"))))
(with-current-buffer gcprobe-two
  (setq-local gcprobe-local (list 'two (concat "held" "-by-two"))))
(while (< n 40)
  (setq n (+ n 1))
  (list n n n)
  (concat "x" (format "%d" n))
  (gensym "tmp")
  (make-symbol "throwaway")
  (intern-soft "never-interned-here")
  (if (= n (* 20 (/ n 20)))
      (progn (put 'gcprobe (intern "p") n)
             (setq kept (cons (get 'gcprobe 'p) kept))
             (with-current-buffer gcprobe-one
               (let ((gcprobe-local (list 'let n)))
                 (set-buffer gcprobe-two)
                 gcprobe-local))
             (sort (list 3 1 2) '<)
             (setq shaped
                   (cons (string-join
                          (seq-filter (lambda (w) (string< "a" w))
                                      (split-string
                                       (replace-regexp-in-string
                                        "-" " " "one-two-three")))
                          ",")
                         shaped)))
    nil))
(list n kept (length kept) (symbol-name keeper)
      (intern-soft "never-interned-here") (intern-soft keeper) shaped
      (with-current-buffer gcprobe-one gcprobe-local)
      (with-current-buffer gcprobe-two gcprobe-local)
      (default-value 'gcprobe-local))
"""

EXPECTED = ('(40 (40 20) 2 "kept-name" nil nil '
            '("one,two,three" "one,two,three") (one "held-by-one") '
            '(two "held-by-two") the-default)')

# The same script with enough iterations to fill at least once the 1 MiB
# arena ARENA_BYTES below pins these runs to, which the stress-affordable
# loop above does not: run through the ORDINARY build only, to assert the
# collector is invoked at all.
BIG_SCRIPT = SCRIPT.replace("< n 40", "< n 4000").replace(
    "(* 20 (/ n 20))", "(* 1000 (/ n 1000))")
BIG_EXPECTED = ('(4000 (4000 3000 2000 1000) 4 "kept-name" nil nil '
                '("one,two,three" "one,two,three" "one,two,three" '
                '"one,two,three") (one "held-by-one") (two "held-by-two") '
                'the-default)')

# The stress build collects per allocation, and the loop above allocates
# far more than one object per iteration, so its count cannot be near the
# iteration count.  Measured on this box: 0 collections ordinary (the
# arena never fills), 14536 stress.
MIN_STRESS_COLLECTIONS = 1000

# A stress collection is O(arena), and the arena holds tens of thousands
# of objects, so this lane is slow by construction and slowest where it
# matters most.  Measured with the ci-05 MSan lane's own binary on the
# 32-core development box, where the whole run was 142.7 s and 15612
# collections: the PRELUDE ALONE was 97.7 s and 11339 of them, before the
# script's first form runs; the forms outside the loop add 686 more; the
# 40 iterations add 3587.  So 73% of the run was fixed cost, and time
# tracks collections almost exactly, at ~9 ms each (0 iterations 104.9 s,
# 10 iterations 128.7 s, 40 iterations 142.7 s).  That is why the loop is
# 40 iterations rather than the 300 it was through Phase 14 (measured
# under MSan then: 300 -> 229 s, 100 -> 149 s, 40 -> 138 s), and why
# cutting it further buys almost nothing: the knee is the prelude.
#
# The embedded-prelude work has since moved those counts and the shape
# they imply.  On this tree the whole run is 12315 collections and the
# prelude alone 6819, so the prelude is 55% of the run rather than the
# 73% above -- still the single largest block, so the 40-iteration choice
# stands, but no longer most of the run.  Counts are what this script
# asserts; doc/plans/2026-08-14-embedded-prelude.md's Phase 3 results
# carry the current MSan timings beside them.  Nor
# does a smaller arena -- a stress build at KG_LISP_ARENA_SIZE=256 KiB
# measures 75 s, only 1.9x, because marking the ~10600 live objects costs
# what sweeping the rest does, and it would want a second object build
# and an arena the shipped editor does not have.
#
# What the per-run timeout is FOR, therefore, is not cost: it is a
# collector that never terminates -- an unclosed mark loop, a free list
# that never refills -- which has to fail the build instead of wedging
# CI.  Nothing here asserts how FAST the stress build is, and everything
# it does assert is counted rather than timed.
#
# So the budget is sized the way the PTY harness sizes its readiness
# wait: a deadline that charges each runner what that runner actually
# costs.  The ordinary build runs the same script first, on the same box
# and in the same build configuration, so its measured seconds are the
# scale factor.  Ratios measured per build, each ordinary run against its
# own stress run over the same script:
#
#   gcc -O0 -g (make check)      0.03 s ->   1.55 s    52x
#   gcc --coverage (ci-02)       0.03 s ->   3.24 s   108x
#   clang ASan+UBSan (ci-04)     0.04 s ->  10.65 s   266x
#   clang MSan (ci-05)           0.53 s -> 142.71 s   269x
#   clang MSan, 3-vCPU CI box    2.10 s -> 644.40 s   307x
#
# The plain build's 52x is not the outlier it looks like: its ordinary
# run is mostly process startup, which is why there is a floor as well as
# a ratio.  The last row is why the number cannot be fixed at all.  That
# box is ~3x slower per core than this one on the ordinary run and ~4.5x
# on the stress run, which is the arena sweep meeting three slow vCPUs --
# so 644 s is what the run costs there with the box IDLE, and a 600 s
# constant sized on this box could not pass on that one at all.  The lane
# then adds its own load on top: `make -j3 check` reaches this check
# through `check:`'s prerequisites and runs it beside check-unit and
# beside check-pty's PTY_JOBS=3 editors, where it was measured holding
# 60% of one vCPU and taking 720.85 s.  A ratio moves with all of that
# because both of its terms do.
#
# 1000 is 3.3x the worst ratio above; on that box it computes a 2100 s
# budget for the 644.4 s run.
#
# The floor is what every build except MSan actually gets, because their
# ordinary run is too short to scale anything from: 120 s is 11x the ASan
# stress run, 37x the coverage one, 77x the plain one.  The cap keeps "a
# hang fails the build" true no matter what the calibration run reports:
# an hour, not an unbounded multiple.  Both numbers are printed against
# what they were given, so the next sizing decision is made from a CI log
# rather than from a guess.
#
# ORDINARY_TIMEOUT stays a plain number because it is the bootstrap --
# something has to bound the run the scale factor is read from -- and it
# can afford to be one: the ordinary runs measured 0.01 s to 6.77 s over
# every configuration above, the slowest of them the big script on the
# hosted box, so 300 s is 44x the worst of them.  It is the one number
# here a slower box could still outgrow, and the run it bounds is the
# cheap one, which is the trade.
ORDINARY_TIMEOUT = 300
STRESS_TIMEOUT_RATIO = 1000
STRESS_TIMEOUT_FLOOR = 120
STRESS_TIMEOUT_CAP = 3600

# Extended additively as kgbatch -g grows fields: every group here keeps its
# position, and a new one is appended, because the tuple below is read by
# index. The payload group is Phase 23.2's five fields, which are zero in kg
# until Phase 25 gives the region an owner -- captured rather than skipped, so
# a stress run that started allocating payloads would be readable here.
STATS = re.compile(
	r"^arena: collections=(\d+) peak-live=(\d+) failures=(\d+) bytes=(\d+)"
	r" payload-capacity=(\d+) payload-live=(\d+) payload-peak=(\d+)"
	r" payload-compactions=(\d+) payload-failures=(\d+)$")


# An instrument holds its own conditions fixed.  What this lane measures
# is collection CORRECTNESS -- the same answer from a build that collects
# before every allocation as from one that collects when the free list
# empties -- and that property does not depend on how big the arena is;
# every number this file carries (the collection counts, the peak-live
# figures, the BIG_SCRIPT sized to fill the arena at least once) was
# measured against a 1 MiB one.  A stress collection is O(total_slots),
# so inheriting the compiled default instead would scale the whole lane
# with it and say nothing new: measured on this box, this run is 1.57 s
# at 1 MiB and 10.64 s at 10 MiB, for the same 14856 collections and the
# same peak-live 10411.  Phase B of
# doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md is what makes
# holding it possible -- before the knob the arena was a constant of the
# build and this lane simply inherited whatever it was.  All three runs
# get it, since the ordinary run is also the scale factor the stress run's
# budget is derived from.
ARENA_BYTES = "1M"


def run(binary: pathlib.Path, script: pathlib.Path, budget: float, why: str):
	"""Evaluate `script` and return its value, arena stats and seconds.

	`budget` is a deadline, not an expected cost, and `why` says where it
	came from: a run that reaches it has hung, and the message has to be
	enough to tell that apart from a box slower than the one the budget
	was derived on -- which is the failure this replaced.
	"""
	env = dict(os.environ, KG_LISP_ARENA_BYTES=ARENA_BYTES)
	start = time.monotonic()
	try:
		proc = subprocess.run(
		    [str(binary), "-b", "-g", str(script)], env=env,
		    capture_output=True, text=True, timeout=budget)
	except subprocess.TimeoutExpired:
		raise SystemExit(
		    f"FAIL: {binary} did not finish {script.name} within "
		    f"{budget:.0f} s ({why}).  Either the collector no longer "
		    f"terminates, or this run is more than "
		    f"{STRESS_TIMEOUT_RATIO}x the ordinary build's cost, in "
		    f"which case re-measure both and re-size the ratio here")
	elapsed = time.monotonic() - start
	if proc.returncode != 0:
		raise SystemExit(
		    f"FAIL: {binary} exited {proc.returncode}\n{proc.stderr}")
	value = None
	stats = None
	for line in proc.stdout.splitlines():
		match = STATS.match(line)
		if match:
			stats = tuple(int(g) for g in match.groups())
		elif ": " in line:
			value = line.split(": ", 1)[1]
	if value is None or stats is None:
		raise SystemExit(
		    f"FAIL: {binary} printed no value/stats pair:\n{proc.stdout}")
	return value, stats, elapsed


def stress_budget(ordinary_seconds: float) -> float:
	"""This box's own ordinary run, scaled -- see STRESS_TIMEOUT_RATIO."""
	scaled = STRESS_TIMEOUT_RATIO * ordinary_seconds
	return min(STRESS_TIMEOUT_CAP, max(STRESS_TIMEOUT_FLOOR, scaled))


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--kgbatch", type=pathlib.Path, required=True)
	parser.add_argument("--stress-kgbatch", type=pathlib.Path, required=True)
	args = parser.parse_args()

	for binary in (args.kgbatch, args.stress_kgbatch):
		if not binary.exists():
			print(f"# lisp-gc-stress-check: {binary} missing, SKIP")
			return 0

	fixed = f"the fixed {ORDINARY_TIMEOUT} s ordinary-run budget"
	with tempfile.TemporaryDirectory() as tmp:
		script = pathlib.Path(tmp) / "gc-stress.el"
		script.write_text(SCRIPT)
		plain_value, plain_stats, plain_seconds = run(
		    args.kgbatch, script, ORDINARY_TIMEOUT, fixed)
		budget = stress_budget(plain_seconds)
		stress_value, stress_stats, stress_seconds = run(
		    args.stress_kgbatch, script, budget,
		    f"{STRESS_TIMEOUT_RATIO}x this build's own "
		    f"{plain_seconds:.2f} s ordinary run, floor "
		    f"{STRESS_TIMEOUT_FLOOR} s, cap {STRESS_TIMEOUT_CAP} s")
		big = pathlib.Path(tmp) / "gc-stress-big.el"
		big.write_text(BIG_SCRIPT)
		big_value, big_stats, _ = run(
		    args.kgbatch, big, ORDINARY_TIMEOUT, fixed)

	problems = []
	if plain_value != EXPECTED:
		problems.append(f"ordinary build answered {plain_value!r}, "
		                f"expected {EXPECTED!r}")
	if stress_value != plain_value:
		problems.append(
		    f"stress build answered {stress_value!r} where the ordinary "
		    f"build answered {plain_value!r}: a collection took something "
		    f"that was still live")
	if big_value != BIG_EXPECTED:
		problems.append(f"ordinary build answered {big_value!r} for the "
		                f"large script, expected {BIG_EXPECTED!r}")
	if big_stats[0] == 0:
		problems.append("ordinary build reported 0 collections over a script "
		                "that fills the arena: either it no longer allocates "
		                "enough or the collector is never invoked")
	if stress_stats[2] or plain_stats[2] or big_stats[2]:
		problems.append(f"allocation failures: ordinary {plain_stats[2]}, "
		                f"stress {stress_stats[2]}, large {big_stats[2]}")
	if stress_stats[0] < MIN_STRESS_COLLECTIONS:
		problems.append(
		    f"stress build collected only {stress_stats[0]} times, under "
		    f"{MIN_STRESS_COLLECTIONS}, so FE_GC_STRESS is probably not "
		    f"compiled in")

	print(f"lisp-gc-stress-check: value {plain_value}; "
	      f"collections {plain_stats[0]} -> {stress_stats[0]}; "
	      f"peak live {plain_stats[1]} -> {stress_stats[1]}; "
	      f"ordinary build over a full arena: {big_stats[0]} collection(s); "
	      f"stress run {stress_seconds:.1f} s of a {budget:.0f} s budget, "
	      f"ordinary run {plain_seconds:.2f} s")
	for problem in problems:
		print(f"  FAIL: {problem}", file=sys.stderr)
	return 1 if problems else 0


if __name__ == "__main__":
	sys.exit(main())
