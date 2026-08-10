#!/bin/bash
# Install Oracle's nbcode -- the NetBeans-based Java language server from
# oracle/javavscode -- and put an `nbcode` on PATH, which is all kg's
# listen-hash recipe needs:
#
#   KG_LSP_SERVER_JAVA="listen-hash: nbcode --start-java-language-server=listen-hash:0"
#
# Two sources, one result.  By default the published VS Code extension is
# downloaded from open-vsx.org and the NetBeans runtime unpacked out of it
# (a .vsix is a zip, and `extension/nbcode` inside it is a complete build);
# --from-source DIR builds a javavscode checkout with Ant instead.  Either
# way the runtime ends up in <prefix>/share/nbcode and <prefix>/bin/nbcode
# is a wrapper that runs its launcher with a userdir of its own.
#
# Re-running is safe: the new tree is unpacked beside the old one and moved
# into place, so a failed download leaves the working install alone.
#
# What it does NOT promise: that the server is on your PATH (it says so and
# stops short of editing your shell profile), that a newer release than the
# one open-vsx reports exists, or that nbcode will make sense of your
# project -- that is nbcode's own business, and it says what it made of the
# tree in its userdir's var/log/messages.log.
set -euo pipefail

prog=$(basename "$0")

usage() {
	cat <<EOF
usage: $prog [--prefix DIR] [--version X.Y.Z] [--from-source DIR] [--dry-run]

  --prefix DIR       where to install (default: \$HOME/.local); the wrapper
                     lands in DIR/bin/nbcode and the runtime in
                     DIR/share/nbcode
  --version X.Y.Z    extension release to download instead of the newest
  --from-source DIR  build DIR (a javavscode checkout, with its netbeans
                     submodule fetched and \`ant apply-patches\` already run)
                     and install that, instead of downloading anything
  --dry-run          say what would happen; touch nothing

Requires Java 17 or newer and -- unless --from-source is used -- curl and
unzip.  --from-source additionally needs ant, maven and the checkout's
netbeans submodule, and takes tens of minutes.
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
# javavscode's own BUILD.md and README both say JDK 17 or later; find that
# out here, where the message can name what to install, rather than as a
# stack trace out of a language server kg spawned in the background.
java_bin=java
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
	java_bin=$JAVA_HOME/bin/java
fi
command -v "$java_bin" >/dev/null 2>&1 || die "no java found (set JAVA_HOME or install a JDK 17+)"
java_version=$("$java_bin" -version 2>&1 | head -1 | sed -n 's/.*version "\([0-9]*\).*/\1/p')
[ -n "$java_version" ] || die "could not read a version out of \`$java_bin -version\`"
[ "$java_version" -ge 17 ] || die "nbcode needs Java 17 or newer; \`$java_bin\` is $java_version"

# ------------------------------------------------------------- sources ---
# open-vsx.org is the registry that serves the extension without a
# Microsoft account, and its API answers with the download URL of a
# release.  Only two fields are read out of that JSON -- the version and
# nothing else -- so a `sed` is enough and no JSON parser is a dependency.
vsx_api=https://open-vsx.org/api/Oracle/oracle-java

discover_version() {
	local json
	json=$(curl -fsS "$vsx_api/latest" 2>/dev/null || true)
	[ -n "$json" ] || die "could not reach $vsx_api/latest; pass --version"
	echo "$json" | tr ',' '\n' \
		| sed -n 's/.*"version":"\([0-9][0-9.]*\)".*/\1/p' | head -1
}

# ------------------------------------------------------------- install ---
share=$prefix/share/nbcode
bindir=$prefix/bin
staging=

cleanup() {
	if [ -n "$staging" ]; then
		rm -rf "$staging"
	fi
}
trap cleanup EXIT

install_tree() {
	# $1 is a directory holding a built nbcode (bin/nbcode.sh, platform/,
	# java/, etc/).  Swap it in, keeping the old one until the new one is
	# in place.
	local src=$1
	[ -f "$src/bin/nbcode.sh" ] || die "$src has no bin/nbcode.sh launcher"
	[ -d "$src/platform" ] || die "$src does not look like an nbcode runtime (no platform/)"
	# A .vsix is a zip, and zip entries carry no execute bit that unzip
	# will honour here, so the three scripts that are actually run have to
	# be made executable -- which is exactly what the VS Code extension
	# does to its own copy on every start (vscode/src/lsp/utils.ts).
	chmod +x "$src/bin/nbcode.sh" "$src/platform/lib/nbexec.sh" 2>/dev/null || true
	[ -f "$src/java/maven/bin/mvn.sh" ] && chmod +x "$src/java/maven/bin/mvn.sh"
	mkdir -p "$prefix/share" "$bindir"
	rm -rf "$share.old"
	if [ -d "$share" ]; then
		mv "$share" "$share.old"
	fi
	mv "$src" "$share"
	rm -rf "$share.old"
	write_wrapper
}

write_wrapper() {
	# Why the wrapper does anything at all beyond naming the launcher:
	# NetBeans' single-instance handler is keyed on the userdir.  Two
	# nbcode processes pointed at one userdir do not both start -- the
	# second finds the first's lock file, hands its command line over and
	# exits without printing a port, which a client sees as a server that
	# spawned and then said nothing.  So each run gets a userdir of its
	# own, named after this shell's pid, which the exec below makes
	# nbcode's pid too; dirs whose pid is gone are swept on the way in.
	# Pinning KG_NBCODE_USERDIR opts out and keeps the index warm between
	# sessions, at the price of only ever running one nbcode at a time.
	cat > "$bindir/nbcode" <<EOF
#!/bin/sh
# Installed by kg's utils/install-nbcode.sh.  Runs the NetBeans launcher in
# $share and gives it a userdir that no other nbcode is using.
share=$share
EOF
	cat >> "$bindir/nbcode" <<'EOF'
if [ -n "${KG_NBCODE_USERDIR:-}" ]; then
	userdir=$KG_NBCODE_USERDIR
else
	root=${TMPDIR:-/tmp}/kg-nbcode-$(id -u)
	mkdir -p "$root"
	for d in "$root"/ud-*; do
		[ -d "$d" ] || continue
		pid=${d##*/ud-}
		case $pid in *[!0-9]*) continue ;; esac
		kill -0 "$pid" 2>/dev/null || rm -rf "$d"
	done
	userdir=$root/ud-$$
fi
exec "$share/bin/nbcode.sh" --userdir "$userdir" "$@"
EOF
	chmod +x "$bindir/nbcode"
}

if [ -n "$from_source" ]; then
	[ -f "$from_source/build.xml" ] || die "$from_source has no build.xml; is it a javavscode checkout?"
	[ -n "$(ls -A "$from_source/netbeans" 2>/dev/null)" ] \
		|| die "$from_source/netbeans is empty; run: git -C $from_source submodule update --init netbeans && ant apply-patches"
	built=$from_source/vscode/nbcode
	say "building $from_source with ant (tens of minutes, and it downloads a lot)"
	if [ "$dry_run" = 1 ]; then
		say "would run: (cd $from_source && ant build-netbeans && ant build-lsp-server)"
		say "would install $built -> $share and write $bindir/nbcode"
		exit 0
	fi
	need ant
	# `ant apply-patches` is the caller's job and is said so above: it
	# patches the netbeans submodule's working tree with git, and running
	# it twice over an already-patched tree fails rather than doing
	# nothing.
	( cd "$from_source" && ant build-netbeans && ant build-lsp-server ) \
		|| die "the Ant build failed; its output above is the reason"
	[ -d "$built" ] || die "the build succeeded but $built is missing"
	staging=$(mktemp -d)
	cp -a "$built" "$staging/tree"
	install_tree "$staging/tree"
	staging=
else
	need curl
	need unzip
	[ -n "$version" ] || version=$(discover_version)
	[ -n "$version" ] || die "could not discover a version; pass --version"
	url=$vsx_api/$version/file/Oracle.oracle-java-$version.vsix
	if [ "$dry_run" = 1 ]; then
		say "would download $url"
		say "would install its extension/nbcode into $share and write $bindir/nbcode"
		exit 0
	fi
	say "downloading the oracle-java extension $version (~150 MB)"
	staging=$(mktemp -d)
	curl -fsSL -o "$staging/ext.vsix" "$url" \
		|| die "download failed: $url"
	unzip -q -o "$staging/ext.vsix" 'extension/nbcode/*' -d "$staging/x" \
		|| die "could not unpack $staging/ext.vsix"
	[ -d "$staging/x/extension/nbcode" ] \
		|| die "$url has no extension/nbcode in it"
	mv "$staging/x/extension/nbcode" "$staging/tree"
	install_tree "$staging/tree"
	staging=
fi

# --------------------------------------------------------------- proof ---
# An install that cannot be started is not an install.  --help is the one
# thing the launcher answers without building a userdir -- and what is
# checked is its output, not its exit status, which is 2.  The output is
# captured rather than piped into a `grep -q`, which would exit at the
# first match, leave the launcher writing into a closed pipe, and fail the
# whole check on the SIGPIPE that `pipefail` then reports.
help_out=$("$bindir/nbcode" --help 2>&1 || true)
case $help_out in
*--jdkhome*) ;;
*) die "$bindir/nbcode was written but does not run; try: $bindir/nbcode --help" ;;
esac
say "installed: $bindir/nbcode -> $share"

case ":$PATH:" in
*":$bindir:"*) say "$bindir is on PATH.  Point kg at it with:
	export KG_LSP_SERVER_JAVA=\"listen-hash: nbcode --start-java-language-server=listen-hash:0\"" ;;
*) say "note: $bindir is NOT on your PATH.  Either add it, or name the
	wrapper in full:
	export KG_LSP_SERVER_JAVA=\"listen-hash: $bindir/nbcode --start-java-language-server=listen-hash:0\"" ;;
esac
