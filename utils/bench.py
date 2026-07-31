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


CORPORA = {
	"lines-10k": (lambda p: corpus_lines(p, 10_000), "log.txt"),
	"lines-100k": (lambda p: corpus_lines(p, 100_000), "log.txt"),
	"lines-1m": (lambda p: corpus_lines(p, 1_000_000), "log.txt"),
	"long-line-1mib": (lambda p: corpus_one_long_line(p, 1 << 20), "min.js"),
	"unicode-20k": (lambda p: corpus_unicode(p, 20_000), "utf8.txt"),
	"comment-c-40k": (lambda p: corpus_comment_c(p, 40_000), "big.c"),
}

# name -> (corpus or None, keys sent after the first frame)
CASES = {
	"startup": (None, ["\x18\x03"]),
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
}
BIG_CASES = {"open-lines-1m": ("lines-1m", ["\x18\x03"])}


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
	"""Spawn kg on a pty, send `keys`, and return (seconds, max_rss_kb)."""
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


def bench_case(kg, name, corpus_path, keys, runs, rows, cols, timeout):
	times, rss, counters = [], 0, {}
	with tempfile.TemporaryDirectory() as tmp:
		for _ in range(runs):
			out = os.path.join(tmp, "perf.json")
			env = dict(os.environ)
			env["KG_PERF_OUT"] = out
			env["TERM"] = "xterm-256color"
			env["HOME"] = tmp  # no user init file
			argv = ["-Q"] + ([str(corpus_path)] if corpus_path else [])
			seconds, maxrss = run_once(
				kg, argv, env, rows, cols, keys, timeout)
			times.append(seconds * 1000.0)
			rss = max(rss, maxrss)
			if os.path.exists(out):
				with open(out, "r", encoding="utf-8") as fp:
					counters = json.load(fp)
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
	args = parser.parse_args()

	cases = dict(CASES)
	if args.big:
		cases.update(BIG_CASES)
	if args.case:
		unknown = set(args.case) - set(cases)
		if unknown:
			print(f"unknown case(s): {', '.join(sorted(unknown))}",
			      file=sys.stderr)
			return 2
		cases = {k: v for k, v in cases.items() if k in args.case}

	corpus_dir = Path(args.corpus_dir)
	corpus_dir.mkdir(parents=True, exist_ok=True)
	needed = {c for c, _ in cases.values() if c}
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
	for name, (corpus, keys) in cases.items():
		print(f"bench {name}", file=sys.stderr)
		report["cases"].append(bench_case(
			args.kg, name, paths.get(corpus), keys, args.runs,
			args.rows, args.cols, args.timeout))

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
