#!/usr/bin/env python3
"""Nothing under external/ may reach a shipped artifact, and every
subfolder must be in the provenance ledger.

external/ holds third-party files vendored for testing only (the ledger,
external/provenance-ledger.toml, carries the policy and the per-folder
licenses).  Deliberately listy and dumb, like the other drift checks: it
does not trace what a build actually packages, it greps the mechanisms
that put files into artifacts and fails on the first one that mentions
external/ -- which catches the failure this exists for, a rule or
manifest edited to copy or embed a vendored file.

Checked, in order:
  * every immediate subdirectory of external/ has a ledger table, and
    every ledger table names a directory that exists;
  * .gitattributes still carries `external/ export-ignore`, the line
    that keeps `git archive` (utils/mkrel.sh's tarball) from shipping
    the tree;
  * no file under src/ mentions external/ at all -- nothing compiled
    or #embed-ed may reach it;
  * the Makefile mentions external/ only on this check's own lines;
  * the packaging manifests (debian/rules, debian/control, PKGBUILD)
    and utils/mkrel.sh never mention it.

Tests and PTY fixtures may reference external/ freely; that is what it
is for.
"""

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEDGER = ROOT / "external" / "provenance-ledger.toml"
ALLOWED_MAKEFILE = re.compile(r"external[-_]quarantine|check_external_quarantine")


def fail(message):
	print(f"external-quarantine-check: {message}", file=sys.stderr)
	return 1


def main():
	errors = 0

	external = ROOT / "external"
	if not LEDGER.is_file():
		return fail(f"missing ledger: {LEDGER.relative_to(ROOT)}")
	with open(LEDGER, "rb") as fp:
		ledger = tomllib.load(fp)
	subdirs = sorted(p.name for p in external.iterdir() if p.is_dir())
	for name in subdirs:
		if name not in ledger:
			errors += fail(f"external/{name}/ has no ledger entry")
	for name in ledger:
		if name not in subdirs:
			errors += fail(f"ledger names external/{name}/, which does not exist")

	gitattributes = (ROOT / ".gitattributes").read_text()
	if not re.search(r"^external/\s+export-ignore\s*$", gitattributes, re.M):
		errors += fail(".gitattributes lost `external/ export-ignore`; "
			       "release tarballs (git archive) would ship the tree")

	for path in sorted((ROOT / "src").iterdir()):
		if path.is_file() and "external/" in path.read_text(errors="replace"):
			errors += fail(f"{path.relative_to(ROOT)} mentions external/; "
				       "nothing compiled may reach it")

	for lineno, line in enumerate(
			(ROOT / "Makefile").read_text().splitlines(), 1):
		if line.lstrip().startswith("#"):
			continue
		if "external/" in line and not ALLOWED_MAKEFILE.search(line):
			errors += fail(f"Makefile:{lineno} mentions external/ outside "
				       f"this check's own lines: {line.strip()!r}")

	for rel in ("debian/rules", "debian/control", "PKGBUILD",
		    "utils/mkrel.sh"):
		path = ROOT / rel
		if path.is_file() and "external/" in path.read_text():
			errors += fail(f"{rel} mentions external/; "
				       "packaging must not copy vendored files")

	if errors:
		return 1
	print(f"# external-quarantine-check: {len(subdirs)} ledgered "
	      "subfolder(s); no artifact mechanism mentions external/")
	return 0


if __name__ == "__main__":
	sys.exit(main())
