#!/usr/bin/env python3
"""Split lisp/prelude.el into an eager array and a deferred array.

Phase 1 of doc/plans/2026-08-14-embedded-prelude.md: 93 of the prelude's
128 top-level definitions are called by no startup path (Phase 0.2's
`--deferrable` census).  Embedding their source unconditionally means every
session pays their arena cost whether it ever calls one or not.  This
script is what `make lisp-prelude-generate`/`lisp-prelude-check` run
instead of calling `utils/embed_lisp.py` directly on the whole file: it
partitions lisp/prelude.el's own bytes into

  * the EAGER array (src/lisp_prelude_generated.inc, same name and shape
    utils/embed_lisp.py has always produced) -- everything except the
    names below, evaluated at startup exactly as before Phase 1;
  * the DEFERRED array plus a name -> (offset, length) index table
    (src/lisp_prelude_deferred_generated.inc) -- the excised forms'
    source, concatenated in their original file order, read and
    evaluated one form at a time by the `internal--force-deferred`
    native (src/lisp_prelude.c) the first time a caller reaches a stub
    lisp/prelude.el's own `internal--make-deferred-stub` installed for
    it.

Deliberately dumb about WHICH names are deferred: that partition is one
hand-maintained, reviewed policy file, utils/prelude_deferred_names.txt
(the same shape as the ratchet manifests under .ci/ -- data, not logic),
never a second copy of the definitions themselves.  Everything this
script does with lisp/prelude.el's own TEXT is mechanical: find each
listed name's top-level form by the same column-0 boundary rule
utils/prelude_slot_census.py and utils/prelude_first_call_census.py
already rely on (nothing nested ever starts at column 0), and move
exactly those bytes, unchanged, into the second array.  A name in the
policy file with no matching form in lisp/prelude.el is a hard failure --
that is what stops the two from silently drifting apart.

This script does NOT decide which names to defer (that is Phase 0.2's
completed measurement, banked as the checked-in policy file) and does not
parse Lisp beyond finding top-level form boundaries -- the same
"deliberately dumb" contract utils/embed_lisp.py states for the whole
file.  The byte-rendering itself is embed_lisp.render(), imported rather
than reimplemented, so the two arrays' formatting cannot drift.

Usage:
    embed_lisp_split.py <lisp/prelude.el> <eager.inc> <deferred.inc>
        [--names FILE]
    embed_lisp_split.py --self-test

`make lisp-prelude-generate` runs this to refresh both checked-in files;
`make lisp-prelude-check` runs it again into temporary files and fails if
either differs from what is checked in, and runs `--self-test` first --
the command line those two rely on is the one this script gets wrong
silently, so it is checked before it is trusted.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import embed_lisp  # noqa: E402  (path set above)

DEFAULT_NAMES_FILE = (
    Path(__file__).resolve().parent / "prelude_deferred_names.txt")

# Only --self-test reads this; the ordinary run takes the prelude as its
# first positional, since `make lisp-prelude-check' names the file it is
# checking rather than letting the script assume one.
DEFAULT_PRELUDE_FILE = (
    Path(__file__).resolve().parent.parent / "lisp" / "prelude.el")

DEFERRED_ARRAY_NAME = "lisp_prelude_deferred_generated"

# A top-level form starts at column 0; nothing nested in lisp/prelude.el
# ever does (the same property utils/prelude_slot_census.py's
# find_sections() and utils/prelude_first_call_census.py's
# toplevel_form_ranges() already rely on).  Matched against bytes, not
# decoded text, so offsets below are exact byte offsets straight into
# what utils/embed_lisp.py will encode -- no line/byte reconciliation
# needed.
FORM_START_RE = re.compile(rb"(?m)^\(")
# No `^` here: matched with .match(source, start), which already anchors
# the attempt to `start`.  A `^` in the pattern itself would (without
# re.MULTILINE, which this one deliberately omits) assert the absolute
# start of the whole buffer instead of just `start`, and silently match
# nothing for every form after the first.
DEFALIAS_NAME_RE = re.compile(rb"\(defalias '(\S+)\s")

INDEX_ENTRY_TEMPLATE = '\t{{ "{name}", {offset}, {length} }},'

INDEX_HEADER = """
/* name -> (offset, length) into {array}[] above, one entry per name
 * utils/prelude_deferred_names.txt lists, in lisp/prelude.el's own
 * source order.  internal--force-deferred (src/lisp_prelude.c) is the
 * only reader; it does one linear scan per name, at most once per name
 * per process (the whole point of a self-replacing stub).
 *
 * `struct lisp_prelude_deferred_entry` is NOT declared in this
 * generated file -- it is a fixed, hand-written type (src/lisp_prelude.c,
 * beside the #include of this file), the same split embed_lisp.py's own
 * array/length pair has between generated data and hand-written use. */
static const struct lisp_prelude_deferred_entry {name}_index[] = {{
{entries}
}};

static const size_t {name}_index_len =
    sizeof({name}_index) / sizeof({name}_index[0]);
"""


def read_names(path: Path) -> list[str]:
	names = []
	for line in path.read_text(encoding="utf-8").splitlines():
		line = line.strip()
		if not line or line.startswith("#"):
			continue
		names.append(line)
	if not names:
		raise SystemExit(f"embed_lisp_split: {path} names no names")
	return names


def toplevel_form_byte_ranges(source: bytes) -> dict[str, tuple[int, int]]:
	"""name -> (start, end) byte offsets for every top-level `(defalias
	'NAME ...)` form, in source order.  A form's extent is "up to the next
	column-0 `(`, or EOF" -- the same rule
	utils/prelude_first_call_census.py's toplevel_form_ranges() uses at
	line grain; this one works in bytes so the caller never needs to
	reconcile a line count against a byte offset."""
	starts = [m.start() for m in FORM_START_RE.finditer(source)]
	ranges: dict[str, tuple[int, int]] = {}
	for idx, start in enumerate(starts):
		end = starts[idx + 1] if idx + 1 < len(starts) else len(source)
		m = DEFALIAS_NAME_RE.match(source, start)
		if m:
			ranges[m.group(1).decode("utf-8")] = (start, end)
	return ranges


def split(source: bytes, names: list[str]) -> tuple[
	bytes, list[tuple[str, bytes]]]:
	"""Partition `source` into (eager_bytes, deferred_forms).
	deferred_forms is [(name, form_bytes), ...] in FILE order (not
	`names`' order), which is what makes the deferred array's own layout
	independent of how the policy file happens to be sorted."""
	ranges = toplevel_form_byte_ranges(source)
	missing = [n for n in names if n not in ranges]
	if missing:
		raise SystemExit(
		    "embed_lisp_split: no top-level (defalias 'NAME ...) form "
		    f"in lisp/prelude.el for: {missing!r} -- "
		    "utils/prelude_deferred_names.txt has drifted from the file")
	wanted = set(names)
	excised = sorted(
	    (span for name, span in ranges.items() if name in wanted),
	    key=lambda span: span[0])

	# No two top-level forms overlap by construction (each starts where
	# the previous one's range ended), so this is a defensive check
	# rather than a live one -- but a future change to the boundary rule
	# should fail loudly here rather than silently corrupt an array.
	for (s0, e0), (s1, _e1) in zip(excised, excised[1:]):
		if s1 < e0:
			raise SystemExit(
			    "embed_lisp_split: overlapping top-level form ranges "
			    f"at bytes {s0}-{e0} and {s1}-.. -- boundary detection "
			    "is broken")

	eager_pieces = []
	deferred_forms = []
	cursor = 0
	for start, end in excised:
		eager_pieces.append(source[cursor:start])
		name = DEFALIAS_NAME_RE.match(source, start).group(1).decode("utf-8")
		deferred_forms.append((name, source[start:end]))
		cursor = end
	eager_pieces.append(source[cursor:])
	eager_bytes = b"".join(eager_pieces)

	# Losslessness, proven rather than assumed: re-interleaving the eager
	# remainder and the excised forms at their ORIGINAL positions must
	# reproduce lisp/prelude.el exactly.  eager_bytes was built by the
	# same single left-to-right pass that recorded each excised span, so
	# this can only fail if toplevel_form_byte_ranges() found a wrong
	# boundary -- exactly the bug this check exists to catch before it
	# reaches a generated .inc file.
	rebuilt_pieces = []
	cursor = 0
	for start, end in excised:
		rebuilt_pieces.append(source[cursor:start])
		rebuilt_pieces.append(source[start:end])
		cursor = end
	rebuilt_pieces.append(source[cursor:])
	if b"".join(rebuilt_pieces) != source:
		raise SystemExit(
		    "embed_lisp_split: internal error -- reassembling the split "
		    "did not reproduce lisp/prelude.el byte-for-byte")

	return eager_bytes, deferred_forms


def render_index(deferred_forms: list[tuple[str, bytes]]) -> str:
	entries = []
	offset = 0
	for name, form_bytes in deferred_forms:
		escaped = name.replace("\\", "\\\\").replace('"', '\\"')
		entries.append(INDEX_ENTRY_TEMPLATE.format(
		    name=escaped, offset=offset, length=len(form_bytes)))
		offset += len(form_bytes)
	return INDEX_HEADER.format(
	    array=DEFERRED_ARRAY_NAME, name=DEFERRED_ARRAY_NAME,
	    entries="\n".join(entries))


def run_split(args: list[str]) -> subprocess.CompletedProcess[str]:
	"""One `embed_lisp_split.py ARGS...' run, as a child process.

	The self-test below is about the COMMAND LINE, so it drives the real
	entry point over a real argv rather than calling main() with a list:
	an exit status and a usage message on stderr are the behaviour under
	test, and neither is observable from inside the process.
	"""
	return subprocess.run(
	    [sys.executable, str(Path(__file__).resolve())] + args,
	    capture_output=True, text=True, check=False)


def self_test() -> int:
	"""Prove `--names FILE' is accepted and a malformed option is not.

	Three cases, all writing into a temp directory and reading only this
	repository's own inputs:

	  * `--names FILE' with the operand the Makefile's default path would
	    have used must succeed AND produce both .inc files byte-for-byte
	    identical to the run that omits the option.  A documented option
	    that silently changes the output would be a worse defect than one
	    that refuses to run.
	  * `--names' with its operand missing must exit non-zero with a
	    message -- not an IndexError traceback out of the argv indexing.
	  * an option this script does not define must be REFUSED, not
	    dropped.  The old hand-rolled filter discarded every `--' word and
	    then split the prelude anyway, so a typo'd flag was a silent
	    success.
	"""
	prelude = DEFAULT_PRELUDE_FILE
	if not prelude.is_file():
		print(f"self-test: {prelude} is missing", file=sys.stderr)
		return 1

	with tempfile.TemporaryDirectory() as tmp:
		root = Path(tmp)
		default_eager = root / "default-eager.inc"
		default_deferred = root / "default-deferred.inc"
		named_eager = root / "named-eager.inc"
		named_deferred = root / "named-deferred.inc"

		base = run_split([str(prelude), str(default_eager),
				  str(default_deferred)])
		if base.returncode != 0:
			print("self-test: the default-path run failed: "
			      f"{base.stderr.strip()}", file=sys.stderr)
			return 1

		named = run_split([str(prelude), str(named_eager),
				   str(named_deferred),
				   "--names", str(DEFAULT_NAMES_FILE)])
		if named.returncode != 0:
			print("self-test: `--names FILE' was rejected: "
			      f"{named.stderr.strip()}", file=sys.stderr)
			return 1
		if (named_eager.read_bytes() != default_eager.read_bytes()
		    or named_deferred.read_bytes()
		    != default_deferred.read_bytes()):
			print("self-test: `--names' with the default path produced "
			      "different .inc output than omitting it",
			      file=sys.stderr)
			return 1

		missing = run_split([str(prelude), str(root / "x.inc"),
				     str(root / "y.inc"), "--names"])
		if missing.returncode == 0:
			print("self-test: `--names' with no operand was accepted",
			      file=sys.stderr)
			return 1
		if "Traceback" in missing.stderr:
			print("self-test: `--names' with no operand died in a "
			      f"traceback:\n{missing.stderr}", file=sys.stderr)
			return 1

		unknown = run_split([str(prelude), str(root / "x.inc"),
				     str(root / "y.inc"), "--not-an-option"])
		if unknown.returncode == 0:
			print("self-test: an undefined option was accepted",
			      file=sys.stderr)
			return 1

	print("self-test: `--names FILE' is accepted and reproduces the "
	      "default path's output byte for byte")
	print(f"self-test: a missing operand exits {missing.returncode} with a "
	      "message, an undefined option exits "
	      f"{unknown.returncode}, neither in a traceback")
	return 0


def main(argv: list[str]) -> int:
	parser = argparse.ArgumentParser(
	    prog=Path(argv[0]).name,
	    description="Split lisp/prelude.el into an eager and a deferred "
			"array.")
	# nargs="?" rather than three required positionals only because
	# --self-test takes none of them; the check below restores the arity
	# for every other run, with argparse's own usage message and exit 2.
	parser.add_argument("prelude", nargs="?", type=Path,
			    help="lisp/prelude.el")
	parser.add_argument("eager", nargs="?", type=Path,
			    help="the eager array, e.g. "
				 "src/lisp_prelude_generated.inc")
	parser.add_argument("deferred", nargs="?", type=Path,
			    help="the deferred array plus its index table")
	parser.add_argument("--names", type=Path, default=DEFAULT_NAMES_FILE,
			    help="the deferred-name policy file "
				 f"(default: {DEFAULT_NAMES_FILE})")
	parser.add_argument("--self-test", action="store_true",
			    help="prove this command line parses as documented; "
				 "writes only into a temp directory")
	args = parser.parse_args(argv[1:])

	if args.self_test:
		return self_test()
	if args.prelude is None or args.eager is None or args.deferred is None:
		parser.error("the prelude, eager and deferred paths are all "
			     "required")

	prelude_path, eager_path, deferred_path = (
	    args.prelude, args.eager, args.deferred)
	names_path = args.names

	source = prelude_path.read_bytes()
	if len(source) == 0:
		print(f"embed_lisp_split: {prelude_path} is empty", file=sys.stderr)
		return 1
	names = read_names(names_path)
	eager_bytes, deferred_forms = split(source, names)
	if len(eager_bytes) == 0:
		print("embed_lisp_split: eager array would be empty",
		      file=sys.stderr)
		return 1

	deferred_blob = b"".join(form_bytes for _name, form_bytes in
	    deferred_forms)
	eager_path.write_text(embed_lisp.render(eager_bytes), encoding="utf-8")
	deferred_text = (
	    embed_lisp.render(deferred_blob, DEFERRED_ARRAY_NAME)
	    + render_index(deferred_forms))
	deferred_path.write_text(deferred_text, encoding="utf-8")

	print(f"embed_lisp_split: wrote {eager_path} ({len(eager_bytes)} bytes, "
	      f"{len(names)} names moved out) and {deferred_path} "
	      f"({len(deferred_blob)} bytes, {len(deferred_forms)} names)")
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
