#!/bin/bash
# Both test layers under valgrind: every unit binary and every PTY case runs
# a kg wrapped in ${VALGRIND}, which is minutes rather than seconds, so this
# is an expensive step -- run-ci-steps.sh skips it unless CI_EXPENSIVE=1 or
# --expensive is given, and running this script directly always runs it.
# The build is a plain gcc one; the analyzer is ci-03's job.
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc"
export TEST_RUNNER="${VALGRIND}"
export KG_RUNNER="${VALGRIND}"

# This lane's kg is 20-50x slower than the one every case's own cover was
# sized against, so the lane raises the two knobs that only ever wait
# LONGER after the last key: the settle floor and the hard deadline.  It
# deliberately does not touch PTY_KEY_DELAY_ADD -- a per-key add stretches
# the gap INSIDE an escape sequence a case spells out in pieces
# (insert-overwrite's M-[ 2 ~) past kg's own ESC window, turning the
# sequence into literal text.  A case that needs time for an asynchronous
# ladder buys it with a SETTLE=<sec>:<text> wait, which costs nothing on a
# fast build.  A pre-set environment value still wins, as everywhere else.
PTY_SETTLE_FLOOR=${PTY_SETTLE_FLOOR_VALGRIND:-3.0}
PTY_TIMEOUT=${PTY_TIMEOUT_VALGRIND:-90}
export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD PTY_SETTLE_FLOOR PTY_JOBS

"${MAKE_PARALLEL[@]}" -B check
