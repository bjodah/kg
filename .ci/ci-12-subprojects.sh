#!/bin/bash
# kg ships both submodules' code: the editor links fe/fe.c and
# fe/tiny-regex-c/re.c directly, so a defect in either is a defect in kg.
# Nothing under .ci/ built them until this step, and the root suites only
# reach the parts of them kg happens to exercise -- tiny's own sanitizer
# lanes spent months building plain "cc -O3" because its Makefile pinned
# CFLAGS with ":=", and no root gate could have noticed.
#
# This stage runs the submodules' *fast* targets, not their numbered
# runners: fe/.ci and fe/tiny-regex-c/.ci are valgrind, MSan, coverage and
# clang-analyzer passes that cost minutes each and duplicate what the root
# lanes already do to the same sources.  They stay the submodules' own
# green light (run them in the submodule before advancing a pin); what
# belongs in every kg run is the standalone behaviour kg cannot see: Fex,
# the Fe script suite, and the regex engine's own test vectors.
#
# Budget: ~15 s wall, against a ~65 s parallel run whose critical path is
# ci-02 and the sanitizer lanes, so it costs nothing there and about 15 s
# serially.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

# Each submodule keeps its own compiler defaults (fe builds -Weverything
# with clang), so no CC/CFLAGS is exported into them here.
"${MAKE_PARALLEL[@]}" -C fe check complexity-check pmccabe-check format-check
"${MAKE_PARALLEL[@]}" -C fe/tiny-regex-c check complexity-check pmccabe-check \
	format-check
