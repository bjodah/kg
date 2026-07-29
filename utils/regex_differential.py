#!/usr/bin/env python3
"""Differential test of kg's regex engine against Emacs' own matcher.

Generates random patterns and subjects from an Emacs-regexp grammar, runs
both through test/regex_differential.c and `emacs -Q --batch -l
utils/regex_oracle.el`, and fails on any disagreement.  This is the harness
that found the span-overshoot bug at a 0.37% hit rate, which is well below
what hand-written cases reach; see
.meta-docs/plans/103-REGEX-EMACS-FIDELITY-FIXES.md.

Both sides report BYTE offsets.  Emacs itself reports character offsets;
utils/regex_oracle.el converts.  Getting that wrong makes every multi-byte
subject look like a divergence.

The grammar deliberately stays out of the places where kg is known to
differ from Emacs on purpose; each exclusion is commented where it is made.
Anything else that diverges is a finding, not something to tune away.

One such finding is open and NOT excluded here: the capture register after
an empty final iteration of an interval-quantified group (`\\(x*\\|a\\)\\{2\\}b`
against `ab`).  It is written up in doc/TODO.md.  At roughly 4 cases per
million it is out of reach of the default budget, but raising --cases will
eventually reach it; that is the tool working, not a regression.
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

# RE_MAX_SPANS is 10, so group 10 and up would be silently dropped by kg
# while Emacs still reports them.  Cases over the limit are regenerated.
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
		# Greedy in both dialects; the non-greedy '?' of other engines
		# is spelled '\?' in Emacs and is not generated here.
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
		# One quantifier per atom: a quantifier on a quantifier
		# ('a\{2\}\{2\}') is valid in Emacs but never matches in kg, a
		# known and documented hole.
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


def rnd_subject(rng):
	return "".join(rng.choice(ALL) for _ in range(rng.randint(0, 8)))


def gen_cases(rng, count):
	cases = []
	seen = set()
	while len(cases) < count:
		case = (rnd_pattern(rng), rnd_subject(rng))
		if case in seen or case[0].count("\\(") > MAX_GROUPS:
			continue
		seen.add(case)
		cases.append(case)
	return cases


def normalise(result):
	"""Drop trailing groups that did not participate.

	Not a divergence, a representation difference: kg always reports one
	span per group in the pattern, while Emacs truncates `match-data' at
	the last group that actually matched.  The spans that both sides do
	report still have to agree exactly.
	"""
	if not result.startswith("match "):
		return result
	fields = result.split()[2:]
	spans = list(zip(fields[0::2], fields[1::2]))
	while len(spans) > 1 and spans[-1] == ("-1", "-1"):
		spans.pop()
	return "match %d %s" % (len(spans), " ".join(a + " " + b for a, b in spans))


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
	cases = gen_cases(rng, args.cases)
	payload = "".join("%s\t%s\n" % case for case in cases).encode("utf-8")

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
	for (pattern, subject), mine, theirs in zip(cases, kg, oracle):
		# A pattern one side refuses to compile, or a match one side
		# gives up on, is not a comparable answer.  These are counted
		# and reported rather than hidden.
		if mine in ("badpat", "toocomplex") or theirs == "badpat":
			skipped += 1
			continue
		if normalise(mine) == normalise(theirs):
			continue
		diverged += 1
		if diverged <= args.show:
			print("DIVERGE pattern=%r subject=%r kg=%r emacs=%r"
			      % (pattern, subject, mine, theirs))
	print("regex differential: cases=%d diverged=%d incomparable=%d seed=%d"
	      % (len(cases), diverged, skipped, args.seed))
	return 1 if diverged else 0


if __name__ == "__main__":
	sys.exit(main())
