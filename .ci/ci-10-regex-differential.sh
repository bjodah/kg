#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

# Randomised, but seeded: the same cases every run, so a failure here is
# reproducible with the printed seed.  Skips itself when emacs is missing.
"${MAKE_PARALLEL[@]}" check-regex-differential
