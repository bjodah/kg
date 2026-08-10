#!/usr/bin/env python3
"""The forecast audit: which names does the Lisp we want to write reach for?

Phase 15's method-first instrument (doc/plans/2026-08-10-elisp-phases-13-19.md).
It reads a corpus of *target* Lisp -- kg's own shipped lisp/*.el plus the
hand-written init file and package sketches under utils/forecast/ -- collects
every name used in function position (plus every `#'sym' and every quoted
symbol handed to a higher-order function), subtracts the names kg actually
implements, and writes the MISSING/COVERED partition to utils/forecast/AUDIT.md
sorted by reference count.

`--check` regenerates into memory and diffs against the checked-in file, which
is what `make forecast-check' runs as part of `make check'.  The report is a
tracked artifact rather than a thing you run to look at: a name that stops
being missing, or a corpus file that starts wanting one more name, shows up in
a review diff.

The implemented-name set is parsed out of the sources, never listed here, and
it has FOUR parts -- a set built from any three of them lies:

  1. fe's `primitive_names[]' and `primitive_aliases[]' (fe/fe.c),
  2. fe's `FeDefineNative' maths natives (fe/fe.c) -- these are NOT in
     primitive_names[], so a manifest-only or primitive-table-only audit
     under-reports `sqrt', `floor', `expt' and eleven more,
  3. kg's `native_bindings[]' (src/lisp_prelude.c),
  4. kg's prelude and shipped packages (lisp/*.el).

(1) and (3) are read through utils/check_lisp_compat.py's own parsers rather
than re-implemented, so the two tools cannot disagree about what fe and kg
declare.  (4) is read with this file's own reader, which is the same reader
the corpus goes through.

Special forms count as covered: they are primitives in fe, and the three the
reader manufactures (`quasiquote', `unquote', `unquote-splicing') are listed
in READER_FORMS below.

What the audit deliberately does NOT do: resolve local bindings.  A name is
counted where it appears in function position, and `(funcall f)' over a
lambda-bound `f' contributes nothing because `f' is not quoted there.  It also
does not descend into quoted data, so `'(car cdr)' is a list of symbols and
not two calls.
"""

from __future__ import annotations

import argparse
import difflib
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_lisp_compat  # noqa: E402  (path set above)

FORECAST_DIR = ROOT / "utils" / "forecast"
REPORT = FORECAST_DIR / "AUDIT.md"
FE_C = ROOT / "fe" / "fe.c"

# Corpus, in report order: kg's own shipped Lisp first (it should contribute
# no MISSING names at all), then the floor, then the sketches.
def corpus_files() -> list[Path]:
	shipped = sorted((ROOT / "lisp").glob("*.el"))
	target = [FORECAST_DIR / "target-init.el"]
	sketches = sorted(p for p in FORECAST_DIR.glob("forecast-*.el"))
	return shipped + target + sketches


# ---------------------------------------------------------------------------
# A small reader.  Enough of Emacs' syntax for hand-written Lisp: comments,
# strings, character literals, the four reader macros, vectors.
# ---------------------------------------------------------------------------


class Sym:
	__slots__ = ("name",)

	def __init__(self, name: str):
		self.name = name


class Atom:
	"""A string, number or character literal -- anything that is not a
	symbol and has no structure the walker cares about."""

	__slots__ = ("text",)

	def __init__(self, text: str):
		self.text = text


class Seq:
	"""A list or a vector.  `reader` marks a form the reader manufactured
	from `'`, `#'`, `` ` ``, `,` or `,@`, so the walker does not count the
	head as a reference to a function nobody wrote."""

	__slots__ = ("items", "vector", "reader")

	def __init__(self, items, vector=False, reader=False):
		self.items = items
		self.vector = vector
		self.reader = reader


SYMBOL_BREAK = set(" \t\r\n()[]\"';`,")

READER_MACROS = {
	"'": "quote",
	"`": "quasiquote",
	",": "unquote",
}


class Reader:
	def __init__(self, text: str, where: str):
		self.text = text
		self.where = where
		self.i = 0
		self.n = len(text)

	def error(self, message: str):
		line = self.text.count("\n", 0, self.i) + 1
		raise SystemExit(f"FAIL: {self.where}:{line}: {message}")

	def skip_blanks(self):
		while self.i < self.n:
			c = self.text[self.i]
			if c in " \t\r\n\f":
				self.i += 1
			elif c == ";":
				while self.i < self.n and self.text[self.i] != "\n":
					self.i += 1
			else:
				return

	def forms(self) -> list:
		out = []
		while True:
			self.skip_blanks()
			if self.i >= self.n:
				return out
			out.append(self.read())

	def read(self):
		self.skip_blanks()
		if self.i >= self.n:
			self.error("unexpected end of input")
		c = self.text[self.i]
		if c == "(":
			return self.read_seq(")", vector=False)
		if c == "[":
			return self.read_seq("]", vector=True)
		if c in ")]":
			self.error(f"stray {c!r}")
		if c == '"':
			return self.read_string()
		if c == "?":
			return self.read_char()
		if c == "#" and self.text.startswith("#'", self.i):
			self.i += 2
			return Seq([Sym("function"), self.read()], reader=True)
		if c in READER_MACROS:
			head = READER_MACROS[c]
			self.i += 1
			if c == "," and self.i < self.n and self.text[self.i] == "@":
				self.i += 1
				head = "unquote-splicing"
			return Seq([Sym(head), self.read()], reader=True)
		return self.read_symbol()

	def read_seq(self, closer: str, vector: bool) -> Seq:
		self.i += 1
		items = []
		while True:
			self.skip_blanks()
			if self.i >= self.n:
				self.error(f"missing {closer!r}")
			if self.text[self.i] == closer:
				self.i += 1
				return Seq(items, vector=vector)
			items.append(self.read())

	def read_string(self) -> Atom:
		start = self.i
		self.i += 1
		while self.i < self.n:
			c = self.text[self.i]
			if c == "\\":
				self.i += 2
				continue
			self.i += 1
			if c == '"':
				return Atom(self.text[start:self.i])
		self.error("unterminated string")

	def read_char(self) -> Atom:
		start = self.i
		self.i += 1  # the '?'
		if self.i < self.n and self.text[self.i] == "\\":
			self.i += 1
		if self.i >= self.n:
			self.error("unterminated character literal")
		c = self.text[self.i]
		self.i += 1
		# ?\C-x, ?\M-\C-x and friends: a modifier prefix continues.
		while (c in "CMSsAH^" and self.i + 1 < self.n
		       and self.text[self.i] == "-"):
			self.i += 1
			if self.text[self.i] == "\\":
				self.i += 1
			c = self.text[self.i]
			self.i += 1
		return Atom(self.text[start:self.i])

	def read_symbol(self):
		start = self.i
		while self.i < self.n:
			c = self.text[self.i]
			if c == "\\":
				self.i += 2
				continue
			if c in SYMBOL_BREAK:
				break
			self.i += 1
		token = self.text[start:self.i]
		if not token:
			self.error("empty token")
		if _looks_numeric(token):
			return Atom(token)
		return Sym(token)


def _looks_numeric(token: str) -> bool:
	body = token[1:] if token[:1] in "+-" else token
	if not body:
		return False
	return body.replace(".", "", 1).replace("e-", "", 1).replace(
		"e+", "", 1).isdigit()


# ---------------------------------------------------------------------------
# The walker.
# ---------------------------------------------------------------------------

# Reader-manufactured heads, never counted as references from a reader form.
READER_FORMS = {"quote", "function", "quasiquote", "unquote",
		"unquote-splicing"}

# Argument positions (1-based, after the head) that name a FUNCTION when the
# argument is a quoted symbol.  `(sort xs 'string<)' is a reference to
# string<; `(add-hook 'after-save-hook 'my-fn)' is one to my-fn and not to
# after-save-hook, which is why this is a position table and not "any quoted
# symbol argument".
FUNCTION_ARGUMENTS = {
	("funcall", 1), ("apply", 1),
	("mapcar", 1), ("mapc", 1), ("mapcan", 1), ("mapconcat", 1),
	("maphash", 1),
	("seq-filter", 1), ("seq-map", 1), ("seq-find", 1), ("seq-some", 1),
	("seq-remove", 1), ("seq-reduce", 1), ("seq-sort", 2),
	("sort", 2),
	("add-hook", 2), ("remove-hook", 2),
	("global-set-key", 2), ("local-set-key", 2), ("define-key", 3),
	("keymap-set", 3),
	("set-process-filter", 2), ("set-process-sentinel", 2),
	("advice-add", 3),
}

# Heads whose definition name this walker records as a corpus definition.
DEFINING_FORMS = {"defun", "defmacro", "defalias", "defsubst"}


class Walker:
	def __init__(self):
		self.refs: Counter[str] = Counter()
		self.ref_files: dict[str, set[str]] = {}
		self.defs: dict[str, set[str]] = {}
		self.file_refs: Counter[str] = Counter()
		self.file_defs: Counter[str] = Counter()
		self.file = ""

	# -- recording -----------------------------------------------------
	def ref(self, name: str):
		self.refs[name] += 1
		self.file_refs[self.file] += 1
		self.ref_files.setdefault(name, set()).add(self.file)

	def define(self, name: str):
		self.defs.setdefault(name, set()).add(self.file)
		self.file_defs[self.file] += 1

	# -- entry ---------------------------------------------------------
	def walk_file(self, path: Path):
		self.file = str(path.relative_to(ROOT))
		for form in Reader(path.read_text(encoding="utf-8"),
				   self.file).forms():
			self.walk(form)

	def body(self, forms):
		for form in forms:
			self.walk(form)

	def walk(self, form):
		if not isinstance(form, Seq) or form.vector or not form.items:
			return
		head = form.items[0]
		if not isinstance(head, Sym):
			self.body(form.items)
			return
		name = head.name
		if name in READER_FORMS:
			self.walk_reader_form(name, form)
			return
		self.ref(name)
		handler = SPECIAL.get(name)
		if handler:
			handler(self, form.items)
			return
		for index, argument in enumerate(form.items[1:], start=1):
			if (name, index) in FUNCTION_ARGUMENTS:
				self.quoted_function(argument)
			self.walk(argument)

	def walk_reader_form(self, name: str, form: Seq):
		if not form.reader:
			# Somebody wrote (quote x) or (function f) out in full;
			# that IS a call of the special form.
			self.ref(name)
		if name == "function":
			if len(form.items) > 1:
				# `#'f' names a function outright, wherever it
				# stands, which is the whole point of writing it.
				if isinstance(form.items[1], Sym):
					self.ref(form.items[1].name)
				self.walk(form.items[1])
			return
		if name in ("unquote", "unquote-splicing", "quasiquote"):
			self.body(form.items[1:])
		# `quote' is data: nothing below it is code.

	def quoted_function(self, argument):
		"""Record `'name' / `#'name' in a function-taking position.

		A BARE symbol here is a variable, not a name: `(funcall f x)'
		over a lambda-bound `f' says nothing about which function is
		called, and counting it reported the prelude's own parameters
		`f' and `body' as missing library names.
		"""
		if (isinstance(argument, Seq) and argument.items
		    and isinstance(argument.items[0], Sym)
		    and argument.items[0].name in ("quote", "function")
		    and len(argument.items) > 1
		    and isinstance(argument.items[1], Sym)):
			self.ref(argument.items[1].name)


def _name_of(form) -> str | None:
	"""The symbol a defining form names: `foo', `'foo' or `#'foo'."""
	if isinstance(form, Sym):
		return form.name
	if (isinstance(form, Seq) and len(form.items) > 1
	    and isinstance(form.items[0], Sym)
	    and form.items[0].name in ("quote", "function")
	    and isinstance(form.items[1], Sym)):
		return form.items[1].name
	return None


# -- the special forms whose operands are not all code ----------------------


def _sp_quote(w: Walker, items):
	pass


def _sp_lambda(w: Walker, items):
	w.body(items[2:])


def _sp_defun(w: Walker, items):
	name = _name_of(items[1]) if len(items) > 1 else None
	if name:
		w.define(name)
	w.body(items[3:])


def _sp_defalias(w: Walker, items):
	name = _name_of(items[1]) if len(items) > 1 else None
	if name:
		w.define(name)
	w.body(items[2:])


def _sp_defvar(w: Walker, items):
	w.body(items[2:])


def _sp_setq(w: Walker, items):
	w.body(items[2::2])


def _sp_let(w: Walker, items):
	if len(items) > 1 and isinstance(items[1], Seq):
		for binding in items[1].items:
			if isinstance(binding, Seq):
				w.body(binding.items[1:])
	w.body(items[2:])


def _sp_cond(w: Walker, items):
	for clause in items[1:]:
		if isinstance(clause, Seq):
			w.body(clause.items)
		else:
			w.walk(clause)


def _sp_dolist(w: Walker, items):
	if len(items) > 1 and isinstance(items[1], Seq):
		w.body(items[1].items[1:])
	w.body(items[2:])


def _sp_condition_case(w: Walker, items):
	w.body(items[2:3])
	for handler in items[3:]:
		if isinstance(handler, Seq):
			w.body(handler.items[1:])


def _sp_place(w: Walker, items):
	"""(push X PLACE) / (pop PLACE): a bare symbol PLACE is a variable."""
	for operand in items[1:]:
		if isinstance(operand, Seq):
			w.walk(operand)


def _sp_declare(w: Walker, items):
	pass


SPECIAL = {
	"quote": _sp_quote,
	"lambda": _sp_lambda,
	"macro": _sp_lambda,
	"defun": _sp_defun,
	"defmacro": _sp_defun,
	"defsubst": _sp_defun,
	"defalias": _sp_defalias,
	"fset": _sp_defalias,
	"defvar": _sp_defvar,
	"defconst": _sp_defvar,
	"defcustom": _sp_defvar,
	"setq": _sp_setq,
	"setq-local": _sp_setq,
	"setq-default": _sp_setq,
	"let": _sp_let,
	"let*": _sp_let,
	"cond": _sp_cond,
	"dolist": _sp_dolist,
	"dotimes": _sp_dolist,
	"condition-case": _sp_condition_case,
	"push": _sp_place,
	"pop": _sp_place,
	"declare": _sp_declare,
}


# ---------------------------------------------------------------------------
# The implemented-name set.
# ---------------------------------------------------------------------------


def parse_fe_math_natives() -> set[str]:
	"""fe's FeDefineNative names.

	These are NOT rows of primitive_names[], and they are not claimed by
	fe/compat/features.json's per-primitive rows either, so an audit built
	from either table alone reports `sqrt' and thirteen others as missing
	when kg has had them since the first pin.
	"""
	text = check_lisp_compat.strip_c_comments(
		FE_C.read_text(encoding="utf-8"))
	import re
	names = set(re.findall(r'FeDefineNative\(\s*ctx\s*,\s*"([^"]+)"', text))
	if not names:
		raise SystemExit(
			"FAIL: no FeDefineNative(ctx, \"...\") calls in fe/fe.c")
	return names


def parse_kg_lisp_definitions() -> set[str]:
	"""Everything lisp/*.el defines, read with this file's own reader.

	The prelude's own `(defalias 'NAME ...)' forms are also what
	check_lisp_compat.py's parse_kg_prelude_defs() finds; both are called,
	and the union is used, so a spelling either parser misses is still
	covered.
	"""
	walker = Walker()
	for path in sorted((ROOT / "lisp").glob("*.el")):
		walker.walk_file(path)
	return set(walker.defs) | check_lisp_compat.parse_kg_prelude_defs()


# Source label -> name set, in the order a covered name is attributed.
def implemented_sets() -> dict[str, set[str]]:
	return {
		"kg-native": check_lisp_compat.parse_kg_natives(),
		"kg-lisp": parse_kg_lisp_definitions(),
		"fe-primitive": check_lisp_compat.parse_fe_primitives(),
		"fe-native": parse_fe_math_natives(),
		"reader": set(READER_FORMS) | {"declare"},
	}


# ---------------------------------------------------------------------------
# The watch item: hash tables, vectors, records (plan's Declined section).
# ---------------------------------------------------------------------------

WATCH = {
	"hash-tables": [
		"make-hash-table", "gethash", "puthash", "remhash", "clrhash",
		"maphash", "hash-table-p", "hash-table-count", "hash-table-keys",
		"hash-table-values", "copy-hash-table", "sxhash", "sxhash-equal",
	],
	"vectors": [
		"vector", "make-vector", "vconcat", "aref", "aset", "vectorp",
	],
	"records": [
		"record", "make-record", "recordp", "cl-defstruct",
		"cl-defgeneric", "cl-defmethod",
	],
}


# ---------------------------------------------------------------------------
# The report.
# ---------------------------------------------------------------------------

HEADER = """\
# Forecast audit

**Generated file.  Do not edit.**  `make forecast-audit` rewrites it;
`make forecast-check` (part of `make check`) fails when it and the tree
disagree.  The tool is `utils/forecast_audit.py`, the corpus is
`utils/forecast/` plus kg's shipped `lisp/*.el`, and
`utils/forecast/README.md` says what each kind of corpus file is for.

MISSING is the implementation order for the Lisp surface: a name the
corpus calls that fe's primitives, fe's maths natives, kg's
`native_bindings[]` and kg's own `lisp/*.el` between them do not define.
COVERED is the other half of the same partition, kept in the report so
that a name which *stops* being covered is a diff and not a silence.
"""


def render(walker: Walker, implemented: dict[str, set[str]],
	   corpus: list[Path]) -> str:
	covered_by: dict[str, str] = {}
	for label, names in implemented.items():
		for name in names:
			covered_by.setdefault(name, label)
	for name in walker.defs:
		covered_by.setdefault(name, "corpus")

	missing = [n for n in walker.refs if n not in covered_by]
	covered = [n for n in walker.refs if n in covered_by]

	def by_count(names):
		return sorted(names, key=lambda n: (-walker.refs[n], n))

	lines = [HEADER, ""]

	lines.append("## Corpus")
	lines.append("")
	lines.append("| File | References | Definitions |")
	lines.append("| --- | ---: | ---: |")
	for path in corpus:
		rel = str(path.relative_to(ROOT))
		lines.append(f"| `{rel}` | {walker.file_refs[rel]} | "
			     f"{walker.file_defs[rel]} |")
	lines.append(f"| **total** | **{sum(walker.refs.values())}** | "
		     f"**{len(walker.defs)}** |")
	lines.append("")

	lines.append("## Implemented-name set")
	lines.append("")
	lines.append("| Source | Names |")
	lines.append("| --- | ---: |")
	for label, names in implemented.items():
		lines.append(f"| {label} | {len(names)} |")
	lines.append(f"| corpus (defined by the corpus itself) | "
		     f"{len(walker.defs)} |")
	lines.append("")

	lines.append(f"## MISSING ({len(missing)} names, "
		     f"{sum(walker.refs[n] for n in missing)} references)")
	lines.append("")
	lines.append("| Refs | Name | Wanted by |")
	lines.append("| ---: | --- | --- |")
	for name in by_count(missing):
		files = ", ".join(sorted(
			Path(f).name for f in walker.ref_files[name]))
		lines.append(f"| {walker.refs[name]} | `{name}` | {files} |")
	lines.append("")

	lines.append("## Watch item: hash tables, vectors, records")
	lines.append("")
	lines.append("Measured demand for the three families the plan's "
		     "Declined section keeps off the roadmap.  Reopening any "
		     "of them needs its own phase; this table is the evidence "
		     "either way.")
	lines.append("")
	lines.append("| Family | References | Names seen |")
	lines.append("| --- | ---: | --- |")
	for family, names in WATCH.items():
		seen = [(n, walker.refs[n]) for n in names if walker.refs[n]]
		total = sum(c for _, c in seen)
		spelled = ", ".join(f"`{n}` x{c}" for n, c in
				    sorted(seen, key=lambda p: (-p[1], p[0])))
		lines.append(f"| {family} | {total} | {spelled or '--'} |")
	lines.append("")

	lines.append(f"## COVERED ({len(covered)} names, "
		     f"{sum(walker.refs[n] for n in covered)} references)")
	lines.append("")
	lines.append("| Refs | Name | Source |")
	lines.append("| ---: | --- | --- |")
	for name in by_count(covered):
		lines.append(f"| {walker.refs[name]} | `{name}` | "
			     f"{covered_by[name]} |")
	lines.append("")
	return "\n".join(lines)


def collect(corpus: list[Path]) -> Walker:
	walker = Walker()
	for path in corpus:
		walker.walk_file(path)
	return walker


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--check", action="store_true",
			    help="diff against the checked-in report instead "
				 "of rewriting it")
	args = parser.parse_args()

	corpus = corpus_files()
	missing_files = [p for p in corpus if not p.is_file()]
	if missing_files:
		print("FAIL: corpus file(s) missing: "
		      + ", ".join(str(p) for p in missing_files),
		      file=sys.stderr)
		return 1

	walker = collect(corpus)
	text = render(walker, implemented_sets(), corpus)

	if not args.check:
		REPORT.write_text(text, encoding="utf-8")
		print(f"forecast-audit: wrote "
		      f"{REPORT.relative_to(ROOT)} from {len(corpus)} "
		      f"corpus file(s)")
		return 0

	current = REPORT.read_text(encoding="utf-8") if REPORT.is_file() else ""
	if current == text:
		print(f"forecast-check: {REPORT.relative_to(ROOT)} matches the "
		      f"tree ({len(corpus)} corpus file(s))")
		return 0
	print(f"forecast-check: {REPORT.relative_to(ROOT)} is stale", file=sys.stderr)
	print("  run 'make forecast-audit' and commit the result.", file=sys.stderr)
	for line in difflib.unified_diff(
			current.splitlines(), text.splitlines(),
			fromfile="checked-in", tofile="regenerated",
			lineterm=""):
		print(f"  {line}", file=sys.stderr)
	return 1


if __name__ == "__main__":
	sys.exit(main())
