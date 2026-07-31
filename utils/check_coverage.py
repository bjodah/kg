#!/usr/bin/env python3
"""Enforce a per-file coverage floor from a checked-in baseline.

`make coverage` used to end in a bare `lcov --summary`: a number printed
into a log that nothing compared against anything, so coverage could fall
for a year without a gate noticing.  This reads the same tracefile and
holds every file to the rate it had when the baseline was recorded.

Rates, not hit counts: deleting dead code lowers the count and is not a
regression.  `make coverage-baseline` is how a measured improvement (or a
reviewed, explained drop) gets recorded.

The slack is measured, not a comfort margin.  Repeated runs of the same
commit agree exactly for most files and wobble for two: src/syntax.c by
up to 3 of its 757 lines, because which highlighting paths run depends on
what the PTY cases painted, and src/compile.c in its branch counts,
because a child process's timing decides how much output the poll loop
sees.  A gate that flakes gets switched off, so a file may lose
SLACK_LINES lines before it fails and the tree may lose TOTAL_SLACK_LINES.
Function coverage was identical across every run measured, so it gets no
slack; branch coverage is reported and not gated, its jitter being the
largest of the three.
"""

import argparse
import json
import sys
from pathlib import Path

SCHEMA = "kg-coverage-baseline/1"
# Measured jitter between runs of one commit: 3 lines in src/syntax.c, 0
# elsewhere.  Raise only with a measurement that says why.
SLACK_LINES = 4
TOTAL_SLACK_LINES = 8
COUNTERS = ("LF", "LH", "FNF", "FNH", "BRF", "BRH")


def parse_tracefile(path):
	"""Return {file: {LF, LH, FNF, FNH, BRF, BRH}} from an lcov .info."""
	files = {}
	current = None
	with open(path, "r", encoding="utf-8") as handle:
		for line in handle:
			line = line.strip()
			if line.startswith("SF:"):
				current = line[3:]
				files[current] = dict.fromkeys(COUNTERS, 0)
			elif current is not None and ":" in line:
				key, _, value = line.partition(":")
				if key in COUNTERS:
					files[current][key] = int(value)
	return files


def normalize(path, root):
	if root:
		try:
			return str(Path(path).resolve().relative_to(Path(root).resolve()))
		except ValueError:
			pass
	parts = Path(path).parts
	return str(Path(*parts[-2:])) if len(parts) >= 2 else path


def rates(counts):
	def rate(hit, found):
		return round(counts[hit] / counts[found], 4) if counts[found] else None
	return {
		"lines_found": counts["LF"],
		"lines_hit": counts["LH"],
		"line_rate": rate("LH", "LF"),
		"functions_found": counts["FNF"],
		"functions_hit": counts["FNH"],
		"function_rate": rate("FNH", "FNF"),
		"branches_found": counts["BRF"],
		"branches_hit": counts["BRH"],
		"branch_rate": rate("BRH", "BRF"),
	}


def totals(measured):
	summed = {key: sum(f[key] for f in measured.values()) for key in COUNTERS}
	return rates(summed)


def load_baseline(path):
	try:
		with open(path, "r", encoding="utf-8") as handle:
			data = json.load(handle)
	except FileNotFoundError:
		print(f"FAIL: coverage baseline {path} is missing; run "
		      "'make coverage-baseline'", file=sys.stderr)
		return None
	if data.get("schema") != SCHEMA:
		print(f"FAIL: coverage baseline {path} has schema "
		      f"{data.get('schema')!r}, expected {SCHEMA!r}", file=sys.stderr)
		return None
	return data


def write_baseline(path, measured, note, how):
	data = {
		"schema": SCHEMA,
		"regenerate_with": "make coverage-baseline",
		"how": how,
		"note": note,
		"total": totals(measured),
		"files": {name: rates(counts) for name, counts in sorted(measured.items())},
	}
	with open(path, "w", encoding="utf-8") as handle:
		json.dump(data, handle, indent=1)
		handle.write("\n")
	total = data["total"]
	print(f"wrote {path}: {len(data['files'])} files, "
	      f"lines {total['line_rate']:.4f}, functions {total['function_rate']:.4f}")


def below_floor(now, was, metric_key, rate_key, slack):
	"""Whether this measurement is a regression rather than jitter.

	The rate is what is compared -- a file that loses covered *and*
	uncovered lines has not regressed -- but a file whose rate slipped
	is only a failure if it also lost more than `slack` covered lines
	than the floor would predict for its current size.
	"""
	if was[rate_key] is None or now[rate_key] is None:
		return False
	if now[rate_key] >= was[rate_key]:
		return False
	expected = was[rate_key] * now[metric_key[0]]
	return now[metric_key[1]] < expected - slack


def compare(measured, baseline, metric_key, rate_key, label, failures, notes,
	    slack):
	recorded = baseline["files"]
	for name in sorted(measured):
		now = rates(measured[name])
		was = recorded.get(name)
		if was is None:
			notes.append(f"  {name}: new file, {label} "
				     f"{fmt(now[rate_key])} (not yet a floor; run "
				     "'make coverage-baseline')")
			continue
		if below_floor(now, was, metric_key, rate_key, slack):
			failures.append(
				f"  {name}: {label} {fmt(now[rate_key])} "
				f"({now[metric_key[1]]}/{now[metric_key[0]]}), "
				f"floor {fmt(was[rate_key])} "
				f"({was[metric_key[1]]}/{was[metric_key[0]]})")
	for name in sorted(set(recorded) - set(measured)):
		notes.append(f"  {name}: measured no longer, floor dropped")


def fmt(rate):
	return "n/a" if rate is None else f"{100 * rate:.2f}%"


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("tracefile")
	parser.add_argument("--baseline", required=True)
	parser.add_argument("--write-baseline", action="store_true")
	parser.add_argument("--root", default="",
			    help="strip this prefix from tracefile paths")
	parser.add_argument("--note", default="")
	parser.add_argument("--how", default="",
			    help="the command that produced this tracefile")
	args = parser.parse_args()

	raw = parse_tracefile(args.tracefile)
	if not raw:
		print(f"FAIL: {args.tracefile} lists no source file -- did the "
		      "coverage run produce anything?", file=sys.stderr)
		return 1
	measured = {normalize(path, args.root): counts for path, counts in raw.items()}

	total = totals(measured)
	print(f"coverage: {len(measured)} files, lines {fmt(total['line_rate'])} "
	      f"({total['lines_hit']}/{total['lines_found']}), functions "
	      f"{fmt(total['function_rate'])} "
	      f"({total['functions_hit']}/{total['functions_found']}), branches "
	      f"{fmt(total['branch_rate'])} "
	      f"({total['branches_hit']}/{total['branches_found']})")

	if args.write_baseline:
		write_baseline(args.baseline, measured, args.note, args.how)
		return 0

	baseline = load_baseline(args.baseline)
	if baseline is None:
		return 1

	failures = []
	notes = []
	compare(measured, baseline, ("lines_found", "lines_hit"), "line_rate",
		"line coverage", failures, notes, SLACK_LINES)
	compare(measured, baseline, ("functions_found", "functions_hit"),
		"function_rate", "function coverage", failures, [], 0)

	was = baseline["total"]
	for metric_key, rate_key, label, slack in (
			(("lines_found", "lines_hit"), "line_rate",
			 "line coverage", TOTAL_SLACK_LINES),
			(("functions_found", "functions_hit"), "function_rate",
			 "function coverage", 0)):
		if below_floor(total, was, metric_key, rate_key, slack):
			failures.append(f"  TOTAL: {label} {fmt(total[rate_key])}, "
					f"floor {fmt(was[rate_key])}")

	for line in notes:
		print(line)
	if failures:
		print(f"FAIL: coverage fell below the baseline in "
		      f"{len(failures)} place(s)", file=sys.stderr)
		for line in failures:
			print(line, file=sys.stderr)
		print("  raise it back with tests, or explain the drop in the "
		      "commit that regenerates the baseline", file=sys.stderr)
		return 1

	improved = sum(1 for name in measured
		       if name in baseline["files"]
		       and rates(measured[name])["line_rate"] is not None
		       and baseline["files"][name]["line_rate"] is not None
		       and rates(measured[name])["line_rate"] > baseline["files"][name]["line_rate"])
	if improved:
		print(f"{improved} file(s) above their floor; "
		      "'make coverage-baseline' banks that")
	return 0


if __name__ == "__main__":
	sys.exit(main())
