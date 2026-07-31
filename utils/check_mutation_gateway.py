#!/usr/bin/env python3
"""Enumerate every module that can mutate buffer text, and hold the set shut.

kg is growing one gateway for buffer mutation (plan 10).  Until every
caller is migrated the old paths have to keep working, so this is not a
ban -- it is a census with a ratchet.  Each source file's count of

  * calls to the raw row primitives (`editor_insert_row`, `editor_row_*`,
    ...), which mutate text without recording anything;
  * calls to `undo_push()`, which is the caller writing its own undo
    record instead of letting the gateway do it;
  * mentions of `suppress_undo`, the ambient flag whose three meanings
    the gateway replaces with explicit options;
  * direct writes to the fields that hold a buffer's text (`row->size`,
    `row->chars[i] =`, `b->numrows`, `b->row_capacity`)

is recorded in a manifest.  No count may rise and no unlisted file may
acquire one; a count that falls is banked by regenerating.  The manifest
therefore reads as the migration's remaining work, and the exit criterion
in the plan ("undo_push appears only in undo.c") is the same file going
to zero.

The probes are textual.  Comments and string literals are stripped first,
so prose about a primitive is not a call to it, but nothing here parses C:
the manifest is an inventory to be driven down, not a proof.
"""

import argparse
import json
import re
import sys
from pathlib import Path

SCHEMA = "kg-mutation-gateway/1"

# Row primitives: they move bytes in a row array and record nothing.  Every
# one of these is what the plan calls "raw mutation".
ROW_PRIMITIVES = (
	"editor_insert_row",
	"editor_del_row",
	"editor_free_row",
	"editor_free_all_rows",
	"editor_row_insert_char",
	"editor_row_insert_string",
	"editor_row_insert_spaces",
	"editor_row_append_string",
	"editor_row_del_char",
	"editor_rows_reserve",
)

# Direct writes to the fields a row's text lives in.  Reads are fine and
# are not matched: only an assignment, an increment or a decrement is.
FIELD_WRITES = (
	re.compile(r"\brow(?:s\[[^]]*\])?->size\s*(?:[-+*/|&^]?=[^=]|\+\+|--)"),
	re.compile(r"->chars\s*\[[^]]*\]\s*=[^=]"),
	re.compile(r"->numrows\s*(?:[-+]?=[^=]|\+\+|--)"),
	re.compile(r"->row_capacity\s*(?:[-+]?=[^=]|\+\+|--)"),
	re.compile(r"->row\s*=[^=]"),
)

PROBES = ("row_primitive", "undo_push", "suppress_undo", "row_field_write")

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
STRING = re.compile(r'"(?:\\.|[^"\\])*"')
CHARLIT = re.compile(r"'(?:\\.|[^'\\])'")


def strip_c(text):
	"""Blank out comments and literals, keeping line structure intact."""

	def blank(match):
		return re.sub(r"[^\n]", " ", match.group(0))

	text = BLOCK_COMMENT.sub(blank, text)
	text = LINE_COMMENT.sub(blank, text)
	text = STRING.sub(blank, text)
	return CHARLIT.sub(blank, text)


def count_calls(text, names):
	total = 0
	for name in names:
		total += len(re.findall(r"\b" + name + r"\s*\(", text))
	return total


def measure(path):
	text = strip_c(Path(path).read_text(encoding="utf-8", errors="replace"))
	counts = {
		"row_primitive": count_calls(text, ROW_PRIMITIVES),
		"undo_push": count_calls(text, ("undo_push",)),
		"suppress_undo": len(re.findall(r"\bsuppress_undo\b", text)),
		"row_field_write": sum(
			len(rx.findall(text)) for rx in FIELD_WRITES),
	}
	return {name: n for name, n in counts.items() if n}


def measure_tree(paths):
	files = []
	for spec in paths:
		root = Path(spec)
		files.extend(sorted(root.glob("*.c")) if root.is_dir() else [root])
	measured = {}
	for path in files:
		counts = measure(path)
		if counts:
			measured[str(path)] = counts
	return measured


def load_manifest(path):
	try:
		with open(path, "r", encoding="utf-8") as fp:
			data = json.load(fp)
	except FileNotFoundError:
		print(f"FAIL: mutation manifest {path} is missing; "
		      "run 'make gateway-baseline'", file=sys.stderr)
		return None
	except json.JSONDecodeError as exc:
		print(f"FAIL: mutation manifest {path} is not valid JSON: {exc}",
		      file=sys.stderr)
		return None
	if data.get("schema") != SCHEMA:
		print(f"FAIL: mutation manifest {path} has schema "
		      f"{data.get('schema')!r}, expected {SCHEMA!r}", file=sys.stderr)
		return None
	return data


def write_manifest(path, measured):
	data = {
		"schema": SCHEMA,
		"regenerate_with": "make gateway-baseline",
		"note": ("Every file that can change buffer text without going "
			 "through the edit gateway, and how many ways it does. "
			 "No count may rise and no unlisted file may appear; "
			 "regenerating is how a decrease is banked. Plan 10 "
			 "drives this to buffer.c alone."),
		"probes": list(PROBES),
		"files": {name: measured[name] for name in sorted(measured)},
	}
	with open(path, "w", encoding="utf-8") as fp:
		json.dump(data, fp, indent=1, sort_keys=False)
		fp.write("\n")
	print(f"wrote {path}: {len(measured)} files, "
	      f"{sum(sum(c.values()) for c in measured.values())} sites")


def check(measured, manifest):
	recorded = manifest["files"]
	failures = []
	improved = []

	for name in sorted(measured):
		listed = recorded.get(name, {})
		for probe in PROBES:
			now = measured[name].get(probe, 0)
			was = listed.get(probe, 0)
			if now > was:
				failures.append(
					f"  {name}: {probe} {now}, manifest {was} "
					f"(+{now - was})")
			elif now < was:
				improved.append(f"  {name}: {probe} {was} -> {now}")
	for name in sorted(set(recorded) - set(measured)):
		improved.append(f"  {name}: no longer mutates rows directly")

	total = sum(sum(c.values()) for c in measured.values())
	budget = sum(sum(c.values()) for c in recorded.values())
	print(f"mutation gateway: {len(measured)} files with raw mutation "
	      f"({total} sites), manifest allows {budget}")
	for line in improved[:12]:
		print(line)
	if improved:
		print("  run 'make gateway-baseline' to lock the migration in")

	if failures:
		print(f"FAIL: {len(failures)} raw-mutation count(s) above the "
		      "manifest", file=sys.stderr)
		for line in failures:
			print(line, file=sys.stderr)
		return 1
	return 0


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--manifest", required=True)
	parser.add_argument("--write", action="store_true",
			    help="record the measurement instead of checking it")
	parser.add_argument("paths", nargs="+")
	args = parser.parse_args()

	measured = measure_tree(args.paths)
	if args.write:
		write_manifest(args.manifest, measured)
		return 0
	manifest = load_manifest(args.manifest)
	if manifest is None:
		return 1
	return check(measured, manifest)


if __name__ == "__main__":
	sys.exit(main())
