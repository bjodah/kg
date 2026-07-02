#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

make coverage \
	CC="ccache gcc" \
	PTY_TIMEOUT="${PTY_TIMEOUT}" \
	PTY_STARTUP_DELAY_ADD="${PTY_STARTUP_DELAY_ADD}" \
	PTY_KEY_DELAY_ADD="${PTY_KEY_DELAY_ADD}"
