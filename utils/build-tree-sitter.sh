#!/bin/bash
# Build the tree-sitter install described by utils/tree-sitter-pins into a
# cached prefix, and print that prefix's path on stdout.
#
# This exists for one caller: .ci/ci-13-with-tree-sitter.sh on a box with no
# tree-sitter install, which is every hosted runner.  The developer box has
# one and never reaches here.  The intended END state is that the CI image
# ships a prefix too and this script stops firing there as well -- the
# resolution order in ci-13 is written that way on purpose (an environment
# variable beats the cache, and the cache beats a build), so putting the
# prefix in the image is a one-variable change and no code change.
#
# What it builds is a prefix in the shape the Makefile's TREE_SITTER_PREFIX
# and TS_GRAMMAR_PATH expect:
#
#   <prefix>/include/tree_sitter/api.h   the core's headers (the guard file)
#   <prefix>/lib/libtree-sitter.so*      the core, linked at build time
#   <prefix>/lib/libtree-sitter-<n>.so   the grammars, dlopen'd at run time
#
# Grammars are compiled straight from each repository's CHECKED-IN
# src/parser.c (plus src/scanner.c or scanner.cc where there is one) with an
# ordinary compiler.  The tree-sitter CLI -- a Rust program with a cargo
# build in front of it -- is deliberately not a dependency: it is only needed
# to REGENERATE parser.c from grammar.js, which a pinned tag has already
# done.
#
# Usage:
#   utils/build-tree-sitter.sh [--prefix DIR] [--pins FILE] [--jobs N]
#                              [--force] [--print-prefix]
#
#   --print-prefix  compute the cache path and print it, building nothing.
#   --force         rebuild even when the cache entry is complete.
#
# Environment: KG_TS_CACHE (cache root), CC, CXX, JOBS.
set -euo pipefail

cd "$(dirname "$0")/.."
repo_root=$(pwd)

pins=utils/tree-sitter-pins
prefix=
jobs=${JOBS:-$(nproc 2>/dev/null || echo 2)}
force=0
print_only=0

while [ $# -gt 0 ]; do
	case "$1" in
	--prefix) prefix=$2; shift 2 ;;
	--pins) pins=$2; shift 2 ;;
	--jobs) jobs=$2; shift 2 ;;
	--force) force=1; shift ;;
	--print-prefix) print_only=1; shift ;;
	-h | --help) sed -n '2,30p' "$0"; exit 0 ;;
	*) echo "$(basename "$0"): unknown argument: $1" >&2; exit 2 ;;
	esac
done

# Everything this script says goes to stderr; stdout carries the prefix path
# and nothing else, so the caller can read it with $(...) while its progress
# still reaches the CI log in real time.
say() { echo "build-tree-sitter: $*" >&2; }
die() { echo "build-tree-sitter: error: $*" >&2; exit 1; }

[ -f "${pins}" ] || die "no pin manifest at ${pins}"

# A grammar is C (and, for a few external scanners, C++) and cares about
# nothing else, so any compiler on the box will do -- but not every box
# spells one `cc`, so the fallbacks are searched rather than assumed.  The
# C++ driver is only reached by a grammar that has a scanner.cc.
pick_tool() {
	local t

	for t in "$@"; do
		if command -v "${t}" >/dev/null 2>&1; then
			echo "${t}"
			return 0
		fi
	done
	return 1
}
CC=${CC:-$(pick_tool cc gcc clang || true)}
CXX=${CXX:-$(pick_tool c++ g++ clang++ || true)}
[ -n "${CC}" ] || die "no C compiler found (tried cc, gcc, clang; set CC=)"

# The cache key is the pin file's own hash, so a pin change cannot be served
# a stale prefix -- there is no invalidation step to forget.  The core tag
# rides in front of the hash for the benefit of whoever reads `ls`.
pins_hash=$(sha256sum "${pins}" | cut -c1-12)
core_tag=$(awk '$1 == "core" { print $3; exit }' "${pins}")
[ -n "${core_tag}" ] || die "${pins} names no core"
cache_root=${KG_TS_CACHE:-${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/kg-tree-sitter}
[ -n "${prefix}" ] || prefix=${cache_root}/tree-sitter-${core_tag}-${pins_hash}

# A prefix is complete when this file is in it, and only then.  A build that
# dies half way never becomes a cache entry at all -- it works in a staging
# directory the exit trap removes -- so no half-built prefix can be mistaken
# for a cached one, whichever way it died.
stamp=${prefix}/.kg-tree-sitter-complete

if [ "${print_only}" = 1 ]; then
	echo "${prefix}"
	exit 0
fi

if [ "${force}" = 0 ] && [ -f "${stamp}" ] &&
   [ -e "${prefix}/include/tree_sitter/api.h" ]; then
	say "cache hit: ${prefix}"
	echo "${prefix}"
	exit 0
fi

say "building ${core_tag} + $(awk '$1 == "grammar"' "${pins}" | wc -l)" \
    "grammars into ${prefix}"

# Staged, then renamed into place: a reader (another CI lane, a developer's
# make) only ever sees a prefix that is finished.  mkdtemp-style names keep
# two concurrent builders out of each other's way; the loser of the race
# throws its work away below rather than clobbering the winner's.
mkdir -p "${cache_root}"
stage=$(mktemp -d "${cache_root}/.stage-XXXXXX")
work=$(mktemp -d "${cache_root}/.work-XXXXXX")
cleanup() { rm -rf "${stage}" "${work}"; }
trap cleanup EXIT

# One clone, and the one error message that matters.  A hosted runner that
# cannot reach the pin's host is a runner that cannot build tree-sitter, and
# that is a hard failure with a named cause rather than a skip: a lane that
# silently stops testing the tree-sitter backend is how the backend rots.
clone() {
	local url=$1 tag=$2 dest=$3

	if ! git clone --quiet --depth 1 --branch "${tag}" \
	     "${url}" "${dest}" >/dev/null 2>"${work}/clone.err"; then
		sed 's/^/  git: /' "${work}/clone.err" >&2
		say "could not clone ${url} at ${tag}."
		say "a box with no network, or no route to that host, cannot"
		say "build a tree-sitter prefix.  Point TREE_SITTER_PREFIX at"
		say "a prebuilt install (the CI image is the right home for"
		say "one) or drop WITH_TREE_SITTER=1 from this run."
		die "clone failed: ${url} ${tag}"
	fi
}

# ---- the core ----------------------------------------------------------
#
# tree-sitter's own Makefile builds and installs libtree-sitter and its
# headers from PREFIX alone; nothing here needs cargo, and the CLI it would
# build is not part of the install kg links against.
clone "$(awk '$1 == "core" { print $2; exit }' "${pins}")" "${core_tag}" \
      "${work}/tree-sitter"
say "core: building libtree-sitter ${core_tag}"
make -C "${work}/tree-sitter" -j"${jobs}" >/dev/null
make -C "${work}/tree-sitter" install PREFIX="${stage}" >/dev/null
[ -e "${stage}/include/tree_sitter/api.h" ] ||
	die "core install left no ${stage}/include/tree_sitter/api.h"

# ---- the grammars ------------------------------------------------------
#
# A generated parser.c is a single enormous translation unit, and -O2 on the
# larger ones (typescript, tsx) costs more than the rest of this script put
# together for no run-time gain worth having: the table-driven inner loop is
# the core's, not the parser's.  -O1 is the compromise the Emacs
# treesit-install-language-grammar path also lands on by default.
grammar_cflags="-O1 -fPIC"

build_grammar() {
	local name=$1 url=$2 tag=$3 subdir=${4:-.}
	local repo=${work}/grammar-${name} src objs=() log=${work}/${name}.log

	clone "${url}" "${tag}" "${repo}"
	src=${repo}/${subdir}/src
	[ -f "${src}/parser.c" ] ||
		die "${name}: ${url} ${tag} ships no ${subdir}/src/parser.c"

	# -I<src> is what makes the checked-in tree_sitter/parser.h (and the
	# alloc.h/array.h the newer external scanners include) resolve; a
	# grammar repository is self-contained by design.
	${CC} ${grammar_cflags} -I"${src}" -c "${src}/parser.c" \
	      -o "${repo}/parser.o" >"${log}" 2>&1 ||
		{ cat "${log}" >&2; die "${name}: parser.c did not compile"; }
	objs=("${repo}/parser.o")

	# An external scanner is C in almost every grammar and C++ in a few;
	# the C++ ones have to be linked by the C++ driver so libstdc++ comes
	# with them.
	local link=${CC}
	if [ -f "${src}/scanner.c" ]; then
		${CC} ${grammar_cflags} -I"${src}" -c "${src}/scanner.c" \
		      -o "${repo}/scanner.o" >"${log}" 2>&1 ||
			{ cat "${log}" >&2
			  die "${name}: scanner.c did not compile"; }
		objs+=("${repo}/scanner.o")
	elif [ -f "${src}/scanner.cc" ]; then
		${CXX} ${grammar_cflags} -I"${src}" -c "${src}/scanner.cc" \
		       -o "${repo}/scanner.o" >"${log}" 2>&1 ||
			{ cat "${log}" >&2
			  die "${name}: scanner.cc did not compile"; }
		objs+=("${repo}/scanner.o")
		link=${CXX}
	fi

	# Emacs' soname, which is the only name kg's loader ever asks for.
	${link} -shared -o "${stage}/lib/libtree-sitter-${name}.so" \
	        "${objs[@]}" >"${log}" 2>&1 ||
		{ cat "${log}" >&2; die "${name}: link failed"; }
	say "grammar: ${name} ${tag}$([ "${subdir}" = . ] || echo " (${subdir})")"
}

mkdir -p "${stage}/lib"
# Sequential on purpose.  The whole grammar phase is a handful of seconds
# per grammar; running them concurrently would interleave their diagnostics
# for a saving that disappears next to the core build and the two full test
# suites this lane runs afterwards.
while read -r kind name url tag subdir; do
	[ "${kind}" = grammar ] || continue
	build_grammar "${name}" "${url}" "${tag}" "${subdir:-.}"
done < <(sed 's/#.*//' "${pins}" | awk 'NF')

# ---- publish -----------------------------------------------------------
: >"${stage}/.kg-tree-sitter-complete"
if [ "${force}" = 1 ]; then
	rm -rf "${prefix}"
fi
if ! mv "${stage}" "${prefix}" 2>/dev/null; then
	# Either another builder published first (fine, its content is ours
	# by construction: same pins, same tags) or the rename genuinely
	# failed, which the existence check below reports.
	rm -rf "${stage}"
fi
[ -f "${stamp}" ] || die "could not publish the build to ${prefix}"

say "built ${prefix}"
echo "${prefix}"
