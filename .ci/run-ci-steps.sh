#!/bin/bash
set -euxo pipefail
source $(compgen -G "/opt-?/cpython-v3.*-apt-deb/bin/activate")
env CC="ccache gcc" make -B check 
env CC="ccache clang" CFLAGS="-fsanitize=address" make -B check

# TODO: run with valgrind? run with MSAN?
