#!/usr/bin/env python3
"""Phase 0.2 of doc/plans/2026-08-14-embedded-prelude.md: a first-call
census of lisp/prelude.el's top-level names.

Phase 0.1 (utils/prelude_slot_census.py) answered "which SECTION of the
prelude holds the arena cost".  This one answers: of those names, which
does *anything in this tree ever call*?  It was written to price lazy
loading, and Phase A of doc/plans/2026-08-19-fe-simplification-and-cheap-
compat.md removed the lazy loading along with the option that measured
what could be deferred.  The partition itself outlived that decision and
is what the script still reports: a name no case, corpus file or startup
path reaches is a name whose behaviour nothing in this tree pins, which
is worth knowing whatever is done about it.

THE MECHANISM

A dynamic first-call census, run through test/kgbatch -- never through the
live PTY editor, which CLAUDE.md and the plan both call not worth
instrumenting for this.  test/kgbatch already evaluates the real,
unmodified, compiled-in prelude via kg_lisp_init() before it touches any
file argument, so a Lisp-level shim installed as kgbatch's *first* file
argument sees the exact function objects a real kg session would.  It
replaces each of the 103 non-macro top-level names' function cell with a
recording wrapper that notes the name was called (deduplicated, so the
report stays small regardless of call volume) and then forwards the call
via `apply` -- unchanged behaviour, observed rather than altered.  Every
later file argument -- corpus content -- runs in the same process, so the
recording persists across all of them; a final report file's value is the
accumulated set, which kgbatch prints via its ordinary one-line-per-file
contract and this script parses back out.

Two dynamic runs:

  * STARTUP: kgbatch given only the shim and the report -- no corpus at
    all.  This is what "called on any startup path" can mean through
    kgbatch, which never calls kg_lisp_load_init() (only the real `kg`
    binary's main() does): kg_lisp_init() is the whole of kgbatch's
    "startup", and this run measures whatever of the 103 names that alone
    invokes.
  * REALISTIC: the same shim, then every corpus file below, then the
    report.  The corpus:
      - every test/lisp-compat/cases/*.json (setup forms then expr, the
        same join utils/check_lisp_oracle.py's case_source() uses,
        reused directly rather than re-implemented) and every
        test/lisp-compat/fixtures/*.el (the files those cases `load`);
      - utils/forecast/target-init.el;
      - every `config_files:` entry under test/pty/*.yaml whose path ends
        `.el` -- EXCEPT `.dir-locals.el`, which is dir-locals data (an
        alist literal kg's dir-locals reader consumes, never handed to
        the general evaluator) and would just raise "nil is not a
        function" if evaluated as a top-level form; running the 114
        others is this script's stand-in for the 581-case PTY corpus
        itself, since instrumenting the live PTY editor is judged not
        worth it (see CLAUDE.md and the plan's 0.2 section) -- this is a
        STATED LIMITATION, not a silent gap: an interactive-only call
        path (a command whose body only runs when its key is pressed in
        the live editor and the extracted init/package file's own
        top-level code never reaches it) is invisible to this run, and is
        exactly why the static cross-check below exists.

Corpus files run in a small number of BATCHED kgbatch invocations rather
than either "all 532 in one process" or "one process per file": a single
process is efficient (one prelude load, not 532) and fe's allocator
reclaims what a batch's own transient data no longer references (the same
property utils/check_lisp_gc_stress.py's cases rely on), but a handful of
lisp-compat cases exist SPECIFICALLY to exhaust the arena or to be
adversarial, and this script cannot prove none of them ever corrupts
process state for everything queued after it in the same batch.  Chunking
bounds the blast radius, and any chunk whose report line does not come
back (a crash, not a caught Lisp error -- kgbatch's own contract already
turns a caught error into a printed, non-fatal record) is automatically
re-run one file at a time so a single poison input costs that one input's
data point rather than the whole chunk's.

THE STATIC CROSS-CHECK

Reuses utils/forecast_audit.py's reader and function-position walker
rather than a second one: every prelude name referenced in function
position, `#'name`, or a quoted symbol in a known higher-order argument
position, anywhere in lisp/*.el, utils/forecast/*.el,
test/lisp-compat/cases/*.json (setup+expr text), test/lisp-compat/*.el
fixtures, and the same PTY-extracted .el content the dynamic run uses.  A
name the dynamic run never reaches but this walker finds is moved from
"never" to "realistic, static-only" rather than left in set 3 -- the plan
is explicit that set 3 must be conservative, since it is the set anything
acting on this report would act on.

Usage:
    make test/kgbatch
    python3 utils/prelude_first_call_census.py --json /tmp/census.json

--slots also measures set 3's arena cost the way
utils/prelude_slot_census.py measures a section's: write a prelude copy
with set 3's defalias forms deleted (never reordered, nothing else
touched), regenerate src/lisp_prelude_generated.inc from it, rebuild
test/kgbatch, read `kgbatch -g`'s peak-live, and restore the real prelude
in a `finally` -- reusing prelude_slot_census's embed/rebuild/restore
helpers rather than re-implementing them.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import prelude_slot_census as pslot  # noqa: E402  (path set above)
import forecast_audit  # noqa: E402
import check_lisp_oracle  # noqa: E402

PRELUDE = ROOT / "lisp" / "prelude.el"
KGBATCH = ROOT / "test" / "kgbatch"
LISP_COMPAT_CASES = ROOT / "test" / "lisp-compat" / "cases"
LISP_COMPAT_FIXTURES = ROOT / "test" / "lisp-compat" / "fixtures"
FORECAST_TARGET_INIT = ROOT / "utils" / "forecast" / "target-init.el"
PTY_DIR = ROOT / "test" / "pty"

TOPLEVEL_DEFALIAS_RE = re.compile(r"(?m)^\(defalias '(\S+)\s+\((\w+)")
PEAK_LIVE_RE = re.compile(r"peak-live=(\d+)")

CHUNK_SIZE = 40

# ---------------------------------------------------------------------------
# lisp/prelude.el's own name list: order, and macro-ness.
# ---------------------------------------------------------------------------


def parse_prelude_names() -> list[tuple[str, str]]:
	"""[(name, "macro"|"function"), ...] in source order.

	A name is a macro exactly when its defalias's second token is the
	literal `macro` -- fe's own FeTMacro constructor, spelled `(macro
	PARAMS . BODY)` the same way `(lambda PARAMS . BODY)` spells a
	function.  Everything else (`(lambda ...)` bodies and the three
	`(symbol-function 'X)` primitive captures in the preamble) is a
	function for this partition's purposes, subject to the
	order-sensitivity check below.
	"""
	text = PRELUDE.read_text(encoding="utf-8")
	names = TOPLEVEL_DEFALIAS_RE.findall(text)
	if not names:
		raise SystemExit(
		    "prelude_first_call_census: no top-level (defalias 'NAME "
		    "(KIND ...)) forms found")
	return [(name, "macro" if kind == "macro" else "function")
		for name, kind in names]


def parse_symbol_function_captures(text: str) -> list[tuple[str, str]]:
	"""[(alias, captured_primitive), ...] for every top-level
	`(defalias 'ALIAS (symbol-function 'PRIM))` form -- the only
	construct in the prelude whose VALUE depends on when its defining
	form runs relative to later shadowing, because every other form's
	free names resolve by lookup at CALL time, not at its own define
	time.  Used by the order-sensitivity check, not by the partition
	itself."""
	return re.findall(
	    r"(?m)^\(defalias '(\S+) \(symbol-function '(\S+)\)\)", text)


def find_shadowed_names(text: str, captured: list[str]) -> set[str]:
	"""Of the primitives `parse_symbol_function_captures` found captured,
	which are later ALSO the target of a top-level `(defalias 'NAME
	...)` themselves -- i.e. shadowed after being aliased away.  The
	prelude's own header states there is exactly one ("only `let' is
	shadowed"); this re-derives it from the source instead of trusting
	the comment, so a future capture added without a matching shadow (or
	vice versa) is not silently assumed away."""
	all_targets = TOPLEVEL_DEFALIAS_RE.findall(text)
	target_names = [name for name, _kind in all_targets]
	shadowed = set()
	for prim in captured:
		# Shadowed if `prim` is itself later defalias'd (i.e. appears as
		# a defalias TARGET anywhere -- capture forms alias to it via
		# symbol-function, they are not defalias targets of `prim`
		# themselves, so any occurrence here is a real redefinition).
		if target_names.count(prim) >= 1:
			shadowed.add(prim)
	return shadowed


# ---------------------------------------------------------------------------
# Top-level form boundaries, for --slots' surgical removal.
# ---------------------------------------------------------------------------


def toplevel_form_ranges(lines: list[str]) -> dict[str, tuple[int, int]]:
	"""name -> (start_line, end_line_exclusive) for every top-level
	`(defalias 'NAME ...)` form.  Every top-level form in lisp/prelude.el
	starts in column 0 and nothing nested ever does (the same property
	utils/check_lisp_compat.py's parse_kg_prelude_defs() relies on), so a
	form's extent is "up to the next column-0 `(`, or EOF" -- exactly
	utils/prelude_slot_census.py's section-boundary assumption, at
	per-form rather than per-section grain."""
	starts = [i for i, line in enumerate(lines) if line.startswith("(")]
	ranges: dict[str, tuple[int, int]] = {}
	for idx, start in enumerate(starts):
		end = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
		m = TOPLEVEL_DEFALIAS_RE.match(lines[start])
		if m:
			ranges[m.group(1)] = (start, end)
	return ranges


# ---------------------------------------------------------------------------
# The census shim.  Deliberately uses NONE of the 103 wrapped names in its
# own implementation (only raw fe primitives: lambda, fset, symbol-function,
# apply, cons, car, cdr, eq, not, and, if, while, setq) -- otherwise
# installing the shim itself would record a "call" the moment the shim
# wraps whichever of those 103 names happens to appear later in the wrap
# order than the shim's own use of it, contaminating the very "startup"
# reading this script exists to take cleanly.  `unless`/`when` (prelude
# macros, never wrapped) would be safe too but raw `if` needs no prelude at
# all, so it is what is used.
# ---------------------------------------------------------------------------

SHIM_PRELUDE = """\
(defvar kg-census--seen nil)
(defalias 'kg-census--member
  (lambda (x lst)
    (while (and lst (not (eq (car lst) x)))
      (setq lst (cdr lst)))
    lst))
(defalias 'kg-census--wrap
  (lambda (name)
    ((lambda (kg-census--orig)
       (fset name
         (lambda (&rest kg-census--a)
           (if (kg-census--member name kg-census--seen)
               nil
             (setq kg-census--seen (cons name kg-census--seen)))
           (apply kg-census--orig kg-census--a))))
     (symbol-function name))))
"""

REPORT_FORM = "kg-census--seen\n"


# internal--let and progn are captured via `(symbol-function 'let)` and
# `(symbol-function 'do)` -- fe SPECIAL FORMS, not ordinary functions: `let`
# does not evaluate its bindings list the way a normal call's arguments are
# evaluated (that is the entire reason `(internal--let NAME VALUE)`
# introduces a binding into the *enclosing* body), and `do` runs its forms
# one at a time rather than receiving pre-evaluated values.  fset-ing
# either to an ordinary eagerly-evaluating lambda does not observe them, it
# BREAKS them: confirmed by hand, wrapping internal--let turns
# `(let ((x 1)) x)` into `void-variable pairs`, because `let`'s own macro
# body is itself written as `(internal--let pairs nil) ...` and the wrapped
# version evaluates `pairs` as a variable reference before binding it. Both
# names are therefore never wrapped and never dynamically measured; they
# are asserted into set 1 by construction instead (see main()), which is
# sound because `let`'s macro expansion calls `internal--let` on every
# single `let`/`let*`/`when`/`unless`/`dolist`/... evaluation and nothing
# in this tree goes an evaluation without one.  UNWRAPPED_SPECIAL_FORM_ALIASES
# names them so a future prelude addition of the same shape is not silently
# missed.
UNWRAPPED_SPECIAL_FORM_ALIASES = {"internal--let", "progn"}


def build_shim(function_names: list[str]) -> str:
	wrap_names = [n for n in function_names
		      if n not in UNWRAPPED_SPECIAL_FORM_ALIASES]
	wraps = "\n".join(f"(kg-census--wrap '{name})" for name in wrap_names)
	return SHIM_PRELUDE + wraps + "\n"


def parse_symbol_list(rendering: str) -> set[str]:
	"""kg's writer renders a list of symbols as `(a b c)`, and the empty
	list as `nil` -- the same two shapes the manual probe in this
	script's development confirmed.  Every prelude name is a bare token
	(no spaces, no quoting the writer would need), so a plain whitespace
	split inside the parens is exact."""
	rendering = rendering.strip()
	if rendering == "nil":
		return set()
	if not (rendering.startswith("(") and rendering.endswith(")")):
		raise SystemExit(
		    f"prelude_first_call_census: unparseable report rendering: "
		    f"{rendering!r}")
	return set(rendering[1:-1].split())


# ---------------------------------------------------------------------------
# Corpus assembly.
# ---------------------------------------------------------------------------


def lisp_compat_case_items() -> list[tuple[str, str]]:
	items = []
	for path in sorted(LISP_COMPAT_CASES.glob("*.json")):
		case = json.loads(path.read_text(encoding="utf-8"))
		items.append((str(path.relative_to(ROOT)),
			      check_lisp_oracle.case_source(case)))
	for path in sorted(LISP_COMPAT_FIXTURES.glob("*.el")):
		items.append((str(path.relative_to(ROOT)),
			      path.read_text(encoding="utf-8")))
	return items


def pty_config_el_items() -> list[tuple[str, str]]:
	items = []
	for path in sorted(PTY_DIR.glob("*.yaml")):
		doc = yaml.safe_load(path.read_text(encoding="utf-8"))
		if not isinstance(doc, dict):
			continue
		config_files = doc.get("config_files") or {}
		for relpath, content in config_files.items():
			if not relpath.endswith(".el") or relpath == ".dir-locals.el":
				continue
			label = f"{path.relative_to(ROOT)}:{relpath}"
			items.append((label, content))
	return items


def realistic_corpus_items() -> list[tuple[str, str]]:
	items = lisp_compat_case_items()
	items.append((str(FORECAST_TARGET_INIT.relative_to(ROOT)),
		      FORECAST_TARGET_INIT.read_text(encoding="utf-8")))
	items += pty_config_el_items()
	return items


# ---------------------------------------------------------------------------
# Driving kgbatch.
# ---------------------------------------------------------------------------


def run_kgbatch(args: list[str], timeout: float = 60.0):
	return subprocess.run(
	    [str(KGBATCH)] + args, cwd=ROOT, capture_output=True, text=True,
	    timeout=timeout)


def report_from_stdout(stdout: str, report_path: Path) -> set[str] | None:
	prefix = f"{report_path}: "
	for line in reversed(stdout.splitlines()):
		if line.startswith(prefix):
			return parse_symbol_list(line[len(prefix):])
	return None


def census_batch(shim_path: Path, items: list[tuple[str, str]],
    tmp_dir: Path) -> set[str] | None:
	"""Run one kgbatch process over shim + items + report.  Returns the
	seen set, or None if the process did not produce a report line (a
	crash, not a caught Lisp error -- see the module docstring)."""
	report_path = tmp_dir / "report.el"
	report_path.write_text(REPORT_FORM, encoding="utf-8")
	files = [str(shim_path)]
	for i, (_label, content) in enumerate(items):
		item_path = tmp_dir / f"item-{i}.el"
		item_path.write_text(content, encoding="utf-8")
		files.append(str(item_path))
	files.append(str(report_path))
	try:
		proc = run_kgbatch(["-b"] + files)
	except subprocess.TimeoutExpired:
		return None
	if proc.returncode < 0:
		return None
	return report_from_stdout(proc.stdout, report_path)


def dynamic_census(function_names: list[str], items: list[tuple[str, str]],
    chunk_size: int = CHUNK_SIZE) -> tuple[set[str], list[str]]:
	"""Run `items` through kgbatch in chunks, unioning each chunk's seen
	set.  Returns (seen, crashed_labels) -- crashed_labels names any
	corpus item whose containing chunk had to be bisected down to
	single-file runs because the chunk itself produced no report line."""
	seen: set[str] = set()
	crashed: list[str] = []
	with tempfile.TemporaryDirectory(prefix="kg-census-") as tmp:
		tmp_dir = Path(tmp)
		shim_path = tmp_dir / "shim.el"
		shim_path.write_text(build_shim(function_names), encoding="utf-8")

		chunks = [items[i:i + chunk_size]
			  for i in range(0, len(items), chunk_size)] or [[]]
		for chunk_idx, chunk in enumerate(chunks):
			chunk_dir = tmp_dir / f"chunk-{chunk_idx}"
			chunk_dir.mkdir()
			result = census_batch(shim_path, chunk, chunk_dir)
			if result is not None:
				seen |= result
				continue
			# Crash somewhere in this chunk: bisect to one file at a
			# time so every OTHER file in the chunk still contributes
			# its data point, and the poison one is named rather than
			# silently dropped.
			for item_idx, item in enumerate(chunk):
				item_dir = chunk_dir / f"solo-{item_idx}"
				item_dir.mkdir()
				solo = census_batch(shim_path, [item], item_dir)
				if solo is None:
					crashed.append(item[0])
				else:
					seen |= solo
	return seen, crashed


# ---------------------------------------------------------------------------
# The static cross-check: reuse forecast_audit's reader/walker.
# ---------------------------------------------------------------------------


def static_referenced_names(items: list[tuple[str, str]]) -> set[str]:
	"""Every name referenced in function position (or a quoted/#' symbol
	in a known higher-order argument position) anywhere in lisp/*.el,
	utils/forecast/*.el (forecast_audit.corpus_files()), plus the same
	lisp-compat and PTY-extracted content the dynamic run uses -- kg's
	tree has no other concentration of Lisp source under test/."""
	walker = forecast_audit.collect(forecast_audit.corpus_files())
	for label, content in items:
		try:
			forms = forecast_audit.Reader(content, label).forms()
		except SystemExit:
			# A handful of lisp-compat cases and .dir-locals-adjacent
			# fragments are deliberately malformed (reader-error
			# regression cases); they contribute nothing statically,
			# which is conservative in the right direction (a name
			# only reachable through unparseable text cannot be
			# un-flagged as set 3 by this pass anyway).
			continue
		walker.file = label
		walker.body(forms)
	return set(walker.refs)


# ---------------------------------------------------------------------------
# Set 3's slot cost: remove its defalias forms from a prelude copy and
# re-read peak-live, exactly utils/prelude_slot_census.py's own method.
# ---------------------------------------------------------------------------


def measure_removed_slot_cost(names_to_remove: list[str]) -> dict:
	if not pslot.KGBATCH.is_file():
		raise SystemExit(
		    "prelude_first_call_census: test/kgbatch is not built "
		    "(run 'make test/kgbatch' first)")
	lines = PRELUDE.read_bytes().decode("utf-8").splitlines(keepends=True)
	ranges = toplevel_form_ranges(lines)
	missing = [n for n in names_to_remove if n not in ranges]
	if missing:
		raise SystemExit(
		    "prelude_first_call_census: no defalias form found for "
		    f"{missing!r}")
	drop_lines: set[int] = set()
	for name in names_to_remove:
		start, end = ranges[name]
		drop_lines.update(range(start, end))
	kept = [line for i, line in enumerate(lines) if i not in drop_lines]

	tmp_dir = Path(
	    subprocess.run(["mktemp", "-d"], capture_output=True, text=True,
		check=True).stdout.strip())
	tmp_slice = tmp_dir / "prelude-minus-set3.el"
	try:
		tmp_slice.write_bytes("".join(kept).encode("utf-8"))
		result = subprocess.run(
		    [sys.executable, str(pslot.EMBED_SCRIPT), str(tmp_slice),
		     str(pslot.GENERATED)],
		    cwd=ROOT, capture_output=True, text=True)
		if result.returncode != 0:
			sys.stderr.write(result.stdout)
			sys.stderr.write(result.stderr)
			raise SystemExit(
			    "prelude_first_call_census: embed_lisp.py failed on "
			    "the set-3-removed copy")
		pslot.rebuild_kgbatch()
		probe = subprocess.run(
		    [str(pslot.KGBATCH), "-g", "/dev/null"], cwd=ROOT,
		    capture_output=True, text=True)
		if probe.returncode != 0:
			sys.stderr.write(probe.stdout)
			sys.stderr.write(probe.stderr)
			raise SystemExit(
			    "prelude_first_call_census: kgbatch -g failed "
			    "evaluating the set-3-removed prelude")
		m = PEAK_LIVE_RE.search(probe.stdout)
		if not m:
			raise SystemExit(
			    "prelude_first_call_census: no peak-live in "
			    f"kgbatch -g output: {probe.stdout!r}")
		removed_peak_live = int(m.group(1))
	finally:
		pslot.restore_real_prelude()

	full = subprocess.run([str(pslot.KGBATCH), "-g", "/dev/null"], cwd=ROOT,
	    capture_output=True, text=True)
	full_m = PEAK_LIVE_RE.search(full.stdout)
	if full.returncode != 0 or not full_m:
		raise SystemExit(
		    "prelude_first_call_census: kgbatch -g failed on the "
		    f"restored prelude: {full.stdout!r} {full.stderr!r}")
	full_peak_live = int(full_m.group(1))
	denom = pslot.total_slots()

	return {
	    "names_removed": names_to_remove,
	    "full_peak_live": full_peak_live,
	    "removed_peak_live": removed_peak_live,
	    "delta": full_peak_live - removed_peak_live,
	    "total_slots": denom,
	}


# ---------------------------------------------------------------------------
# The partition.
# ---------------------------------------------------------------------------


def main(argv: list[str]) -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--json", type=Path, default=None,
	    help="write the full partition and every raw reading as JSON")
	parser.add_argument("--slots", action="store_true",
	    help="also measure set 3's arena cost (rebuilds test/kgbatch "
		 "twice; restores the real prelude when done)")
	args = parser.parse_args(argv[1:])

	if not KGBATCH.is_file():
		raise SystemExit(
		    "prelude_first_call_census: test/kgbatch is not built "
		    "(run 'make test/kgbatch' first)")

	prelude_text = PRELUDE.read_text(encoding="utf-8")
	names = parse_prelude_names()
	macros = [n for n, kind in names if kind == "macro"]
	functions = [n for n, kind in names if kind == "function"]
	print(f"prelude names: {len(names)} total, {len(macros)} macros, "
	      f"{len(functions)} functions")

	# -- dynamic: startup-only -----------------------------------------
	startup_seen, startup_crashed = dynamic_census(functions, [])
	print(f"startup-only run: {len(startup_seen)} function(s) called "
	      f"({sorted(startup_seen)})")

	# -- dynamic: realistic ---------------------------------------------
	items = realistic_corpus_items()
	print(f"realistic corpus: {len(items)} item(s) "
	      f"({len(lisp_compat_case_items())} lisp-compat, 1 forecast "
	      f"target-init.el, {len(pty_config_el_items())} PTY config .el "
	      "fragments)")
	realistic_seen, realistic_crashed = dynamic_census(functions, items)
	print(f"realistic run: {len(realistic_seen)} function(s) called")
	if startup_crashed or realistic_crashed:
		print(f"CRASHED (isolated, excluded from the dynamic reading): "
		      f"{sorted(set(startup_crashed) | set(realistic_crashed))}")

	# -- static cross-check ----------------------------------------------
	static_seen = static_referenced_names(items)
	static_functions = static_seen & set(functions)
	print(f"static cross-check: {len(static_functions)} function name(s) "
	      "referenced in function position somewhere in the corpus")

	# -- order-sensitivity: alias-before-shadow --------------------------
	captures = parse_symbol_function_captures(prelude_text)
	shadowed_prims = find_shadowed_names(
	    prelude_text, [prim for _alias, prim in captures])
	order_sensitive = {alias for alias, prim in captures
			   if prim in shadowed_prims}

	# -- the partition -----------------------------------------------------
	# internal--let and progn cannot be wrapped (see
	# UNWRAPPED_SPECIAL_FORM_ALIASES) and so never appear in either
	# dynamic reading; they are asserted into set 1 directly rather than
	# left looking uncalled, which would be the exact wrong-entry-in-set-3
	# failure mode the plan warns about.
	set1 = set(macros) | startup_seen | UNWRAPPED_SPECIAL_FORM_ALIASES
	never_reached = (set(functions) - realistic_seen
			  - UNWRAPPED_SPECIAL_FORM_ALIASES)
	static_only = never_reached & static_functions
	set2 = (realistic_seen - set1) | static_only
	set3 = never_reached - static_functions

	order_sensitive_in_set3 = sorted(order_sensitive & set3)

	partition = {
	    "total_names": len(names),
	    "macros": sorted(macros),
	    "functions": sorted(functions),
	    "set1_every_session": {
		"macros": sorted(macros),
		"dynamic_startup": sorted(startup_seen),
		"unwrapped_special_form_aliases": sorted(
		    UNWRAPPED_SPECIAL_FORM_ALIASES),
		"count": len(set1),
	    },
	    "set2_realistic_session": {
		"dynamic": sorted(realistic_seen - set1),
		"static_only": sorted(static_only),
		"count": len(set2),
	    },
	    "set3_nothing_calls": {
		"names": sorted(set3),
		"count": len(set3),
	    },
	    "order_sensitive_alias_before_shadow": {
		"all_captures": captures,
		"shadowed_primitives": sorted(shadowed_prims),
		"order_sensitive_names": sorted(order_sensitive),
		"in_set3": order_sensitive_in_set3,
	    },
	    "crashed_corpus_items": sorted(
		set(startup_crashed) | set(realistic_crashed)),
	}

	print()
	print(f"set 1 (every session, macros + dynamic startup): "
	      f"{len(set1)} names")
	print(f"set 2 (realistic session): {len(set2)} names "
	      f"({len(realistic_seen - set1)} dynamic, {len(static_only)} "
	      "static-only)")
	print(f"set 3 (nothing calls): {len(set3)} names -> {sorted(set3)}")
	if order_sensitive_in_set3:
		print(f"ORDER-SENSITIVE set-3 candidates (alias-before-shadow, "
		      f"cannot be lazily loaded regardless): "
		      f"{order_sensitive_in_set3}")

	if args.slots:
		print()
		print("measuring set 3's slot cost "
		      f"({len(set3)} names removed)...")
		slot_result = measure_removed_slot_cost(sorted(set3))
		partition["set3_slot_cost"] = slot_result
		print(f"full prelude peak-live: {slot_result['full_peak_live']} "
		      f"of {slot_result['total_slots']}")
		print(f"prelude with set 3 removed: "
		      f"{slot_result['removed_peak_live']} of "
		      f"{slot_result['total_slots']}")
		print(f"delta (set 3's estimated slot cost): "
		      f"{slot_result['delta']}")

	if args.json:
		args.json.write_text(json.dumps(partition, indent=2) + "\n")
		print(f"\nJSON written to {args.json}")

	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
