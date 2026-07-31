#!/bin/bash
# Print the toolchain a CI run is actually using, and the machine it is
# using it on.  A quality gate that cannot say which compiler, analyzer or
# oracle produced its result is not evidence; this is the header every
# hosted job prints before it starts, and what a "works here, fails there"
# report should be diffed against.
#
# Missing tools are reported as missing rather than skipped silently, but
# this script never fails: it is a description of the box, and the steps
# themselves are what fail when something they need is absent.
# No pipefail: every value here is a `tool --version | head -1`, and the
# SIGPIPE that head sends the tool is not a failure to report.
set -u

cd "$(dirname "$0")/.."

show() {
	local name=$1
	shift
	local path
	path=$(command -v "${name}" 2>/dev/null)
	if [ -z "${path}" ]; then
		printf '%-20s MISSING\n' "${name}"
		return
	fi
	printf '%-20s %s\n' "${name}" "$("$@" 2>&1 | head -n 1)"
}

echo "=== machine ==="
printf '%-20s %s\n' "uname" "$(uname -srmo)"
printf '%-20s %s\n' "nproc" "$(nproc 2>/dev/null || echo '?')"
printf '%-20s %s\n' "libc" \
	"$(ldd --version 2>/dev/null | head -n 1)"
printf '%-20s %s\n' "target" \
	"$(${CC:-cc} -dumpmachine 2>/dev/null || echo 'unknown')"
if [ -r /etc/os-release ]; then
	# shellcheck disable=SC1091
	printf '%-20s %s\n' "os-release" \
		"$(. /etc/os-release && echo "${PRETTY_NAME}")"
fi
printf '%-20s %s\n' "container" "${KG_CI_IMAGE:-<unset>}"

echo ""
echo "=== git ==="
printf '%-20s %s\n' "kg" "$(git rev-parse HEAD 2>/dev/null)"
printf '%-20s %s\n' "fe" "$(git -C fe rev-parse HEAD 2>/dev/null)"
printf '%-20s %s\n' "tiny-regex-c" \
	"$(git -C fe/tiny-regex-c rev-parse HEAD 2>/dev/null)"

echo ""
echo "=== tools ==="
show make make --version
show "${CC:-cc}" "${CC:-cc}" --version
show gcc gcc --version
show clang clang --version
show ccache ccache --version
show valgrind valgrind --version
show scc scc --version
show pmccabe pmccabe -v
show lcov lcov --version
show genhtml genhtml --version
show gcov gcov --version
show clang-format clang-format --version
show clang-tidy clang-tidy --version
show clang-check clang-check --version
show cppcheck cppcheck --version
show bear bear --version
show parallel parallel --version
show tmux tmux -V
show emacs emacs --version
show python3 python3 --version
show cbmc cbmc --version
printf '%-20s %s\n' "iwyu" \
	"$("${IWYU:-include-what-you-use}" --version 2>&1 | head -n 1 ||
		echo MISSING)"
# The PTY harness needs these two, for whichever interpreter the Makefile
# picks -- which is not always python3.
for interpreter in python3 python; do
	command -v "${interpreter}" >/dev/null 2>&1 || continue
	printf '%-20s %s\n' "${interpreter} modules" \
		"$("${interpreter}" -c 'import pexpect, yaml; print("pexpect", pexpect.__version__, "PyYAML", yaml.__version__)' 2>&1 |
			tail -n 1)"
done

echo ""
echo "=== ci settings ==="
CI_PARALLEL=${CI_PARALLEL:-0} source .ci/ci-env.sh
printf '%-20s %s\n' "JOBS" "${JOBS}"
printf '%-20s %s\n' "CI_PARALLEL_LANES" "${CI_PARALLEL_LANES}"
printf '%-20s %s\n' "PTY_JOBS" "${PTY_JOBS}"
printf '%-20s %s\n' "PTY_TIMEOUT" "${PTY_TIMEOUT}"
printf '%-20s %s\n' "PTY_STARTUP_DELAY_ADD" "${PTY_STARTUP_DELAY_ADD}"
printf '%-20s %s\n' "PTY_KEY_DELAY_ADD" "${PTY_KEY_DELAY_ADD}"
