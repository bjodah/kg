#!/usr/bin/env python3
"""Differential test of kg's regex engine against Emacs' own matcher.

Generates random patterns and subjects from an Emacs-regexp grammar, runs
both through test/regex_differential.c and `emacs -Q --batch -l
utils/regex_oracle.el`, and fails on any disagreement.  This is the harness
that found the span-overshoot bug at a 0.37% hit rate, which is well below
what hand-written cases reach; the remaining deliberate differences are
tracked in doc/TODO.md.

Both sides report BYTE offsets.  Emacs itself reports character offsets;
utils/regex_oracle.el converts.  Getting that wrong makes every multi-byte
subject look like a divergence.

The grammar deliberately stays out of the places where kg is known to
differ from Emacs on purpose; each exclusion is commented where it is made.
Anything else that diverges is a finding, not something to tune away.

Whether a pattern *compiles* is part of the dialect and is compared like
any other answer.  A share of the generated cases are deliberately
malformed -- unclosed groups, unterminated bracket expressions, malformed
intervals, unknown class names -- and both sides must reject them.  The
only permitted acceptance differences are the four named in KG_STRICTER
and KG_LOOSER below, each of which the generator tags on purpose; every
other one is reported as DIVERGE-ACCEPT.  Only kg's work budget
("toocomplex") is incomparable, Emacs having none.

Its first find beyond the previously known cases was the capture
register left by an empty repetition of a quantified group
(`\\(x*\\|a\\)\\{2\\}b` against `ab`), at roughly 4 cases per million -- out of
reach of the default budget, which is the argument for raising --cases when
hunting.  That one is fixed; see doc/TODO.md and match_rep() in
fe/tiny-regex-c/re.c.  `--cases 200000` is clean on seeds 12 (the seed that
found it), 13, 14, 15 and 20260729.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORACLE = os.path.join(HERE, "regex_oracle.el")

# Alphabet: ASCII plus 2-, 3- and 4-byte characters, so that byte offsets
# and character offsets diverge in the subject.
ASCII = list("abcxyz01")
WIDE = list("åäöéç") + list("日本") + list("🙂")
ALL = ASCII + WIDE

# Bracket members avoid ']', '\\', '^' and '-', which have their own
# (dialect-divergent) meanings inside a bracket expression.
MEMBERS = ALL

# kg supports 11 POSIX classes, but only [:ascii:] and [:nonascii:] are
# free of the deliberate ASCII-only divergence: kg's [:alpha:] and friends,
# like its \w \d \s and its case folding, are ASCII-only where Emacs' are
# not (Emacs' \w matches 'å').  Excluded here for that reason, not because
# they are untested -- test/test_regex.c covers them.
POSIX_CLASSES = ["[:ascii:]", "[:nonascii:]"]

# RE_MAX_SPANS is 10 -- the whole match plus nine groups -- so kg refuses
# a pattern with a tenth '\\(' outright, while Emacs compiles it and
# reports every group.  An ordinary case is regenerated when it goes over;
# rnd_bad_pattern() generates the difference deliberately, tagged
# "group-count", so it is checked rather than avoided.
MAX_GROUPS = 9


def rnd_range(rng):
	a, b = sorted(rng.sample(ALL, 2), key=ord)
	return a + "-" + b


def rnd_class(rng):
	body = ""
	for _ in range(rng.randint(1, 3)):
		body += rnd_range(rng) if rng.random() < 0.4 else rng.choice(MEMBERS)
	if rng.random() < 0.15:
		body += rng.choice(POSIX_CLASSES)
	neg = "^" if rng.random() < 0.3 else ""
	return "[" + neg + body + "]"


def rnd_atom(rng, depth):
	r = rng.random()
	if r < 0.35:
		return rng.choice(ALL)
	if r < 0.55:
		return "."
	if r < 0.8:
		return rnd_class(rng)
	if depth > 0 and r < 0.95:
		return "\\(" + rnd_alt(rng, depth - 1) + "\\)"
	return rng.choice(ASCII)


def rnd_quant(rng):
	r = rng.random()
	if r < 0.3:
		return "*"
	if r < 0.5:
		return "+"
	if r < 0.65:
		# Greedy in both dialects.  Emacs' non-greedy operators are a
		# second '?' on a quantifier ('a*?', 'a+?', '??'); kg has no
		# such thing and rejects them, which rnd_bad_pattern() tags
		# "double-quantifier" and generates on purpose.
		return "?"
	n = rng.randint(0, 3)
	m = n + rng.randint(0, 2)
	return rng.choice(
		["\\{%d\\}" % n, "\\{%d,\\}" % n, "\\{,%d\\}" % m, "\\{%d,%d\\}" % (n, m)]
	)


def rnd_seq(rng, depth):
	out = ""
	for _ in range(rng.randint(1, 4)):
		atom = rnd_atom(rng, depth)
		# One quantifier per atom here; a quantifier on a quantifier is
		# a deliberate acceptance difference and rnd_bad_pattern()
		# generates it on purpose.
		if rng.random() < 0.45:
			atom += rnd_quant(rng)
		out += atom
	return out


def rnd_alt(rng, depth):
	# '\|' binds looser than concatenation in both dialects, so whole
	# sequences are the alternatives.
	parts = [rnd_seq(rng, depth) for _ in range(1 if rng.random() < 0.7 else 2)]
	return "\\|".join(parts)


def rnd_pattern(rng):
	pattern = rnd_alt(rng, 2)
	# '^' anchors at offset 0 and '$' at the end; subjects are single-line,
	# so neither has a second place to match.
	if rng.random() < 0.12:
		pattern = "^" + pattern
	if rng.random() < 0.12:
		pattern = pattern + "$"
	return pattern


# Acceptance differences kg documents and Emacs does not share.  A tag
# names the reason; any *other* disagreement about whether a pattern
# compiles is a divergence, not something to skip past.
#
#   double-quantifier  kg rejects 'a++' and 'a\{2\}\{3\}'.  Emacs folds the
#                      two quantifiers into one; kg has no faithful
#                      spelling for the composition and used to compile a
#                      node that could never match.
#   representation-class
#                      kg rejects '[[:multibyte:]]' and '[[:unibyte:]]'.
#                      Emacs answers them from the string's internal
#                      representation, which a byte matcher cannot see;
#                      rejecting beats answering wrongly.
#   group-count        kg rejects a pattern with ten or more '\('.
#                      RE_MAX_SPANS is 10, the whole match plus nine
#                      groups, so the tenth has nowhere to be reported;
#                      Emacs has no limit.
#   group-question     kg reads '\(?' as a group holding a literal '?'.
#                      Emacs rejects it, reserving the spelling for shy
#                      groups, which kg does not have.
KG_STRICTER = {"double-quantifier", "representation-class", "group-count"}
KG_LOOSER = {"group-question"}


def rnd_bad_pattern(rng):
	"""A malformed or edge-case pattern, with the tag it exercises.

	Most of these must be rejected by *both* sides; the four tagged ones
	are the documented acceptance differences.  The untagged edge cases
	(a leading quantifier, a ']' in first position) are accepted by both
	and compared span for span like any other case.
	"""
	body = rnd_seq(rng, 0)
	kinds = [
		("\\(" + body, None),
		(body + "\\)", None),
		("\\(" + body + "\\(" + body + "\\)", None),
		(body + "[" + rng.choice(ASCII), None),
		(body + "[^" + rng.choice(ASCII), None),
		(body + "[]", None),
		(body + "[^]", None),
		(body + "\\", None),
		(body + rng.choice(
			["\\{2,1\\}", "\\{x\\}", "\\{1,2,3\\}", "\\{1", "\\{65536\\}"]), None),
		(body + "[[:" + rng.choice(["foo", "Alpha", "digitx"]) + ":]]", None),
		(body + "[[:" + rng.choice(["multibyte", "unibyte"]) + ":]]",
		 "representation-class"),
		(body + rnd_quant(rng) + rnd_quant(rng), "double-quantifier"),
		("\\(?" + body + "\\)", "group-question"),
		("\\(a\\)" * (MAX_GROUPS + rng.randint(1, 3)), "group-count"),
		(rng.choice(["*", "+", "?"]) + body, None),
		("^" + rng.choice(["*", "+", "?"]) + body, None),
		("[]" + rng.choice(ASCII) + "]" + body, None),
	]
	return rng.choice(kinds)


def rnd_subject(rng):
	return "".join(rng.choice(ALL) for _ in range(rng.randint(0, 8)))


# How often a generated case is a deliberately malformed or edge-case
# pattern rather than a well-formed one.
BAD_PATTERN_RATE = 0.15


def gen_cases(rng, count):
	cases = []
	seen = set()
	while len(cases) < count:
		if rng.random() < BAD_PATTERN_RATE:
			pattern, tag = rnd_bad_pattern(rng)
		else:
			pattern, tag = rnd_pattern(rng), None
		case = (pattern, rnd_subject(rng))
		# Over the group limit kg rejects and Emacs does not, so such a
		# pattern is only usable as the tagged acceptance difference;
		# anywhere else it would drown every other answer.
		if case in seen or (
				tag != "group-count" and case[0].count("\\(") > MAX_GROUPS):
			continue
		seen.add(case)
		cases.append(case + (tag,))
	return cases


def _normalise_spans(fields, index):
	"""Read one "N s0 e0 ..." group, dropping trailing non-participants.

	Not a divergence, a representation difference: kg always reports one
	span per group in the pattern, while Emacs truncates `match-data' at
	the last group that actually matched.  The spans that both sides do
	report still have to agree exactly.
	"""
	count = int(fields[index])
	index += 1
	spans = [(fields[index + 2 * k], fields[index + 2 * k + 1])
		 for k in range(count)]
	index += 2 * count
	while len(spans) > 1 and spans[-1] == ("-1", "-1"):
		spans.pop()
	return ("%d %s" % (len(spans),
			   " ".join(a + " " + b for a, b in spans)), index)


def normalise(result):
	"""Canonical form of one result line, for either mode."""
	fields = result.split()
	if not fields:
		return result
	try:
		if fields[0] == "match":
			return "match " + _normalise_spans(fields, 1)[0]
		if fields[0] == "matches":
			count = int(fields[1])
			index = 2
			matches = []
			for _ in range(count):
				text, index = _normalise_spans(fields, index)
				matches.append(text)
			return "matches %d %s" % (count, " ".join(matches))
	except (IndexError, ValueError):
		return result
	return result


def run_side(argv, payload, env=None):
	proc = subprocess.run(argv, input=payload, capture_output=True, env=env)
	if proc.returncode != 0:
		sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
		raise SystemExit("%s exited %d" % (argv[0], proc.returncode))
	return proc.stdout.decode("utf-8").splitlines()


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--driver", default="test/regex_differential")
	parser.add_argument("--emacs", default="emacs")
	parser.add_argument("--oracle", default=ORACLE)
	parser.add_argument("--cases", type=int, default=2000)
	parser.add_argument("--seed", type=int, default=20260729)
	parser.add_argument("--modes", default="f,fa",
			    help="comma-separated result modes to compare: "
				 "f (first match), fa (every match)")
	parser.add_argument("--show", type=int, default=10,
			    help="how many divergences to print")
	args = parser.parse_args()

	emacs = shutil.which(args.emacs)
	if emacs is None:
		print("SKIP: %s is not on PATH, regex differential test not run"
		      % args.emacs)
		return 0
	if not os.path.exists(args.driver):
		raise SystemExit("driver %s is missing; run 'make %s'"
				 % (args.driver, args.driver))

	rng = random.Random(args.seed)
	modes = [m for m in args.modes.split(",") if m]
	# Every case is asked in every mode.  "f" is the first match from
	# offset 0, which is all this harness compared for its first two
	# years; "fa" is the whole succession of matches, iterated by the
	# rule callers must use, which is where empty-match progress and
	# capture registers left over from a previous match show up.
	cases = [(pattern, subject, tag, mode)
		 for pattern, subject, tag in gen_cases(rng, args.cases)
		 for mode in modes]
	payload = "".join("%s\t%s\t%s\n" % (mode, p, t)
			  for p, t, _, mode in cases).encode("utf-8")

	kg = run_side([args.driver], payload)
	# TERM matters: Emacs refuses to start under some values even in batch.
	oracle = run_side([emacs, "-Q", "--batch", "-l", args.oracle], payload,
			  env=dict(os.environ, TERM="xterm-256color"))
	for name, out in (("driver", kg), ("oracle", oracle)):
		if len(out) != len(cases):
			raise SystemExit("%s produced %d lines for %d cases"
					 % (name, len(out), len(cases)))

	diverged = 0
	skipped = 0
	allowed = 0
	rejected = 0
	per_mode = {mode: 0 for mode in modes}
	for (pattern, subject, tag, mode), mine, theirs in zip(cases, kg, oracle):
		# Giving up is a resource answer, not a statement about the
		# language: kg alone has a budget, so there is nothing to
		# compare it against.  Counted and reported, never hidden.
		if mine == "toocomplex":
			skipped += 1
			continue
		# Whether a pattern compiles at all is part of the dialect and
		# is compared like everything else.  Only the two documented
		# differences above are allowed to disagree.
		if (mine == "badpat") != (theirs == "badpat"):
			if (mine == "badpat" and tag in KG_STRICTER) or (
					theirs == "badpat" and tag in KG_LOOSER):
				allowed += 1
				continue
			diverged += 1
			per_mode[mode] += 1
			if diverged <= args.show:
				print("DIVERGE-ACCEPT mode=%s pattern=%r "
				      "subject=%r kg=%r emacs=%r"
				      % (mode, pattern, subject, mine, theirs))
			continue
		if mine == "badpat":
			rejected += 1
			continue
		if normalise(mine) == normalise(theirs):
			continue
		diverged += 1
		per_mode[mode] += 1
		if diverged <= args.show:
			print("DIVERGE mode=%s pattern=%r subject=%r kg=%r "
			      "emacs=%r" % (mode, pattern, subject, mine, theirs))
	print("regex differential: cases=%d (%d patterns x %s) diverged=%d "
	      "both-reject=%d allowed-diff=%d incomparable=%d seed=%d"
	      % (len(cases), args.cases, ",".join(modes), diverged, rejected,
		 allowed, skipped, args.seed))
	if diverged:
		print("  by mode: %s"
		      % ", ".join("%s=%d" % (m, per_mode[m]) for m in modes))
	return 1 if diverged else 0


if __name__ == "__main__":
	sys.exit(main())
