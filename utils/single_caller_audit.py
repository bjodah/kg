#!/usr/bin/env python3
"""List the static helpers that exist for exactly one caller, worst first.

A complexity ratchet rewards splitting a function up, and splitting is
usually the right answer -- but not always.  The split that made things
worse leaves a fingerprint: a `static` helper with one call site whose
parameter list is a handful of the caller's locals threaded through a
function boundary.  Nothing was decoupled; the coupling was just written
out longhand, and now it has to be read in two places.  Cyclomatic
complexity cannot see this (both halves score better than the whole), so
this looks for the shape instead.

This is a manual audit, not a gate.  It has no Makefile target, no
baseline file and no failing exit code: run it after a round of
extractions, read the top of the list, and inline what should never have
been split.  Being wrong costs nothing here, so it errs towards saying
nothing -- a function it cannot attribute confidently is dropped from the
report rather than guessed at.

How to read the output
----------------------
The list is sorted by parameter count descending, then body length
ascending, so the first rows are the highest-leverage inline candidates.

  * Few parameters, one caller: usually FINE.  A three-line helper taking
    `(b)` or nothing is a name for a step, and a name is worth a
    function.
  * Many parameters, one caller, short body: the smell this exists for.
    Five parameters means five of the caller's locals crossed the
    boundary; if the body is a dozen lines, the boundary is buying less
    than it costs.  Read it, and consider putting it back.

The middle of the list is not a verdict either way.  Nothing here is a
defect on its own -- the tool finds candidates for a human to judge.

Limitations, all deliberate
---------------------------
Parsing is textual and heuristic.  Comments and string/character
literals are blanked (preserving every byte offset) before anything is
matched, and `[[...]]` attributes with them, so a name in prose is never
a call.  Beyond that:

  * The preprocessor is not evaluated.  A name pasted together with `##`,
    produced by an X-macro, or called from inside a `#define` body is not
    seen as a call.  To keep that from inventing single-caller helpers,
    any mention at file scope that is not a prototype counts as a
    reference and drops the function from the report.
  * A mention that is not a call -- `&fn`, a bare `fn` in a table such as
    `cmdtable` in src/cmd.c, a callback handed to `qsort` -- drops the
    function too.  A function whose address is taken is not an inline
    candidate at all, and the address might be taken far from the one
    apparent call.
  * Call sites are counted across every scanned file even though a
    `static` function is only reachable from its own.  That can only
    over-count callers, which loses candidates rather than inventing
    them.  A name defined `static` more than once -- in two files, or in
    the two arms of an `#if` -- is skipped outright rather than
    attributed to a guess.
  * A function that calls itself is skipped: recursion is not something
    to inline.
  * Code inside `#if 0` (or any disabled branch) is read like the rest.
  * Definitions this recognizes are the shape kg writes: a head of type
    words and `*` only.  A K&R definition, a function returning a
    function pointer, or a head carrying `__attribute__((...))` is not
    recognized, and neither it nor its callees appear.  Its body is still
    scanned for calls, so callers are not lost -- only candidates are.
  * Parameter count is a proxy for "threads the caller's locals through".
    A helper taking one fat struct pointer scores 1 and may still be the
    bad split; a helper taking four independent buffers may be fine.
"""

from __future__ import annotations

import argparse
import bisect
import re
import signal
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Names that are followed by a parenthesised something and then a brace
# without being a function definition.  `return (struct kg_pos){0}` is the
# one that actually occurs; the rest are here so a future one does not
# have to be diagnosed twice.
KEYWORDS = {
	"if", "else", "while", "for", "switch", "do", "case", "return",
	"sizeof", "typeof", "alignof", "_Generic", "defined", "static_assert",
}

# A definition's head, once preprocessor lines are dropped: type words,
# `static`, and pointer stars.  Anything else (a call, an initialiser, a
# macro invocation) has punctuation in it and is not a definition.
HEAD_OK = re.compile(r"^[\w\s*]+$")
# A preprocessor directive, backslash continuations and all: a multi-line
# `#define` sits between two definitions and would otherwise put
# punctuation in the second one's head.
PREPROC_LINE = re.compile(r"^[ \t]*#(?:\\\n|[^\n])*", re.M)
IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
ATTRIBUTE = re.compile(r"\[\[.*?\]\]", re.S)


def display_path(path: Path) -> str:
	"""How to write a scanned file's name: relative when it is below."""
	try:
		return str(path.relative_to(Path.cwd()))
	except ValueError:
		return str(path)


def blank(text: str) -> str:
	"""The same text with every non-newline byte replaced by a space."""
	return re.sub(r"[^\n]", " ", text)


def strip_c(text: str) -> str:
	"""Blank comments and literals, keeping every byte offset intact.

	One pass, because the orders are not interchangeable: stripping line
	comments first eats the `//` inside a "file://" literal, and
	stripping literals first eats the quote inside a comment.
	"""
	out = list(text)
	n = len(text)
	i = 0
	while i < n:
		c = text[i]
		if c == "/" and i + 1 < n and text[i + 1] == "*":
			end = text.find("*/", i + 2)
			end = n if end < 0 else end + 2
		elif c == "/" and i + 1 < n and text[i + 1] == "/":
			end = text.find("\n", i)
			end = n if end < 0 else end
		elif c in "\"'":
			end = i + 1
			while end < n and text[end] != c and text[end] != "\n":
				end += 2 if text[end] == "\\" else 1
			end = min(end + 1, n)
		else:
			i += 1
			continue
		out[i:end] = blank(text[i:end])
		i = end
	return ATTRIBUTE.sub(lambda m: blank(m.group(0)), "".join(out))


def match_bracket(text: str, start: int, opener: str, closer: str) -> int:
	"""Offset of the bracket closing the one at `start`, or -1."""
	depth = 0
	for i in range(start, len(text)):
		if text[i] == opener:
			depth += 1
		elif text[i] == closer:
			depth -= 1
			if depth == 0:
				return i
	return -1


def skip_space(text: str, i: int) -> int:
	while i < len(text) and text[i].isspace():
		i += 1
	return i


def count_params(params: str) -> int:
	"""Top-level commas plus one; `(void)` and `()` are zero.

	`...` counts as a parameter, and a function-pointer parameter counts
	as one however many commas its own signature has.
	"""
	params = params.strip()
	if not params or params == "void":
		return 0
	count = 1
	depth = 0
	for c in params:
		if c in "([":
			depth += 1
		elif c in ")]":
			depth -= 1
		elif c == "," and depth == 0:
			count += 1
	return count


class Definition:
	"""One function definition, in one file."""

	def __init__(self, path, name, name_offset, line, params, body_start,
		     body_end, body_lines, is_static):
		self.path = path
		self.name = name
		self.name_offset = name_offset
		self.line = line
		self.params = params
		self.body_start = body_start
		self.body_end = body_end
		self.body_lines = body_lines
		self.is_static = is_static


class Source:
	"""A scanned file: its blanked text, its line index, its definitions."""

	def __init__(self, path: Path):
		self.path = path
		self.label = display_path(path)
		raw = path.read_text(encoding="utf-8", errors="replace")
		self.text = strip_c(raw)
		# Definitions are looked for with the preprocessor directives
		# blanked as well, so that a multi-line `#define` between two
		# functions cannot put a brace or a paren in the second one's
		# head.  Both strings have the same length, so offsets in one
		# are offsets in the other.  References are still counted in
		# `self.text`: a name mentioned inside a macro body is a use.
		self.code = PREPROC_LINE.sub(lambda m: blank(m.group(0)), self.text)
		self.newlines = [i for i, c in enumerate(self.text) if c == "\n"]
		self.defs = list(self.find_definitions())
		self.defs.sort(key=lambda d: d.body_start)
		self.body_starts = [d.body_start for d in self.defs]
		self.name_offsets = {d.name_offset for d in self.defs}

	def line_of(self, offset: int) -> int:
		return bisect.bisect_right(self.newlines, offset) + 1

	def head_of(self, start: int) -> str | None:
		"""The declaration head before `start`, or None if it is not one.

		The head runs back to the nearest `;`, `{` or `}`, which is where
		the previous declaration, block or initialiser ended.
		"""
		cut = max(self.code.rfind(c, 0, start) for c in ";{}")
		head = self.code[cut + 1:start]
		if not head.strip() or not HEAD_OK.match(head):
			return None
		return head

	def find_definitions(self):
		text = self.code
		for m in IDENTIFIER.finditer(text):
			name = m.group(0)
			after = skip_space(text, m.end())
			if name in KEYWORDS or after >= len(text) or text[after] != "(":
				continue
			close = match_bracket(text, after, "(", ")")
			if close < 0:
				continue
			brace = skip_space(text, close + 1)
			if brace >= len(text) or text[brace] != "{":
				continue
			head = self.head_of(m.start())
			if head is None:
				continue
			end = match_bracket(text, brace, "{", "}")
			if end < 0:
				continue
			yield Definition(
				path=self.label,
				name=name,
				name_offset=m.start(),
				line=self.line_of(m.start()),
				params=count_params(text[after + 1:close]),
				body_start=brace,
				body_end=end,
				body_lines=self.line_of(end) - self.line_of(brace) + 1,
				is_static=bool(re.search(r"\bstatic\b", head)))

	def enclosing(self, offset: int) -> Definition | None:
		"""The definition whose body contains `offset`, if any."""
		i = bisect.bisect_right(self.body_starts, offset) - 1
		if i < 0:
			return None
		found = self.defs[i]
		return found if offset < found.body_end else None


def scan_references(sources, wanted):
	"""Every mention of a wanted name: its calls, and its other uses.

	Returns (calls, referenced): calls maps a name to a list of
	(path, line, caller name) call sites; referenced is the set of names
	mentioned somewhere that is not a call -- a function-pointer table,
	an `&fn`, or file-scope code the preprocessor owns.
	"""
	calls: dict[str, list[tuple[str, int, str]]] = {}
	referenced: set[str] = set()
	for source in sources:
		text = source.text
		for m in IDENTIFIER.finditer(text):
			name = m.group(0)
			if name not in wanted or m.start() in source.name_offsets:
				continue
			after = skip_space(text, m.end())
			is_call = after < len(text) and text[after] == "("
			holder = source.enclosing(m.start())
			if holder is None:
				# File scope: a prototype is not a use.  Anything
				# else there is a table row, an initialiser or a
				# macro body -- none of them inlinable.
				close = (match_bracket(text, after, "(", ")")
					 if is_call else -1)
				tail = skip_space(text, close + 1) if close >= 0 else -1
				if 0 <= tail < len(text) and text[tail] == ";":
					continue
				referenced.add(name)
			elif is_call:
				calls.setdefault(name, []).append(
					(source.label, source.line_of(m.start()),
					 holder.name))
			else:
				referenced.add(name)
	return calls, referenced


def collect(paths):
	sources = []
	for path in paths:
		try:
			sources.append(Source(path))
		except OSError as exc:
			print("single_caller_audit: %s: %s" % (path, exc),
			      file=sys.stderr)
	return sources


def candidates(sources, max_callers):
	"""Static definitions with between one and `max_callers` call sites."""
	statics: dict[str, list[Definition]] = {}
	for source in sources:
		for definition in source.defs:
			if definition.is_static:
				statics.setdefault(definition.name, []).append(definition)

	ambiguous = {n for n, d in statics.items() if len(d) > 1}
	calls, referenced = scan_references(sources, set(statics) - ambiguous)

	hits = []
	skipped = {"ambiguous": len(ambiguous), "referenced": 0, "recursive": 0}
	for name, defs in statics.items():
		if name in ambiguous:
			continue
		definition = defs[0]
		if name in referenced:
			skipped["referenced"] += 1
			continue
		sites = calls.get(name, [])
		if any(caller == name for _, _, caller in sites):
			skipped["recursive"] += 1
			continue
		if 1 <= len(sites) <= max_callers:
			hits.append((definition, sites))
	hits.sort(key=lambda h: (-h[0].params, h[0].body_lines, str(h[0].path)))
	return hits, skipped, sum(len(d) for d in statics.values())


def report(hits, skipped, total_statics, sources, max_callers) -> None:
	print("single-caller audit: %d of %d static functions in %d file%s have"
	      " %s"
	      % (len(hits), total_statics, len(sources),
		 "" if len(sources) == 1 else "s",
		 "1 call site" if max_callers == 1
		 else "1..%d call sites" % max_callers))
	print("  (%d not judged: %d mentioned somewhere other than a call,"
	      " %d recursive, %d defined static more than once)"
	      % (skipped["referenced"] + skipped["recursive"]
		 + skipped["ambiguous"], skipped["referenced"],
		 skipped["recursive"], skipped["ambiguous"]))
	if not hits:
		return
	print()
	width = max(len(h[0].name) for h in hits)
	place = max(len("%s:%d" % (h[0].path, h[0].line)) for h in hits)
	print("%-4s %-5s  %-*s  %-*s  %s"
	      % ("args", "lines", place, "definition", width, "function",
		 "caller"))
	for definition, sites in hits:
		callers = ", ".join(
			"%s (%s:%d)" % (caller, path, line)
			if path != definition.path else "%s (:%d)" % (caller, line)
			for path, line, caller in sites)
		print("%4d %5d  %-*s  %-*s  %s"
		      % (definition.params, definition.body_lines, place,
			 "%s:%d" % (definition.path, definition.line), width,
			 definition.name, callers))


def main() -> int:
	parser = argparse.ArgumentParser(
		description="Find static helpers with a single call site --"
			    " candidates for inlining back into their caller.",
		epilog="An audit, not a gate: it always exits 0.  Many"
		       " parameters plus one caller is the shape worth"
		       " reading; few parameters plus one caller is usually a"
		       " helper earning its name.")
	parser.add_argument("paths", nargs="*",
			    help="files to scan (default: src/*.c)")
	parser.add_argument("--max-callers", type=int, default=1, metavar="N",
			    help="report functions with at most N call sites"
				 " (default: 1)")
	args = parser.parse_args()

	# The list is meant to be skimmed through `head`; die on SIGPIPE the
	# way every other filter does instead of printing a traceback.
	if hasattr(signal, "SIGPIPE"):
		signal.signal(signal.SIGPIPE, signal.SIG_DFL)

	if args.paths:
		paths = [Path(p) for p in args.paths]
	else:
		paths = sorted((ROOT / "src").glob("*.c"))
	paths = [p for p in paths if p.is_file()]
	if not paths:
		print("single_caller_audit: no files to scan", file=sys.stderr)
		return 2
	if args.max_callers < 1:
		print("single_caller_audit: --max-callers must be at least 1",
		      file=sys.stderr)
		return 2

	sources = collect(paths)
	hits, skipped, total = candidates(sources, args.max_callers)
	report(hits, skipped, total, sources, args.max_callers)
	return 0


if __name__ == "__main__":
	sys.exit(main())
