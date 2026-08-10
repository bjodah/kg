#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

compile_db_files() {
	python3 - <<'PY'
import json
with open("compile_commands.json", "r", encoding="utf-8") as fp:
    for entry in json.load(fp):
        print(entry["file"])
PY
}

run_clang_check() {
	local file=$1
	local out

	cd "$(dirname "${file}")"
	out=$(mktemp)
	trap 'rm -f "${out}"' RETURN
	# A job shell inherits no shell options, so the tool's own exit
	# status has to be looked at here: a clang-check that is missing or
	# that dies would otherwise print nothing, match no "warning:", and
	# report the file clean.
	if ! clang-check -analyze -p "${COMPILE_DB}" "$(basename "${file}")" \
		>"${out}" 2>&1; then
		cat "${out}"
		exit 1
	fi
	cat "${out}"
	if grep -q "warning:" "${out}"; then
		exit 1
	fi
}

run_clang_tidy() {
	local file=$1
	local out

	out=$(mktemp)
	trap 'rm -f "${out}"' RETURN
	if ! clang-tidy --quiet -p "${COMPILE_DB}" "${file}" >"${out}" 2>&1; then
		cat "${out}"
		exit 1
	fi
}

# Run one analyser over every file in the compilation database, and insist
# that it really ran over every one of them.  GNU parallel exits 0 when it
# runs no job at all, so an empty database, a database the build failed to
# record, or a mis-parsed command line all read as a fast pass otherwise --
# which is exactly how the $PARALLEL name collision (see .ci/ci-env.sh) hid
# both phases of this step for as long as it did.
analyse_db_files() {
	local fn=$1 list joblog expected ran

	list=$(compile_db_files)
	expected=$(printf '%s\n' "${list}" | grep -c . || true)
	joblog=$(mktemp)
	printf '%s\n' "${list}" | "${GNU_PARALLEL}" --halt soon,fail=1 \
		--jobs "${JOBS}" --line-buffer --joblog "${joblog}" "${fn}"
	ran=$(grep -c . "${joblog}" || true)
	rm -f "${joblog}"
	ran=$((ran - 1)) # the job log's header line
	if [ "${expected}" -lt 1 ] || [ "${ran}" -ne "${expected}" ]; then
		echo "${fn}: analysed ${ran} of ${expected} files" >&2
		exit 1
	fi
}

export -f run_clang_check run_clang_tidy
export CC="ccache clang"

# https://btorpey.github.io/blog/2015/04/27/static-analysis-with-clang/
bear -- "${MAKE_PARALLEL[@]}" -B
export COMPILE_DB=$(/bin/pwd)
make iwyu

analyse_db_files run_clang_check

cppcheck --quiet --error-exitcode=1 --suppress=normalCheckLevelMaxBranches -j "${JOBS}" src/*.c

analyse_db_files run_clang_tidy
