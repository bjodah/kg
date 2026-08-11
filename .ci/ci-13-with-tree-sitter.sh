#!/bin/bash
# The WITH_TREE_SITTER=1 configuration: it has to build, link, say so, and
# run both test layers -- and it has to do that without src/syntax_legacy.c,
# because the backend is chosen by the Makefile's source list rather than by
# an #ifdef, and "the other backend is not compiled" is the property that
# claim rests on (doc/plans/kg-tree-sitter-plan.md, Phase 11).
#
# The tree-sitter core is a prebuilt prefix on the developer box, not a
# submodule (Refinement decision 1), and hosted runners have no such prefix.
# This lane used to SKIP there, which made the whole tree-sitter backend
# developer-box-only; it now builds the prefix instead, so hosted CI
# exercises it.  The cost of that promotion is one dependency this step did
# not have: network access to the pins' hosts.  It is spent on purpose -- a
# lane that quietly stops testing a backend is how the backend rots -- so a
# build that cannot happen FAILS with the reason named rather than skipping.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

# Same default, and the same order, as the Makefile's TREE_SITTER_PREFIX:
# an explicit value, else the box's $TREE_SITTER_ROOT, else the install this
# development environment has.  It is resolved here as well as there
# because what happens next has to know the prefix before make runs.
TREE_SITTER_PREFIX=${TREE_SITTER_PREFIX:-${TREE_SITTER_ROOT:-/opt-2/tree-sitter-v0.26.12-release}}

# Nothing found: build utils/tree-sitter-pins into a cache keyed by that
# file's hash, and use that.  The order -- environment, then cache, then
# build -- is what makes the end state a one-variable change: an image that
# ships a prefix and sets TREE_SITTER_ROOT never reaches this branch, and
# the CI image is built from a repository whose business that is.  Until
# then a hosted runner's workspace is ephemeral, so the cache is a
# within-run one and the build is paid per run (~20 s, against the two full
# test suites below).
#
# TREE_SITTER_GRAMMAR_DIR is exported with it because the two are separate
# knobs in the Makefile: the prefix is what the editor LINKS, the grammar
# directory is the compiled-in path it dlopen's grammars from, and a built
# prefix supplies both.  On the developer-box path neither is touched, so
# whatever the box exports still decides.
if [ ! -e "${TREE_SITTER_PREFIX}/include/tree_sitter/api.h" ]; then
	echo "no tree-sitter prefix at ${TREE_SITTER_PREFIX};" \
	     "falling back to the pinned build (utils/tree-sitter-pins)"
	TREE_SITTER_PREFIX=$(utils/build-tree-sitter.sh --jobs "${JOBS}")
	export TREE_SITTER_GRAMMAR_DIR="${TREE_SITTER_PREFIX}/lib"
fi

export TREE_SITTER_PREFIX
echo "tree-sitter prefix: ${TREE_SITTER_PREFIX}"

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
