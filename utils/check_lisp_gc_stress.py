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

Cheap enough for `make check`: three kg processes, well under a second.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

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
      (intern-soft "never-interned-here") (intern-soft keeper) shaped)
"""

EXPECTED = ('(40 (40 20) 2 "kept-name" nil nil '
            '("one,two,three" "one,two,three"))')

# The same script with enough iterations to fill kg's 1 MB arena at least
# once, which the stress-affordable loop above does not: run through the
# ORDINARY build only, to assert the collector is invoked at all.
BIG_SCRIPT = SCRIPT.replace("< n 40", "< n 4000").replace(
    "(* 20 (/ n 20))", "(* 1000 (/ n 1000))")
BIG_EXPECTED = ('(4000 (4000 3000 2000 1000) 4 "kept-name" nil nil '
                '("one,two,three" "one,two,three" "one,two,three" '
                '"one,two,three"))')

# The stress build collects per allocation, and the loop above allocates
# far more than one object per iteration, so its count cannot be near the
# iteration count.  Measured on this box: 0 collections ordinary (the
# arena never fills), 14536 stress.
MIN_STRESS_COLLECTIONS = 1000

# A stress collection is O(arena), and the arena holds tens of thousands
# of objects, so this lane is slow by construction and slowest where it
# matters most.  Measured on this box with the ci-05 MSan lane's own
# binary: the PRELUDE ALONE costs 69 s and 9222 collections before the
# script's first form runs -- that is fixed cost, and it grew with Phase
# 15's library -- and each iteration of the loop above adds about 0.8 s.
# That is why the loop is 40 iterations rather than the 300 it was
# through Phase 14 -- measured under MSan: 300 iterations 229 s (past the
# 120 s this timeout used to be, which is how the regression was found),
# 100 iterations 149 s, 40 iterations 138 s -- and why the timeout now has
# real headroom rather than a little.  The knee is the prelude, not the
# loop, so cutting the loop further buys almost nothing.  A plain build
# runs the whole check in about 1 s.
STRESS_TIMEOUT = 600

STATS = re.compile(r"^arena: collections=(\d+) peak-live=(\d+) failures=(\d+)$")


def run(binary: pathlib.Path, script: pathlib.Path):
	proc = subprocess.run(
	    [str(binary), "-b", "-g", str(script)],
	    capture_output=True, text=True, timeout=STRESS_TIMEOUT)
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
	return value, stats


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--kgbatch", type=pathlib.Path, required=True)
	parser.add_argument("--stress-kgbatch", type=pathlib.Path, required=True)
	args = parser.parse_args()

	for binary in (args.kgbatch, args.stress_kgbatch):
		if not binary.exists():
			print(f"# lisp-gc-stress-check: {binary} missing, SKIP")
			return 0

	with tempfile.TemporaryDirectory() as tmp:
		script = pathlib.Path(tmp) / "gc-stress.el"
		script.write_text(SCRIPT)
		plain_value, plain_stats = run(args.kgbatch, script)
		stress_value, stress_stats = run(args.stress_kgbatch, script)
		big = pathlib.Path(tmp) / "gc-stress-big.el"
		big.write_text(BIG_SCRIPT)
		big_value, big_stats = run(args.kgbatch, big)

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
	      f"ordinary build over a full arena: {big_stats[0]} collection(s)")
	for problem in problems:
		print(f"  FAIL: {problem}", file=sys.stderr)
	return 1 if problems else 0


if __name__ == "__main__":
	sys.exit(main())
