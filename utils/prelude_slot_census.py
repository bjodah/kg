#!/usr/bin/env python3
"""Attribute post-prelude live arena slots to lisp/prelude.el's sections.

Phase 0.1 of doc/plans/2026-08-14-embedded-prelude.md.  `kgbatch -g`
already prints `peak-live` for the *whole* prelude; nothing before this
script attributed any of it to a section, so "move the rarely used
definitions out" was a guess about which those are.  This script turns
that guess into a table: peak-live after every *cumulative* prefix of
lisp/prelude.el, cut at each `;; --- name ---` section marker, so a
section's own cost is the delta between two consecutive cumulative
readings.  Cumulative and in source order, never reordered or evaluated
independently -- the prelude's own header says ordering is load-bearing
(alias-before-shadow; a macro must be defined before any later form uses
it), and a prefix that stopped short of a real definition a later prefix
depends on would not even be the question this script answers.

The very first reading needs a reference point of its own: peak-live
already counts everything kg_lisp_init() allocates before
evaluate_prelude() ever runs (register_natives()'s 127 native
bindings, lisp_hooks_init(), lisp_process_init()), so the preamble
slice's delta measured against 0 would silently credit lisp/prelude.el
with a fixed C-side cost that is not its own.  This script measures that
baseline directly (a one-line-comment "prelude" that evaluates to
nothing) and reports every later delta against it instead of against 0.

Why this is a build-and-measure script and not a call into a running kg:
FeArenaStats has no notion of a "section", and there is no public way to
run kg_lisp_init()'s natives/hooks setup without also running the
compiled-in prelude -- evaluate_prelude() and register_natives() are
private to src/lisp_prelude.c, reachable only through fe.h, and fe.h may
only be included by the src/lisp_*.c adapter files (see CLAUDE.md).
test/ code, this script included, only ever reaches kg's Lisp through
test/kgbatch, which only ever runs kg_lisp_init()'s real, whole,
compiled-in prelude.

So instead: for each cumulative section boundary, this script writes a
truncated COPY of lisp/prelude.el to a temp file, regenerates
src/lisp_prelude_generated.inc from *that* copy with the same
utils/embed_lisp.py `make lisp-prelude-generate` uses, forces
test/kgbatch to rebuild against the truncated array (touching
src/lisp_prelude.c -- the Makefile has no dependency edge from
src/lisp_prelude.o to the .inc it #includes, a documented gotcha), and
reads `test/kgbatch -g /dev/null`'s peak-live the same way
utils/check_lisp_gc_stress.py already does.  kg_lisp_init() then
evaluates exactly that prefix AS the prelude, through the ordinary
evaluate_prelude() code path with the same eval_options as a real binary
-- so each reading is exactly what a kg embedding only that much prelude
would show, not an approximation of it.

This is a *destructive* operation on a generated, checked-in file
(src/lisp_prelude_generated.inc): the script always restores it from the
real lisp/prelude.el before exiting, success or failure, and does not
consider itself done until `make lisp-prelude-check` agrees the restore
took.  Do not run this alongside anything else that builds kg or
test/kgbatch in the same tree -- the restore assumes it is the only
writer of that one file for the duration of the run.

Usage:
    utils/prelude_slot_census.py [--json OUT.json]

Prints a Markdown table to stdout (cumulative peak-live and this
section's delta) and, at the end, the total_slots denominator from a
`-a` run of the real, fully restored prelude -- total_slots is fixed by
the arena partition, not by how much of the prelude ran, so one such
reading is the denominator every delta above is a fraction of.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRELUDE = ROOT / "lisp" / "prelude.el"
GENERATED = ROOT / "src" / "lisp_prelude_generated.inc"
LISP_PRELUDE_C = ROOT / "src" / "lisp_prelude.c"
EMBED_SCRIPT = ROOT / "utils" / "embed_lisp.py"
KGBATCH = ROOT / "test" / "kgbatch"

SECTION_RE = re.compile(rb"^;; --- (.+?) -+\s*$")
PEAK_LIVE_RE = re.compile(r"peak-live=(\d+)")
CENSUS_RE = re.compile(
    r"total=(\d+) free=(\d+) peak-live=(\d+) collections=(\d+) failures=(\d+)"
)


def find_sections(lines: list[bytes]) -> list[tuple[int, str]]:
	"""Return [(line index, section name), ...] for every `;; --- ... ---`
	marker, in source order.  Line index is where the marker itself sits;
	a cumulative slice through this section runs up to the NEXT marker
	(or EOF for the last one)."""
	sections = []
	for i, line in enumerate(lines):
		m = SECTION_RE.match(line)
		if m:
			sections.append((i, m.group(1).decode("utf-8")))
	return sections


def build_slices(lines: list[bytes]) -> list[tuple[str, int]]:
	"""Return [(label, end_line_exclusive), ...], cumulative and in source
	order.  The first entry is the preamble before any section marker
	(lisp/prelude.el's header comment plus the handful of alias forms
	`let`/`progn`/`null`/... that predate any named section); the rest
	name a section by the text after `;; --- `."""
	sections = find_sections(lines)
	if not sections:
		raise SystemExit(
		    "prelude_slot_census: no `;; --- name ---` markers found in "
		    f"{PRELUDE}")
	slices = [("(preamble, before first section marker)", sections[0][0])]
	for idx, (_, name) in enumerate(sections):
		end = sections[idx + 1][0] if idx + 1 < len(sections) else len(lines)
		slices.append((name, end))
	return slices


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
	return subprocess.run(
	    cmd, cwd=ROOT, capture_output=True, text=True, check=False, **kw)


def rebuild_kgbatch() -> None:
	"""Force test/kgbatch to relink against whatever
	src/lisp_prelude_generated.inc currently holds. Touching the .c file
	is required: the Makefile's object rule depends on $(HDRS), not on
	the .inc the source #includes, so an unchanged .c mtime would let
	`make` skip recompiling src/lisp_prelude.o even though the array it
	embeds changed underneath it."""
	LISP_PRELUDE_C.touch()
	result = run(["make", "test/kgbatch"])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit("prelude_slot_census: make test/kgbatch failed")


def peak_live_for_slice(lines: list[bytes], end: int, tmp: Path) -> int:
	slice_bytes = b"".join(lines[:end])
	tmp.write_bytes(slice_bytes)
	result = run(
	    [sys.executable, str(EMBED_SCRIPT), str(tmp), str(GENERATED)])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit("prelude_slot_census: embed_lisp.py failed")
	rebuild_kgbatch()
	result = run([str(KGBATCH), "-g", "/dev/null"])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit(
		    "prelude_slot_census: test/kgbatch failed evaluating a "
		    "prelude prefix -- see stderr above for which one")
	m = PEAK_LIVE_RE.search(result.stdout)
	if not m:
		raise SystemExit(
		    f"prelude_slot_census: no peak-live in kgbatch -g output: "
		    f"{result.stdout!r}")
	return int(m.group(1))


BASELINE_COMMENT = b";; empty, evaluates to nothing\n"


def baseline_peak_live(tmp: Path) -> int:
	"""peak-live with a prelude that evaluates to nothing at all --
	register_natives(), lisp_hooks_init() and lisp_process_init() (all of
	kg_lisp_init()'s work before evaluate_prelude() even starts) already
	cost live arena slots, and that cost is not `lisp/prelude.el`'s: it
	is paid identically no matter what the embedded array holds, so it
	is inside EVERY cumulative reading this script takes, including the
	first one. Without subtracting it, the preamble slice's delta (which
	the rest of this script computes against 0, there being no prior
	slice) would silently attribute this fixed C-side cost to
	`lisp/prelude.el`'s own first nine forms. embed_lisp.py refuses a
	genuinely empty file, hence one comment line, which the reader
	discards as a no-op form."""
	return peak_live_for_slice([BASELINE_COMMENT], 1, tmp)


def total_slots() -> int:
	result = run([str(KGBATCH), "-a", "/dev/null"])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit("prelude_slot_census: test/kgbatch -a failed")
	m = CENSUS_RE.search(result.stdout)
	if not m:
		raise SystemExit(
		    f"prelude_slot_census: no census line in kgbatch -a output: "
		    f"{result.stdout!r}")
	return int(m.group(1))


def restore_real_prelude() -> None:
	"""Regenerate src/lisp_prelude_generated.inc from the real,
	untruncated lisp/prelude.el and rebuild test/kgbatch against it --
	the same two steps `make lisp-prelude-generate` documents -- then
	confirm with `make lisp-prelude-check` that the tree is back to its
	committed state. Runs from a `finally`, so it executes even when a
	slice failed to evaluate."""
	result = run(
	    [sys.executable, str(EMBED_SCRIPT), str(PRELUDE), str(GENERATED)])
	if result.returncode != 0:
		sys.stderr.write(result.stdout)
		sys.stderr.write(result.stderr)
		raise SystemExit(
		    "prelude_slot_census: FAILED TO RESTORE "
		    "src/lisp_prelude_generated.inc from the real "
		    "lisp/prelude.el -- fix by hand with "
		    "'make lisp-prelude-generate' before doing anything else")
	rebuild_kgbatch()
	check = run(["make", "lisp-prelude-check"])
	if check.returncode != 0:
		sys.stderr.write(check.stdout)
		sys.stderr.write(check.stderr)
		raise SystemExit(
		    "prelude_slot_census: restored file does not pass "
		    "'make lisp-prelude-check' -- tree is NOT clean, fix by "
		    "hand before doing anything else")


def main(argv: list[str]) -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument(
	    "--json", type=Path, default=None,
	    help="also write the raw per-section readings as JSON")
	args = parser.parse_args(argv[1:])

	if not KGBATCH.is_file():
		raise SystemExit(
		    "prelude_slot_census: test/kgbatch is not built "
		    "(run 'make test/kgbatch' first)")

	lines = PRELUDE.read_bytes().splitlines(keepends=True)
	slices = build_slices(lines)

	tmp_dir = Path(
	    subprocess.run(["mktemp", "-d"], capture_output=True, text=True,
		check=True).stdout.strip())
	tmp_slice = tmp_dir / "prelude-slice.el"

	readings: list[tuple[str, int, int]] = []
	try:
		baseline = baseline_peak_live(tmp_slice)
		readings.append((
		    "*(baseline: register_natives + lisp_hooks_init + "
		    "lisp_process_init, before any prelude form runs)*",
		    baseline, baseline))
		previous = baseline
		for label, end in slices:
			cumulative = peak_live_for_slice(lines, end, tmp_slice)
			delta = cumulative - previous
			readings.append((label, cumulative, delta))
			previous = cumulative
	finally:
		restore_real_prelude()
		denom = total_slots()

	print(f"# Prelude per-section slot census\n")
	print(f"Source: {PRELUDE.relative_to(ROOT)} "
	      f"({len(lines)} lines, {len(slices)} cumulative slices: "
	      f"1 preamble + {len(slices) - 1} named sections)")
	print(f"total_slots (fixed arena partition, from `kgbatch -a`): "
	      f"{denom}\n")
	print("| section | cumulative peak-live | this section's cost "
	      "(delta) | % of total_slots |")
	print("| --- | ---: | ---: | ---: |")
	for label, cumulative, delta in readings:
		pct = 100.0 * delta / denom
		print(f"| {label} | {cumulative} | {delta} | {pct:.2f}% |")
	final_cumulative = readings[-1][1] if readings else 0
	print(f"\nFinal cumulative peak-live (whole prelude): "
	      f"{final_cumulative} of {denom} "
	      f"({100.0 * final_cumulative / denom:.2f}%)")

	if args.json:
		args.json.write_text(json.dumps({
		    "total_slots": denom,
		    "sections": [
			{"label": label, "cumulative_peak_live": cumulative,
			 "delta": delta}
			for label, cumulative, delta in readings
		    ],
		}, indent=2) + "\n")
		print(f"\nJSON written to {args.json}")

	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
