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
"""

import argparse
import errno
import fcntl
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

SCHEMA = "kg-bench/1"
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
	"(defun my-before-save-hook () (= my-fill-column my-fill-column))"
	"(add-hook 'before-save-hook 'my-before-save-hook)"
	"(global-set-key \"C-c g\" \"my-greet\")"
	"(global-set-key \"C-c w\" \"my-count-words\")"
	"(defun my-loop (n acc) (if (<= n 0) acc (my-loop (- n 1) (cons n acc))))"
	"(my-loop 25 nil)"
)


def home_files_auto_fill():
	lisp_dir = repo_root() / "lisp"
	init = (f'(add-to-load-path "{lisp_dir}")' "(require 'auto-fill)")
	return {".config/kg/init.el": init}


def home_files_representative_init():
	return {".config/kg/init.el": REPRESENTATIVE_INIT}


CORPORA = {
	"lines-10k": (lambda p: corpus_lines(p, 10_000), "log.txt"),
	"lines-100k": (lambda p: corpus_lines(p, 100_000), "log.txt"),
	"lines-1m": (lambda p: corpus_lines(p, 1_000_000), "log.txt"),
	"long-line-1mib": (lambda p: corpus_one_long_line(p, 1 << 20), "min.js"),
	"unicode-20k": (lambda p: corpus_unicode(p, 20_000), "utf8.txt"),
	"comment-c-40k": (lambda p: corpus_comment_c(p, 40_000), "big.c"),
	"tabs-100k": (lambda p: corpus_tabs(p, 100_000), "tabs.txt"),
	"invalid-bytes-20k": (lambda p: corpus_invalid_bytes(p, 20_000), "invalid.txt"),
}

# name -> (corpus or None, keys sent after the first frame).  A case may
# extend the tuple with two optional trailing fields -- (corpus, keys,
# home_files, assert_gt) -- see normalize_case() below; every plain
# 2-tuple case above and below still means "isolated $HOME, no init file,
# nothing to assert".
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
	# assert_gt is 2 (not 0): 2 is exactly the prelude's own
	# lisp_peak_eval_depth, measured directly in
	# test/test_perf.c's test_lisp_prelude_arena_margin. A case whose key
	# script silently failed to reach the evaluator at all -- the sub-plan
	# 07C trap, a key that turned out to be a prefix map -- still shows
	# that same value, not zero, so the threshold has to be the baseline's
	# own reading rather than 0.
	# This case *is* the baseline lisp_peak_eval_depth == 2 reading, so
	# it cannot assert against that threshold the way the cases below
	# do; asserting total_slots instead still catches "Lisp inactive or
	# the snapshot never ran", which would read 0.
	"lisp-arena-prelude": (None, ["\x18\x03"], None,
			       {"lisp_arena_total_slots": 0}),
	"lisp-arena-auto-fill": (None, ["\x18\x03"], home_files_auto_fill(),
				 {"lisp_peak_eval_depth": 2}),
	"lisp-arena-representative-init": (
		None, ["\x18\x03"], home_files_representative_init(),
		{"lisp_peak_eval_depth": 2}),
	# Fe evaluation throughput on representative shapes.  Each sends
	# `M-:` (eval-expression), the expression as literal self-insert text
	# in the minibuffer, then RET; the buffer itself is never modified,
	# so `C-x C-c` exits without a save prompt.
	# 150, not a rounder/bigger number: `lw` is not tail-call optimised
	# (Fe's evaluator does not flatten it, and Phase 3's frame machine is
	# the piece of this program that will change that), so its GC-stack
	# cost is linear in recursion depth and every intermediate cons stays
	# live until the outermost call returns. Measured directly: this
	# expression's peak_gc_stack_depth is 3914 of the 4096-slot ceiling
	# at n=300, and overflows by n=400. n=150 leaves comfortable margin
	# (about half the stack) while still being a real multi-hundred-cell
	# walk; see test/test_perf.c's identical expression for the same
	# margin as a shape assertion.
	"lisp-list-walk": (None, [
		"\x1b:",
		"(defun lw (n l) (if (<= n 0) l (lw (- n 1) (cons n l)))) "
		"(length (lw 150 nil))\r",
		"\x18\x03",
	], None, {"lisp_peak_eval_depth": 2}),
	"lisp-arithmetic-loop": (None, [
		"\x1b:",
		"(setq i 0) (setq acc 0) (while (< i 20000) (setq acc (+ acc i)) "
		"(setq i (+ i 1))) acc\r",
		"\x18\x03",
	], None, {"lisp_peak_eval_depth": 2}),
	"lisp-macro-heavy": (None, [
		"\x1b:",
		"(setq m (macro (x) (list '+ x 1))) (setq n 0) (setq i 0) "
		"(while (< i 2000) (setq n (m n)) (setq i (+ i 1))) n\r",
		"\x18\x03",
	], None, {"lisp_peak_eval_depth": 2}),
	"lisp-deep-call-chain": (None, [
		"\x1b:",
		"(defun dc (n) (if (<= n 0) 0 (+ 1 (dc (- n 1))))) (dc 300)\r",
		"\x18\x03",
	], None, {"lisp_peak_eval_depth": 2}),
	# Representative command latency: the minibuffer round trip and eval
	# dispatch on a trivial expression, with none of the above shapes'
	# own cost mixed in. Deliberately as shallow as the prelude's own
	# deepest call (nesting 2), so -- unlike the shapes above --
	# lisp_peak_eval_depth cannot be the non-trivial-execution signal
	# here; total_slots merely confirms Lisp ran at all.
	"lisp-command-latency": (None, ["\x1b:", "(+ 1 2)\r", "\x18\x03"],
				 None, {"lisp_arena_total_slots": 0}),
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


def bench_case(kg, name, corpus_path, keys, runs, rows, cols, timeout,
	       home_files=None, assert_gt=None):
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
		"counters": counters,
	}


def normalize_case(value):
	"""CASES/BIG_CASES entries are (corpus, keys) 2-tuples, optionally
	extended to (corpus, keys, home_files, assert_gt) by the Lisp cases
	above; this is the one place both shapes are unpacked."""
	corpus, keys = value[0], value[1]
	home_files = value[2] if len(value) > 2 else None
	assert_gt = value[3] if len(value) > 3 else None
	return corpus, keys, home_files, assert_gt


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

	corpus_dir = Path(args.corpus_dir)
	corpus_dir.mkdir(parents=True, exist_ok=True)
	needed = {normalize_case(v)[0] for v in cases.values()
		 if normalize_case(v)[0]}
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
		"note": "counting build (-DKG_PERF_COUNTERS=1); times are not "
			"comparable with a release build",
		"build": {"kg": args.kg, "cc": args.cc,
			  "compiler": toolchain(args.cc), "cflags": args.cflags},
		"host": {"platform": platform.platform(),
			 "machine": platform.machine(),
			 "cpus": os.cpu_count()},
		"cases": [],
	}
	for name, value in cases.items():
		corpus, keys, home_files, assert_gt = normalize_case(value)
		print(f"bench {name}", file=sys.stderr)
		report["cases"].append(bench_case(
			args.kg, name, paths.get(corpus), keys, args.runs,
			args.rows, args.cols, args.timeout,
			home_files=home_files, assert_gt=assert_gt))

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
		print(f"  {case['name']:<24} {case['wall_ms']['median']:>9.2f} ms "
		      f"median  {case['max_rss_kb'] // 1024:>5} MiB peak")
	return 0


if __name__ == "__main__":
	sys.exit(main())
