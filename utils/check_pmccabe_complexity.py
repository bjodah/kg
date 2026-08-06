#!/usr/bin/env python3
"""Check pmccabe output against a per-symbol baseline and a global budget.

The aggregate budget (--max-function) is a backstop: it says nothing about
a function that grows from 12 to 40, which is how complexity actually
arrives.  The baseline manifest is the ratchet -- every symbol's measured
complexity, checked in, with "no increase" as the rule and one command
(`make pmccabe-baseline`) to record a decrease or a new function.
"""

import argparse
import json
import re
import sys
from pathlib import Path

SCHEMA = "kg-pmccabe-baseline/1"

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
LOCATION = re.compile(r"^(?P<file>.+)\((?P<line>\d+)\):\s*(?P<name>.*)$")

# pmccabe does not preprocess, so a GCC-only decoration in front of a
# definition is what it reports as the function's name -- src/kbd.c once
# measured its worst function as "__attribute__", and a baseline keyed on
# that string would have been invalidated by any edit to the pragma block.
# Nothing here is ever accepted as a symbol name.
NOT_SYMBOLS = {
	"__attribute__",
	"__extension__",
	"_Noreturn",
	"inline",
	"static",
	"extern",
	"const",
	"unsigned",
	"signed",
	"struct",
	"union",
	"enum",
	"if",
	"for",
	"while",
	"switch",
	"return",
	"sizeof",
}


def resolve_symbol(path, line_no, name):
	"""Return the real function name for a pmccabe location.

	The reported name is used as-is when it is a plain identifier that is
	not a decoration; otherwise the cited file(line) is read and the first
	identifier followed by "(" wins.  Failing that, the caller gets None
	and the gate fails: an unnamed symbol cannot be ratcheted.
	"""
	if IDENTIFIER.match(name) and name not in NOT_SYMBOLS:
		return name
	try:
		lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
	except OSError:
		return None
	for text in lines[max(0, line_no - 1):line_no + 8]:
		for candidate in CALL.findall(text):
			if candidate not in NOT_SYMBOLS:
				return candidate
	return None


def iter_functions(lines):
	for line in lines:
		parts = line.split(None, 5)
		if len(parts) != 6:
			continue
		try:
			complexity = int(parts[0])
			traditional = int(parts[1])
			statements = int(parts[2])
			first_line = int(parts[3])
			line_count = int(parts[4])
		except ValueError:
			continue
		location = parts[5].strip()
		match = LOCATION.match(location)
		if match is None:
			yield {
				"complexity": complexity,
				"traditional": traditional,
				"statements": statements,
				"first_line": first_line,
				"line_count": line_count,
				"location": location,
				"file": None,
				"symbol": None,
			}
			continue
		symbol = resolve_symbol(match.group("file"), int(match.group("line")),
					match.group("name"))
		yield {
			"complexity": complexity,
			"traditional": traditional,
			"statements": statements,
			"first_line": first_line,
			"line_count": line_count,
			"location": location,
			"file": match.group("file"),
			"symbol": symbol,
		}


def key_of(func):
	return f"{func['file']}:{func['symbol']}"


def load_baseline(path, quiet=False):
	try:
		with open(path, "r", encoding="utf-8") as fp:
			data = json.load(fp)
	except FileNotFoundError:
		if not quiet:
			print(f"FAIL: complexity baseline {path} is missing; "
			      "run 'make pmccabe-baseline'", file=sys.stderr)
		return None
	except json.JSONDecodeError as exc:
		print(f"FAIL: complexity baseline {path} is not valid JSON: {exc}",
		      file=sys.stderr)
		return None
	if data.get("schema") != SCHEMA:
		print(f"FAIL: complexity baseline {path} has schema "
		      f"{data.get('schema')!r}, expected {SCHEMA!r}", file=sys.stderr)
		return None
	return data


def baseline_deltas(path, functions, max_new):
	"""What a rewrite of `path` would bank that nobody asked for.

	`make pmccabe-baseline` exists to record *improvements*, and it used
	to record anything at all: commit b8581e8 re-banked four per-symbol
	regressions (format_walk 5 -> 21, format_argument 6 -> 20,
	format_integer 6 -> 16) and a new function at 25 against a
	new-function budget of 15, and the command said nothing about any of
	it.  Two classes are reported here, and both need --allow-regressions
	before --write-baseline will proceed: a symbol measuring above the
	entry the manifest already holds, and a symbol with no entry at all
	measuring above the new-function budget.  The comparison is the same
	one check_baseline already does, so refusing to do it silently costs
	one dict lookup per symbol.
	"""
	previous = load_baseline(path, quiet=True)
	recorded = previous["functions"] if previous else {}
	deltas = []
	for func in sorted(functions, key=key_of):
		key = key_of(func)
		if key in recorded:
			if func["complexity"] > recorded[key]:
				deltas.append(
					f"  {key}: {recorded[key]} -> "
					f"{func['complexity']} "
					f"(+{func['complexity'] - recorded[key]})")
		elif func["complexity"] > max_new:
			deltas.append(
				f"  {key}: new function at {func['complexity']}, "
				f"budget for a new function {max_new}")
	return deltas


def write_baseline(path, functions, max_new):
	data = {
		"schema": SCHEMA,
		"regenerate_with": "make pmccabe-baseline",
		"note": ("Per-symbol pmccabe cyclomatic complexity. No symbol may "
			 "exceed its entry here; a symbol with no entry is new and "
			 f"must measure at most {max_new}. Decreases are recorded by "
			 "regenerating, which is the only way this file changes."),
		"max_new_function": max_new,
		"functions": {key_of(f): f["complexity"] for f in
			      sorted(functions, key=key_of)},
	}
	with open(path, "w", encoding="utf-8") as fp:
		json.dump(data, fp, indent=1, sort_keys=False)
		fp.write("\n")
	print(f"wrote {path}: {len(data['functions'])} symbols, "
	      f"max {max(f['complexity'] for f in functions)}")


def check_baseline(functions, baseline, max_new):
	recorded = baseline["functions"]
	failures = []
	improved = []
	added = []
	seen = set()

	for func in sorted(functions, key=lambda f: -f["complexity"]):
		key = key_of(func)
		seen.add(key)
		if key in recorded:
			was = recorded[key]
			if func["complexity"] > was:
				failures.append(
					f"  {key}: complexity {func['complexity']}, "
					f"baseline {was} (+{func['complexity'] - was})")
			elif func["complexity"] < was:
				improved.append(f"  {key}: {was} -> {func['complexity']}")
			continue
		added.append(key)
		if func["complexity"] > max_new:
			failures.append(
				f"  {key}: new function, complexity "
				f"{func['complexity']}, limit for new functions "
				f"{max_new}")

	removed = sorted(set(recorded) - seen)
	print(f"pmccabe baseline: {len(recorded)} symbols recorded, "
	      f"{len(functions)} measured, {len(added)} new, "
	      f"{len(removed)} gone, {len(improved)} improved")
	for line in improved[:10]:
		print(line)
	if improved:
		print("  run 'make pmccabe-baseline' to lock the improvements in")

	if failures:
		print(f"FAIL: {len(failures)} symbol(s) above their per-symbol "
		      "budget", file=sys.stderr)
		for line in failures:
			print(line, file=sys.stderr)
		return 1
	return 0


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--max-function", type=int, required=True)
	parser.add_argument("--top", type=int, default=10)
	parser.add_argument("--baseline",
			    help="per-symbol baseline manifest to enforce")
	parser.add_argument("--write-baseline",
			    help="regenerate the manifest from this measurement "
				 "instead of checking it")
	parser.add_argument("--max-new-function", type=int, default=15,
			    help="budget for a symbol with no baseline entry")
	parser.add_argument("--allow-regressions", action="store_true",
			    help="permit --write-baseline to record a symbol "
				 "at a higher complexity than the manifest "
				 "currently holds, or a new symbol above the "
				 "new-function budget; without this such a "
				 "rewrite is refused, so an increase cannot be "
				 "banked by accident")
	args = parser.parse_args()

	functions = sorted(
		iter_functions(sys.stdin),
		key=lambda func: func["complexity"],
		reverse=True,
	)

	# An empty stream is a broken gate, not a clean tree: make does not
	# use `pipefail`, so a missing pmccabe binary leaves this checker
	# reading nothing and the pipeline exiting on our status.  Reporting
	# a max of 0 there silently passed the gate for as long as pmccabe
	# was absent from the image.
	if not functions:
		print(
			"FAIL: no pmccabe output to check -- is pmccabe installed?",
			file=sys.stderr,
		)
		return 1

	unnamed = [f for f in functions if f["symbol"] is None]
	if unnamed:
		print(f"FAIL: {len(unnamed)} location(s) have no resolvable "
		      "function name; a symbol that cannot be named cannot be "
		      "ratcheted", file=sys.stderr)
		for func in unnamed[:args.top]:
			print(f"  {func['complexity']:4d}  {func['location']}",
			      file=sys.stderr)
		return 1

	max_complexity = functions[0]["complexity"]
	print(
		f"pmccabe max function complexity: {max_complexity} "
		f"(limit {args.max_function})"
	)
	print("pmccabe most complex functions:")
	for func in functions[:args.top]:
		print(
			f"  {func['complexity']:4d}  {func['line_count']:5d} LOC  "
			f"{func['location']}"
		)

	if args.write_baseline:
		# Banking an increase is a deliberate act with a reason, so it
		# needs a flag and the reason goes in the commit message;
		# banking one by accident is what this refuses.
		deltas = baseline_deltas(args.write_baseline, functions,
					 args.max_new_function)
		if deltas and not args.allow_regressions:
			print(f"FAIL: {len(deltas)} symbol(s) would be banked "
			      "above what the current baseline allows; re-run "
			      "with --allow-regressions (make pmccabe-baseline "
			      "PMCCABE_BASELINE_ARGS=--allow-regressions) and "
			      "say why in the commit message", file=sys.stderr)
			for line in deltas:
				print(line, file=sys.stderr)
			return 1
		for line in deltas:
			print(f"banking increase: {line.strip()}")
		write_baseline(args.write_baseline, functions,
			       args.max_new_function)
		return 0

	over_limit = [
		func for func in functions if func["complexity"] > args.max_function
	]
	if over_limit:
		print(
			f"FAIL: {len(over_limit)} function(s) exceed complexity "
			f"limit {args.max_function}",
			file=sys.stderr,
		)
		for func in over_limit:
			print(
				f"  {func['complexity']:4d}  {func['location']}",
				file=sys.stderr,
			)
		return 1

	if args.baseline:
		baseline = load_baseline(args.baseline)
		if baseline is None:
			return 1
		return check_baseline(functions, baseline,
				      args.max_new_function)

	return 0


if __name__ == "__main__":
	sys.exit(main())
