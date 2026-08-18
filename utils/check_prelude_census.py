#!/usr/bin/env python3
"""Hold the embedded Lisp prelude's startup cost to a ceiling.

Phase 0.1-0.3 of doc/plans/2026-08-14-embedded-prelude.md measured what
lisp/prelude.el costs (roughly a fifth of the Lisp arena, permanently, before
a single line of user code runs) and found nothing in the tree had ever
measured it before: it grew to that size one review at a time, with no gate
anywhere to say so. This is that gate, in the shape `utils/check_mutation_
gateway.py` and `utils/check_pmccabe_complexity.py` already use for their
own ratchets: a JSON manifest of the measured actual, checked against a
fresh measurement on every run, with regeneration as the only way to bank a
change. No number may rise without a rationale and measured proof in the
commit message; a fall is banked by regenerating.

Four numbers, all read off the real, compiled-in prelude through
test/kgbatch and test/prelude_gc_probe -- never re-evaluated or
re-implemented here:

  * peak_live_objects -- the post-prelude high-water mark
    (`FeArenaStats.peak_live_objects`, `test/kgbatch -g`'s `peak-live=`
    field). What an exhaustion failure is actually measured against
    (`test_lisp.c`'s `peak_live * 3 < total_slots`, the four PTY
    exhaustion cases), and the number that moves when an implementation
    detail changes how much transient macro-expansion garbage a
    definition produces while loading -- even when what is reachable
    afterwards does not move at all (Phase 0.3's Phase-11A cross-check:
    gensym-based hygiene moved this from 11281 to 20906 for identical
    reachable behaviour).
  * reachable_live_objects -- what a session actually keeps: arena slots
    still reachable after the prelude AND a forced collection
    (`test/prelude_gc_probe`'s "reachable-live (total - free)" line --
    kg_lisp_init() itself now runs the one forced collection this needs,
    the post-prelude collect of doc/plans/2026-08-14-embedded-prelude.md,
    so the probe only has to read `kg_lisp_arena_stats()` afterward).
    The stabler, implementation-independent definition footprint,
    and what a lazy-loading phase's saving should be measured against
    instead of peak_live_objects, whose delta conflates fewer permanent
    definitions with less transient garbage from however the remaining
    ones happen to be written.
  * embedded_bytes -- lisp/prelude.el's own byte size. `utils/embed_lisp.py`
    copies those bytes into `src/lisp_prelude_generated.inc` unchanged (no
    reformatting, no dedup), so the source file's size on disk IS the
    embedded array's length; nothing here reads the generated .inc.
  * definition_count -- top-level `(defalias 'NAME (KIND ...))` forms in
    lisp/prelude.el, i.e. how many names the prelude declares.

Reuse note: `utils/prelude_first_call_census.py` already has a
`parse_prelude_names()` that counts these, but importing that module here
would also import PyYAML, `forecast_audit` and `check_lisp_oracle` for
census and slot-removal machinery this four-number ratchet never touches --
a heavier and more fragile coupling than the four-line regex below, which
is deliberately kept in the same shape as that module's own
`TOPLEVEL_DEFALIAS_RE` so the two counts cannot silently diverge in what
they consider a top-level definition.

Usage:
    make prelude-census-check
    make prelude-census-baseline   # regenerate after a deliberate change
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SCHEMA = "kg-prelude-startup-census/1"

QUANTITIES = (
	"peak_live_objects",
	"reachable_live_objects",
	"embedded_bytes",
	"definition_count",
)

PEAK_LIVE_RE = re.compile(r"peak-live=(\d+)")
REACHABLE_LIVE_RE = re.compile(r"reachable-live \(total - free\) = (\d+)")
DEFALIAS_RE = re.compile(r"(?m)^\(defalias '\S+\s+\(\w+")


def run(cmd: list[str]) -> subprocess.CompletedProcess:
	return subprocess.run(cmd, capture_output=True, text=True, check=False)


def measure_peak_live(kgbatch: Path) -> int:
	if not kgbatch.is_file():
		raise SystemExit(
		    f"check_prelude_census: {kgbatch} is not built "
		    f"(run 'make {kgbatch}' first)")
	result = run([str(kgbatch), "-g", "/dev/null"])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit(f"check_prelude_census: {kgbatch} -g failed")
	m = PEAK_LIVE_RE.search(result.stdout)
	if not m:
		raise SystemExit(
		    f"check_prelude_census: no peak-live in {kgbatch} -g output: "
		    f"{result.stdout!r}")
	return int(m.group(1))


def measure_reachable_live(probe: Path) -> int:
	if not probe.is_file():
		raise SystemExit(
		    f"check_prelude_census: {probe} is not built "
		    f"(run 'make {probe}' first)")
	result = run([str(probe)])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit(f"check_prelude_census: {probe} failed")
	m = REACHABLE_LIVE_RE.search(result.stdout)
	if not m:
		raise SystemExit(
		    f"check_prelude_census: no reachable-live line in "
		    f"{probe} output: {result.stdout!r}")
	return int(m.group(1))


def measure_embedded_bytes(prelude: Path) -> int:
	return prelude.stat().st_size


def measure_definition_count(prelude: Path) -> int:
	text = prelude.read_text(encoding="utf-8")
	names = DEFALIAS_RE.findall(text)
	if not names:
		raise SystemExit(
		    f"check_prelude_census: no top-level (defalias 'NAME (KIND "
		    f"...)) forms found in {prelude}")
	return len(names)


def measure(kgbatch: Path, probe: Path, prelude: Path) -> dict:
	return {
		"peak_live_objects": measure_peak_live(kgbatch),
		"reachable_live_objects": measure_reachable_live(probe),
		"embedded_bytes": measure_embedded_bytes(prelude),
		"definition_count": measure_definition_count(prelude),
	}


def load_manifest(path: Path):
	try:
		with open(path, "r", encoding="utf-8") as fp:
			data = json.load(fp)
	except FileNotFoundError:
		print(f"FAIL: prelude census manifest {path} is missing; "
		      "run 'make prelude-census-baseline'", file=sys.stderr)
		return None
	except json.JSONDecodeError as exc:
		print(f"FAIL: prelude census manifest {path} is not valid JSON: "
		      f"{exc}", file=sys.stderr)
		return None
	if data.get("schema") != SCHEMA:
		print(f"FAIL: prelude census manifest {path} has schema "
		      f"{data.get('schema')!r}, expected {SCHEMA!r}", file=sys.stderr)
		return None
	return data


def write_manifest(path: Path, measured: dict) -> None:
	data = {
		"schema": SCHEMA,
		"regenerate_with": "make prelude-census-baseline",
		"note": ("What the embedded Lisp prelude (lisp/prelude.el) costs at "
			 "startup, in four numbers: post-prelude peak live arena "
			 "slots, still-reachable slots after a forced collection, "
			 "embedded byte count, and top-level definition count. No "
			 "number may rise without a rationale and measured proof "
			 "in the commit message; regenerating is how a fall is "
			 "banked."),
		"measured_with": {
			"peak_live_objects": "test/kgbatch -g /dev/null, peak-live=",
			"reachable_live_objects": ("test/prelude_gc_probe, the "
				"\"excl. the triggering call's own object\" line"),
			"embedded_bytes": "lisp/prelude.el, byte size on disk",
			"definition_count": ("lisp/prelude.el, count of top-level "
				"(defalias 'NAME (KIND ...)) forms"),
		},
		"ceiling": {name: measured[name] for name in QUANTITIES},
	}
	with open(path, "w", encoding="utf-8") as fp:
		json.dump(data, fp, indent=1, sort_keys=False)
		fp.write("\n")
	print(f"wrote {path}: " +
	      ", ".join(f"{name}={measured[name]}" for name in QUANTITIES))


def check(measured: dict, manifest: dict) -> int:
	ceiling = manifest["ceiling"]
	failures = []
	improved = []

	for name in QUANTITIES:
		now = measured[name]
		was = ceiling.get(name)
		if was is None:
			failures.append(f"  {name}: manifest has no ceiling recorded")
			continue
		if now > was:
			failures.append(f"  {name}: {now}, manifest {was} (+{now - was})")
		elif now < was:
			improved.append(f"  {name}: {was} -> {now}")

	print("prelude startup census: " +
	      ", ".join(f"{name}={measured[name]}" for name in QUANTITIES))
	for line in improved:
		print(line)
	if improved:
		print("  run 'make prelude-census-baseline' to lock the "
		      "improvement in")

	if failures:
		print(f"FAIL: {len(failures)} prelude census number(s) above the "
		      "manifest", file=sys.stderr)
		for line in failures:
			print(line, file=sys.stderr)
		return 1
	return 0


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--manifest", required=True, type=Path)
	parser.add_argument("--kgbatch", required=True, type=Path)
	parser.add_argument("--probe", required=True, type=Path)
	parser.add_argument("--prelude", required=True, type=Path)
	parser.add_argument("--write", action="store_true",
			    help="record the measurement instead of checking it")
	args = parser.parse_args()

	measured = measure(args.kgbatch, args.probe, args.prelude)
	if args.write:
		write_manifest(args.manifest, measured)
		return 0
	manifest = load_manifest(args.manifest)
	if manifest is None:
		return 1
	return check(measured, manifest)


if __name__ == "__main__":
	sys.exit(main())
