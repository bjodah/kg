#!/usr/bin/env python3
"""Run the native unit-test binaries and report what actually happened.

This replaces a shell loop in the Makefile that printed "# SKIP: 0" and
"# XFAIL: 0" as literals next to counted PASS/FAIL numbers, which read as
parity with the PTY layer's summary without being measured.  The native
layer has no skip or xfail concept -- those two really are zero -- but it
does have an outcome the loop could not tell apart: a binary killed by a
signal (an ASan abort, a SIGSEGV) was reported as a plain FAIL.  That is
an ERROR here, the same word the PTY summary uses for it.
"""

import argparse
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path


def run_one(argv, name):
	started = time.monotonic()
	proc = subprocess.run(argv, capture_output=True, text=True)
	elapsed = time.monotonic() - started
	if proc.returncode == 0:
		status = "PASS"
	elif proc.returncode < 0:
		status = "ERROR"
	else:
		status = "FAIL"
	return {
		"name": name,
		"status": status,
		"exit": proc.returncode,
		"seconds": round(elapsed, 3),
		"output": "" if status == "PASS" else proc.stdout + proc.stderr,
	}


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--runner", default="",
			    help="command prefix for every binary (e.g. valgrind)")
	parser.add_argument("--json", help="write per-test results here")
	parser.add_argument("tests", nargs="+")
	args = parser.parse_args()

	prefix = shlex.split(args.runner)
	results = [run_one(prefix + [test], Path(test).name) for test in args.tests]

	counts = {k: 0 for k in ("PASS", "SKIP", "FAIL", "XFAIL", "XPASS", "ERROR")}
	for result in results:
		counts[result["status"]] += 1
		print(f"{result['status']}: {result['name']}")
		if result["output"]:
			print(result["output"].rstrip())

	if args.json:
		path = Path(args.json)
		path.parent.mkdir(parents=True, exist_ok=True)
		with path.open("w", encoding="utf-8") as fp:
			json.dump({
				"schema": "kg-unit-results/1",
				"runner": args.runner,
				"counts": counts,
				"cases": [{k: v for k, v in r.items() if k != "output"}
					  for r in results],
			}, fp, indent=1)
			fp.write("\n")

	print()
	print("=" * 76)
	print("Testsuite summary for kg")
	print("=" * 76)
	print(f"# TOTAL: {len(results)}")
	print(f"# PASS:  {counts['PASS']}")
	# Measured, not assumed: a native binary reports one outcome, so the
	# only way SKIP or XFAIL becomes non-zero is a harness that grows the
	# concept.  ERROR is a binary that died on a signal.
	print(f"# SKIP:  {counts['SKIP']}")
	print(f"# XFAIL: {counts['XFAIL']}")
	print(f"# FAIL:  {counts['FAIL']}")
	print(f"# XPASS: {counts['XPASS']}")
	print(f"# ERROR: {counts['ERROR']}")
	print("=" * 76)

	return 1 if counts["FAIL"] or counts["ERROR"] or counts["XPASS"] else 0


if __name__ == "__main__":
	sys.exit(main())
