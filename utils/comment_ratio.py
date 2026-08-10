#!/usr/bin/env python3
"""Rank files by how much of them is comment, for verbosity hunting.

A one-off tool, not a gate.  There is no Makefile target and no CI step
that runs it, on purpose: a percentage threshold is gameable in the wrong
direction -- the cheapest way past it is to delete a header comment that
was earning its keep -- so this ranks and says nothing about pass or fail.

A HIGH RATIO IS A LEAD, NOT A VERDICT.  A 40-line header explaining what
a module is for, in front of 60 lines of code, is the tool working; a
1000-line append-only changelog of every time a knob moved is the thing
this exists to find.  Open the file before believing the number.

Counting is scc's (`scc --by-file --format json`), so "comment" means
whatever scc's lexer says it means for that language.  One correction is
applied on top: scc counts a Python docstring as code, which under-reports
every well-documented script under utils/, so module, class and function
docstrings are moved to the comment column for .py files.  Anything else
scc miscounts, it miscounts here too.

Default file set: git-tracked files under src/, test/, utils/ and .ci/,
plus the Makefile.  fe/ is a submodule with its own conventions and is
never included.  Explicit paths as arguments replace that set.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PATHS = ["src", "test", "utils", ".ci", "Makefile"]


def tracked_files(paths: list[str]) -> list[str]:
	"""Everything git tracks under these paths, submodules excluded.

	`git ls-files` lists a submodule as one entry for its directory, and
	fe/ is not asked for anyway, so nothing under it can appear.
	"""
	out = subprocess.run(
	    ["git", "-C", str(ROOT), "ls-files", "-z", "--"] + paths,
	    check=True, capture_output=True, text=True).stdout
	return [name for name in out.split("\0") if name]


def scc_rows(scc: str, files: list[str]) -> list[dict]:
	"""One scc file record per file, flattened out of its language groups."""
	try:
		out = subprocess.run(
		    [scc, "--by-file", "--format", "json", "--"] + files,
		    check=True, capture_output=True, text=True).stdout
	except FileNotFoundError:
		sys.exit("comment_ratio: %s not found on PATH" % scc)
	except subprocess.CalledProcessError as exc:
		sys.exit("comment_ratio: %s failed: %s" % (scc, exc.stderr.strip()))
	rows = []
	for language in json.loads(out):
		for row in language.get("Files", []):
			if row.get("Binary"):
				continue
			row["Language"] = language.get("Name", "")
			rows.append(row)
	return rows


def docstring_lines(path: Path) -> int:
	"""Lines of module, class and function docstring in a Python file.

	scc calls these code, since a docstring is an expression statement.
	They are documentation by every other measure, so this puts them back.
	"""
	try:
		tree = ast.parse(path.read_text(encoding="utf-8"))
	except (OSError, SyntaxError, ValueError):
		return 0
	total = 0
	for node in ast.walk(tree):
		if not isinstance(node, (ast.Module, ast.ClassDef,
		                         ast.FunctionDef, ast.AsyncFunctionDef)):
			continue
		body = getattr(node, "body", None)
		if not body or not isinstance(body[0], ast.Expr):
			continue
		value = body[0].value
		if not isinstance(value, ast.Constant) or not isinstance(value.value, str):
			continue
		total += value.end_lineno - value.lineno + 1
	return total


def main() -> int:
	parser = argparse.ArgumentParser(
	    description=__doc__,
	    formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("paths", nargs="*",
	    help="files to rank; default is the tracked set described above")
	parser.add_argument("--top", type=int, default=0, metavar="N",
	    help="print only the N most comment-heavy files")
	parser.add_argument("--min-lines", type=int, default=1, metavar="N",
	    help="skip files shorter than N lines (default 1)")
	parser.add_argument("--scc", default=os.environ.get("SCC", "scc"),
	    help="scc binary to count with (default $SCC, else scc)")
	args = parser.parse_args()

	files = args.paths or tracked_files(DEFAULT_PATHS)
	if not files:
		sys.exit("comment_ratio: no files to rank")

	rows = []
	for row in scc_rows(args.scc, files):
		location = row.get("Location") or row.get("Filename", "?")
		lines = row.get("Lines", 0)
		comment = row.get("Comment", 0)
		if location.endswith(".py"):
			comment += docstring_lines(ROOT / location)
		if lines < max(args.min_lines, 1):
			continue
		rows.append((100.0 * comment / lines, comment, lines, location))
	rows.sort(key=lambda row: (-row[0], -row[1], row[3]))

	shown = rows[:args.top] if args.top > 0 else rows
	print("%7s %7s %7s  %s" % ("comment", "lines", "share", "file"))
	for share, comment, lines, location in shown:
		print("%7d %7d %6.1f%%  %s" % (comment, lines, share, location))
	total_comment = sum(row[1] for row in rows)
	total_lines = sum(row[2] for row in rows)
	print("%7d %7d %6.1f%%  %d files (%d shown)"
	      % (total_comment, total_lines,
	         100.0 * total_comment / total_lines if total_lines else 0.0,
	         len(rows), len(shown)))
	return 0


if __name__ == "__main__":
	sys.exit(main())
