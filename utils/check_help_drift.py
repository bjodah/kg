#!/usr/bin/env python3
"""Every key the built-in help names must also appear in doc/kg.1.

Deliberately listy and dumb.  It does not parse English and it does not
know what a command does; it takes the key column out of src/help.c's
box-drawn table, expands the "A/B" shorthands the table uses to fit 9
columns, and asks whether each resulting sequence is spelled anywhere in
the man page.  That catches the failure this exists for: a binding added
to the help table (or renamed there) and never written into kg(1).

The other direction is deliberately not checked.  kg(1) documents far
more than the one-screen help table shows, so "in the man page but not in
help" is the normal state, not drift.

Prints the number of keys it checked, because a table it failed to parse
would otherwise look exactly like a pass.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELP_C = ROOT / "src" / "help.c"
MAN = ROOT / "doc" / "kg.1"

# The help table writes keys in nine columns, the man page in prose and
# roff.  Each entry here is one spelling difference that is not drift, and
# every one of them has to be written down rather than silently tolerated.
#
# Abbreviations the help table uses for a key name, expanded wherever they
# appear: "C-u C-SPC" is looked up as "C-u C-Space".
NAMES = {
    "SPC": "Space",
    "BS": "Backspace",
    "DEL": "Delete",
}
# Whole keys the man page spells another way entirely.  The arrow keys are
# roff escapes there; the CUA trio is written out.
WHOLE_KEYS = {
    "C-up": r"C-\(ua",
    "C-dn": r"C-\(da",
    "S-arrow": r"S-\(<-",
    "S-Del": "Shift-Delete",
    "S-Ins": "Shift-Insert",
    "C-Ins": "Ctrl-Insert",
}
# Key names that are whole keys on their own, so the alternative before
# the slash lends them no modifier: "C-d/DEL" is C-d and DEL, where
# "M-\\/SPC" is M-\\ and M-SPC.
STANDALONE = {"DEL", "BS", "RET", "F3", "F4"}


def help_table_lines() -> list[str]:
	"""The kg_help_lines[] initialiser, one element per list entry."""
	src = HELP_C.read_text(encoding="utf-8")
	m = re.search(r"kg_help_lines\[\]\s*=\s*\{(.*?)NULL\s*\};", src, re.S)
	if not m:
		sys.exit("check_help_drift: kg_help_lines[] not found in src/help.c")
	body = m.group(1)
	out: list[str] = []
	cur: list[str] = []
	i = 0
	while i < len(body):
		c = body[i]
		if c == '"':
			j = i + 1
			piece: list[str] = []
			while body[j] != '"':
				if body[j] == "\\":
					piece.append(body[j + 1])
					j += 2
				else:
					piece.append(body[j])
					j += 1
			cur.append("".join(piece))
			i = j + 1
		elif c == ",":
			if cur:
				out.append("".join(cur))
				cur = []
			i += 1
		else:
			i += 1
	if cur:
		out.append("".join(cur))
	return out


def is_key_cell(cell: str) -> bool:
	"""Section headings are the only all-caps cells; keys always have
	either a modifier or a lowercase word beside them."""
	return bool(cell.strip()) and any(ch.islower() for ch in cell)


def sections(lines: list[str]) -> list[list[list[str]]]:
	"""Table body rows, split into their three cells and grouped by the
	rule lines between them.  The two halves of the table use different
	key-column widths, so each block is measured on its own."""
	out: list[list[list[str]]] = []
	block: list[list[str]] = []
	for line in lines:
		if not line.startswith("│"):
			if block:
				out.append(block)
				block = []
			continue
		cells = line.split("│")[1:-1]
		if len(cells) == 3:
			block.append(cells)
	if block:
		out.append(block)
	return out


def key_fields(rows: list[list[str]]) -> list[str]:
	"""The key half of every key cell.

	The table's two halves use different key-column widths (9 and 11), so
	the width is measured rather than assumed: it is the earliest a
	description starts in that column, over the rows that separate key
	from description by two spaces or more.  A row whose key happens to
	fill the column exactly -- "M-d/M-BS kill word" -- then still splits
	correctly, and the assertion below is what says so.
	"""
	keys: list[str] = []
	for col in range(3):
		starts = []
		for row in rows:
			cell = row[col]
			if not is_key_cell(cell):
				continue
			m = re.search(r"\s{2,}", cell[1:])
			if m:
				starts.append(1 + m.end())
		if not starts:
			continue
		width = min(starts)
		for row in rows:
			cell = row[col]
			if not is_key_cell(cell):
				continue
			if len(cell) <= width or cell[width - 1] != " ":
				sys.exit(
				    "check_help_drift: column %d width %d cuts a cell: %r"
				    % (col, width, cell))
			key = cell[1:width].strip()
			if key:
				keys.append(key)
	return keys


def expand(key: str) -> list[str]:
	""""C-f/C-b" is two keys; "M-C-s/r" is M-C-s and M-C-r."""
	parts = key.split("/")
	if len(parts) == 1:
		return [key]
	out = [parts[0]]
	for part in parts[1:]:
		if "-" in part or " " in part or part in STANDALONE:
			out.append(part)
			continue
		# A bare tail inherits the modifiers of the alternative before it:
		# "C-x 0/1" is C-x 0 and C-x 1, "M-C-s/r" is M-C-s and M-C-r.
		head = out[-1]
		cut = max(head.rfind("-"), head.rfind(" "))
		out.append(head[:cut + 1] + part if cut >= 0 else part)
	return out


def man_spelling(key: str) -> str:
	"""How doc/kg.1 would write this key."""
	if key in WHOLE_KEYS:
		return WHOLE_KEYS[key]
	return re.sub(r"\b(?:%s)\b" % "|".join(NAMES),
	    lambda m: NAMES[m.group(0)], key)


def main() -> int:
	# Strip the two roff escapes that only exist to stop a punctuation
	# character being read as a macro argument, so "M-\&;" in the source
	# is compared as the "M-;" a reader sees.
	man = MAN.read_text(encoding="utf-8").replace(r"\&", "").replace(r"\e", "\\")
	checked = 0
	missing: list[tuple[str, str]] = []
	fields = []
	for block in sections(help_table_lines()):
		fields += key_fields(block)
	for field in fields:
		for key in expand(field):
			checked += 1
			wanted = man_spelling(key)
			if wanted not in man:
				missing.append((key, wanted))
	if checked < 50:
		print("check_help_drift: only %d keys parsed out of the help table;"
		      " the parser has lost track of it" % checked, file=sys.stderr)
		return 1
	for key, wanted in missing:
		print("check_help_drift: src/help.c names %r, doc/kg.1 has no %r"
		      % (key, wanted), file=sys.stderr)
	if missing:
		print("check_help_drift: %d of %d help keys are undocumented"
		      % (len(missing), checked), file=sys.stderr)
		return 1
	print("check_help_drift: %d help keys all appear in doc/kg.1" % checked)
	return 0


if __name__ == "__main__":
	sys.exit(main())
