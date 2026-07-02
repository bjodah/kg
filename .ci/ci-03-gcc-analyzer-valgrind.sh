#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

"${MAKE_PARALLEL[@]}" -B check \
	CC="ccache gcc -Werror -fanalyzer" \
	TEST_RUNNER="${VALGRIND}" \
	KG_RUNNER="${VALGRIND}" \
	PTY_TIMEOUT="${PTY_TIMEOUT}" \
	PTY_STARTUP_DELAY_ADD="${PTY_STARTUP_DELAY_ADD}" \
	PTY_KEY_DELAY_ADD="${PTY_KEY_DELAY_ADD}"
