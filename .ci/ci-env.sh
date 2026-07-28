#!/bin/bash

# Set to 1 by `run-ci-steps.sh --parallel`, and exported to every step it
# launches.  Concurrent lanes share the machine, so each lane builds with a
# slice of the cores and the (timing sensitive) PTY suite gets more slack.
CI_PARALLEL=${CI_PARALLEL:-0}
CI_NPROC=$(nproc 2>/dev/null || echo 2)

# The slow steps spend their time in the PTY suite, which waits on terminal
# timing rather than on cores, so lanes are cheap; the build phase is the only
# real CPU burst and ccache absorbs most of it.  Six lanes is enough to start
# every slow step at once, and one core per lane is the floor below that.
if [ -z "${CI_PARALLEL_LANES:-}" ]; then
	CI_PARALLEL_LANES=$((CI_NPROC / 4))
	if [ "${CI_PARALLEL_LANES}" -lt 2 ]; then
		CI_PARALLEL_LANES=2
	elif [ "${CI_PARALLEL_LANES}" -gt 6 ]; then
		CI_PARALLEL_LANES=6
	fi
fi

if [ "${CI_PARALLEL}" = 1 ]; then
	JOBS=${JOBS:-$((CI_NPROC / CI_PARALLEL_LANES > 0 ? CI_NPROC / CI_PARALLEL_LANES : 1))}
	PTY_TIMEOUT=${PTY_TIMEOUT:-40}
else
	JOBS=${JOBS:-${CI_NPROC}}
	PTY_TIMEOUT=${PTY_TIMEOUT:-20}
fi

# The delay adds do not vary by mode, measurement having killed the
# reasons to.  kg's startup is polled rather than slept, so the startup add
# now only pads the Emacs oracle; and the key add cannot buy safety under
# load, because contention only ever makes a sleep longer, never shorter,
# so it cannot push an inter-key gap below the 30 ms paste threshold in
# kbd.c.  PTY_TIMEOUT still doubles under --parallel, as failure headroom.
PTY_STARTUP_DELAY_ADD=${PTY_STARTUP_DELAY_ADD:-0.3}
PTY_KEY_DELAY_ADD=${PTY_KEY_DELAY_ADD:-0.01}

# PTY cases are independent and spend their time waiting on a child editor,
# so pooling them scales past the core count.  8 is half the 16 measured
# safe at 6-way lane concurrency (96 concurrent cases held 13 of 32 cores).
PTY_JOBS=${PTY_JOBS:-8}

# Where run-ci-steps.sh keeps its lock, per-step state and per-step logs.  It
# has to stay in the real tree: a lane runs in a throwaway copy of it.
CI_RUN_DIR=${CI_RUN_DIR:-.ci/.run}

PARALLEL=${PARALLEL:-parallel}
VALGRIND=${VALGRIND:-valgrind --quiet --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,possible --error-exitcode=1}

MAKE_PARALLEL=(make -j "${JOBS}" --output-sync=target)
