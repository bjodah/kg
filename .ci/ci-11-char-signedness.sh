#!/bin/bash
# Whether a plain `char` is signed is implementation-defined, and kg is
# built for both kinds of host.  It used to matter: display.c tested
# `c[j] <= 26` on a char, so on a signed-char target byte 0xDB passed and
# rendered as a raw ESC, and syntax.c asked isprint() about a negative
# char, which is undefined behaviour and in the "C" locale answered "not
# printable" for every UTF-8 byte.  Neither is true any more, and this
# step is what keeps it that way: the unit suite has to agree byte for
# byte under both signednesses, on input that contains UTF-8, ESC and DEL.
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

BASE_CFLAGS="-Wall -W -pedantic -Os -std=c23"

run_suite() {
	make clean
	make -j"${JOBS}" check-unit CFLAGS="${BASE_CFLAGS} $1" \
		FE_CFLAGS="${BASE_CFLAGS} $1"
}

run_suite -fsigned-char
run_suite -funsigned-char
make clean
