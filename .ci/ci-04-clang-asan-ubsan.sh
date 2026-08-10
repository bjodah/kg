#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
# KG_DEBUG_COORDS arms doc/coordinates.md's chars/render offset assertions
# (src/def.h) and KG_DEBUG_STATE the buffer/view invariants
# (src/bufhandle.h).  This is the lane to arm them in: it is already a
# debug build, and it drives the whole PTY suite, so an offset handed to
# the wrong coordinate space, or a window and a buffer table that stopped
# agreeing, aborts here rather than drawing something odd.
export CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g -DKG_DEBUG_COORDS=1 -DKG_DEBUG_STATE=1"

"${MAKE_PARALLEL[@]}" -B check
