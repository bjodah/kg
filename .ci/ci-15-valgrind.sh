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
export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD PTY_SETTLE_FLOOR PTY_JOBS

"${MAKE_PARALLEL[@]}" -B check
