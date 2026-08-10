#!/bin/bash
# The WITH_TREE_SITTER=1 configuration: it has to build, link, say so, and
# run both test layers -- and it has to do that without src/syntax_legacy.c,
# because the backend is chosen by the Makefile's source list rather than by
# an #ifdef, and "the other backend is not compiled" is the property that
# claim rests on (doc/plans/kg-tree-sitter-plan.md, Phase 11).
#
# The tree-sitter core is a prebuilt prefix on the developer box, not a
# submodule (Refinement decision 1), and hosted runners have no /opt-9.  v1
# keeps this lane developer-box-only: with no prefix it SKIPs with a printed
# reason, the same culture as the Emacs oracle and tmux cases.  Promoting it
# to hosted CI means building the core from its pinned tag in a cached step,
# and is a separate decision.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

TREE_SITTER_PREFIX=${TREE_SITTER_PREFIX:-/opt-9/tree-sitter-v0.26.11-release}

if [ ! -e "${TREE_SITTER_PREFIX}/include/tree_sitter/api.h" ]; then
	echo "SKIP: no tree-sitter prefix at ${TREE_SITTER_PREFIX}" \
	     "(set TREE_SITTER_PREFIX=... to a prefix containing" \
	     "include/tree_sitter/api.h)"
	exit 0
fi

export TREE_SITTER_PREFIX

# (WITH_LISP=1, WITH_TREE_SITTER=1): the full-feature build.
make clean
make -j"${JOBS}" WITH_TREE_SITTER=1
./src/kg -V | grep -- '+tree-sitter'
# Backend exclusion, and the reason this runs straight after a clean build:
# a stale object from an earlier default build would pass a weaker test.
test ! -e src/syntax_legacy.o
test -e src/syntax_tree_sitter.o
# Both layers.  The PTY cases assert saved-file outcomes, not colours, so
# they must pass identically under a backend that highlights nothing; a case
# that fails here is a case that was quietly depending on the legacy
# scanners.
make check WITH_TREE_SITTER=1

# (WITH_LISP=0, WITH_TREE_SITTER=1): the two axes are orthogonal, so
# tree-sitter must carry no accidental Fe/Lisp dependency.
make clean
make -j"${JOBS}" WITH_LISP=0 WITH_TREE_SITTER=1
./src/kg -V | grep -- '-lisp +tree-sitter'
test ! -e src/syntax_legacy.o

# Leave the tree in no configuration at all rather than in this one: the
# next step, or the next human, gets the default build it asks for.
make clean
