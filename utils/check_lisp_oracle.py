#!/usr/bin/env python3
"""Run kg's `comparison: emacs` corpus against the checked-in Emacs snapshots.

kg's half of the milestone gate's oracle item (sub-plan 10C Part 3, 10A
Decision 3).  fe's half has been automated since 00B
(`fe/utils/run-fe-compat.py`); kg's was a sentence in
`test/lisp-compat/README.md` telling a human to read the case and the
snapshot side by side.  This is the command that exits non-zero instead.

No Emacs is required or invoked: `oracle/<id>.json` *is* the pinned
oracle's answer, recorded by `make lisp-compat-oracle`.  What this runs is
kg, once per case, through `test/kgbatch` -- the editor's own objects and
its own `kg_lisp_eval_string()`, with kgbatch's `-p` (prin1-shaped
printing, so a string-valued case is compared as `"abc"` and not `abc`)
and `-b` (a live scratch buffer, so a case that reads point runs instead
of raising `current buffer is dead`).

HOW A RECORD IS DECIDED

Structurally, from kgbatch's exit status, never by pattern-matching
prose:

  * exit 0            -> {"kind": "value", "printed": <the rendering>}
  * exit non-zero     -> {"kind": "condition", "condition_source":
                          "message", "message": <the error text>}
  * no exit in time   -> {"kind": "timeout"}
  * feature status "unsupported" -> decided from the manifest, kg is
    never run (the manifest already says kg cannot do this; running it
    would only ask which error a missing name happens to produce)

kg has no host-visible condition *symbol* -- `lisp.h` exports the
completion kind (error/quit/budget) and the message text, not the
condition object -- so a condition record is compared the weaker way
fe's runner already documents for its own message-source records: the
oracle's condition name must appear in kg's message.  kg's messages lead
with the condition name (`void-function no-such-fn`,
`wrong-number-of-arguments`), so this is a real check and not a
formality, but it is a substring claim and this file says so rather than
letting a reader assume symbol equality.  Narrowing it is condition-data
work in `src/lisp.h`, not runner work.

THE XPASS RULE, WHICH FE'S RUNNER DOES NOT HAVE

`fe/utils/run-fe-compat.py` prints `agrees early` and counts a pass when
a case whose feature is *not* `supported` nevertheless matches the
oracle.  That tolerance is why a stale gap row can sit in a manifest
forever: nothing ever tells you the divergence stopped diverging.

Here, a `divergent` case that agrees with the oracle is a FAILURE, for
the same reason `XPASS` fails kg's PTY suite: the manifest is claiming a
disagreement that is no longer there, and the row (and its rationale, and
usually a `doc/fe-upstream.md` entry beside it) has to be corrected before
the run goes green again.  `planned` keeps the softer treatment -- a
planned feature that already works is progress, not a lie -- and is
reported without failing.

  status        agrees with oracle      disagrees
  ----------    --------------------    ---------------------------
  supported     pass                    FAIL
  divergent     FAIL (XPASS)            pass, counted as a divergence
  planned       reported, not a pass    reported, not a failure
  unsupported   kg never run; the manifest decides the record

A feature's status is a property of the *feature*, and a divergent
feature can legitimately have cases on both sides of the line -- one
pinning the part kg gets right, one pinning the disagreement itself.  A
case says so for itself with `"expect": "agree"` or `"expect":
"diverge"` in its own file; without the field the feature's status
decides, which is what every case written before this runner existed
relies on.  `native-type-of` is the worked example: `(type-of 1)` agrees
(`integer`) and `(type-of 1.0)` does not (Emacs `float`, kg `double`),
and before this field the manifest could only claim one of them.

SKIPS

A missing `test/kgbatch` or a missing snapshot SKIPs with a printed
reason, the same discipline `utils/pty_accept.py` applies to a missing
tmux or Emacs -- and `--require-tools` turns that into an upfront failure
naming what is missing, exactly as the PTY harness's flag does.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CORPUS = ROOT / "test" / "lisp-compat"
DEFAULT_KGBATCH = ROOT / "test" / "kgbatch"
DEFAULT_TIMEOUT = 10.0


def load_manifest(corpus_root: Path) -> dict:
	with open(corpus_root / "features.json", "r", encoding="utf-8") as fp:
		return json.load(fp)


def case_source(case: dict) -> str:
	"""The setup forms then the expression, one form per line.

	kgbatch's `-p` wraps the whole file in `(format "%S" (progn ...))`,
	so the file's value is the expression's value -- the same shape
	oracle/emacs-shim.el runs (every setup form, then the expr).
	"""
	return "\n".join(list(case.get("setup", [])) + [case["expr"]]) + "\n"


def run_kg_case(kgbatch: Path, case: dict, timeout: float):
	"""One kg record, or (None, runner_error)."""
	with tempfile.NamedTemporaryFile(
			mode="w", suffix=".el", delete=False,
			encoding="utf-8") as tmp:
		tmp.write(case_source(case))
		tmp_path = Path(tmp.name)
	try:
		try:
			proc = subprocess.run(
				[str(kgbatch), "-p", "-b", str(tmp_path)],
				capture_output=True, timeout=timeout)
		except subprocess.TimeoutExpired:
			return {"kind": "timeout", "seconds": timeout}, None
	finally:
		tmp_path.unlink(missing_ok=True)

	prefix = f"{tmp_path}: "
	if proc.returncode < 0:
		return None, (
			f"kgbatch was killed by signal {-proc.returncode}; that is "
			f"a crash or a runner bug, not a documented divergence")
	if proc.returncode == 0:
		printed = proc.stdout.decode("utf-8", "replace").strip("\n")
		if not printed.startswith(prefix):
			return None, (f"kgbatch printed {printed!r}, which does not "
				      f"start with the path it was given")
		return {"kind": "value", "printed": printed[len(prefix):]}, None

	message = proc.stderr.decode("utf-8", "replace").strip("\n")
	if message.startswith(prefix):
		message = message[len(prefix):]
	if not message:
		return None, (f"kgbatch exited {proc.returncode} with nothing on "
			      f"stderr; there is no record to compare")
	return {"kind": "condition", "condition_source": "message",
		"message": message}, None


def records_agree(oracle: dict, kg: dict) -> bool:
	if oracle.get("kind") != kg.get("kind"):
		return False
	kind = oracle["kind"]
	if kind == "value":
		return oracle.get("printed") == kg.get("printed")
	if kind == "condition":
		# The substring claim the module docstring explains: kg has no
		# host-visible condition symbol to compare, so the oracle's
		# condition name has to appear in kg's message.
		condition = oracle.get("condition", "")
		return bool(condition) and condition in kg.get("message", "")
	if kind == "unsupported":
		return oracle.get("feature") == kg.get("feature")
	# "quit", "timeout": kind equality is the whole claim.
	return True


def emacs_cases(manifest: dict) -> list[tuple[str, dict]]:
	"""(case_id, feature) for every comparison=emacs case, id-unique.

	A case id may be listed by more than one feature entry; it is run
	once, against the first entry that names it, which is also how the
	snapshot directory is keyed.
	"""
	seen: dict[str, dict] = {}
	for feature in manifest.get("features", []):
		if feature.get("comparison") != "emacs":
			continue
		for case_id in feature.get("cases") or []:
			seen.setdefault(case_id, feature)
	return sorted(seen.items())


class Report:
	def __init__(self) -> None:
		self.passed = 0
		self.divergences = 0
		self.reported = 0
		self.skipped: list[str] = []
		self.failures: list[str] = []


def check_case(case_id, feature, corpus_root, kgbatch, timeout, report):
	case_path = corpus_root / "cases" / f"{case_id}.json"
	if not case_path.is_file():
		report.failures.append(
			f"{case_id}: no cases/{case_id}.json, but "
			f"features.json:{feature['id']} names it")
		return
	with open(case_path, "r", encoding="utf-8") as fp:
		case = json.load(fp)

	oracle_path = corpus_root / "oracle" / f"{case_id}.json"
	if not oracle_path.is_file():
		report.skipped.append(
			f"{case_id}: no oracle snapshot at "
			f"{oracle_path.relative_to(ROOT)} "
			f"(run 'make lisp-compat-oracle')")
		return
	with open(oracle_path, "r", encoding="utf-8") as fp:
		oracle = json.load(fp)["record"]

	status = feature["status"]
	if status == "unsupported":
		kg_record = {"kind": "unsupported", "feature": feature["id"]}
		error = None
	else:
		kg_record, error = run_kg_case(kgbatch, case, timeout)
	if error:
		report.failures.append(f"{case_id}: {error}")
		return

	agree = records_agree(oracle, kg_record)
	# A case may override the verdict its feature's status implies; see
	# the module docstring's note on "expect".
	expect = case.get("expect")
	if expect not in (None, "agree", "diverge"):
		report.failures.append(
			f"{case_id}: \"expect\" is {expect!r}; the only values are "
			f"\"agree\" and \"diverge\"")
		return
	if expect == "agree":
		status = "supported"
	elif expect == "diverge":
		status = "divergent"

	if status == "supported":
		if agree:
			report.passed += 1
		else:
			report.failures.append(
				f"{case_id} (feature {feature['id']}, status "
				f"supported): oracle={oracle!r} kg={kg_record!r}")
	elif status == "divergent":
		if agree:
			# The XPASS rule.  See the module docstring.
			report.failures.append(
				f"{case_id} (feature {feature['id']}, status "
				f"divergent): kg now AGREES with the oracle, so the "
				f"recorded divergence is stale -- re-classify the "
				f"entry (and its doc/fe-upstream.md row) instead of "
				f"leaving it. oracle={oracle!r} kg={kg_record!r}")
		else:
			report.divergences += 1
			print(f"# {case_id} (feature {feature['id']}): recorded "
			      f"divergence, still diverging -- oracle={oracle!r} "
			      f"kg={kg_record!r}")
	else:
		report.reported += 1
		tag = "agrees early" if agree else "known gap"
		print(f"# {case_id} (feature {feature['id']}, status {status}): "
		      f"{tag} -- oracle={oracle!r} kg={kg_record!r}")


SELF_TEST_CASE = {
	"id": "runner-self-test",
	"setup": [],
	"expr": "(+ 1 2)",
	"note": "kgbatch answers 3; the planted snapshot says 4.",
}
SELF_TEST_MANIFEST = {
	"schema": "fe-compat-manifest/1",
	"features": [{
		"id": "runner-self-test",
		"category": "self-test",
		"status": "supported",
		"owner": "kg",
		"comparison": "emacs",
		"cases": ["runner-self-test"],
		"kg_test": "utils/check_lisp_oracle.py",
		"rationale": None,
		"source_name": None,
	}],
}
SELF_TEST_SNAPSHOT = {
	"schema": "fe-compat-oracle/1",
	"case": "runner-self-test",
	"emacs_version": "self-test, no Emacs was run",
	"record": {"kind": "value", "type": "integer", "printed": "4"},
}


def self_test(kgbatch: Path, timeout: float) -> int:
	"""A deliberately broken snapshot in a temp corpus must fail the run.

	The gate this script is has to be shown failing, or "0 failed" means
	nothing.  Everything below is built in a temp directory: no corpus
	file is read, written or regenerated.
	"""
	with tempfile.TemporaryDirectory() as tmp:
		root = Path(tmp)
		(root / "cases").mkdir()
		(root / "oracle").mkdir()
		(root / "features.json").write_text(
			json.dumps(SELF_TEST_MANIFEST), encoding="utf-8")
		(root / "cases" / "runner-self-test.json").write_text(
			json.dumps(SELF_TEST_CASE), encoding="utf-8")
		(root / "oracle" / "runner-self-test.json").write_text(
			json.dumps(SELF_TEST_SNAPSHOT), encoding="utf-8")

		report = Report()
		manifest = load_manifest(root)
		for case_id, feature in emacs_cases(manifest):
			check_case(case_id, feature, root, kgbatch, timeout, report)
		if len(report.failures) != 1 or report.passed:
			print("FAIL: the self-test corpus was expected to produce "
			      f"exactly one failure and no pass, got "
			      f"{len(report.failures)} failure(s) and "
			      f"{report.passed} pass(es)", file=sys.stderr)
			for line in report.failures:
				print(f"  {line}", file=sys.stderr)
			return 1
		print("self-test: a snapshot saying 4 where kg answers 3 fails "
		      "the run, as it must:")
		print(f"  {report.failures[0]}")
	return 0


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--kgbatch", type=Path, default=DEFAULT_KGBATCH)
	parser.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS)
	parser.add_argument("--case", action="append", default=[],
			    help="run only this case id (repeatable)")
	parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
	parser.add_argument("--require-tools", action="store_true",
			    help="a skip becomes a failure, as in "
				 "utils/pty_accept.py")
	parser.add_argument("--self-test", action="store_true",
			    help="prove the runner fails a broken snapshot, in a "
				 "temp corpus; touches no corpus file")
	args = parser.parse_args()

	if not args.kgbatch.is_file():
		reason = (f"{args.kgbatch} is not built "
			  f"(run 'make kgbatch')")
		if args.require_tools:
			print(f"FAIL: {reason}", file=sys.stderr)
			return 1
		print(f"SKIP: kg oracle run skipped -- {reason}")
		return 0

	if args.self_test:
		return self_test(args.kgbatch, args.timeout)

	manifest = load_manifest(args.corpus_root)
	cases = emacs_cases(manifest)
	if args.case:
		wanted = set(args.case)
		cases = [(cid, f) for cid, f in cases if cid in wanted]

	report = Report()
	for case_id, feature in cases:
		check_case(case_id, feature, args.corpus_root, args.kgbatch,
			   args.timeout, report)

	for line in report.skipped:
		print(f"# SKIP {line}")
	print(f"kg oracle: {len(cases)} comparison=emacs case(s), "
	      f"{report.passed} passed, {report.divergences} recorded "
	      f"divergence(s), {report.reported} reported, "
	      f"{len(report.skipped)} skipped, {len(report.failures)} failed")
	if report.skipped and args.require_tools:
		print("FAIL: --require-tools and the run skipped:",
		      file=sys.stderr)
		for line in report.skipped:
			print(f"  {line}", file=sys.stderr)
		return 1
	if report.failures:
		print("FAIL:", file=sys.stderr)
		for line in report.failures:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
