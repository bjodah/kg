#!/bin/bash
# The WITH_LSP=0 configuration must keep building and behaving like the
# editor before the LSP client existed: no lsp objects, LSP PTY cases
# skipped by feature (`kg -V` says -lsp, which is what
# utils/pty_accept.py reads).
#
# Unlike tree-sitter, this axis defaults ON -- there is nothing to install
# for it, servers being found at run time -- so it is the DISABLED build
# that no other lane covers, exactly as .ci/ci-08-with-lisp-0.sh is for
# Lisp.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

make clean
make -j"${JOBS}" WITH_LSP=0
./src/kg -V | grep -- '-lsp'
make check WITH_LSP=0

# (WITH_LISP=0, WITH_LSP=1): the two axes are orthogonal, and the LSP
# client carries no KG_USE_LISP conditional, so it has to build and say so
# with Fe out of the picture.
make clean
make -j"${JOBS}" WITH_LISP=0 WITH_LSP=1
./src/kg -V | grep -- '-lisp' | grep -- '+lsp'

# Leave the tree in no configuration at all rather than in this one: the
# next step, or the next human, gets the default build it asks for.
make clean
