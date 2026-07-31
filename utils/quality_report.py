#!/usr/bin/env python3
"""Collect one machine-readable summary of a CI run.

Every producer this reads already exists and already writes a file; what
was missing was one place that says, for a given commit, which stages ran
and for how long, what the two test layers measured, where coverage and
complexity stand against their baselines, and what the pins were.  It
scrapes no logs: `.ci/.run/<step>.state` is written by the runner,
`test/.results/{unit,pty}.json` by `make check`, `coverage/src.info` by
lcov, and the two baselines are checked in.

Never committed -- .ci/.run is gitignored, and the hosted workflow uploads
it as an artifact.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

SCHEMA = "kg-quality/1"


def git(*args):
	try:
		out = subprocess.run(("git",) + args, capture_output=True,
				     text=True, check=True)
	except (OSError, subprocess.CalledProcessError):
		return None
	return out.stdout.strip() or None


def read_json(path):
	try:
		with open(path, "r", encoding="utf-8") as handle:
			return json.load(handle)
	except (OSError, json.JSONDecodeError):
		return None


def read_state(path):
	fields = {}
	try:
		with open(path, "r", encoding="utf-8") as handle:
			for line in handle:
				key, _, value = line.strip().partition("=")
				fields[key] = value
	except OSError:
		return None
	for key in ("start", "end", "elapsed"):
		if fields.get(key):
			try:
				fields[key] = float(fields[key])
			except ValueError:
				pass
	return fields


def stages(run_dir):
	found = []
	for path in sorted(Path(run_dir).glob("*.state")):
		state = read_state(path)
		if state and state.get("step"):
			found.append({
				"step": state["step"],
				"state": state.get("state"),
				"seconds": state.get("elapsed") or None,
				"exit": state.get("exit") or None,
				"log": state.get("log") or None,
			})
	return found


def coverage_summary(tracefile, baseline_path):
	counters = ("LF", "LH", "FNF", "FNH", "BRF", "BRH")
	totals = dict.fromkeys(counters, 0)
	files = 0
	try:
		with open(tracefile, "r", encoding="utf-8") as handle:
			for line in handle:
				key, _, value = line.strip().partition(":")
				if key == "SF":
					files += 1
				elif key in counters:
					totals[key] += int(value)
	except OSError:
		return None
	baseline = read_json(baseline_path) or {}
	rate = lambda hit, found: (round(totals[hit] / totals[found], 4)
				   if totals[found] else None)
	return {
		"files": files,
		"line_rate": rate("LH", "LF"),
		"function_rate": rate("FNH", "FNF"),
		"branch_rate": rate("BRH", "BRF"),
		"lines_hit": totals["LH"],
		"lines_found": totals["LF"],
		"baseline_line_rate": baseline.get("total", {}).get("line_rate"),
		"baseline_function_rate":
			baseline.get("total", {}).get("function_rate"),
	}


def complexity_summary(baseline_path):
	baseline = read_json(baseline_path)
	if not baseline:
		return None
	functions = baseline.get("functions", {})
	worst = max(functions.items(), key=lambda kv: kv[1], default=(None, None))
	return {
		"symbols": len(functions),
		"max": worst[1],
		"max_symbol": worst[0],
		"max_new_function": baseline.get("max_new_function"),
	}


def slowest(results, count):
	cases = (results or {}).get("cases", [])
	ranked = sorted(cases, key=lambda case: case.get("seconds") or 0,
			reverse=True)
	return [{"name": case.get("name"), "seconds": case.get("seconds")}
		for case in ranked[:count]]


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--run-dir", default=".ci/.run")
	parser.add_argument("--results-dir", default="test/.results")
	parser.add_argument("--coverage", default="coverage/src.info")
	parser.add_argument("--coverage-baseline",
			    default=".ci/coverage-baseline.json")
	parser.add_argument("--pmccabe-baseline",
			    default=".ci/pmccabe-baseline.json")
	parser.add_argument("--slowest", type=int, default=10)
	parser.add_argument("--output", default="")
	args = parser.parse_args()

	unit = read_json(Path(args.results_dir) / "unit.json")
	pty = read_json(Path(args.results_dir) / "pty.json")
	binary = Path("src/kg")

	report = {
		"schema": SCHEMA,
		"commit": git("rev-parse", "HEAD"),
		"branch": git("rev-parse", "--abbrev-ref", "HEAD"),
		"dirty": bool(git("status", "--porcelain")),
		"pins": {
			"fe": git("-C", "fe", "rev-parse", "HEAD"),
			"tiny-regex-c": git("-C", "fe/tiny-regex-c", "rev-parse",
					    "HEAD"),
		},
		"stages": stages(args.run_dir),
		"tests": {
			"unit": (unit or {}).get("counts"),
			"pty": (pty or {}).get("counts"),
			"pty_jobs": (pty or {}).get("jobs"),
			"pty_oracle": (pty or {}).get("emacs"),
			"slowest_pty_cases": slowest(pty, args.slowest),
			"slowest_unit_tests": slowest(unit, args.slowest),
		},
		"coverage": coverage_summary(args.coverage,
					     args.coverage_baseline),
		"complexity": complexity_summary(args.pmccabe_baseline),
		"binary_bytes": binary.stat().st_size if binary.exists() else None,
	}

	text = json.dumps(report, indent=1) + "\n"
	if args.output:
		path = Path(args.output)
		path.parent.mkdir(parents=True, exist_ok=True)
		path.write_text(text, encoding="utf-8")
		print(f"wrote {path}")
	else:
		sys.stdout.write(text)
	return 0


if __name__ == "__main__":
	sys.exit(main())
