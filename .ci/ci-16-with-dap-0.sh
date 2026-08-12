#!/bin/bash
# The WITH_DAP=0 configuration must keep building and behaving like the
# editor before the debugger client existed: no debugger keymaps, no
# adapter, DAP PTY cases skipped by feature (`kg -V` says -dap, which is
# what utils/pty_accept.py reads).
#
# The axis defaults ON, as WITH_LSP does and for the same reason -- adapters
# are found at run time, so there is nothing to install -- which makes the
# DISABLED build the one no other lane covers.  This is .ci/ci-14's shape
# with one addition: DAP is the third optional subsystem, so the
# orthogonality runs are against BOTH of the others, and the all-off build
# is the one that proves the three stubs coexist.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

make clean
make -j"${JOBS}" WITH_DAP=0
./src/kg -V | grep -- '-dap'
make check WITH_DAP=0

# (WITH_LSP=0, WITH_DAP=1): the axes are orthogonal, and the debugger
# carries no KG_USE_LSP conditional -- deliberately, since its own config
# discovery may not reach for lsp_workspace_root().  The unit layer is what
# says so beyond linking.
make clean
make -j"${JOBS}" WITH_LSP=0 WITH_DAP=1
./src/kg -V | grep -- '-lsp' | grep -- '+dap'
make check-unit WITH_LSP=0 WITH_DAP=1

# (WITH_LISP=0, WITH_DAP=1): the debugger's maps are created from C before
# any init file is read, so they must exist in a build with no interpreter
# to read one.
make clean
make -j"${JOBS}" WITH_LISP=0 WITH_DAP=1
./src/kg -V | grep -- '-lisp' | grep -- '+dap'
make check-unit WITH_LISP=0 WITH_DAP=1

# All three off: the editor as it was before any of the optional
# subsystems, which is the configuration in which every stub half is the
# only half linked.
make clean
make -j"${JOBS}" WITH_LISP=0 WITH_LSP=0 WITH_DAP=0
./src/kg -V | grep -- '-lisp' | grep -- '-lsp' | grep -- '-dap'
make check-unit WITH_LISP=0 WITH_LSP=0 WITH_DAP=0

# Leave the tree in no configuration at all rather than in this one: the
# next step, or the next human, gets the default build it asks for.
make clean
