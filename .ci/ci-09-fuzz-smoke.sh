#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

make clean
make fuzz-smoke
