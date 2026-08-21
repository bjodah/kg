#!/usr/bin/env python3
"""Wall-clock benchmarks for kg, reported as JSON.

Deliberately not a CI gate.  The gates in this tree are the counter
assertions in test/test_perf.c: a counter is the same number on an idle
box, inside a sanitizer lane and under valgrind, so it can fail a build
honestly.  A wall time cannot -- five sanitizer lanes driving PTYs at once
already stretch the acceptance suite's timeouts -- so these numbers are
published and read, never enforced.

The binary this drives is test/perfobj/kg (`make bench`), a counting build
compiled with -DKG_PERF_COUNTERS=1.  It is NOT the release build: counter
increments and a different optimisation context make its wall times
comparable only against another counting build.  Every case reports the
counters kg accumulated as well as the time it took, because the counters
are what an optimisation is argued from.

Each case opens a generated corpus in a real pty, waits for the first
frame, sends a key script, and measures the whole process lifetime.  That
includes startup, which is a constant of a few milliseconds and is
reported separately as the `startup` case so it can be subtracted by eye.

A case may name a build feature it needs (`kg -V`'s +words, the same
names `requires_feature:` uses in a PTY case).  A run whose binary lacks
it reports the case as skipped, with the reason, rather than measuring
something else under the case's name: that is what keeps the tree-sitter
cases below (`ts-*`) out of a plain `make bench` while leaving them in
the file.  To measure them, build the counting kg with the backend:

    make bench WITH_TREE_SITTER=1

No other knob: the feature stamp in $(OBJDIR) rebuilds test/perfobj for
the new configuration, so this and a plain `make bench` can alternate.
That build also re-measures every other case against the tree-sitter
backend -- open-comment-c-40k and open-comment-c-40k-edit under both
configurations are the backend comparison, since a case name means the
same workload either way.

Every report opens with an `artifact` header naming what produced the
numbers: `kg -V` verbatim from the measured binary, its sha256, and
`git describe --always --dirty` for this tree and for fe.  A number whose
artifact line is not the tree under discussion is not evidence about it,
so the file says which tree it was rather than leaving the reader to
assume the one they have checked out.
"""

import argparse
import errno
import fcntl
import hashlib
import json
import os
import platform
import resource
import select
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

SCHEMA = "kg-bench/2"
DEFAULT_ROWS, DEFAULT_COLS = 24, 80


# ---------------------------------------------------------------- corpora

def corpus_lines(path, lines, text="the quick brown fox jumps over the lazy dog"):
	with open(path, "w", encoding="utf-8") as fp:
		for i in range(lines):
			fp.write(f"{i:07d} {text}\n")


def corpus_one_long_line(path, size):
	with open(path, "w", encoding="utf-8") as fp:
		fp.write("x" * size + "\n")


def corpus_unicode(path, lines):
	# Combining marks, wide glyphs and a zero-width joiner: the width
	# table's work, not the parser's.
	sample = "café́ 日本語のテキスト 👩‍💻 ünïcödé"
	with open(path, "w", encoding="utf-8") as fp:
		for i in range(lines):
			fp.write(f"{i:05d} {sample}\n")


def corpus_comment_c(path, lines):
	with open(path, "w", encoding="utf-8") as fp:
		for i in range(lines // 4):
			fp.write("/* a multi-line comment that stays open\n")
			fp.write(f" * across several rows, number {i}\n")
			fp.write(" */\n")
			fp.write(f"static int fn_{i}(void) {{ return {i}; }}\n")


def corpus_c_source(path, lines):
	# Ordinary C, not the comment-heavy shape corpus_comment_c() writes:
	# a parser's cost is in the code it has to build nodes for, and a file
	# that is three-quarters comment measures the lexer skipping bytes.
	# Each block declares a struct and defines a function over it, so the
	# tree has real nesting (a loop with an if/else inside a function
	# inside a translation unit) at a realistic density of about one node
	# per token.
	block = (
		"struct node_%(i)d {\n"
		"\tint id;\n"
		"\tchar *name;\n"
		"\tstruct node_%(i)d *next;\n"
		"};\n"
		"\n"
		"static int visit_%(i)d(struct node_%(i)d *n, int depth)\n"
		"{\n"
		"\tint total = 0;\n"
		"\n"
		"\twhile (n != NULL) {\n"
		"\t\tif (n->id %% 3 == 0)\n"
		"\t\t\ttotal += n->id * depth;\n"
		"\t\telse\n"
		"\t\t\ttotal -= depth;\n"
		"\t\tn = n->next;\n"
		"\t}\n"
		"\treturn total;\n"
		"}\n"
		"\n"
	)
	per_block = block.count("\n")
	with open(path, "w", encoding="utf-8") as fp:
		for i in range((lines + per_block - 1) // per_block):
			fp.write(block % {"i": i})


def corpus_tabs(path, lines):
	# Every column boundary is a tab stop recomputation, which is where
	# render-space and display-column diverge from chars-space (see
	# doc/coordinates.md): the wrap-width cache's key is a display width,
	# not a byte count, and this is the corpus that would catch the two
	# being confused.
	with open(path, "w", encoding="utf-8") as fp:
		for i in range(lines):
			fp.write(f"{i:07d}\tone\ttwo\tthree\tfour\tfive\n")


def corpus_invalid_bytes(path, lines):
	# A stray continuation byte and a truncated multi-byte lead spliced
	# into otherwise-ASCII text every line.  display_glyph_at() (src/
	# width.c) substitutes the four-cell "\xnn" escape for both, which
	# wrap_pad() must still budget for at a wrap boundary; this corpus is
	# what exercises that rather than the common well-formed-UTF-8 path
	# unicode-20k does.
	with open(path, "wb") as fp:
		for i in range(lines):
			fp.write(f"{i:07d} valid text ".encode("ascii"))
			fp.write(bytes([0x80 + (i % 0x40), 0xC2]))
			fp.write(b" more text after the bad bytes\n")


def repo_root():
	return Path(__file__).resolve().parent.parent


# ---------------------------------------------------------- Lisp fixtures

# A handful of forms shaped like an ordinary user init: a defvar, two
# defuns (one documented, one interactive), a hook, two key bindings, and
# a small bounded recursion -- not exhaustive, just representative of what
# doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/00d-baselines-and-arena-observability.md
# calls "prelude plus a representative init".  The same text test/test_perf.c's
# arena-margin shape assertions load, so the two baselines describe the
# same workload.
REPRESENTATIVE_INIT = (
	"(defvar my-fill-column 100)"
	"(defun my-greet (name) \"Say hello.\" (message \"hi %s\" name))"
	"(defun my-count-words () (interactive) (message \"n/a\"))"
	# A numeric comparison on purpose, post-Phase-2 (= is chained numeric
	# equality, not assignment) -- the return value is deliberately
	# discarded, since a hook body just needs to do a trivial amount of
	# work. test/test_perf.c keeps an equivalent comment for the same
	# reason.
	"(defun my-before-save-hook () (= my-fill-column my-fill-column))"
	"(add-hook 'before-save-hook 'my-before-save-hook)"
	"(global-set-key \"C-c g\" \"my-greet\")"
	"(global-set-key \"C-c w\" \"my-count-words\")"
	"(defun my-loop (n acc) (if (<= n 0) acc (my-loop (- n 1) (cons n acc))))"
	"(my-loop 25 nil)"
)


def home_files_require(feature):
	"""init.el that adds lisp/ to the load-path and requires FEATURE --
	the shape every "lisp-arena-<package>" case below shares, one per
	shipped lisp/*.el package that is a require TARGET.  prelude.el is
	not one of those: it is embedded (src/lisp_prelude_generated.inc)
	and evaluated before any init file runs, never reached via
	load-path, so "every shipped kg Lisp package" (Phase 21.2 item 3)
	means the other five files in lisp/, and prelude.el's own load-time
	cost is what lisp-arena-prelude and .ci/prelude-startup-census.json
	already measure -- see the block comment above CASES."""
	lisp_dir = repo_root() / "lisp"
	init = f'(add-to-load-path "{lisp_dir}")' f"(require '{feature})"
	return {".config/kg/init.el": init}


def home_files_auto_fill():
	return home_files_require("auto-fill")


def home_files_representative_init():
	return {".config/kg/init.el": REPRESENTATIVE_INIT}


# Phase 21.2 item 9: "an interactive command that calls a small Lisp
# function on every invocation" -- a key bound to a Lisp `defun`, pressed
# repeatedly, as opposed to lisp-command-latency's single M-: round trip or
# lisp-arithmetic-loop's one Lisp-side `while`.  `my-tick' conses one small
# list onto a retained log and bumps a counter; nothing here is reclaimed
# (every case in this file reads lisp_gc_count 1, the post-prelude collect
# and nothing after it -- see the block comment above CASES), so the
# retained log is what a real per-keystroke Lisp hook's garbage would look
# like if nothing ever collected it either.
INTERACTIVE_COMMAND_TICKS = 100
INTERACTIVE_COMMAND_INIT = (
	"(defvar my-tick-count 0)"
	"(defvar my-tick-log nil)"
	"(defun my-tick () (interactive)"
	" (setq my-tick-log (cons (list 'tick my-tick-count) my-tick-log))"
	" (setq my-tick-count (+ my-tick-count 1)))"
	'(global-set-key "C-c t" "my-tick")')


def home_files_interactive_command():
	return {".config/kg/init.el": INTERACTIVE_COMMAND_INIT}


CORPORA = {
	"lines-10k": (lambda p: corpus_lines(p, 10_000), "log.txt"),
	"lines-100k": (lambda p: corpus_lines(p, 100_000), "log.txt"),
	"lines-1m": (lambda p: corpus_lines(p, 1_000_000), "log.txt"),
	"long-line-1mib": (lambda p: corpus_one_long_line(p, 1 << 20), "min.js"),
	"unicode-20k": (lambda p: corpus_unicode(p, 20_000), "utf8.txt"),
	"comment-c-40k": (lambda p: corpus_comment_c(p, 40_000), "big.c"),
	"c-source-40k": (lambda p: corpus_c_source(p, 40_000), "src.c"),
	"tabs-100k": (lambda p: corpus_tabs(p, 100_000), "tabs.txt"),
	"invalid-bytes-20k": (lambda p: corpus_invalid_bytes(p, 20_000), "invalid.txt"),
}

# name -> (corpus or None, keys sent after the first frame).  A case may
# extend the tuple with three optional trailing fields -- (corpus, keys,
# home_files, assert_gt, requires_feature) -- see normalize_case() below;
# every plain 2-tuple case above and below still means "isolated $HOME, no
# init file, nothing to assert, runs on any build".
CASES = {
	"startup": (None, ["\x18\x03"]),
	# ---- Lisp / Fe evaluator (sub-plan 00D) ----
	# Every case here runs with `-Q` (no init file) unless it supplies
	# `home_files`, in which case `run_once()` drops `-Q` so kg actually
	# loads `$HOME/.config/kg/init.el` -- see normalize_case().
	#
	# "startup" above already gives "prelude alone" arena numbers for
	# free now that kg_lisp_perf_snapshot() (src/lisp_core.c) runs before
	# every exit's kg_perf_dump(); this case exists anyway under a name a
	# reader looking for "the prelude-alone baseline" will find.
	#
	# Sub-plan 03F split the old single lisp_peak_eval_depth counter into
	# lisp_peak_frame_depth (Lisp nesting) and lisp_peak_native_reentry
	# (native re-entry).  Re-measured against test/perfobj/kg at this fe
	# pin (Phase 21.2), which moved both baseline numbers this comment
	# used to give -- current tree: the prelude alone is peak_frame_depth
	# 8 and peak_native_reentry 1, both up from the 2/0 measured at the
	# Phase 12 fix cycle. This paragraph used to explain that rise as
	# install_deferred_stubs()' doing (Prelude Phase 1, b94e795: a
	# native that re-entered the evaluator once per deferred name to
	# build its stub closure). Phase A of doc/plans/2026-08-19-fe-
	# simplification-and-cheap-compat.md deleted that loop and both
	# numbers stayed put -- 8 and 1, measured either side of the removal
	# -- so the explanation was wrong. What actually moved them is the
	# prelude growing across Phases 15-20; no single cause is claimed
	# here in its place, because none has been measured. peak_native_
	# reentry remains a high-water mark of SIMULTANEOUS re-entry rather
	# than a count, which is why a prelude that re-enters repeatedly
	# still reads 1, and test_perf.c's test_lisp_prelude_arena_margin
	# pins it at 1 for that reason. fe's own bare-context baseline moving too
	# (per fe's Phase 21.2 commit) is unrelated: fe has no prelude and
	# nothing here claims a shared cause. What matters for this file
	# either way is that a threshold has to clear the CURRENT baseline,
	# not the number a comment last measured -- exactly the trap this
	# paragraph exists to keep in view. That baseline is exactly the
	# reading a case whose key script silently failed to reach the
	# evaluator at all would also show -- the sub-plan 07C trap, a key
	# that turned out to be a prefix map -- so "nonzero" was never the
	# bar; "above the baseline this pin measures" is.
	#
	# frame_depth does not clear it by a useful margin for every shape
	# below: `while`-loop bodies do not grow frame nesting per iteration
	# (that lack of growth is the frame machine's own property), so
	# lisp-arithmetic-loop and lisp-macro-heavy read frame_depth 8 and 10
	# respectively (both barely off the 8-deep baseline above) regardless
	# of whether the loop ran 20000/2000 times or broke after 3 --
	# exactly the silent-no-op failure this assertion exists to catch.
	# Each such case below picks a counter that actually scales with its
	# iteration count instead: lisp_gc_count (the loop's own garbage
	# forces a SECOND collection, on top of the one every case pays for
	# the prelude's own post-prelude collect -- see lisp-arithmetic-loop's
	# own comment for the trap that is, freshly found at this pin) and
	# lisp_arena_peak_live (2000 macro expansions leave thousands of live
	# cells; a handful of iterations would not).
	#
	# This case *is* the baseline peak_frame_depth == 8 reading, so it
	# cannot assert against that threshold the way the cases below do;
	# asserting total_slots instead still catches "Lisp inactive or the
	# snapshot never ran", which would read 0.
	"lisp-arena-prelude": (None, ["\x18\x03"], None,
			       {"lisp_arena_total_slots": 0}),
	# Loading lisp/auto-fill.el nests deeper than the bare prelude
	# (measured peak_frame_depth 33 at this pin, up from 14 at the last
	# measurement -- see the block comment above) and, unlike the cases
	# below, actually exercises native re-entry while doing it (measured
	# peak_native_reentry 1, same as the bare-prelude baseline, so THAT
	# counter does not discriminate this case at all; frame_depth does).
	#
	# The threshold used to be 5, which is now BELOW the bare prelude's
	# own baseline of 8 -- a key script that silently reached nothing
	# would still read frame_depth 8 and this assertion would pass
	# anyway, exactly the "case that cannot fail" defect
	# lisp-macro-heavy had a different shape of (see that case's
	# comment). Raised to 20, matching lisp-arena-representative-init's
	# own margin below: comfortably clear of the baseline, comfortably
	# under the 33 measured with auto-fill genuinely loaded.
	"lisp-arena-auto-fill": (None, ["\x18\x03"], home_files_auto_fill(),
				 {"lisp_peak_frame_depth": 20}),
	# Phase 21.2 item 3 asks for "the representative init and EVERY
	# SHIPPED KG LISP PACKAGE"; auto-fill above was the only lisp/*.el
	# this file benched. These four are the rest of load-path (prelude.el
	# is not a require target -- see home_files_require()'s docstring).
	# Each is the same shape as lisp-arena-auto-fill: `(add-to-load-path
	# ...)(require 'PACKAGE)` as init.el, threshold 20 for the same
	# reason (clears the bare-prelude baseline of 8 with margin, well
	# under every measured value below).
	#
	# Measured peak_frame_depth at this pin: grep-buffer 33, help-fns 33,
	# pipeline 32, pipeline-text 48 (pipeline-text requires pipeline, so
	# its nesting is the deeper of the two -- matching
	# test_lisp_load_time_counters' finding that a chained require is
	# timed inside its outermost one).  All four also read
	# peak_native_reentry 1, same as auto-fill and the bare-prelude
	# baseline -- consistent with that counter being "package loading
	# touches a native at least once", not something that scales with
	# which package.
	"lisp-arena-grep-buffer": (
		None, ["\x18\x03"], home_files_require("grep-buffer"),
		{"lisp_peak_frame_depth": 20}),
	"lisp-arena-help-fns": (
		None, ["\x18\x03"], home_files_require("help-fns"),
		{"lisp_peak_frame_depth": 20}),
	# Pure half of Proof 3 (doc/plans §14): no buffer, no window, no
	# interactive command -- see lisp/pipeline.el's own header.
	"lisp-arena-pipeline": (
		None, ["\x18\x03"], home_files_require("pipeline"),
		{"lisp_peak_frame_depth": 20}),
	# Editor half: requires 'pipeline itself (chained require, like
	# test_lisp_load_time_counters' perfouter/perfinner pair), so this
	# case's arena numbers are "prelude + pipeline + pipeline-text", not
	# pipeline-text alone.
	"lisp-arena-pipeline-text": (
		None, ["\x18\x03"], home_files_require("pipeline-text"),
		{"lisp_peak_frame_depth": 20}),
	# A representative init evaluates real, moderately nested forms
	# (measured peak_frame_depth 54, unchanged from the last measurement:
	# the representative init's own forms dominate its nesting enough
	# that the 2->8 baseline drift above does not move this figure by a
	# margin worth re-measuring). Threshold 20 clears the current
	# baseline of 8 the same way lisp-arena-auto-fill's does now.
	"lisp-arena-representative-init": (
		None, ["\x18\x03"], home_files_representative_init(),
		{"lisp_peak_frame_depth": 20}),
	# Fe evaluation throughput on representative shapes.  Each sends
	# `M-:` (eval-expression), the expression as literal self-insert text
	# in the minibuffer, then RET; the buffer itself is never modified,
	# so `C-x C-c` exits without a save prompt.
	# 150, not a rounder/bigger number, was chosen against a claim this
	# comment used to make -- that the shape's GC-root stack grows
	# linearly with recursion depth and is what bounds it (peak_gc_stack_
	# depth "3914 of the 4096-slot ceiling at n=300, overflows by
	# n=400"). Re-measured at this pin, that claim is backwards: THE ROOT
	# STACK DOES NOT GROW WITH n AT ALL. peak_gc_stack_depth reads 107 --
	# kg's own prelude baseline, identical across every case in this file
	# -- at n=150, n=300, n=400 and n=600 alike (test/perfobj/kg via this
	# file's own bench_case(), each n run as its own case). What actually
	# scales is peak_frame_depth (measured 305/605/805/1087 at those same
	# four n, ~2 per recursion level: `lw`'s body is one `if` wrapping
	# the recursive call, no extra arithmetic frame -- exactly what fe's
	# own Phase 21.2 commit found for the equivalent bare-context shape,
	# now confirmed here for kg's prelude-loaded one too) against the
	# fixed frame_capacity of 1087: n=600 saturates it and both
	# test/kgbatch and this pty path raise "evaluation frame limit
	# exceeded" somewhere between n=520 (still fits) and n=540-545
	# (does not; the two entry paths' own frame overhead differs by a
	# handful, which is why this is a range and not one number). 150
	# leaves about 72% of frame_capacity free (305 of 1087) while still
	# being a real multi-hundred-cell walk; see test/test_perf.c's
	# identical expression, whose own comment carried the same stale
	# claim and is fixed alongside this one.
	"lisp-list-walk": (None, [
		"\x1b:",
		"(defun lw (n l) (if (<= n 0) l (lw (- n 1) (cons n l)))) "
		"(length (lw 150 nil))\r",
		"\x18\x03",
	], None, {"lisp_peak_frame_depth": 200, "lisp_minibuffer_eval": 0}),
	# Iterative, not recursive: peak_frame_depth stays flat at 8 (the
	# bare-prelude baseline itself, per the block comment above)
	# regardless of iteration count, so it cannot be the signal.
	# The GC assertion this case carried is GONE, and the removal is a
	# measurement rather than a relaxation. It asserted the loop's own
	# `+` garbage forcing a collection (lisp_gc_count > 1, raised from
	# > 0 when kg_lisp_init()'s post-prelude collect began being counted
	# before any case's key script runs). Since Phase B of
	# doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md grew
	# the default arena to 10 MiB (~440k slots, ~430k free after the
	# prelude), that collection is unreachable BY CONSTRUCTION for any
	# loop that completes: KG_LISP_STEP_LIMIT is 1 << 20 steps and each
	# iteration costs at least ~5 of them, so a budget-completing loop
	# allocates at most ~200k slots -- measured 90k for this one --
	# while pushing past the free space needs >430k. Every variant tried
	# (200000 scalar iterations; discarded-list bodies of 3, 10 and 16
	# elements per iteration, which trade steps for slots at just under
	# 1 slot/step) truncated at the step limit or peaked below the line,
	# and a truncated run measures nothing. What the case CAN still
	# witness is allocation volume far beyond the startup constant:
	# lisp_arena_peak_live reads 11865 at startup (-Q, quit immediately)
	# and 90551 here, so the floor below sits well between them with
	# margin for prelude rewrites on both sides.
	"lisp-arithmetic-loop": (None, [
		"\x1b:",
		"(setq i 0) (setq acc 0) (while (< i 20000) (setq acc (+ acc i)) "
		"(setq i (+ i 1))) acc\r",
		"\x18\x03",
	], None, {"lisp_arena_peak_live": 50000, "lisp_minibuffer_eval": 0}),
	# Also iterative (peak_frame_depth 10, same non-signal as the
	# arithmetic loop above): fe.c re-expands a macro on every call
	# (doc/fe-upstream.md) rather than rewriting the call site, so 2000
	# calls leave 2000 expansions' worth of garbage live at once.
	#
	# Lisp-2 spelling, not the Lisp-1 shape a prior version of this case
	# used: `(setq m (macro (x) ...))` binds the VARIABLE m to an
	# anonymous macro object without installing anything in the function
	# namespace, so `(m n)` raised void-function on its very first
	# iteration -- fe has resolved call position in the function
	# namespace since Phase 4 (doc/fe-upstream.md) and kg's prelude adds
	# no Lisp-1 fallback. Measured with that spelling at commit dd35a2b's
	# fe pin: `test/kgbatch -r -a` on the equivalent expression reported
	# `E:void-function` and `peak-live=6819` -- the bare prelude's own
	# figure -- against the `lisp_arena_peak_live` >= 10000 this case
	# asserted, so it would have FAILED if anything had run it; nobody
	# noticed because `make bench` is deliberately not a CI step. `defmacro`
	# is the fix: it expands to `(defalias 'NAME (macro PARAMS . BODY))`
	# (lisp/prelude.el), which puts `m` in the function namespace the way
	# fe's own respelling of this shape (perf_workloads.c's
	# `(fset 'm (macro (x) ...))`) does, and is the same spelling
	# test/test_perf.c's evaluator-shapes case already used -- that C test
	# runs in-process and was never affected by this bug. Re-measured with
	# the repair, same pin: lisp_arena_peak_live 24101 through this file's
	# own `test/perfobj/kg` M-: path (24138 through kgbatch's `-r -b`
	# wrapper instead -- a few hundred cells apart because the two
	# harnesses wrap the expression differently, not because either is
	# nondeterministic), comfortably above the bare prelude's 6819 and
	# every non-macro case's low thousands; a truncated run (a handful of
	# expansions rather than 2000) would peak far below 15000.
	"lisp-macro-heavy": (None, [
		"\x1b:",
		"(defmacro m (x) (list '+ x 1)) (setq n 0) (setq i 0) "
		"(while (< i 2000) (setq n (m n)) (setq i (+ i 1))) n\r",
		"\x18\x03",
	], None, {"lisp_arena_peak_live": 15000, "lisp_minibuffer_eval": 0}),
	# Deep call chain: 300 levels of non-tail self-recursion; measured
	# peak_frame_depth 904 (~3 frames per level: `if`, `+`, and the
	# recursive call each open one -- see test/test_perf.c's identical
	# expression), comfortably above frame_capacity's other consumers.
	"lisp-deep-call-chain": (None, [
		"\x1b:",
		"(defun dc (n) (if (<= n 0) 0 (+ 1 (dc (- n 1))))) (dc 300)\r",
		"\x18\x03",
	], None, {"lisp_peak_frame_depth": 500, "lisp_minibuffer_eval": 0}),
	# Representative command latency: the minibuffer round trip and eval
	# dispatch on a trivial expression, with none of the above shapes'
	# own cost mixed in. Deliberately as shallow as the prelude's own
	# deepest call (nesting 8, per the block comment above), so --
	# unlike the shapes above -- peak_frame_depth cannot be the
	# non-trivial-execution signal here.
	#
	# `lisp_arena_total_slots > 0` USED TO BE the whole assertion, and it
	# is not an assertion at all: the arena is sized at kg_lisp_init()
	# and total_slots reads 56147 after a startup that then exits
	# immediately. The Phase 21 adversarial review (finding 4)
	# demonstrated the consequence -- this case's own `assert_gt` and its
	# own LISP_ANSWERS entry both PASSED against an exit-only key script
	# of `["\x18\x03"]`, so a broken key sequence would have left a
	# startup benchmark wearing the name of a command round trip.
	# `lisp_minibuffer_eval` (src/perf.h, incremented only where
	# eval-expression reaches a value) is zero for that script and 1 for
	# this one, which is the difference the case is named for. The arena
	# gauge stays beside it: it costs nothing and still says "Lisp is
	# compiled in and initialised", which is the precondition rather than
	# the measurement.
	"lisp-command-latency": (None, ["\x1b:", "(+ 1 2)\r", "\x18\x03"],
				 None, {"lisp_arena_total_slots": 0,
					"lisp_minibuffer_eval": 0}),
	# Phase 21.2 item 9: a key-bound interactive command that calls a
	# small Lisp function on every invocation, pressed
	# INTERACTIVE_COMMAND_TICKS (100) times -- the shape of a real
	# per-keystroke Lisp hook,
	# as opposed to lisp-command-latency's single M-: round trip above or
	# lisp-arithmetic-loop's one Lisp-side `while' loop. See
	# home_files_interactive_command()'s comment for what `my-tick' does
	# and why it retains its own garbage.
	#
	# peak_frame_depth is NOT the signal here (measured flat at 13
	# regardless of tick count, the same non-scaling this file's other
	# iterative shapes hit -- see the block comment above CASES): a
	# command handler returns to the top level between invocations, so
	# nesting never accumulates. lisp_arena_peak_live is, but only past
	# a threshold: per this file's own corrected understanding (see
	# lisp-list-walk and lisp-arithmetic-loop's comments for the same
	# trap in different shapes), peak_live_objects is a HIGH-WATER MARK
	# since kg_lisp_init(), and the prelude's own construction already
	# set it to 6819 before any user code runs -- a handful of ticks
	# does not exceed that, so lisp_arena_peak_live reads exactly 6819,
	# UNCHANGED, for 20 and 30 ticks (measured). It moves once retained
	# per-tick garbage exceeds that old peak: measured 6819 (0/20/30
	# ticks), 6906 (50), 7236 (80), 7456 (100), 8006 (150) via
	# test/perfobj/kg. 100 ticks clears the baseline by 637 cells, a
	# real margin without paying more than 100 keys' worth of this
	# harness's 0.06s-per-key pacing (~6s, the same order
	# yank-multiline-100k's 200 keys already costs).
	"lisp-interactive-command": (
		None, ["\x03t"] * INTERACTIVE_COMMAND_TICKS + ["\x18\x03"],
		home_files_interactive_command(),
		{"lisp_arena_peak_live": 7200}),
	"open-lines-10k": ("lines-10k", ["\x18\x03"]),
	"open-lines-100k": ("lines-100k", ["\x18\x03"]),
	"open-long-line-1mib": ("long-line-1mib", ["\x18\x03"]),
	"open-unicode-20k": ("unicode-20k", ["\x18\x03"]),
	"open-comment-c-40k": ("comment-c-40k", ["\x18\x03"]),
	# End of buffer, then back to the top: a scroll over every row.
	"scroll-lines-100k": ("lines-100k", ["\x1b>", "\x1b<", "\x18\x03"]),
	# Visual-line mode on a 100k-line file, then end of buffer: the
	# geometry scans the visual-line index plan is about.  Off by default, which
	# is why it is a case of its own rather than part of the others.
	"visual-line-100k": ("lines-100k",
			     ["\x1bx", "visual-line-mode\r", "\x1b>",
			      "\x10\x10\x10", "\x18\x03"]),
	# Open a block comment at the top of a comment-heavy C file: the
	# downstream hl_oc propagation counter used by syntax benchmarks.
	"open-comment-c-40k-edit": ("comment-c-40k",
				    ["\x1b<", "/*", "\x18\x03", "y"]),
	# Yank a 5-line region 200 times into a 100k-line file: the
	# multiline insertion path, which used to serialise the whole
	# buffer and rebuild every row for each one.
	"yank-multiline-100k": ("lines-100k", ["\x1b<", "\x00", "\x0e" * 5,
					      "\x1bw", "\x1b>"]
			       + ["\x19"] * 200 + ["\x18\x03", "y"]),
	# Type into the middle of a 1 MiB row: the render/highlight rebuild.
	# The trailing "y" answers the modified-buffer prompt C-x C-c raises
	# once the row has been edited.
	"type-in-long-line": ("long-line-1mib", ["hello", "\x18\x03", "y"]),

	# ---- Visual-line geometry matrix (plan 07 phase 0) ----
	# visual-line-100k above is the historical aggregate case: one repaint
	# that mixes every shape below, kept as-is for before/after
	# continuity.  Each case here isolates one shape, so a scan/byte
	# count regression shows up in the shape that caused it.
	#
	# Warm, unchanged repaint: cursor motion within the already-painted
	# top of the buffer, no edit and no width change.  This is the case
	# phase 1's cache exists for.
	"visual-line-warm-100k": ("lines-100k",
				  ["\x1bx", "visual-line-mode\r",
				   "\x0e" * 5, "\x10" * 5, "\x18\x03"]),
	# One-row edit, then the repaint it forces.
	"visual-line-edit-100k": ("lines-100k",
				  ["\x1bx", "visual-line-mode\r", "\x1b>",
				   "x", "\x18\x03", "y"]),
	# A width change mid-session: TIOCSWINSZ delivers SIGWINCH the way a
	# real terminal resize does, which is the case that must cost at most
	# one cold scan per row rather than reusing the old width's cache.
	"visual-line-resize-100k": ("lines-100k",
				    ["\x1bx", "visual-line-mode\r", "\x1b>",
				     ("settle",), ("resize", 24, 40), "\x18\x03"]),
	# Two windows on the same buffer at unequal widths: an 80-column
	# vertical split gives 39 and 40 (win_reflow() hands the split
	# remainder to the last column group), so this is "two widths" with
	# no extra harness support.  One entry per row can thrash when the
	# two windows repaint alternately -- this case is what phase 1's "do
	# not add multiple entries preemptively" note is measured against.
	"visual-line-vsplit-100k": ("lines-100k",
				    ["\x1bx", "visual-line-mode\r",
				     "\x18", "3", "\x0e" * 5,
				     "\x18", "o", "\x0e" * 5, "\x18\x03"]),
	# Horizontal split: two windows, same width, stacked.
	"visual-line-hsplit-100k": ("lines-100k",
				    ["\x1bx", "visual-line-mode\r",
				     "\x18", "2", "\x0e" * 5,
				     "\x18", "o", "\x0e" * 5, "\x18\x03"]),
	# Four windows (vertical split, then each half split horizontally):
	# the window-count end of the matrix.
	"visual-line-4win-100k": ("lines-100k",
				  ["\x1bx", "visual-line-mode\r",
				   "\x18", "3", "\x18", "2",
				   "\x18", "o", "\x18", "2",
				   "\x0e" * 3, "\x18\x03"]),
	# Buffer top, then middle (M-g goto-line), then end: the position end
	# of the matrix, each its own case rather than folded into one so a
	# regression at one position does not hide inside the others.
	"visual-line-pos-top-100k": ("lines-100k",
				     ["\x1bx", "visual-line-mode\r", "\x18\x03"]),
	# M-g is a prefix map, not goto-line directly (kbd.c's "M-g g" /
	# "M-g M-g"), so the key script has to spell the full sequence or
	# "50000\r" lands as literal self-insert instead of the goto-line
	# minibuffer, which then leaves the modified-buffer save prompt
	# unanswered and the process never exits.
	"visual-line-pos-middle-100k": ("lines-100k",
					["\x1bx", "visual-line-mode\r",
					 "\x1bg", "g", "50000\r", "\x18\x03"]),
	"visual-line-pos-end-100k": ("lines-100k",
				     ["\x1bx", "visual-line-mode\r", "\x1b>",
				      "\x18\x03"]),
	# Corpora: tabs (render-space vs. display-column divergence) and
	# invalid bytes (the escaped "\xnn" spelling's four-cell width),
	# alongside unicode-20k above for wide/combining glyphs.
	"visual-line-tabs-100k": ("tabs-100k",
				  ["\x1bx", "visual-line-mode\r", "\x1b>",
				   "\x18\x03"]),
	"visual-line-invalid-bytes-20k": ("invalid-bytes-20k",
					  ["\x1bx", "visual-line-mode\r",
					   "\x1b>", "\x18\x03"]),
	"visual-line-unicode-20k": ("unicode-20k",
				    ["\x1bx", "visual-line-mode\r", "\x1b>",
				     "\x10" * 5, "\x18\x03"]),

	# ---- tree-sitter backend ----
	# `requires_feature: tree-sitter` here means what it means in a PTY
	# case: a binary that reports -tree-sitter reports these as skipped
	# with the reason, so a plain `make bench` still runs clean.  Build
	# the counting kg with `make bench WITH_TREE_SITTER=1` to measure
	# them.
	#
	# Both cases exist because the tree-sitter plan's latency policy
	# (doc/plans/kg-tree-sitter-plan.md, Refinement "Latency policy") is
	# to parse synchronously with no timeout and no cancellation, and to
	# defer a cancellation/budget mechanism until a measurement says one
	# is needed.  These are that measurement, and the place to re-read
	# before anyone reopens that question: the open case is the one full
	# parse a load pays, the edit case is what an ordinary keystroke
	# costs on top of it.
	#
	# Times: the harness paces keys at a flat 0.06 s (run_once()), which
	# it must -- under 30 ms kg decides it is watching a paste and turns
	# auto-indent and autocompletion off, so a faster script would stop
	# measuring typing.  The edit case's wall time is therefore mostly
	# that pacing, and its per-keystroke reading is (wall time - the same
	# script without the typing) / keys, one subtraction further than the
	# `startup` constant every other case is read against.  Measured that
	# way on this box: 20 keystrokes cost 1231 ms against 1260 ms of
	# pacing for the entries they occupy -- under a millisecond each, at
	# or below what the pacing can resolve, and the low end of the
	# 1.3-5.6 ms slice 7 measured ad hoc.  Which is exactly why the
	# counters are the durable half of both cases: ts_parse minus
	# ts_full_parse is the number of incremental reparses (measured 20
	# here, one per keystroke, on top of the load's one full parse), and
	# ts_rehighlight_row is the damage window they authorised (40, two
	# rows per edit).  The open case's own reading is 319 ms against a
	# 113 ms `startup`: about 205 ms to parse and paint 40k lines /
	# 545 KB, paid once at load.
	"ts-open-c-40k": ("c-source-40k", ["\x18\x03"], None,
			  # Loading a file has no old tree to reuse, so its
			  # parse is a full one.  Asserting that distinguishes
			  # "measured a 40k-line parse" from "measured a build
			  # whose grammar failed to load", which would still
			  # open the file, still exit 0, and read zero here.
			  {"ts_full_parse": 0}, "tree-sitter"),
	# Type an identifier's worth of characters, one keystroke per entry,
	# in the middle of the same file (M-g g, spelled in full because M-g
	# is a prefix map -- see visual-line-pos-middle-100k).  ("settle",)
	# after the jump for the same reason the resize cases use it: the
	# repaint the jump provokes must have landed before the first
	# keystroke, or the case measures it inside the first edit.  The
	# trailing "y" answers the modified-buffer prompt.
	"ts-edit-c-40k": ("c-source-40k",
			  ["\x1bg", "g", "20000\r", ("settle",)]
			  + list("int extra_field = 0;")
			  + ["\x18\x03", "y"], None,
			  # One incremental reparse per keystroke is the shape
			  # this case pins: 20 keystrokes on top of the load's
			  # single full parse.  A script that silently reached
			  # nothing -- the trap bench_case()'s assert_gt
			  # docstring describes -- would read ts_parse 1 and
			  # ts_rehighlight_row 0.
			  {"ts_parse": 10, "ts_rehighlight_row": 0},
			  "tree-sitter"),
}
BIG_CASES = {
	"open-lines-1m": ("lines-1m", ["\x18\x03"]),
	# The 1M-line counterpart to visual-line-100k: the corpus the plan's
	# 583k-rows/30MB-per-repaint evidence was measured against.
	"visual-line-1m": ("lines-1m",
			   ["\x1bx", "visual-line-mode\r", "\x1b>",
			    "\x10" * 5, "\x18\x03"]),
	# The 1M-line counterpart to visual-line-edit-100k: sub-plan 07-C's
	# question is whether content_generation's O(rows) integer rebuild
	# per edit is material, and the only corpus where that walk could
	# plausibly be felt is this one -- the 100k-line matrix above has no
	# case that both builds the index and then edits, at this row count.
	"visual-line-edit-1m": ("lines-1m",
				["\x1bx", "visual-line-mode\r", "\x1b>",
				 "x", "\x18\x03", "y"]),
	# The 1M-line counterpart to visual-line-hsplit-100k: two windows on
	# the *same* buffer at the *same* width, which is exactly the
	# duplicate-vector case sub-plan 07-C's memory question is about (the
	# vsplit/4win cases above are unequal widths -- legitimately distinct
	# keys, not the dedup opportunity a global (buffer,width) LRU would
	# address).
	"visual-line-hsplit-1m": ("lines-1m",
				  ["\x1bx", "visual-line-mode\r",
				   "\x18", "2", "\x0e" * 5,
				   "\x18", "o", "\x0e" * 5, "\x18\x03"]),
}


def typed_expression(name):
	"""The expression text an `M-:` case in CASES types, recovered from
	its own key script rather than kept as a second copy: `keys[0]` is
	the `M-:` prefix and `keys[1]` is the typed text with a trailing RET,
	so LISP_ANSWERS below checks the exact source a pty case runs -- the
	same trap this file's other duplicated-text case (REPRESENTATIVE_INIT,
	kept identical to test/test_perf.c's copy only by a comment saying so)
	exists to avoid where a second copy is avoidable at all."""
	keys = CASES[name][1]
	assert keys[0] == "\x1b:"
	return keys[1].rstrip("\r")


# Phase 21.2's rule ("each workload checks its answer") for the seven Lisp
# cases above whose shape has one: name -> (source, expected value).
# `kgbatch_answer()` (see bench_case()) evaluates `source` through
# `test/kgbatch -r -b`, which needs no terminal and runs the identical
# kg_lisp_eval_string() seam the M-: minibuffer path does, and compares its
# `V:` payload byte for byte.
#
# WHAT THAT PROVES, EXACTLY: that this expression, under kg's Lisp, at this
# build, evaluates to this value. It is a VALUE ORACLE and nothing more --
# a SEPARATE test/kgbatch process, started after every measured run has
# already exited, so it cannot say anything at all about what the measured
# process did. The Phase 21 adversarial review (finding 4) is the
# demonstration: an answer of "3" was returned for a case whose key script
# was `C-x C-c` and nothing else. What proves the measured process ran the
# expression is the `lisp_minibuffer_eval` counter every M-: case above now
# asserts; the two are complementary and neither substitutes for the other
# (the counter cannot tell a right answer from a wrong one at the same
# cost, which is the bug the fe pin's commit message found in the old
# `lisp-macro-heavy` case).
#
# Deliberately NOT a check of the pty's own screen output: for an `M-:`
# case the typed expression's own digits are already present in the
# terminal stream before RET is ever sent (they are the self-insert echo of
# what was typed), so grepping the whole session's bytes for "150" would
# pass whether or not evaluation happened at all -- exactly the silent-no-op
# failure mode assert_gt's docstring describes for counters, here for the
# computed value instead.
#
# "lisp-arena-prelude" has no expression to check (its key script is just
# `C-x C-c`) and is not here, matching fe's own context-open workload,
# whose `answer` field is likewise not a computed value.
LISP_ANSWERS = {
	"lisp-list-walk": (typed_expression("lisp-list-walk"), "150"),
	"lisp-arithmetic-loop": (
		typed_expression("lisp-arithmetic-loop"), "199990000"),
	"lisp-macro-heavy": (typed_expression("lisp-macro-heavy"), "2000"),
	"lisp-deep-call-chain": (
		typed_expression("lisp-deep-call-chain"), "300"),
	"lisp-command-latency": (typed_expression("lisp-command-latency"), "3"),
	# The same text home_files_representative_init() plants as init.el,
	# evaluated directly rather than through kg's init-file loader (which
	# kgbatch, having no $HOME of its own, does not run) -- kgbatch's
	# wrapper is `(progn FILE-CONTENTS)`, the same "run every form, answer
	# the last one's value" shape `-Q`-less kg gives an init file, so this
	# is the same evaluation the pty case's init.el gets, not a different
	# one. `my-loop` conses 1..25 in ascending order (it decrements n from
	# 25, so the SMALLEST n is consed LAST, ending up at the list's head).
	"lisp-arena-representative-init": (
		REPRESENTATIVE_INIT,
		"(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25)"),
	# The same require home_files_auto_fill() plants as init.el, with one
	# more form appended to turn "did not raise" into an answer: both the
	# `provide` and one function the package defines.
	"lisp-arena-auto-fill": (
		home_files_auto_fill()[".config/kg/init.el"]
		+ "(and (featurep 'auto-fill) (fboundp 'auto-fill-mode))",
		"t"),
	# Same "featurep and fboundp" shape as auto-fill above for the two
	# packages with no natural numeric answer.
	"lisp-arena-grep-buffer": (
		home_files_require("grep-buffer")[".config/kg/init.el"]
		+ "(and (featurep 'grep-buffer) (fboundp 'grep-buffer-word-at-point))",
		"t"),
	"lisp-arena-help-fns": (
		home_files_require("help-fns")[".config/kg/init.el"]
		+ "(and (featurep 'help-fns) (fboundp 'describe-function))",
		"t"),
	# pipeline.el is pure (no buffer, no window, no interactive command --
	# see its header), so its answer check actually RUNS the package
	# instead of only asking whether it loaded: pipeline-adder and
	# pipeline-scaler are the closures its own header names as the
	# closures bullet, funcalled left to right by pipeline-run, the fold
	# its header names as the funcall/apply bullet -- (5+3)*2 = 16.
	"lisp-arena-pipeline": (
		home_files_require("pipeline")[".config/kg/init.el"]
		+ "(pipeline-run (list (pipeline-adder 3) (pipeline-scaler 2)) 5)",
		"16"),
	# pipeline-text.el's commands read the current buffer (see its
	# header); kgbatch's -b gives it one, but an empty one is enough for
	# the same feature+fboundp shape the other non-numeric packages use.
	"lisp-arena-pipeline-text": (
		home_files_require("pipeline-text")[".config/kg/init.el"]
		+ "(and (featurep 'pipeline-text) (featurep 'pipeline)"
		  " (fboundp 'pipeline-text-run))",
		"t"),
	# The same definitions the pty case's init.el installs, but calling
	# `my-tick' directly INTERACTIVE_COMMAND_TICKS times with `dotimes'
	# rather than through a real key press each time -- kgbatch has no
	# terminal to bind "C-c t" through, so this checks the same function
	# was called the same number of times, not that key dispatch itself
	# worked (the pty case's own counter check is what proves that).
	"lisp-interactive-command": (
		INTERACTIVE_COMMAND_INIT
		+ f"(dotimes (i {INTERACTIVE_COMMAND_TICKS}) (my-tick))"
		  " my-tick-count",
		str(INTERACTIVE_COMMAND_TICKS)),
}


# ------------------------------------------------------------------- run

def set_winsize(fd, rows, cols):
	fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def drain(fd, deadline, quiet_for=0.0):
	"""Read until EOF or `deadline`.  Returns the bytes read.

	With `quiet_for`, stop early once the child has produced nothing for
	that long -- which is how the first painted frame is detected without
	sleeping a fixed startup delay.
	"""
	out = bytearray()
	last = time.monotonic()
	while True:
		now = time.monotonic()
		if now > deadline:
			return bytes(out)
		if quiet_for and out and now - last > quiet_for:
			return bytes(out)
		ready, _, _ = select.select([fd], [], [], 0.01)
		if not ready:
			continue
		try:
			chunk = os.read(fd, 65536)
		except OSError as exc:
			if exc.errno in (errno.EIO, errno.EBADF):
				return bytes(out)
			raise
		if not chunk:
			return bytes(out)
		out += chunk
		last = time.monotonic()


def wait_or_kill(pid, deadline):
	"""Reap `pid`, SIGKILLing it if it has not exited by `deadline`.

	Returns (timed_out, status, rusage).  Never a bare blocking wait: a
	case whose key script does not reach an exit -- an unanswered
	prompt, say -- would otherwise hang the whole run forever.
	"""
	while time.monotonic() < deadline:
		done, status, usage = os.wait4(pid, os.WNOHANG)
		if done == pid:
			return False, status, usage
		time.sleep(0.01)
	os.kill(pid, signal.SIGKILL)
	_, status, usage = os.wait4(pid, 0)
	return True, status, usage


def run_once(kg, argv, env, rows, cols, keys, timeout):
	"""Spawn kg on a pty, send `keys`, and return (seconds, max_rss_kb).

	A `keys` entry may be a ("resize", rows, cols) tuple instead of a
	string: TIOCSWINSZ on the pty master mid-session, which delivers
	SIGWINCH to the foreground process group the way a real terminal
	resize does.  This is what the visual-line-resize-* cases use to
	measure a width change without restarting kg.

	A `keys` entry may also be a ("settle",) tuple: drain until quiet
	instead of the flat 0.06s every other key gets.  A jump to the end
	of a 100k-line buffer does not finish redrawing in 0.06s, and a case
	that immediately follows one with a width-sensitive action (a resize,
	an edit whose row-scan count the case is measuring) needs that
	redraw to have actually landed first, the way the initial "wait for
	the first frame" wait does -- confirmed by instrumenting
	visual_line_width() directly: without this, the resize case's own
	drain (below) faithfully caught a repaint, just the repaint of the
	still-in-flight end-of-buffer jump instead of the resize.
	"""
	pid, fd = os.forkpty()
	if pid == 0:  # child
		try:
			os.execve(kg, [kg] + argv, env)
		finally:
			os._exit(127)
	set_winsize(fd, rows, cols)
	start = time.monotonic()
	deadline = start + timeout
	try:
		drain(fd, min(deadline, start + 5.0), quiet_for=0.05)
		for key in keys:
			if isinstance(key, tuple) and key[0] == "resize":
				set_winsize(fd, key[1], key[2])
				# TIOCSWINSZ only queues pending_resize (src/tty.c);
				# kg does not apply it and redraw until its blocking
				# read() next returns, and a full-buffer repaint at
				# the new width is not instant.  A flat 0.06s here
				# raced that redraw badly enough that the resize
				# cases measured almost no width change at all --
				# wait for the frame it provokes the way the
				# initial startup wait does.
				drain(fd, min(deadline, time.monotonic() + 2.0),
				      quiet_for=0.1)
			elif isinstance(key, tuple) and key[0] == "settle":
				drain(fd, min(deadline, time.monotonic() + 3.0),
				      quiet_for=0.2)
			else:
				try:
					os.write(fd, key.encode("utf-8"))
				except OSError:
					break  # kg has already gone
				time.sleep(0.06)
		drain(fd, deadline)
		timed_out, status, usage = wait_or_kill(pid, deadline)
	finally:
		try:
			os.close(fd)
		except OSError:
			pass
	elapsed = time.monotonic() - start
	if timed_out:
		raise RuntimeError(f"kg did not exit within {timeout} s")
	if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
		raise RuntimeError(f"kg exited with status {status}")
	return elapsed, usage.ru_maxrss


def percentile(values, pct):
	ordered = sorted(values)
	if len(ordered) == 1:
		return ordered[0]
	pos = (len(ordered) - 1) * pct / 100.0
	low = int(pos)
	high = min(low + 1, len(ordered) - 1)
	return ordered[low] + (ordered[high] - ordered[low]) * (pos - low)


def kgbatch_answer(kgbatch, source, timeout=10.0):
	"""Evaluate `source` through `test/kgbatch -r -b` and return the tagged
	record it prints, as (kind, value): ("V", printed value), ("E",
	condition symbol) or ("Q", None).

	Same output contract test/kgbatch.c's header comment documents and
	utils/check_lisp_oracle.py's run_kg_case() already parses: on exit 0
	stdout is `PATH: V:VALUE`/`PATH: E:CONDITION`/`PATH: Q:quit`, `-r`'s
	wrapper is `(condition-case ... (format "V:%S" (progn SOURCE)) ...)`,
	so the answer is SOURCE's last form's value. `-b` gives it a live
	scratch buffer for parity with that caller; none of LISP_ANSWERS'
	sources below read or move point.
	"""
	with tempfile.NamedTemporaryFile(mode="w", suffix=".el", delete=False,
					 encoding="utf-8") as tmp:
		tmp.write(source)
		tmp_path = tmp.name
	try:
		proc = subprocess.run([kgbatch, "-r", "-b", tmp_path],
				      capture_output=True, timeout=timeout)
	finally:
		os.unlink(tmp_path)
	prefix = f"{tmp_path}: "
	stream = proc.stdout if proc.returncode == 0 else proc.stderr
	text = stream.decode("utf-8", "replace").strip("\n")
	if not text.startswith(prefix):
		raise RuntimeError(
			f"kgbatch produced unparseable output: {text!r}")
	payload = text[len(prefix):]
	if payload.startswith("V:"):
		return "V", payload[2:]
	if payload.startswith("E:"):
		return "E", payload[2:]
	if payload == "Q:quit":
		return "Q", None
	raise RuntimeError(f"kgbatch printed an invalid record {payload!r}")


def bench_case(kg, name, corpus_path, keys, runs, rows, cols, timeout,
	       home_files=None, assert_gt=None, kgbatch=None, answer=None):
	"""Run one case `runs` times and report wall time, RSS and counters.

	`home_files` (relative path -> content) plants files under a fresh
	$HOME before every run -- e.g. `.config/kg/init.el` -- and, when
	given, `-Q` is dropped so kg actually loads it; every other case
	still runs `-Q`, isolated, the way every case did before sub-plan
	00D. `assert_gt` (counter name -> minimum) raises if the counter
	kg reported is not strictly greater than that minimum in every run:
	the trap sub-plan 07C already found once -- a key script that
	silently reached nothing, so the case measured the startup constant
	instead of what it was named for.

	`answer`, when given, is a (source, expected) pair from LISP_ANSWERS,
	checked once via `kgbatch_answer()` after every measured run has
	exited. It is a VALUE ORACLE, not a witness: the check runs in a
	SEPARATE `test/kgbatch` process, so what it establishes is that this
	expression evaluates to this value under kg's Lisp at this build --
	never that the process whose time and counters are reported above
	evaluated anything. The Phase 21 adversarial review (finding 4)
	showed the gap by returning a correct answer for a case driven with
	an exit-only key script. `assert_gt` is the half that speaks for the
	measured process, which is why every `M-:` case in CASES asserts
	`lisp_minibuffer_eval`; the oracle is still worth having, since a
	counter threshold alone cannot tell a workload that computed the
	right answer from one that computed the WRONG thing at exactly the
	same cost -- the bug the fe pin's commit message found in the
	`lisp-macro-heavy` case this file used to have.
	"""
	times, rss, counters = [], 0, {}
	with tempfile.TemporaryDirectory() as tmp:
		for relpath, content in (home_files or {}).items():
			path = os.path.join(tmp, relpath)
			os.makedirs(os.path.dirname(path), exist_ok=True)
			with open(path, "w", encoding="utf-8") as fp:
				fp.write(content)
		for _ in range(runs):
			out = os.path.join(tmp, "perf.json")
			env = dict(os.environ)
			env["KG_PERF_OUT"] = out
			env["TERM"] = "xterm-256color"
			env["HOME"] = tmp
			flags = [] if home_files else ["-Q"]
			argv = flags + ([str(corpus_path)] if corpus_path else [])
			seconds, maxrss = run_once(
				kg, argv, env, rows, cols, keys, timeout)
			times.append(seconds * 1000.0)
			rss = max(rss, maxrss)
			if os.path.exists(out):
				with open(out, "r", encoding="utf-8") as fp:
					counters = json.load(fp)
			for counter_name, minimum in (assert_gt or {}).items():
				value = counters.get(counter_name, 0)
				if not value > minimum:
					raise RuntimeError(
						f"bench case {name!r}: {counter_name} was "
						f"{value}, expected > {minimum} -- this case "
						"is measuring nothing beyond the startup "
						"constant (see bench_case()'s assert_gt "
						"docstring)")
	answer_value = None
	if answer is not None:
		source, expected = answer
		if not kgbatch or not os.path.isfile(kgbatch):
			raise RuntimeError(
				f"bench case {name!r} needs test/kgbatch to check its "
				"answer (run 'make kgbatch', or pass --kgbatch)")
		kind, value = kgbatch_answer(kgbatch, source)
		if kind != "V" or value != expected:
			raise RuntimeError(
				f"bench case {name!r}: kgbatch answered "
				f"{kind}:{value!r}, expected V:{expected!r} -- this "
				"workload computed the wrong thing (or raised, or "
				"quit), which its counters alone would not show")
		answer_value = value
	size = os.path.getsize(corpus_path) if corpus_path else 0
	return {
		"name": name,
		"input": {"path": str(corpus_path) if corpus_path else None,
			  "bytes": size},
		"dimensions": [rows, cols],
		"runs": runs,
		"wall_ms": {
			"median": round(statistics.median(times), 2),
			"p95": round(percentile(times, 95), 2),
			"min": round(min(times), 2),
		},
		"max_rss_kb": rss,
		"answer": answer_value,
		"counters": counters,
	}


def normalize_case(value):
	"""CASES/BIG_CASES entries are (corpus, keys) 2-tuples, optionally
	extended to (corpus, keys, home_files, assert_gt, requires_feature)
	by the Lisp and tree-sitter cases above; this is the one place every
	shape is unpacked."""
	corpus, keys = value[0], value[1]
	home_files = value[2] if len(value) > 2 else None
	assert_gt = value[3] if len(value) > 3 else None
	requires_feature = value[4] if len(value) > 4 else None
	return corpus, keys, home_files, assert_gt, requires_feature


def kg_version(kg):
	"""What `kg -V` prints, verbatim.

	Deliberately from the binary rather than from the build flags: what a
	case needs, and what the report names, is a property of the kg it is
	about to drive.
	"""
	out = subprocess.run([kg, "-V"], check=True, capture_output=True,
			     text=True)
	return out.stdout.strip()


def kg_features(version):
	"""The +words in a `kg -V` line, as a set of feature names.

	The same reading utils/pty_accept.py does.
	"""
	return {word[1:] for word in version.split() if word.startswith("+")}


def git_describe(tree):
	"""`git describe --always --dirty` for one tree, or None.

	Run here, at measurement time, rather than baked into the build: a
	describe compiled into a binary names the tree that last triggered a
	rebuild, which is the wrong tree exactly when it matters.
	"""
	try:
		out = subprocess.run(["git", "-C", str(tree), "describe",
				      "--always", "--dirty"],
				     check=False, capture_output=True, text=True)
	except OSError:
		return None
	return out.stdout.strip() or None


def file_sha256(path):
	"""Digest of the exact file this run drove, or None if unreadable."""
	digest = hashlib.sha256()
	try:
		with open(path, "rb") as handle:
			for block in iter(lambda: handle.read(1 << 16), b""):
				digest.update(block)
	except OSError:
		return None
	return digest.hexdigest()


def artifact(kg, version):
	"""Which binary, built from which trees, produced these numbers.

	`kg -V` verbatim rather than the +words alone, because the -words are
	half of what identifies a build; both describes, because kg's numbers
	are as much fe's tree as kg's own.
	"""
	return {
		"kg_version": version,
		"kg_sha256": file_sha256(kg),
		"kg_describe": git_describe(repo_root()),
		"fe_describe": git_describe(repo_root() / "fe"),
	}


def toolchain(cc):
	try:
		out = subprocess.run([cc.split()[-1], "--version"], check=False,
				     capture_output=True, text=True)
		return out.stdout.splitlines()[0] if out.stdout else cc
	except (OSError, IndexError):
		return cc


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--kg", required=True,
			    help="counting kg binary (test/perfobj/kg)")
	parser.add_argument("--kgbatch",
			    default=str(repo_root() / "test" / "kgbatch"),
			    help="terminal-free driver LISP_ANSWERS checks a "
				 "Lisp case's computed value against (make "
				 "kgbatch); a release build, not the counting "
				 "one, since it reports no counters and needs "
				 "none")
	parser.add_argument("--json", help="write the report here")
	parser.add_argument("--corpus-dir", default="test/.bench")
	parser.add_argument("--runs", type=int, default=3)
	parser.add_argument("--timeout", type=float, default=120.0)
	parser.add_argument("--rows", type=int, default=DEFAULT_ROWS)
	parser.add_argument("--cols", type=int, default=DEFAULT_COLS)
	parser.add_argument("--case", action="append", default=[],
			    help="run only these cases (repeatable)")
	parser.add_argument("--big", action="store_true",
			    help="include the 1M-line corpus (slow)")
	parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
	parser.add_argument("--cflags", default="")
	parser.add_argument("--kg-no-lisp",
			    help="a second kg binary built WITH_LISP=0, for "
				 "the startup-with-vs-without-Lisp comparison "
				 "(see doc/plans/2026-08-03-elisp-subset-and-"
				 "fe-evaluator-subplans/00d-baselines-and-"
				 "arena-observability.md); wall time only, "
				 "not a counting build, so not comparable to "
				 "the other cases' times -- omit to skip")
	parser.add_argument("--binary-size",
			    help="path=path of two release (non-counting) "
				 "kg binaries, WITH_LISP=1 then WITH_LISP=0, "
				 "whose sizes are recorded verbatim; omit to "
				 "skip")
	args = parser.parse_args()

	cases = dict(CASES)
	if args.big:
		cases.update(BIG_CASES)
	if args.case:
		# "startup-no-lisp" is not in CASES/BIG_CASES -- it only exists
		# when --kg-no-lisp is given, handled after the loop below --
		# so it is a known name for --case filtering purposes whether
		# or not that flag was actually passed this run.
		known_extra = {"startup-no-lisp"} if args.kg_no_lisp else set()
		unknown = set(args.case) - set(cases) - known_extra
		if unknown:
			print(f"unknown case(s): {', '.join(sorted(unknown))}",
			      file=sys.stderr)
			return 2
		cases = {k: v for k, v in cases.items() if k in args.case}

	# Decided before any corpus is generated, so a run that will skip
	# every case needing a corpus does not write one first.
	version = kg_version(args.kg)
	features = kg_features(version)
	skipped = {}
	for name, value in cases.items():
		needs = normalize_case(value)[4]
		if needs is not None and needs not in features:
			skipped[name] = needs

	corpus_dir = Path(args.corpus_dir)
	corpus_dir.mkdir(parents=True, exist_ok=True)
	needed = {normalize_case(v)[0] for name, v in cases.items()
		 if normalize_case(v)[0] and name not in skipped}
	paths = {}
	for name in sorted(needed):
		build, filename = CORPORA[name]
		path = corpus_dir / f"{name}-{filename}"
		if not path.exists():
			print(f"generating {path}", file=sys.stderr)
			build(path)
		paths[name] = path

	report = {
		"schema": SCHEMA,
		"generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
		"artifact": artifact(args.kg, version),
		"note": "counting build (-DKG_PERF_COUNTERS=1); times are not "
			"comparable with a release build",
		"build": {"kg": args.kg, "cc": args.cc,
			  "compiler": toolchain(args.cc), "cflags": args.cflags},
		"host": {"platform": platform.platform(),
			 "machine": platform.machine(),
			 "cpus": os.cpu_count()},
		"features": sorted(features),
		"cases": [],
	}
	for name, value in cases.items():
		corpus, keys, home_files, assert_gt, _ = normalize_case(value)
		if name in skipped:
			# In the report as well as on stderr: a case silently
			# absent from the JSON looks the same as a case nobody
			# thought to write, and the next reader of a trend needs
			# to see that this run could not take the measurement.
			reason = f"skipped, kg reports -{skipped[name]}"
			print(f"bench {name}: {reason}", file=sys.stderr)
			report["cases"].append({"name": name, "skipped": reason,
						"requires_feature": skipped[name]})
			continue
		print(f"bench {name}", file=sys.stderr)
		report["cases"].append(bench_case(
			args.kg, name, paths.get(corpus), keys, args.runs,
			args.rows, args.cols, args.timeout,
			home_files=home_files, assert_gt=assert_gt,
			kgbatch=args.kgbatch, answer=LISP_ANSWERS.get(name)))

	if args.kg_no_lisp and (not args.case or "startup-no-lisp" in args.case):
		# Wall time only: a WITH_LISP=0 binary has no Lisp counters to
		# report, and (per this file's own docstring) is not a
		# counting build in the first place, so its time is not
		# comparable to the cases above -- it exists only to subtract
		# against "startup" by eye, the same way "startup" itself is
		# subtracted from every other case.
		print("bench startup-no-lisp", file=sys.stderr)
		case = bench_case(args.kg_no_lisp, "startup-no-lisp", None,
				  ["\x18\x03"], args.runs, args.rows, args.cols,
				  args.timeout)
		case["note"] = ("WITH_LISP=0 release build, not a counting "
				"build; wall time only, not comparable to the "
				"other cases' counting-build times")
		report["cases"].append(case)

	if args.binary_size:
		with_lisp, without_lisp = args.binary_size.split("=", 1)
		report["binary_size"] = {
			"with_lisp": {"path": with_lisp,
				     "bytes": os.path.getsize(with_lisp)},
			"without_lisp": {"path": without_lisp,
					 "bytes": os.path.getsize(without_lisp)},
		}

	text = json.dumps(report, indent=1)
	if args.json:
		Path(args.json).write_text(text + "\n", encoding="utf-8")
		print(f"wrote {args.json}", file=sys.stderr)
	for case in report["cases"]:
		if "skipped" in case:
			print(f"  {case['name']:<24} {case['skipped']}")
			continue
		print(f"  {case['name']:<24} {case['wall_ms']['median']:>9.2f} ms "
		      f"median  {case['max_rss_kb'] // 1024:>5} MiB peak")
	return 0


if __name__ == "__main__":
	sys.exit(main())
