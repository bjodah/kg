#!/usr/bin/env python3
"""Structural checks on the two-repository Lisp compatibility manifest.

kg's half of Phase 0 sub-plan 00C
(doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/00c-feature-inventory.md),
built on the mechanism sub-plan 00B landed in the fe submodule
(fe/compat/, fe/utils/{run-emacs-oracle,run-fe-compat,check_compat_manifest}.py).
Deliberately dumb and listy, in check_help_drift.py's shape: this does not
run fe, kg, or Emacs -- it only checks that the manifest and the source it
claims to describe have not drifted apart.

Three things, in order:

1. Reuse (not reimplement) fe/utils/check_compat_manifest.py's schema and
   per-entry validation for both manifests, each pointed at the other via
   --other-manifest so the two id spaces are also checked for collisions.
2. The check that keeps the inventory alive: every one of Fe's 54
   primitives + 1 alias (fe/fe.c's primitive_names[]/primitive_aliases[]),
   kg's 78 natives (src/lisp_prelude.c's native_bindings[]), and kg's 53
   prelude definitions (lisp/prelude.el's top-level "(defalias 'NAME ...)"
   forms) appears in exactly one feature entry's "source_name" field, across
   the two manifests combined. A native or prelude definition added without a
   manifest entry fails this.
3. Two rules 00B's own checker did not need yet: every status="planned"
   entry's rationale names a phase ("Phase <digit>"), and the "defcustom"
   entry exists with the shape 00C's gate specifies.
4. Every test a feature entry's "kg_test" cites exists: the file is
   there, a C file is cited *with* a function (a bare .c names no test),
   and the named function is defined in it. 00B's checker only asks that
   the field is non-empty, so a renamed test or a moved PTY case left the
   manifest pointing at nothing.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FE_C = ROOT / "fe" / "fe.c"
LISP_PRELUDE_C = ROOT / "src" / "lisp_prelude.c"
LISP_PRELUDE_EL = ROOT / "lisp" / "prelude.el"
FE_MANIFEST = ROOT / "fe" / "compat" / "features.json"
KG_MANIFEST = ROOT / "test" / "lisp-compat" / "features.json"
FE_CHECKER = ROOT / "fe" / "utils" / "check_compat_manifest.py"


def strip_c_comments(text: str) -> str:
	return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def parse_fe_primitives() -> set[str]:
	"""54 primitive names plus the 1 alias ('fn' -> 'lambda') from fe.c."""
	text = strip_c_comments(FE_C.read_text(encoding="utf-8"))
	names_block = re.search(
		r"static const char\* primitive_names\[\] = \{(.*?)\};", text, re.S)
	if not names_block:
		raise SystemExit("FAIL: could not find primitive_names[] in fe/fe.c")
	names = set(re.findall(r'=\s*"([^"]+)"', names_block.group(1)))

	aliases_block = re.search(
		r"static const PrimitiveAlias primitive_aliases\[\] = \{(.*?)\};",
		text, re.S)
	if not aliases_block:
		raise SystemExit(
			"FAIL: could not find primitive_aliases[] in fe/fe.c")
	aliases = set(re.findall(r'\{"([^"]+)"', aliases_block.group(1)))

	return names | aliases


def parse_kg_natives() -> set[str]:
	"""78 native_bindings[] names from src/lisp_prelude.c."""
	text = strip_c_comments(LISP_PRELUDE_C.read_text(encoding="utf-8"))
	block = re.search(
		r"static const struct native_binding native_bindings\[\] = \{"
		r"(.*?)\n\};", text, re.S)
	if not block:
		raise SystemExit(
			"FAIL: could not find native_bindings[] in src/lisp_prelude.c")
	return set(re.findall(r'\{\s*"([^"]+)"', block.group(1)))


def parse_kg_prelude_defs() -> set[str]:
	"""53 top-level "(defalias 'NAME ...)" definitions from lisp/prelude.el.

	Sub-plan 01A moved these out of three C string literals in
	src/lisp_prelude.c and into a real Lisp source file, so this reads the
	file rather than un-escaping C.  Sub-plan 02D's dialect cutover deleted
	the kg-owned `setq` macro (built on assignment `=`) and rewrote the
	definitions from `=` to core `setq`, making them column-zero
	"(setq NAME ...)" forms.  Sub-plan 04E's Lisp-2 cut retargeted the
	function cell and respelled them "(defalias 'NAME ...)", deleting the
	identity-lambda `function` alias (count 52).  Sub-plan 06E added
	`ignore-errors` (count 53).  Nothing nested is ever in column 0, because
	every continuation line in the file is indented.
	"""
	text = LISP_PRELUDE_EL.read_text(encoding="utf-8")
	names = set(re.findall(r"(?m)^\(defalias '(\S+)", text))
	if not names:
		raise SystemExit(
			"FAIL: no top-level (defalias 'NAME ...) forms in "
			"lisp/prelude.el")
	return names


def load_manifest(path: Path) -> dict:
	with open(path, "r", encoding="utf-8") as fp:
		return json.load(fp)


def run_fe_checker(manifest: Path, other: Path) -> list[str]:
	proc = subprocess.run(
		[sys.executable, str(FE_CHECKER),
		 "--manifest", str(manifest), "--other-manifest", str(other)],
		capture_output=True, text=True, cwd=ROOT)
	print(proc.stdout, end="")
	if proc.returncode != 0:
		return [line for line in proc.stderr.splitlines() if line.strip()]
	return []


def _claims_in(data: dict, manifest_path: Path) -> dict[str, list[str]]:
	claims: dict[str, list[str]] = {}
	for feature in data.get("features", []):
		name = feature.get("source_name")
		if name is None:
			continue
		claims.setdefault(name, []).append(
			f"{manifest_path.relative_to(ROOT)}:{feature.get('id')}")
	return claims


def _check_pool(pool_name: str, expected_names: set[str],
		 claims: dict[str, list[str]]) -> list[str]:
	"""Coverage, not uniqueness: every name in the pool has to be claimed
	by at least one entry, and no entry may claim a name that is not in
	the pool.  More than one entry per name is legitimate -- one source
	declaration can have several separately pinned behaviours, each with
	its own case and its own oracle snapshot, which is how fe's manifest
	splits `funcall` into `lisp2-funcall-designator` (a symbol operand is
	resolved through its function cell) and `lisp2-funcall-callable-kind`
	(a macro or special-form operand is `invalid-function`).  This check
	insisted on exactly one until that split landed; the invariant it
	exists for -- a primitive, native or prelude definition added without
	a manifest entry -- is the "no entry" clause alone.
	"""
	errors = []
	for name in sorted(expected_names):
		locs = claims.get(name, [])
		if not locs:
			errors.append(
				f"{pool_name} {name!r} has no feature entry naming it "
				f"as source_name")
	unknown = set(claims) - expected_names
	for name in sorted(unknown):
		errors.append(
			f"feature entry source_name {name!r} ({claims[name]}) is not "
			f"a {pool_name} found in source -- stale entry or a typo'd "
			f"source_name")
	return errors


def check_source_coverage(fe_data: dict, kg_data: dict) -> list[str]:
	"""Each of the three source pools is checked for 1:1 coverage
	*within the manifest that owns it*, not as one name flattened across
	both files: fe's raw `let` primitive and kg's prelude `let` macro that
	shadows it are two different source declarations (fe.c's
	primitive_names[] vs lisp/prelude.el) that happen
	to share a spelling, and both legitimately get their own entry, in
	their own manifest -- see fe/compat/features.json's primitive-let and
	test/lisp-compat/features.json's prelude-let. Flattening all three
	pools into one global name set would wrongly flag that as a
	duplicate claim.
	"""
	fe_primitives = parse_fe_primitives()
	kg_natives = parse_kg_natives()
	kg_prelude_defs = parse_kg_prelude_defs()
	print(f"source inventory: {len(fe_primitives)} fe primitives/aliases, "
	      f"{len(kg_natives)} kg natives, {len(kg_prelude_defs)} kg "
	      f"prelude definitions")

	fe_claims = _claims_in(fe_data, FE_MANIFEST)
	kg_claims = _claims_in(kg_data, KG_MANIFEST)

	overlap = kg_natives & kg_prelude_defs
	if overlap:
		# Would silently break the combined-pool check below by letting
		# a native's claim satisfy a prelude definition's coverage (or
		# vice versa) without anyone noticing; fail loudly instead if
		# this pool assumption ever stops holding.
		return [f"kg native and kg prelude definition source pools are "
			f"no longer disjoint ({sorted(overlap)}); "
			f"check_source_coverage's combined-pool check assumes they "
			f"are and needs splitting"]

	errors: list[str] = []
	errors += _check_pool("fe primitive/alias", fe_primitives, fe_claims)
	errors += _check_pool("kg native or prelude definition",
			       kg_natives | kg_prelude_defs, kg_claims)
	return errors


PHASE_RE = re.compile(r"Phase \d")


def check_planned_names_phase(data: dict, path: Path) -> list[str]:
	errors = []
	for feature in data.get("features", []):
		if feature.get("status") != "planned":
			continue
		rationale = feature.get("rationale") or ""
		if not PHASE_RE.search(rationale):
			errors.append(
				f"{path.relative_to(ROOT)}:{feature.get('id')}: "
				f"status=planned but rationale names no phase "
				f"(expected to match {PHASE_RE.pattern!r})")
	return errors


REQUIRED_DEFCUSTOM_PHRASES = [
	"unbound",
	"bound",
	"standard",
	"docstring",
	":type",
	":options",
	":group",
	":tag",
	":link",
	":version",
	":package-version",
	"unknown keyword",
	"dynamic binding",
	"setopt",
	"Customize",
]


def check_defcustom(kg_data: dict) -> list[str]:
	errors = []
	entries = [f for f in kg_data.get("features", []) if f.get("id") == "defcustom"]
	if not entries:
		return ["test/lisp-compat/features.json: no 'defcustom' entry"]
	entry = entries[0]
	if entry.get("status") != "supported":
		errors.append(f"defcustom: status is {entry.get('status')!r}, "
			      f"expected 'supported'")
	if entry.get("owner") != "kg":
		errors.append(f"defcustom: owner is {entry.get('owner')!r}, "
			      f"expected 'kg'")
	if entry.get("comparison") != "emacs":
		errors.append(f"defcustom: comparison is {entry.get('comparison')!r}, "
			      f"expected 'emacs'")
	if not PHASE_RE.search(entry.get("rationale") or ""):
		errors.append("defcustom: rationale names no phase")
	rationale = entry.get("rationale") or ""
	missing_phrases = [p for p in REQUIRED_DEFCUSTOM_PHRASES
			    if p.lower() not in rationale.lower()]
	if missing_phrases:
		errors.append(
			f"defcustom: rationale is missing the case distinctions "
			f"00C's gate specifies (phrases not found: {missing_phrases})")
	return errors


KG_TEST_REF_RE = re.compile(
	r"[A-Za-z0-9_./-]+\.(?:c|h|yaml|el)(?::[A-Za-z_][A-Za-z0-9_]*)?")


def _function_is_defined(text: str, name: str) -> bool:
	"""A C definition or declaration of `name` at the start of a line.

	Deliberately dumb, like the rest of this file: kg's tests are
	`static void test_x(void)` at column 0, so "a line that starts with
	a type and reaches `name(`" is enough. It is not a parser and does
	not need to be -- the drift it catches is a renamed or deleted test,
	not a subtle one.
	"""
	return re.search(r"(?m)^\w[\w *]*\b" + re.escape(name) + r"\s*\(",
			 text) is not None


def check_kg_test_targets(data: dict, path: Path) -> list[str]:
	"""Every test a feature entry cites has to exist.

	fe/utils/check_compat_manifest.py checks that `kg_test` is non-empty
	for the entries that require one, and stops there -- so a renamed
	test function, or a PTY case that was deleted or moved, leaves a row
	citing evidence that is not there, and every reader downstream (the
	manifest is the thing a review reads to decide whether a behaviour is
	pinned) believes it.

	A `kg_test` field is prose that *contains* references, not a
	reference list: some entries name two files, some add an explanation
	after them. So references are extracted rather than parsed out of a
	fixed shape -- a token ending in .c/.h/.yaml/.el, optionally followed
	by `:function`. Every extracted token must resolve, and a field with
	no token at all is an entry that cites nothing.

	Paths are resolved from the *superproject* root in both manifests:
	fe's own rows say `fe/test_api.c`, which is where that file is from
	here.
	"""
	errors = []
	for feature in data.get("features", []):
		claim = feature.get("kg_test")
		if not claim:
			continue
		where = f"{path.relative_to(ROOT)}:{feature.get('id')}"
		refs = [m.group(0) for m in KG_TEST_REF_RE.finditer(claim)]
		if not refs:
			errors.append(
				f"{where}: kg_test names no test file "
				f"({claim!r})")
			continue
		for ref in refs:
			file_part, _, symbol = ref.partition(":")
			target = ROOT / file_part
			if not target.is_file():
				errors.append(
					f"{where}: kg_test names {file_part}, "
					f"which does not exist")
				continue
			if not symbol and file_part.endswith((".c", ".h")):
				# A C file on its own cites no test. Sub-plan 10C
				# closed this: the rule is "an existing test
				# *function* or a case file", and a bare .c
				# satisfied the first half by naming a file that
				# happens to contain tests.
				errors.append(
					f"{where}: kg_test names the C file "
					f"{file_part} but no function in it; "
					f"cite {file_part}:test_something")
				continue
			if symbol and not _function_is_defined(
					target.read_text(encoding="utf-8"),
					symbol):
				errors.append(
					f"{where}: kg_test names {symbol}(), "
					f"which {file_part} does not define")
	return errors


def check_orphan_snapshots(kg_data: dict) -> list[str]:
	"""An oracle snapshot no comparison=emacs case asks for.

	fe's own checker catches an orphan *case* file but not an orphan
	*snapshot*, which is how a hand-written
	oracle/interactive-prompting-metadata.json -- with a truncated
	emacs_version banner, so plainly never produced by the runner --
	survived beside a kg-policy row that by design has no snapshot to
	compare. A checked-in snapshot is a claim that the pinned Emacs
	answered this; one nothing asks for is a claim nothing tests.
	"""
	wanted = {
		cid
		for feature in kg_data.get("features", [])
		if feature.get("comparison") == "emacs"
		for cid in (feature.get("cases") or [])
	}
	oracle_dir = KG_MANIFEST.parent / "oracle"
	errors = []
	for path in sorted(oracle_dir.glob("*.json")):
		if path.stem not in wanted:
			errors.append(
				f"{path.relative_to(ROOT)}: oracle snapshot for a case "
				f"no comparison=emacs feature entry names")
	return errors


def main() -> int:
	errors: list[str] = []

	errors += run_fe_checker(KG_MANIFEST, FE_MANIFEST)
	errors += run_fe_checker(FE_MANIFEST, KG_MANIFEST)

	fe_data = load_manifest(FE_MANIFEST)
	kg_data = load_manifest(KG_MANIFEST)

	errors += check_source_coverage(fe_data, kg_data)
	errors += check_planned_names_phase(fe_data, FE_MANIFEST)
	errors += check_planned_names_phase(kg_data, KG_MANIFEST)
	errors += check_defcustom(kg_data)
	errors += check_orphan_snapshots(kg_data)
	errors += check_kg_test_targets(kg_data, KG_MANIFEST)
	errors += check_kg_test_targets(fe_data, FE_MANIFEST)

	total = len(fe_data.get("features", [])) + len(kg_data.get("features", []))
	print(f"lisp compat check: {total} feature(s) across both manifests, "
	      f"{len(errors)} problem(s)")
	if errors:
		print("FAIL:", file=sys.stderr)
		for line in errors:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
