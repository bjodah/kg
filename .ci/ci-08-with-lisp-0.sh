#!/bin/bash
# The WITH_LISP=0 configuration must keep building and behaving like the
# pre-Lisp editor: no Fe objects, Lisp PTY cases skipped by feature.
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

make clean
make -j"${JOBS}" WITH_LISP=0
./src/kg -V | grep -- '-lisp'
make check WITH_LISP=0
make clean
