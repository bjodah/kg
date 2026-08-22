#!/bin/bash
# This lane stays WITH_TREE_SITTER=0 by policy, not by omission: MSan reports
# uninstrumented code as uninitialised, and the tree-sitter core and its
# grammars are prebuilt prefixes nothing here rebuilds, so a tree-sitter
# build under MSan is guaranteed false positives
# (doc/plans/kg-tree-sitter-plan.md, Refinement, "Sanitizer lanes").  ASan
# and valgrind have no such problem and .ci/ci-13-with-tree-sitter.sh is
# where the configuration is exercised.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-memory-param-retval -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"

# The slowest build in the tree, and the cases that feel it are the ones
# whose budget is counted in STEPS rather than seconds: the deliberate
# catastrophic-backtracking regexes, bounded at MAX_MATCH_STEPS
# (fe/tiny-regex-c/re.c), and the two that cons until the arena is full.
# Their wall time is therefore whatever a step costs in this build, and
# nothing in the case can know that.  Measured on the development box with
# this build pinned to three cores: isearch-regexp-too-complex 7.5 s,
# query-replace-regexp-too-complex 8.2 s, query-replace-regexp-long-line
# 9.6 s, of which ~3 s each is the case's own fixed sleeps.  The hosted CI
# box runs the same MSan work 4.0-4.5x slower (measured twice over: an
# ordinary kgbatch run 0.53 -> 2.10 s, the gc stress 142.7 -> 644.4 s),
# which puts those three at 23-33 s of compute against ci-env.sh's 20 s
# before any contention -- and all three timed out there.  90 leaves
# roughly 3x, and a deadline is free until a case is already failing.
#
# Only the deadline.  The settle floor stays where ci-env.sh puts it: the
# tmux-backed cases' silent gap measures under 50 ms in this build here,
# which is ~0.2 s at that box's ratio against a 0.7 s floor, so there is no
# measurement here that would justify a number.  A pre-set environment
# value still wins, as everywhere else.
PTY_TIMEOUT=${PTY_TIMEOUT_MSAN:-90}
export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_ORACLE_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD PTY_SETTLE_FLOOR PTY_JOBS

"${MAKE_PARALLEL[@]}" -B check
