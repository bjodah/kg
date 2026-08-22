#!/usr/bin/env python3
"""Package-scenario compatibility gate for vendored Elisp packages.

Phase M1 of doc/plans/2026-08-21-mature-elisp-master-plan.md.  Reuses the
canonical-record machinery that kg's `comparison: emacs` corpus already
owns -- the same one-line JSON record protocol, the same
test/lisp-compat/oracle/emacs-shim.el (copied under this corpus as
oracle/emacs-shim.el so vendored `external/elpa/' sources resolve), and
the same test/kgbatch wrapper that check_lisp_oracle.py drives.  This is
NOT a second comparison format: a package scenario is a case file with a
setup form that `(load ...)'s a tracked package source, and its oracle is
produced by the pinned Emacs running the same form through the shim.

A scenario pins, per master plan section 3.5, an exact value, an exact
condition, or a deliberately recorded divergence -- never a prose claim
that a package "loads" or "is supported".  The four labels from section
3.1 are kept distinct and the manifest says which one each package has
earned; only `scenario-green` and `supported` are product claims.

The gate fails on each of six conditions, proven in --self-test (modeled
on utils/check_lisp_compat.py's self-test, which proves the
id/snapshot rules fail):

  * a wrong value      -- an `expect: agree` case whose kg value != oracle;
  * a wrong condition  -- an `expect: agree` case whose kg condition != oracle;
  * a stale source hash -- the manifest's recorded SHA256 of the tracked
                           source differs from the file on disk;
  * a missing scenario -- a scenario id the manifest names has no s/<id>.json;
  * a stale snapshot   -- an oracle/<id>.json whose case != id, that is
                           missing, or whose recorded source hash differs
                           from the manifest's (it was generated against an
                           older copy of the package source);
  * an XPASS           -- a case the manifest records as a divergence
                           (`expect: diverge`) that kg now agrees with -- the
                           recorded gap has closed and the manifest row must
                           be corrected, exactly like the XPASS rule in
                           utils/check_lisp_oracle.py.

Snapshot regeneration is a SEPARATE explicit target (`make package-oracle`,
or `--regenerate-oracle` here); the ordinary check regenerates nothing and
runs inside `make check`.  Every oracle/*.json is runner-generated; hand
edits are refused by the source-hash and snapshot-identity rules.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CORPUS = ROOT / "test" / "elisp-packages"
DEFAULT_KGBATCH = ROOT / "test" / "kgbatch"
DEFAULT_EMACS = "/opt-3/emacs-31-lucid/bin/emacs"
DEFAULT_TIMEOUT = 10.0
SCHEMA = "elisp-package-oracle/1"


# --- record parsing, mirrored from utils/check_lisp_oracle.py ----------------

def kg_record_from_output(stdout: str, stderr: str, returncode: int, path: str):
	"""One kg record, or (None, runner_error).

	kgbatch `-r -b` prints `<path>: V:<value>', `<path>: E:<symbol>', or
	`<path>: Q:quit'.  Anything else -- a crash (signal), a reader error
	(returncode > 0 with a message), or a timeout -- is captured as a
	condition record whose source is `message' (the oracle condition name
	is matched as a substring) or as a `timeout' record.
	"""
	prefix = f"{path}: "
	stripped = stdout.strip("\n")
	if returncode < 0:
		return None, (f"kgbatch was killed by signal {-returncode}; that "
			      f"is a crash or a runner bug, not a divergence")
	if returncode == 0:
		if not stripped.startswith(prefix):
			return None, (f"kgbatch printed {stripped!r}, which does "
				      f"not start with the path it was given")
		payload = stripped[len(prefix):]
		if payload.startswith("V:"):
			return {"kind": "value", "printed": payload[2:]}, None
		if payload.startswith("E:") and payload[2:]:
			return {"kind": "condition",
				"condition_source": "structured",
				"condition": payload[2:]}, None
		if payload == "Q:quit":
			return {"kind": "quit"}, None
		return None, (f"kgbatch printed an invalid record {payload!r}; "
			      "expected V:, E:, or Q:quit")
	message = stderr.strip("\n")
	if message.startswith(prefix):
		message = message[len(prefix):]
	if not message:
		return None, (f"kgbatch exited {returncode} with nothing on "
			      f"stderr; there is no record to compare")
	return {"kind": "condition", "condition_source": "message",
		"message": message}, None


def run_kg_case(kgbatch: Path, case: dict, timeout: float, tmpdir: Path):
	"""One kg record for a scenario, or (None, runner_error)."""
	src = "\n".join(list(case.get("setup", [])) + [case["expr"]]) + "\n"
	tmp = tmpdir / "scenario.el"
	tmp.write_text(src, encoding="utf-8")
	try:
		try:
			proc = subprocess.run(
				[str(kgbatch), "-r", "-b", str(tmp)],
				capture_output=True, timeout=timeout)
		except subprocess.TimeoutExpired:
			return {"kind": "timeout",
				"seconds": timeout}, None
		# A NONZERO `-r` RUN IS THE READER'S; a reader error in the case
		# swallows the wrapper's closing text, so the diagnostic is taken
		# from a second unwrapped evaluation of the same file.
		if proc.returncode > 0:
			try:
				plain = subprocess.run(
					[str(kgbatch), "-b", str(tmp)],
					capture_output=True, timeout=timeout)
			except subprocess.TimeoutExpired:
				plain = None
			if plain is not None and plain.returncode > 0:
				proc = plain
	finally:
		tmp.unlink(missing_ok=True)
	return kg_record_from_output(
		proc.stdout.decode("utf-8", "replace"),
		proc.stderr.decode("utf-8", "replace"),
		proc.returncode, str(tmp))


def records_agree(oracle: dict, kg: dict) -> bool:
	"""Structural agreement, never prose-matching.

	Mirrors utils/check_lisp_oracle.py: kind must match, then a value is
	compared by its printed rendering and a structured condition by its
	exact symbol.  Reader errors and uncatchable budgets that never enter
	the Lisp wrapper retain the message-substring fallback.
	"""
	if oracle.get("kind") != kg.get("kind"):
		return False
	kind = oracle["kind"]
	if kind == "value":
		return oracle.get("printed") == kg.get("printed")
	if kind == "condition":
		condition = oracle.get("condition", "")
		if kg.get("condition_source") == "structured":
			return bool(condition) and condition == kg.get("condition")
		return bool(condition) and condition in kg.get("message", "")
	if kind == "quit":
		return True
	if kind == "timeout":
		return True
	return False


# --- structural checks -------------------------------------------------------

def load_manifest(corpus_root: Path) -> dict:
	with open(corpus_root / "manifest.json", "r", encoding="utf-8") as fp:
		return json.load(fp)


def sha256_of(path: Path) -> str:
	with open(path, "rb") as fp:
		return hashlib.sha256(fp.read()).hexdigest()


def check_source_hash(manifest: dict, corpus_root: Path) -> list[str]:
	"""The manifest's recorded source hash must match the file on disk."""
	errors = []
	for pkg in manifest.get("packages", []):
		rel = pkg.get("source")
		recorded = pkg.get("source_sha256")
		if not rel or not recorded:
			continue
		path = corpus_root / rel
		if not path.is_file():
			path = ROOT / rel
		if not path.is_file():
			errors.append(
				f"package {pkg.get('name')!r}: source {rel!r} does "
				f"not exist")
			continue
		actual = sha256_of(path)
		if actual != recorded:
			errors.append(
				f"package {pkg.get('name')!r}: stale source hash -- "
				f"manifest records {recorded}, file "
				f"{rel!r} is now {actual}")
	return errors


def check_missing_scenarios(manifest: dict, corpus_root: Path) -> list[str]:
	"""Every scenario id a package names must have a s/<id>.json file."""
	errors = []
	for pkg in manifest.get("packages", []):
		for sid in pkg.get("scenarios", []):
			case_path = corpus_root / "s" / f"{sid}.json"
			if not case_path.is_file():
				errors.append(
					f"package {pkg.get('name')!r}: missing "
					f"scenario {sid!r} (no s/{sid}.json)")
	return errors


def check_snapshots(manifest: dict, corpus_root: Path) -> list[str]:
	"""Identity + source-hash freshness for every oracle snapshot.

	A stale snapshot is one whose `case' != its filename, that is missing
	for a scenario the manifest names, or whose recorded source hash no
	longer matches the manifest's (it was generated against an older copy
	of the tracked source).
	"""
	errors = []
	manifest_hashes = {
		p.get("name"): p.get("source_sha256")
		for p in manifest.get("packages", [])
	}
	for pkg in manifest.get("packages", []):
		name = pkg.get("name")
		expect_hash = manifest_hashes.get(name)
		for sid in pkg.get("scenarios", []):
			oracle_path = corpus_root / "oracle" / f"{sid}.json"
			if not oracle_path.is_file():
				errors.append(
					f"package {name!r}: stale snapshot -- no "
					f"oracle/{sid}.json for scenario {sid!r}")
				continue
			try:
				snap = json.loads(
					oracle_path.read_text(encoding="utf-8"))
			except json.JSONDecodeError as exc:
				errors.append(
					f"oracle/{sid}.json: not valid JSON ({exc})")
				continue
			if snap.get("case") != sid:
				errors.append(
					f"oracle/{sid}.json: embedded case is "
					f"{snap.get('case')!r} but the file is named "
					f"{sid!r}")
			snap_hash = snap.get("source_sha256")
			if expect_hash and snap_hash != expect_hash:
				errors.append(
					f"oracle/{sid}.json: stale snapshot -- recorded "
					f"source hash {snap_hash} != manifest "
					f"{expect_hash} (regenerate with "
					f"`make package-oracle')")
	return errors


# --- the run -----------------------------------------------------------------

class Report:
	def __init__(self) -> None:
		self.passed = 0
		self.divergences = 0
		self.skipped: list[str] = []
		self.failures: list[str] = []


def check_scenario(pkg, case_id, corpus_root, kgbatch, timeout, report, tmp):
	case_path = corpus_root / "s" / f"{case_id}.json"
	case = json.loads(case_path.read_text(encoding="utf-8"))
	oracle_path = corpus_root / "oracle" / f"{case_id}.json"
	oracle = json.loads(oracle_path.read_text(encoding="utf-8"))["record"]

	kg_record, error = run_kg_case(kgbatch, case, timeout, tmp)
	if error:
		report.failures.append(f"{case_id}: {error}")
		return
	agree = records_agree(oracle, kg_record)

	expect = case.get("expect", "agree")
	if expect not in ("agree", "diverge"):
		report.failures.append(
			f"{case_id}: \"expect\" is {expect!r}; the only values "
			f"are \"agree\" and \"diverge\"")
		return

	if expect == "agree":
		if agree:
			report.passed += 1
		else:
			report.failures.append(
				f"{case_id} (package {pkg['name']}, expect agree): "
				f"oracle={oracle!r} kg={kg_record!r}")
	else:  # diverge
		if agree:
			# The XPASS rule: the recorded gap has closed.
			report.failures.append(
				f"{case_id} (package {pkg['name']}, expect "
				f"diverge): kg now AGREES with the oracle, so the "
				f"recorded divergence is stale -- re-classify the "
				f"scenario instead of leaving it. oracle="
				f"{oracle!r} kg={kg_record!r}")
		else:
			report.divergences += 1


def print_support_report(manifest: dict) -> None:
	for pkg in manifest.get("packages", []):
		name = pkg.get("name")
		label = pkg.get("support_label")
		scenarios = pkg.get("scenarios", [])
		domains: dict[str, int] = {}
		for sid in scenarios:
			case_path = DEFAULT_CORPUS / "s" / f"{sid}.json"
			if case_path.is_file():
				try:
					c = json.loads(case_path.read_text(
						encoding="utf-8"))
					domains[c.get("domain", "?")] = \
						domains.get(c.get("domain", "?"), 0) + 1
				except json.JSONDecodeError:
					pass
		dom = ", ".join(f"{k}:{v}" for k, v in sorted(domains.items()))
		print(f"  {name}: label={label} scenarios={len(scenarios)} "
		      f"({dom}) exclusions={len(pkg.get('known_exclusions', []))}")


def run_check(corpus_root: Path, kgbatch: Path, timeout: float) -> int:
	manifest = load_manifest(corpus_root)
	errors = []
	errors += check_source_hash(manifest, corpus_root)
	errors += check_missing_scenarios(manifest, corpus_root)
	errors += check_snapshots(manifest, corpus_root)
	for e in errors:
		print(f"  {e}", file=sys.stderr)

	report = Report()
	if errors:
		# Structural failures already recorded; still run the scenario
		# comparisons so a single `make check' shows everything that is
		# wrong rather than failing on the first gate.
		pass
	with tempfile.TemporaryDirectory() as tmp:
		for pkg in manifest.get("packages", []):
			for sid in pkg.get("scenarios", []):
				case_path = corpus_root / "s" / f"{sid}.json"
				oracle_path = corpus_root / "oracle" / f"{sid}.json"
				if not case_path.is_file():
					continue
				if not oracle_path.is_file():
					report.skipped.append(
						f"{sid}: no oracle snapshot "
						f"(run `make package-oracle')")
					continue
				check_scenario(pkg, sid, corpus_root, kgbatch,
					       timeout, report, Path(tmp))

	total = sum(len(p.get("scenarios", []))
		    for p in manifest.get("packages", []))
	print(f"package-compat-check: {total} scenario(s) across "
	      f"{len(manifest.get('packages', []))} package(s)")
	print("  support status:")
	print_support_report(manifest)
	print(f"  passed={report.passed} recorded-divergences="
	      f"{report.divergences} skipped={len(report.skipped)} "
	      f"failures={len(report.failures)}")
	for s in report.skipped:
		print(f"  SKIP {s}")
	if report.failures:
		print("FAIL:", file=sys.stderr)
		for line in report.failures:
			print(f"  {line}", file=sys.stderr)
		return 1
	if errors:
		return 1
	return 0


# --- oracle regeneration (explicit target, not part of `make check`) ---------

def resolve_emacs(explicit: str) -> str | None:
	"""Same resolution order as utils/pty_accept.py's resolve_emacs()."""
	if explicit:
		return explicit if Path(explicit).is_file() else None
	env = os.environ.get("KG_PTY_EMACS")
	if env:
		return env if Path(env).is_file() else None
	import shutil
	found = shutil.which("emacs")
	if found:
		return found
	if Path(DEFAULT_EMACS).is_file():
		return DEFAULT_EMACS
	return None


def regenerate_oracle(corpus_root: Path, kgbatch: Path, emacs: str,
		      timeout: float, allow_version_change: bool) -> int:
	import os
	import shutil
	manifest = load_manifest(corpus_root)
	shim = corpus_root / "oracle" / "emacs-shim.el"
	if not shim.is_file():
		raise SystemExit(f"FAIL: oracle shim missing: {shim}")
	version = emacs_version(emacs)
	print(f"# oracle: {emacs} ({version})")
	written = 0
	failed = []
	for pkg in manifest.get("packages", []):
		name = pkg.get("name")
		source = pkg.get("source")
		path = (corpus_root / source if not Path(source).is_absolute()
			else Path(source))
		if not path.is_file():
			path = (ROOT / source if not Path(source).is_absolute()
				else Path(source))
		if not path.is_file():
			failed.append(f"{name}: source {source!r} missing")
			continue
		source_hash = sha256_of(path)
		for sid in pkg.get("scenarios", []):
			case_path = corpus_root / "s" / f"{sid}.json"
			oracle_path = corpus_root / "oracle" / f"{sid}.json"
			if not case_path.is_file():
				failed.append(
					f"{sid}: no s/{sid}.json to snapshot")
				continue
			record = run_emacs_case(emacs, shim, case_path, timeout)
			existing = None
			if oracle_path.is_file():
				existing = json.loads(oracle_path.read_text(
					encoding="utf-8"))
			if (existing is not None
					and existing.get("emacs_version") != version
					and not allow_version_change):
				failed.append(
					f"{sid}: snapshot under "
					f"{existing.get('emacs_version')!r}, running "
					f"Emacs is {version!r}; pass "
					f"--allow-version-change to re-pin")
				continue
			snap = {
				"schema": SCHEMA,
				"case": sid,
				"emacs_version": version,
				"source_sha256": source_hash,
				"record": record,
			}
			oracle_path.write_text(
				json.dumps(snap, indent=2, ensure_ascii=False)
				+ "\n", encoding="utf-8")
			written += 1
	print(f"# wrote {written} snapshot(s)")
	if failed:
		print("FAIL:", file=sys.stderr)
		for line in failed:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


def emacs_version(emacs: str) -> str:
	import subprocess
	proc = subprocess.run(
		[emacs, "-Q", "--batch", "--eval", "(princ (emacs-version))"],
		capture_output=True, timeout=DEFAULT_TIMEOUT)
	if proc.returncode != 0:
		raise SystemExit(f"FAIL: {emacs} --eval emacs-version exited "
				 f"{proc.returncode}")
	return proc.stdout.decode("utf-8", "replace").strip()


def run_emacs_case(emacs: str, shim: Path, case_path: Path, timeout: float):
	import subprocess
	proc = subprocess.run(
		[emacs, "-Q", "--batch", "-L", str(ROOT), "-l", str(shim),
		 str(case_path)],
		capture_output=True, timeout=timeout)
	if proc.returncode != 0:
		raise SystemExit(
			f"FAIL: oracle shim exited {proc.returncode} for "
			f"{case_path}: {proc.stderr.decode('utf-8', 'replace')}")
	lines = [l for l in proc.stdout.decode('utf-8', 'replace').splitlines()
		 if l]
	if len(lines) != 1:
		raise SystemExit(
			f"FAIL: oracle shim printed {len(lines)} line(s) for "
			f"{case_path}, expected 1")
	return json.loads(lines[0])


# --- self-test: prove the six failure modes fire -----------------------------

SELF_SOURCE = "(defun kg-self-test-thunk () 1)\n"
SELF_HASH = hashlib.sha256(SELF_SOURCE.encode("utf-8")).hexdigest()


def self_test(kgbatch: Path, timeout: float) -> int:
	"""Prove all six gate failure modes fire, in a temp corpus.

	Mirrors utils/check_lisp_compat.py's self-test: build a temp corpus,
	run the real check over it, and assert each deliberate violation is
	flagged while the well-formed files pass.  Touches no corpus file.
	"""
	import os
	with tempfile.TemporaryDirectory() as tmp:
		root = Path(tmp)
		(root / "s").mkdir()
		(root / "oracle").mkdir()
		(root / "pkg.el").write_text(SELF_SOURCE, encoding="utf-8")

		def case(cid, expr, expect, setup=None, domain="ascii"):
			(root / "s" / f"{cid}.json").write_text(
				json.dumps({
					"id": cid, "package": "pkg",
					"domain": domain,
					"setup": setup or [],
					"expr": expr, "expect": expect,
					"note": "self-test",
				}, indent=2) + "\n", encoding="utf-8")

		def oracle(cid, record, source_hash=SELF_HASH):
			(root / "oracle" / f"{cid}.json").write_text(
				json.dumps({
					"schema": SCHEMA, "case": cid,
					"emacs_version": "self-test",
					"source_sha256": source_hash,
					"record": record,
				}, indent=2) + "\n", encoding="utf-8")

		# Well-formed: a value that agrees, and a condition that agrees.
		case("good-val", "(+ 1 2)", "agree")
		oracle("good-val", {"kind": "value", "printed": "3"})
		case("good-cond", "(car 1)", "agree")
		oracle("good-cond", {"kind": "condition",
				     "condition_source": "structured",
				     "condition": "wrong-type-argument"})

		# 1. wrong value: oracle says 4, kg answers 3.
		case("wrong-val", "(+ 1 2)", "agree")
		oracle("wrong-val", {"kind": "value", "printed": "4"})

		# 2. wrong condition: oracle void-variable, kg wrong-type-argument.
		case("wrong-cond", "(car 1)", "agree")
		oracle("wrong-cond", {"kind": "condition",
				      "condition_source": "structured",
				      "condition": "void-variable"})

		# 3. stale source hash: manifest records a wrong hash.
		# 4. missing scenario: manifest names "missing-scn", no file.
		# 5. stale snapshot: oracle source_sha256 != manifest's.
		case("stale-snap", "(+ 1 2)", "agree")
		oracle("stale-snap", {"kind": "value", "printed": "3"},
		       source_hash="0" * 64)

		# 6. XPASS: recorded divergence, but kg now agrees.
		case("xpass", "(+ 1 2)", "diverge")
		oracle("xpass", {"kind": "value", "printed": "3"})

		manifest = {
			"schema": "elisp-packages/1",
			"packages": [{
				"name": "pkg",
				"source": "pkg.el",
				"source_sha256": "deadbeef" * 8,
				"scenarios": ["good-val", "good-cond",
					      "wrong-val", "wrong-cond",
					      "missing-scn", "stale-snap",
					      "xpass"],
			}],
		}
		(root / "manifest.json").write_text(
			json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

		errors = (check_source_hash(manifest, root)
			  + check_missing_scenarios(manifest, root)
			  + check_snapshots(manifest, root))
		report = Report()
		for sid in ("good-val", "good-cond", "wrong-val", "wrong-cond",
			    "stale-snap", "xpass"):
			check_scenario(manifest["packages"][0], sid, root,
				       kgbatch, timeout, report, Path(tmp))

		ok = True

		def need_flagged(text, needle, what):
			nonlocal ok
			if not any(needle in e for e in text):
				print(f"FAIL: {what} was not flagged",
				      file=sys.stderr)
				ok = False

		def need_clean(text, needle, what):
			nonlocal ok
			if any(needle in e for e in text):
				print(f"FAIL: {what} was wrongly flagged",
				      file=sys.stderr)
				ok = False

		need_flagged(errors, "stale source hash", "stale source hash")
		need_flagged(errors, "missing scenario 'missing-scn'",
			     "missing scenario")
		need_flagged(errors, "stale snapshot", "stale snapshot")
		need_flagged(report.failures, "wrong-val", "wrong value")
		need_flagged(report.failures, "wrong-cond", "wrong condition")
		need_flagged(report.failures, "xpass", "XPASS")
		need_clean(report.failures, "good-val", "a good value case")
		need_clean(report.failures, "good-cond", "a good condition case")

		if not ok:
			return 1
		print("self-test: stale source hash fails the gate")
		print("self-test: missing scenario fails the gate")
		print("self-test: stale snapshot fails the gate")
		print("self-test: a wrong value fails the gate")
		print("self-test: a wrong condition fails the gate")
		print("self-test: an XPASS (recorded divergence that now "
		      "agrees) fails the gate")
		print("self-test: correctly-formed cases pass")
		return 0


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--corpus-root", type=Path,
			    default=DEFAULT_CORPUS)
	parser.add_argument("--kgbatch", type=Path, default=DEFAULT_KGBATCH)
	parser.add_argument("--emacs", default="")
	parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
	parser.add_argument("--self-test", action="store_true",
			    help="prove the six gate failure modes fire, in a "
				 "temp corpus; touches no corpus file")
	parser.add_argument("--regenerate-oracle", action="store_true",
			    help="regenerate oracle/*.json from the pinned "
				 "Emacs; explicit target, not part of "
				 "`make check'")
	parser.add_argument("--allow-version-change", action="store_true",
			    help="with --regenerate-oracle, re-pin snapshots "
				 "to a different Emacs version")
	args = parser.parse_args()

	if args.self_test:
		return self_test(args.kgbatch, args.timeout)
	if args.regenerate_oracle:
		emacs = resolve_emacs(args.emacs)
		if emacs is None:
			print("FAIL: emacs not found (set --emacs, "
			      "$KG_PTY_EMACS, PATH, or /opt-3 pin)",
			      file=sys.stderr)
			return 1
		return regenerate_oracle(
			args.corpus_root, args.kgbatch, emacs, args.timeout,
			args.allow_version_change)
	return run_check(args.corpus_root, args.kgbatch, args.timeout)


if __name__ == "__main__":
	sys.exit(main())
