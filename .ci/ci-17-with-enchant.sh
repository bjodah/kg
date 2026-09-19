#!/bin/bash
# The WITH_ENCHANT=1 configuration: it has to build, link, say so, and
# run both test layers -- and the spell PTY cases must run rather than
# skip, because `kg -V` says +enchant there.
#
# Unlike LSP/DAP, this axis defaults OFF -- libenchant-2 is a build-time
# dependency -- so it is the ENABLED build that no other lane covers,
# exactly as .ci/ci-13-with-tree-sitter.sh is for tree-sitter.  Unlike
# tree-sitter there is no pinned source build to fall back to: the
# library comes from the box.  A box without it cannot exercise this
# lane, so that box SKIPs with the reason named rather than failing;
# hosted CI carries the verdict in the summary either way.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

if ! pkg-config --exists enchant-2; then
	echo "SKIP: no enchant-2 known to pkg-config;" \
	     "install libenchant-2-dev to run this lane"
	exit 0
fi

# (WITH_LISP=1, WITH_ENCHANT=1): the spell build.
make clean
make -j"${JOBS}" WITH_ENCHANT=1
./src/kg -V | grep -- '+enchant'
make check WITH_ENCHANT=1

# (WITH_LISP=0, WITH_ENCHANT=1): the two axes are orthogonal -- the
# spell checker reads `spell-language' through the disabled init-file
# settings with Fe out of the picture -- so it has to build and say so.
make clean
make -j"${JOBS}" WITH_LISP=0 WITH_ENCHANT=1
./src/kg -V | grep -- '-lisp' | grep -- '+enchant'
make check WITH_LISP=0 WITH_ENCHANT=1

# Leave the tree in no configuration at all rather than in this one: the
# next step, or the next human, gets the default build it asks for.
make clean
