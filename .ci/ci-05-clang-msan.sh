#!/bin/bash
# This lane stays WITH_TREE_SITTER=0 by policy, not by omission: MSan reports
# uninstrumented code as uninitialised, and the tree-sitter core and its
# grammars are prebuilt prefixes nothing here rebuilds, so a tree-sitter
# build under MSan is guaranteed false positives
# (doc/plans/kg-tree-sitter-plan.md, Refinement, "Sanitizer lanes").  ASan
# and valgrind have no such problem and .ci/ci-13-with-tree-sitter.sh is
# where the configuration is exercised.
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-memory-param-retval -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"

"${MAKE_PARALLEL[@]}" -B check
