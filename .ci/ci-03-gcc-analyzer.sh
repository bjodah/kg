#!/bin/bash
# gcc's static analyzer, as a warning-free build of the whole editor, and
# both test layers at native speed on the binary it produced.  Running the
# suite under valgrind is a separate, expensive step (ci-15).
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc -Werror -fanalyzer"
export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD PTY_SETTLE_FLOOR PTY_JOBS

"${MAKE_PARALLEL[@]}" -B check
