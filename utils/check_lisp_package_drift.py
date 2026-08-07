#!/usr/bin/env python3
"""A PTY case that plants a tracked lisp/ package must plant it verbatim.

Sub-plan 10D Part 1's drift gate, in `lisp-prelude-check`'s mold: a dumb
structural check that two copies of one source have not drifted apart.

`config_files:` in a PTY case is inline YAML, so a case that exercises
`lisp/auto-fill.el` carries a *copy* of it inside the case file.  The
tracked file said, in a comment, that the copy was byte-for-byte
identical; it was not -- the copy had lost every comment and header line
and carried a shortened `auto-fill-mode' docstring -- and nothing
noticed for two phases.  A drifted copy is worse than no copy: the case
goes green while testing a package the repository does not ship.

The rule, and it is deliberately general rather than a list of file
names: for every `test/pty/*.yaml` and every `config_files` key of the
form `.config/kg/lisp/NAME.el`, if `lisp/NAME.el` exists in the
repository then the planted text must equal it byte for byte.  A case
that plants a package kg does not ship (a fixture invented for the case)
is untouched -- there is nothing for it to drift from.

Healing a failure is a copy, not a merge: put the tracked file's exact
bytes back into the case, indented under the block scalar.  If the case
genuinely needs a *different* package, give it a different name so this
check stops claiming they are the same thing.
"""

from __future__ import annotations

import difflib
import re
import sys
from pathlib import Path

try:
	import yaml
except ImportError:  # pragma: no cover - reported, not crashed
	yaml = None

ROOT = Path(__file__).resolve().parent.parent
PTY_DIR = ROOT / "test" / "pty"
LISP_DIR = ROOT / "lisp"
PLANTED_RE = re.compile(r"^\.config/kg/lisp/(?P<name>[^/]+\.el)$")


def check_case(path: Path) -> tuple[list[str], int]:
	try:
		data = yaml.safe_load(path.read_text(encoding="utf-8"))
	except yaml.YAMLError as exc:
		return [f"{path.relative_to(ROOT)}: cannot parse: {exc}"], 0
	if not isinstance(data, dict):
		return [], 0
	planted = data.get("config_files") or {}
	if not isinstance(planted, dict):
		return [], 0

	errors = []
	tracked_count = 0
	for key, text in sorted(planted.items()):
		match = PLANTED_RE.match(str(key))
		if not match:
			continue
		tracked = LISP_DIR / match.group("name")
		if not tracked.is_file():
			# A fixture package invented for this case; nothing to
			# drift from.
			continue
		tracked_count += 1
		want = tracked.read_text(encoding="utf-8")
		if text == want:
			continue
		diff = "\n".join(
			list(difflib.unified_diff(
				want.splitlines(), str(text).splitlines(),
				fromfile=str(tracked.relative_to(ROOT)),
				tofile=f"{path.relative_to(ROOT)}:{key}",
				lineterm=""))[:40])
		errors.append(
			f"{path.relative_to(ROOT)} plants {key}, which has drifted "
			f"from {tracked.relative_to(ROOT)}:\n{diff}")
	return errors, tracked_count


def main() -> int:
	if yaml is None:
		print("SKIP: lisp-package-check needs PyYAML "
		      "(the PTY harness needs it too; use the same interpreter)")
		return 0

	cases = sorted(PTY_DIR.glob("*.yaml"))
	errors: list[str] = []
	planted_count = 0
	for case in cases:
		found, count = check_case(case)
		errors += found
		planted_count += count

	print(f"lisp-package-check: {planted_count} planted copy/copies of a "
	      f"tracked lisp/ package across {len(cases)} PTY case(s), "
	      f"{len(errors)} drifted")
	if errors:
		print("FAIL:", file=sys.stderr)
		for line in errors:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
