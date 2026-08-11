#!/bin/bash
# Install Eclipse JDT Language Server and put a `jdtls` on PATH, which is
# all kg's Java LSP support needs: kg spawns the bare name `jdtls` (or
# whatever KG_LSP_SERVER_JAVA names) and nothing else about the server is
# kg's business.
#
# Two sources, one result.  By default a milestone tarball is downloaded
# from download.eclipse.org; --from-source DIR builds the checkout in DIR
# with its own Maven wrapper instead.  Either way the server ends up in
# <prefix>/share/jdtls and <prefix>/bin/jdtls is a wrapper that runs its
# launcher with this box's python3.
#
# Re-running is safe: the new tree is unpacked beside the old one and moved
# into place, so a failed download leaves the working install alone.
#
# What it does NOT promise: that the server is on your PATH (it says so and
# stops short of editing your shell profile), that a milestone newer than
# the one it found exists, or that jdt.ls will import your project -- that
# is jdt.ls's own business, and its log lives in the -data directory its
# launcher picks.
set -euo pipefail

prog=$(basename "$0")

usage() {
	cat <<EOF
usage: $prog [--prefix DIR] [--version X.Y.Z] [--from-source DIR] [--dry-run]

  --prefix DIR       where to install (default: \$HOME/.local); the launcher
                     lands in DIR/bin/jdtls and the server in DIR/share/jdtls
  --version X.Y.Z    milestone to download instead of the newest one found
  --from-source DIR  build DIR (an eclipse.jdt.ls checkout) with its mvnw
                     and install that, instead of downloading anything
  --dry-run          say what would happen; touch nothing

Requires Java 21 or newer, python3, and -- unless --from-source is used --
curl and tar.
EOF
}

prefix=${HOME:-/root}/.local
version=
from_source=
dry_run=0

while [ $# -gt 0 ]; do
	case $1 in
	--prefix) prefix=$2; shift 2 ;;
	--prefix=*) prefix=${1#*=}; shift ;;
	--version) version=$2; shift 2 ;;
	--version=*) version=${1#*=}; shift ;;
	--from-source) from_source=$2; shift 2 ;;
	--from-source=*) from_source=${1#*=}; shift ;;
	--dry-run) dry_run=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) echo "$prog: unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

die() { echo "$prog: $*" >&2; exit 1; }
say() { echo "$prog: $*"; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 is required and was not found on PATH"; }

# ---------------------------------------------------------------- java ---
# jdt.ls refuses to start below Java 21 and says so; find that out here,
# where the message can name what to install, rather than as a stack trace
# out of a language server kg spawned in the background.
java_bin=java
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
	java_bin=$JAVA_HOME/bin/java
fi
command -v "$java_bin" >/dev/null 2>&1 || die "no java found (set JAVA_HOME or install a JDK 21+)"
java_version=$("$java_bin" -version 2>&1 | head -1 | sed -n 's/.*version "\([0-9]*\).*/\1/p')
[ -n "$java_version" ] || die "could not read a version out of \`$java_bin -version\`"
[ "$java_version" -ge 21 ] || die "jdtls needs Java 21 or newer; \`$java_bin\` is $java_version"
need python3

# ------------------------------------------------------------- sources ---
# The one machine-readable pointer download.eclipse.org publishes is
# snapshots/latest.txt, whose file name carries the version the next
# milestone will have; milestones/<version>/latest.txt then names the
# tarball.  So: take that version, and if it has not been cut as a milestone
# yet, step the minor down until one answers.  Deliberately dumb -- there is
# no index to parse and no JSON API to depend on.
milestone_url=https://download.eclipse.org/jdtls/milestones
snapshot_url=https://download.eclipse.org/jdtls/snapshots

discover_version() {
	local newest minor major try i
	newest=$(curl -fsS "$snapshot_url/latest.txt" 2>/dev/null || true)
	newest=$(echo "$newest" | sed -n 's/^jdt-language-server-\([0-9]*\.[0-9]*\.[0-9]*\)-.*/\1/p')
	[ -n "$newest" ] || die "could not read a version from $snapshot_url/latest.txt; pass --version"
	major=${newest%%.*}
	minor=$(echo "$newest" | cut -d. -f2)
	for ((i = 0; i < 10 && minor >= 0; i++)); do
		try="$major.$minor.0"
		if curl -fsS -o /dev/null "$milestone_url/$try/latest.txt" 2>/dev/null; then
			echo "$try"
			return 0
		fi
		minor=$((minor - 1))
	done
	die "no milestone found at or below $newest; pass --version"
}

# ------------------------------------------------------------- install ---
share=$prefix/share/jdtls
bindir=$prefix/bin
staging=

cleanup() {
	if [ -n "$staging" ]; then
		rm -rf "$staging"
	fi
}
trap cleanup EXIT

install_tree() {
	# $1 is a directory holding a built server (plugins/, config_linux/,
	# bin/jdtls).  Swap it in, keeping the old one until the new one is
	# in place.
	local src=$1
	[ -d "$src/plugins" ] || die "$src does not look like a jdtls install (no plugins/)"
	[ -f "$src/bin/jdtls" ] || die "$src has no bin/jdtls launcher"
	mkdir -p "$prefix/share" "$bindir"
	rm -rf "$share.old"
	if [ -d "$share" ]; then
		mv "$share" "$share.old"
	fi
	mv "$src" "$share"
	rm -rf "$share.old"
	# bin/jdtls is the entry point and bin/jdtls.py the module it loads,
	# so the wrapper must run the former; the latter defines main() and
	# calls nothing, which is a launcher that exits silently.
	cat > "$bindir/jdtls" <<EOF
#!/bin/sh
# Installed by kg's utils/install-jdtls.sh.  The launcher is a Python
# script; this wrapper only says which python3 runs it and where it lives.
exec python3 "$share/bin/jdtls" "\$@"
EOF
	chmod +x "$bindir/jdtls"
}

if [ -n "$from_source" ]; then
	[ -x "$from_source/mvnw" ] || die "$from_source has no mvnw; is it an eclipse.jdt.ls checkout?"
	repo=$from_source/org.eclipse.jdt.ls.product/target/repository
	say "building $from_source with its Maven wrapper (several minutes, and it downloads a lot)"
	if [ "$dry_run" = 1 ]; then
		say "would run: (cd $from_source && ./mvnw clean verify -DskipTests=true)"
		say "would install $repo -> $share and write $bindir/jdtls"
		exit 0
	fi
	# JAVA_HOME is passed through when it is set and otherwise left alone:
	# the Maven wrapper finds a java on PATH by itself, and guessing a
	# JAVA_HOME from `which java` is how a build ends up using a JDK the
	# user did not choose.
	( cd "$from_source" && ./mvnw clean verify -DskipTests=true ) \
		|| die "the Maven build failed; its output above is the reason"
	[ -d "$repo" ] || die "the build succeeded but $repo is missing"
	staging=$(mktemp -d)
	cp -a "$repo" "$staging/tree"
	install_tree "$staging/tree"
	staging=
else
	need curl
	need tar
	[ -n "$version" ] || version=$(discover_version)
	tarball=$(curl -fsS "$milestone_url/$version/latest.txt" 2>/dev/null || true)
	case $tarball in
	jdt-language-server-*.tar.gz) ;;
	*) die "milestone $version has no usable latest.txt (got: ${tarball:-nothing})" ;;
	esac
	if [ "$dry_run" = 1 ]; then
		say "would download $milestone_url/$version/$tarball"
		say "would install it into $share and write $bindir/jdtls"
		exit 0
	fi
	say "downloading $tarball (milestone $version)"
	staging=$(mktemp -d)
	curl -fsSL -o "$staging/jdtls.tar.gz" "$milestone_url/$version/$tarball" \
		|| die "download failed: $milestone_url/$version/$tarball"
	mkdir -p "$staging/tree"
	# --no-same-owner: the archive carries the build machine's uid, and
	# unpacking as root would otherwise fail on every entry.
	tar xzf "$staging/jdtls.tar.gz" --no-same-owner -C "$staging/tree" \
		|| die "could not unpack $staging/jdtls.tar.gz"
	install_tree "$staging/tree"
	staging=
fi

# --------------------------------------------------------------- proof ---
# An install that cannot be started is not an install.  --help is the one
# thing the launcher answers without starting a JVM workspace -- and what
# is checked is its output, not its exit status: the wrong entry point in
# this tree exits 0 having done nothing at all.  The output is captured
# rather than piped into a `grep -q`, which would exit at the first match,
# leave the launcher writing into a closed pipe, and fail the whole check
# on the SIGPIPE that `pipefail` then reports.
help_out=$("$bindir/jdtls" --help 2>/dev/null || true)
case $help_out in
*-data*) ;;
*) die "$bindir/jdtls was written but does not run; try: $bindir/jdtls --help" ;;
esac
say "installed: $bindir/jdtls -> $share"

case ":$PATH:" in
*":$bindir:"*) say "$bindir is on PATH; kg will find jdtls" ;;
*) say "note: $bindir is NOT on your PATH.  Either add it, or point kg at
	the launcher directly:  export KG_LSP_SERVER_JAVA=$bindir/jdtls" ;;
esac
