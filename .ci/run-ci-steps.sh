#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

usage() {
	cat <<'EOF'
usage: run-ci-steps.sh [--parallel] [--expensive]
       run-ci-steps.sh --status

Without arguments the numbered .ci/ci-NN-*.sh steps run one after another in
this working tree, streaming to the terminal, stopping at the first failure.

  --parallel   Run the steps concurrently.  Every step that builds gets a
               throwaway copy of this working tree, so the copies cannot
               clobber each other's objects, and every step writes its own
               log file.  The terminal only gets a PASS/FAIL line per step
               plus a replay of the failing logs.
  --expensive  Also run the expensive steps, which both run modes otherwise
               report as SKIP.  CI_EXPENSIVE=1 in the environment does the
               same thing; hosted CI sets it.  Running an expensive step's
               script directly always runs it -- the gate is here, not there.
  --status     Print what the runner in this tree is doing (or did last):
               per-step state, timings and log paths.  Never blocks, never
               starts anything, safe to poll.

Both run modes take a lock in .ci/.run; a second run in the same tree is
refused because the two would fight over src/*.o.  A lock whose process is
gone is reported as stale and taken over.

Environment (see .ci/ci-env.sh): CI_EXPENSIVE=1 includes the expensive steps,
CI_PARALLEL_LANES caps how many steps run at once, CI_RUN_DIR moves the run
state, CI_LANE_ROOT picks where lane trees are put, JOBS and the PTY_* knobs
keep overriding the defaults.
EOF
}

parallel_mode=0
status_only=0
expensive=${CI_EXPENSIVE:-0}
while [ "$#" -gt 0 ]; do
	case "$1" in
	--parallel)
		parallel_mode=1
		;;
	--expensive)
		expensive=1
		;;
	--status)
		status_only=1
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "run-ci-steps.sh: unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
	shift
done

export CI_PARALLEL="${parallel_mode}"
source .ci/ci-env.sh

steps=(.ci/ci-[0-9][0-9]-*.sh)
tree_root=$(/bin/pwd)
case "${CI_RUN_DIR}" in
/*) ;;
*) CI_RUN_DIR="${tree_root}/${CI_RUN_DIR}" ;;
esac
lock_file="${CI_RUN_DIR}/lock"

step_name() {
	local base=${1##*/}

	echo "${base%.sh}"
}

# ci-01 and ci-07 only read src/*.[ch]; they build nothing and can share this
# working tree.  Every other step does a `-B` rebuild with its own CC/CFLAGS,
# and ci-08/ci-09 also run `make clean`, so those each need a tree of their own.
in_tree_steps=(ci-01-complexity ci-07-format-check)

# Steps that cost minutes rather than seconds.  Both run modes report them as
# SKIP unless --expensive or CI_EXPENSIVE=1 asks for them; hosted CI does.
expensive_steps=(ci-15-valgrind)

skipped_step() {
	local name=$1

	if [ "${expensive}" = 1 ]; then
		return 1
	fi
	case " ${expensive_steps[*]} " in
	*" ${name} "*) return 0 ;;
	esac
	return 1
}

lane_dir_for() {
	local name=$1

	if [ "${parallel_mode}" -eq 0 ] || skipped_step "${name}"; then
		return 0
	fi
	case " ${in_tree_steps[*]} " in
	*" ${name} "*) return 0 ;;
	esac
	echo "${lane_root}/${name}"
}

log_for() {
	if [ "${parallel_mode}" -eq 0 ] || skipped_step "$1"; then
		return 0
	fi
	echo "${CI_RUN_DIR}/logs/$1.log"
}

now() {
	date +%s.%N
}

# The run state lives in the real tree (a lane copy is thrown away) and
# survives the run, so `--status` still explains what happened afterwards.
state_set() {
	local name=$1 st=$2 start=$3 end=$4 rc=$5
	local file="${CI_RUN_DIR}/${name}.state" elapsed=""

	if [ -n "${start}" ] && [ -n "${end}" ]; then
		elapsed=$(awk -v a="${start}" -v b="${end}" \
			'BEGIN { printf "%.1f", b - a }')
	fi
	printf 'step=%s\nstate=%s\nstart=%s\nend=%s\nelapsed=%s\nexit=%s\nlog=%s\nlane=%s\n' \
		"${name}" "${st}" "${start}" "${end}" "${elapsed}" "${rc}" \
		"$(log_for "${name}")" "$(lane_dir_for "${name}")" \
		>"${file}.tmp"
	mv "${file}.tmp" "${file}"
}

state_field() {
	sed -n "s/^$2=//p" "$1" 2>/dev/null || true
}

human_time() {
	local stamp=$1

	if [ -z "${stamp}" ]; then
		echo "-"
	else
		date -d "@${stamp}" +%Y-%m-%dT%H:%M:%S
	fi
}

print_status() {
	local lock_pid lock_mode lock_start file name st start end elapsed log

	echo "run state: ${CI_RUN_DIR}"
	if [ -f "${lock_file}" ]; then
		lock_pid=$(state_field "${lock_file}" pid)
		lock_mode=$(state_field "${lock_file}" mode)
		lock_start=$(state_field "${lock_file}" start)
		if kill -0 "${lock_pid}" 2>/dev/null; then
			printf 'runner pid %s RUNNING (%s), started %s, %ss ago\n' \
				"${lock_pid}" "${lock_mode}" \
				"$(human_time "${lock_start}")" \
				"$(awk -v a="${lock_start}" -v b="$(now)" \
					'BEGIN { printf "%.0f", b - a }')"
		else
			printf 'runner pid %s is GONE: stale lock, a new run may start\n' \
				"${lock_pid}"
		fi
	else
		echo "no run in progress"
	fi

	for file in "${CI_RUN_DIR}"/*.state; do
		[ -e "${file}" ] || continue
		name=$(state_field "${file}" step)
		st=$(state_field "${file}" state)
		start=$(state_field "${file}" start)
		end=$(state_field "${file}" end)
		elapsed=$(state_field "${file}" elapsed)
		log=$(state_field "${file}" log)
		if [ "${st}" = RUNNING ] && [ -n "${start}" ] && [ -z "${end}" ]; then
			elapsed=$(awk -v a="${start}" -v b="$(now)" \
				'BEGIN { printf "%.1f", b - a }')
		fi
		printf '  %-11s %-28s %8ss  %s\n' \
			"${st}" "${name}" "${elapsed:--}" "${log:--}"
	done
}

if [ "${status_only}" -eq 1 ]; then
	print_status
	exit 0
fi

# A second runner in the same tree would corrupt the first one's objects, so
# take a lock.  noclobber makes the create atomic; a lock left behind by a
# runner that is no longer alive is stale and gets taken over.
acquire_lock() {
	local existing

	mkdir -p "${CI_RUN_DIR}"
	if ! (set -o noclobber; : >"${lock_file}") 2>/dev/null; then
		existing=$(state_field "${lock_file}" pid)
		if [ -n "${existing}" ] && kill -0 "${existing}" 2>/dev/null; then
			echo "run-ci-steps.sh: already running in ${tree_root}:" >&2
			echo "  pid ${existing}, mode $(state_field "${lock_file}" mode)," \
				"started $(human_time "$(state_field "${lock_file}" start)")" >&2
			echo "  use '$0 --status' to follow it" >&2
			exit 3
		fi
		echo "run-ci-steps.sh: taking over stale lock (pid ${existing:-?} is gone)" >&2
		rm -f "${lock_file}"
		(set -o noclobber; : >"${lock_file}") 2>/dev/null
	fi
	printf 'pid=%s\nmode=%s\nstart=%s\ntree=%s\n' \
		"$$" "$([ "${parallel_mode}" -eq 1 ] && echo parallel || echo serial)" \
		"$(now)" "${tree_root}" >"${lock_file}"
	lock_held=1
}

lock_held=0
cleanup() {
	local rc=$? file name st start pids stopped

	trap - EXIT
	if [ "${lock_held}" -eq 1 ]; then
		pids=$(jobs -pr || true)
		if [ -n "${pids}" ]; then
			# shellcheck disable=SC2086
			kill ${pids} 2>/dev/null || true
		fi
		stopped=0
		for file in "${CI_RUN_DIR}"/*.state; do
			[ -e "${file}" ] || continue
			st=$(state_field "${file}" state)
			name=$(state_field "${file}" step)
			start=$(state_field "${file}" start)
			case "${st}" in
			RUNNING)
				state_set "${name}" INTERRUPTED "${start}" "$(now)" ""
				stopped=1
				;;
			PENDING)
				state_set "${name}" CANCELLED "" "" ""
				stopped=1
				;;
			esac
		done
		if [ "${stopped}" -eq 1 ] && [ -n "${lane_root:-}" ] &&
			[ -n "$(ls -A "${lane_root}" 2>/dev/null)" ]; then
			echo "run-ci-steps.sh: lane trees left in ${lane_root}" >&2
		fi
		rm -f "${lock_file}"
	fi
	exit "${rc}"
}

acquire_lock
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# The developer box this grew up on keeps the interpreter that has
# pexpect and PyYAML outside PATH.  Anywhere else -- a CI image, a fresh
# checkout -- there is nothing to activate and the runner has to keep
# going: the Makefile's PYTHON discovery finds a working interpreter, and
# a box with none fails in the step, by name.
python_activate=$(compgen -G "/opt-?/cpython-v3.*-apt-deb/bin/activate" | head -n 1 || true)
if [ -n "${python_activate}" ]; then
	source "${python_activate}"
fi

export JOBS GNU_PARALLEL VALGRIND
export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD PTY_SETTLE_FLOOR PTY_JOBS
export KG_TEST_TIME_SCALE

rm -f "${CI_RUN_DIR}"/*.state "${CI_RUN_DIR}"/*.state.tmp
rm -rf "${CI_RUN_DIR}/logs"

# Every step is accounted for before the first one starts, so --status lists
# the whole run: the expensive ones it is not going to run say so as SKIP.
init_states() {
	local step name

	for step in "${steps[@]}"; do
		name=$(step_name "${step}")
		if skipped_step "${name}"; then
			state_set "${name}" SKIP "" "" ""
		else
			state_set "${name}" PENDING "" "" ""
		fi
	done
}

if [ "${parallel_mode}" -eq 0 ]; then
	init_states
	for step in "${steps[@]}"; do
		name=$(step_name "${step}")
		if skipped_step "${name}"; then
			# Already SKIP in the state files; say so on the way past.
			echo "SKIP  ${name} (expensive; --expensive or CI_EXPENSIVE=1 runs it)"
			continue
		fi
		started=$(now)
		state_set "${name}" RUNNING "${started}" "" ""
		rc=0
		"${step}" || rc=$?
		state_set "${name}" "$([ "${rc}" -eq 0 ] && echo PASS || echo FAIL)" \
			"${started}" "$(now)" "${rc}"
		if [ "${rc}" -ne 0 ]; then
			exit "${rc}"
		fi
	done
	exit 0
fi

lane_root=${CI_LANE_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/kg-ci-lanes-XXXXXX")}
mkdir -p "${lane_root}" "${CI_RUN_DIR}/logs"

# A lane has to start from a clean tree, but it also has to see the uncommitted
# changes that are the whole point of running this script, so copy the files
# instead of asking git for them.  The cheap-to-name artifacts are skipped while
# copying; `make distclean coverage-clean` then removes the rest (test binaries,
# fuzz binaries, ...) without duplicating the Makefile's lists here.
#
# test/.results is excluded for a different reason than size: a lane that
# inherits it and never runs `make check` copies it back out again, and the
# run then reports a full acceptance suite for a step that ran none.  A lane
# starts with no results and keeps only what it measured.  test/.bench is a
# generated corpus cache that no step reads.
copy_tree() {
	local dest=$1

	mkdir -p "${dest}"
	tar -cf - \
		--exclude=.git \
		--exclude=.ci/.run \
		--exclude=coverage \
		--exclude=compile_commands.json \
		--exclude=test/.results \
		--exclude=test/.bench \
		--exclude='*.o' \
		--exclude='*.gcda' \
		--exclude='*.gcno' \
		--exclude='*.plist' \
		--exclude=src/kg \
		--exclude='src/.features-*' \
		--exclude='src/.with-lisp-*' \
		. | tar -xf - -C "${dest}"
	make -C "${dest}" -s distclean coverage-clean
}

# Runs one step, in its own tree when it needs one, and records the outcome.
run_lane() {
	local step=$1 name=$2 lane_dir=$3 log=$4
	local rc=0 started elapsed

	started=$(now)
	state_set "${name}" RUNNING "${started}" "" ""
	if [ -n "${lane_dir}" ]; then
		{ copy_tree "${lane_dir}" && "${lane_dir}/${step}"; } || rc=$?
	else
		"${step}" || rc=$?
	fi
	state_set "${name}" "$([ "${rc}" -eq 0 ] && echo PASS || echo FAIL)" \
		"${started}" "$(now)" "${rc}"

	elapsed=$(state_field "${CI_RUN_DIR}/${name}.state" elapsed)
	if [ "${rc}" -eq 0 ]; then
		printf 'PASS  %-28s %8ss\n' "${name}" "${elapsed}" >&3
	else
		printf 'FAIL  %-28s %8ss  (exit %d)  %s\n' \
			"${name}" "${elapsed}" "${rc}" "${log}" >&3
	fi
}

# fd 3 is the terminal; everything a lane prints goes to its log file.
exec 3>&1
echo "running ${#steps[@]} CI steps, ${CI_PARALLEL_LANES} at a time, JOBS=${JOBS} per lane"
echo "run state and logs: ${CI_RUN_DIR}"
echo "lane trees: ${lane_root}"

init_states

started_all=$(now)
for step in "${steps[@]}"; do
	name=$(step_name "${step}")
	if skipped_step "${name}"; then
		# No lane, and so no tree copy: the copy is pure waste for a
		# step that is not going to run.
		echo "skip  ${name} (expensive)"
		continue
	fi
	while [ "$(jobs -rp | wc -l)" -ge "${CI_PARALLEL_LANES}" ]; do
		wait -n || true
	done
	echo "start ${name}"
	run_lane "${step}" "${name}" "$(lane_dir_for "${name}")" "$(log_for "${name}")" \
		>"$(log_for "${name}")" 2>&1 &
done
wait || true
elapsed_all=$(awk -v a="${started_all}" -v b="$(now)" 'BEGIN { printf "%.0f", b - a }')

failed=()
echo ""
echo "============================================================================"
echo "CI summary (parallel)"
echo "============================================================================"
for step in "${steps[@]}"; do
	name=$(step_name "${step}")
	state_file="${CI_RUN_DIR}/${name}.state"
	st=$(state_field "${state_file}" state)
	rc=$(state_field "${state_file}" exit)
	secs=$(state_field "${state_file}" elapsed)
	lane_dir=$(lane_dir_for "${name}")

	# Each lane's `make check` results, kept per step: which stage they
	# came from is half of what they mean, since the valgrind lane's
	# timings are not the plain build's.
	if [ -n "${lane_dir}" ] && [ -d "${lane_dir}/test/.results" ]; then
		mkdir -p "${CI_RUN_DIR}/results"
		rm -rf "${CI_RUN_DIR}/results/${name}"
		cp -a "${lane_dir}/test/.results" "${CI_RUN_DIR}/results/${name}"
	fi
	if [ "${st}" = SKIP ]; then
		printf '  SKIP  %-28s %8s   (expensive)\n' "${name}" "-"
	elif [ "${st}" = PASS ]; then
		printf '  PASS  %-28s %8ss\n' "${name}" "${secs}"
		# ci-02 builds coverage/ inside its lane; put it back where a
		# serial run would have left it.  The report's file names still
		# spell out the lane path it was generated in.
		if [ -n "${lane_dir}" ] && [ -d "${lane_dir}/coverage" ]; then
			rm -rf "${tree_root}/coverage"
			cp -a "${lane_dir}/coverage" "${tree_root}/coverage"
		fi
		if [ -n "${lane_dir}" ]; then
			rm -rf "${lane_dir}"
		fi
	else
		printf '  %-5s %-28s %8ss  (exit %s)\n' \
			"${st:-FAIL}" "${name}" "${secs:--}" "${rc:-?}"
		failed+=("${name}")
	fi
done
printf '  total %-28s %8ss\n' "" "${elapsed_all}"
echo "============================================================================"

# One machine-readable summary of the run, from the files the steps already
# wrote: stage states and durations, both test layers' counts and slowest
# cases, coverage against its floor, the complexity manifest, the pins.
# Never fatal -- a report is not a gate.
if ! "${PYTHON:-python3}" utils/quality_report.py \
	--results-dir "${CI_RUN_DIR}/results/ci-03-gcc-analyzer" \
	--output "${CI_RUN_DIR}/quality.json"; then
	echo "(quality report not written)"
fi

if [ "${#failed[@]}" -eq 0 ]; then
	rmdir "${lane_root}" 2>/dev/null || true
	echo "all steps passed; logs kept in ${CI_RUN_DIR}/logs"
	exit 0
fi

for name in "${failed[@]}"; do
	echo ""
	echo "-------------------------------------------------------------- ${name}"
	cat "${CI_RUN_DIR}/logs/${name}.log" 2>/dev/null ||
		echo "(no log: ${name} never started)"
done

echo ""
echo "FAILED: ${failed[*]}"
echo "logs in ${CI_RUN_DIR}/logs"
if [ -n "$(ls -A "${lane_root}" 2>/dev/null)" ]; then
	echo "the failing lanes' trees were kept under ${lane_root}"
else
	rmdir "${lane_root}" 2>/dev/null || true
	echo "no lane tree was kept (the failing steps run in the tree itself)"
fi
exit 1
