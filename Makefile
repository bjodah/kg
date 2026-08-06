# Makefile for kg editor

ifeq ($(origin CC),default)
CC      = gcc
endif
CFLAGS_ORIGIN := $(origin CFLAGS)
CFLAGS  ?= -Wall -W -pedantic -Os
FE_CFLAGS ?= -Wall -Wextra -pedantic -Os

# Sanitizer and analyzer jobs override CFLAGS as a complete flag set.  Unless
# FE_CFLAGS was also overridden, instrument Fe with that same set.
ifneq ($(filter command line environment override,$(CFLAGS_ORIGIN)),)
ifeq ($(origin FE_CFLAGS),file)
FE_CFLAGS := $(CFLAGS)
endif
endif

override CFLAGS += -std=c23 -Ife/tiny-regex-c
override FE_CFLAGS += -std=c23 -Ife/tiny-regex-c
PROG    = kg
OBJDIR  = src
TARGET  = $(OBJDIR)/$(PROG)
MAN1    = doc/kg.1
WITH_LISP ?= 1

ifneq ($(WITH_LISP),0)
ifneq ($(WITH_LISP),1)
$(error WITH_LISP must be 0 or 1)
endif
endif
ifeq ($(WITH_LISP),1)
ifeq ($(wildcard fe/fe.c),)
FE_SPLIT_MISSING = 1
endif
ifeq ($(wildcard fe/fe_eval.c),)
FE_SPLIT_MISSING = 1
endif
ifeq ($(FE_SPLIT_MISSING),1)
ifeq ($(filter-out clean distclean coverage-clean,$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),)
SKIP_FE_CHECK = 1
endif
endif
ifneq ($(SKIP_FE_CHECK),1)
$(error fe/fe.c and/or fe/fe_eval.c is missing; run 'git submodule update --init --recursive' or build with 'WITH_LISP=0')
endif
endif
override CFLAGS += -DKG_USE_LISP=1
override LDLIBS += -lm
# The evaluator lives in its own translation unit since Fe sub-plan 03B
# (fe.c -> fe.c + fe_eval.c, behind a private fe/fe_internal.h); a list so
# every consumer below is a one-line change.
FE_OBJ = $(OBJDIR)/fe.o $(OBJDIR)/fe_eval.o
FUZZ_FE_OBJ = $(TESTDIR)/fe_fuzz.o $(TESTDIR)/fe_eval_fuzz.o
endif

ifeq ($(wildcard fe/tiny-regex-c/re.c),)
ifeq ($(filter-out clean distclean coverage-clean,$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),)
SKIP_REGEX_CHECK = 1
endif
endif
ifneq ($(SKIP_REGEX_CHECK),1)
$(error fe/tiny-regex-c/re.c is missing; run 'git submodule update --init --recursive')
endif
endif

LISP_CONFIG = $(OBJDIR)/.with-lisp-$(WITH_LISP)

prefix  = /usr/local
bindir  = $(prefix)/bin
mandir  = $(prefix)/share/man
man1dir = $(mandir)/man1

# Show a leading "~" on lines past end-of-buffer (vim/kilo style).
# Off by default for an Emacs-like presentation.  Override on the make
# command line, e.g. `make KG_SHOW_TILDE=1`.
KG_SHOW_TILDE ?= 0
override CFLAGS += -DKG_SHOW_TILDE=$(KG_SHOW_TILDE)

# Required for POSIX/GNU interfaces when source files include system headers
# directly, as enforced by Include What You Use.
override CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

# The Lisp adapter is one public Fe-free header (lisp.h) in front of a set
# of adapter implementation files sharing a private lisp_internal.h.  Only
# src/lisp_*.c may include fe.h or lisp_internal.h (the header includes
# fe.h itself for standalone header checking) -- `lisp-include-check`
# enforces that.  lisp_core.c is always built: it holds the interpreter
# lifecycle and the WITH_LISP=0 stubs behind the same #ifdef the pre-split
# file used.  The rest exist only when Fe is linked in.
LISP_SRCS = lisp_core.c
ifeq ($(WITH_LISP),1)
LISP_SRCS += lisp_prelude.c lisp_string.c lisp_buffer.c lisp_word.c \
             lisp_io.c lisp_cmd.c lisp_obj.c lisp_search.c lisp_hooks.c \
             lisp_process.c lisp_require.c
endif
LISP_OBJS = $(addprefix $(OBJDIR)/,$(LISP_SRCS:.c=.o))

# Source files
SRCS = main.c tty.c syntax.c autocomplete.c buffer.c fileio.c \
       display.c search.c basic.c word.c kbd.c yank.c undo.c help.c describe.c bufmgr.c winmgr.c cmd.c cmdstate.c keyevent.c keymap.c macro.c \
       shell.c path.c rect.c $(LISP_SRCS) keybind.c mode.c vgeom.c localvars.c compile.c compile_parse.c \
       compile_nav.c register.c \
       width.c dired.c perf.c process.c process_table.c marker.c decor.c event.c

# Object and header files
OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
REGEX_ENGINE_OBJ = $(OBJDIR)/tiny_regex.o
REGEX_WRAPPER_OBJ = $(OBJDIR)/regex.o
REGEX_OBJS = $(REGEX_ENGINE_OBJ) $(REGEX_WRAPPER_OBJ)
HDRS = $(OBJDIR)/def.h
# Every header in src/ is checked for compiling on its own (see
# `header-check`): a module header that only works once def.h has been
# included is def.h with extra steps.
ALL_HDRS = $(sort $(wildcard $(OBJDIR)/*.h))

# Test infrastructure
TESTDIR  = test
TESTBINS = $(TESTDIR)/test_undo $(TESTDIR)/test_buffer \
           $(TESTDIR)/test_syntax $(TESTDIR)/test_yank \
           $(TESTDIR)/test_autocomplete $(TESTDIR)/test_word \
           $(TESTDIR)/test_basic $(TESTDIR)/test_region \
           $(TESTDIR)/test_shell $(TESTDIR)/test_complete \
           $(TESTDIR)/test_lisp $(TESTDIR)/test_regex \
           $(TESTDIR)/test_localvars $(TESTDIR)/test_compile \
           $(TESTDIR)/test_compile_parse $(TESTDIR)/test_compile_nav \
           $(TESTDIR)/test_tty $(TESTDIR)/test_minibuf \
           $(TESTDIR)/test_dired $(TESTDIR)/test_winmgr \
           $(TESTDIR)/test_cmd $(TESTDIR)/test_keys \
           $(TESTDIR)/test_keyevent $(TESTDIR)/test_keymap \
           $(TESTDIR)/test_describe $(TESTDIR)/test_marker \
           $(TESTDIR)/test_decor $(TESTDIR)/test_event \
           $(TESTDIR)/test_register $(TESTDIR)/test_process_table \
           $(TESTDIR)/test_vgeom \
           $(TESTDIR)/test_perf
# test_perf is not built like the other unit tests: it needs the whole
# editor compiled with -DKG_PERF_COUNTERS=1 (src/perf.h), which must not
# be mixed with the src/*.o everything else links.  Its objects live in
# their own directory, and the counting kg `make bench` drives is the same
# objects plus main.o -- so a counting build never leaves a stale object
# in src/ for the next plain `make` to link.
PERFOBJDIR = $(TESTDIR)/perfobj
PERF_KG = $(PERFOBJDIR)/kg
# --coverage is dropped on purpose: the coverage lane builds every test
# binary, and instrumenting a second copy of every src/*.c would merge two
# differently-compiled builds of the same source into one tracefile.  It
# stays in the *link* flags so libgcov is still there for the objects that
# were instrumented ($(FE_OBJ)).
PERF_CFLAGS = $(filter-out --coverage,$(CFLAGS)) -DKG_PERF_COUNTERS=1
PERF_SRC_OBJS = $(addprefix $(PERFOBJDIR)/,$(SRCS:.c=.o)) $(PERFOBJDIR)/regex.o
PERF_TEST_OBJS = $(PERFOBJDIR)/test_perf.o $(PERFOBJDIR)/test.o \
		 $(PERFOBJDIR)/stubs_main.o \
		 $(filter-out $(PERFOBJDIR)/main.o,$(PERF_SRC_OBJS))
BENCH_OUT ?= $(TESTDIR)/.results/bench.json
BENCH_ARGS ?=
FUZZBIN = $(TESTDIR)/fuzz_keypress
FUZZ_SRCS = $(TESTDIR)/fuzz_keypress.c $(TESTDIR)/fuzz_stubs.c \
	    $(OBJDIR)/kbd.c $(OBJDIR)/buffer.c $(OBJDIR)/basic.c \
	    $(OBJDIR)/word.c $(OBJDIR)/autocomplete.c $(OBJDIR)/yank.c \
	    $(OBJDIR)/undo.c $(OBJDIR)/rect.c $(OBJDIR)/syntax.c \
	    $(OBJDIR)/tty.c $(OBJDIR)/macro.c \
	    $(addprefix $(OBJDIR)/,$(LISP_SRCS)) \
	    $(OBJDIR)/keybind.c $(OBJDIR)/width.c $(OBJDIR)/cmdstate.c $(OBJDIR)/keyevent.c \
	    $(OBJDIR)/keymap.c $(OBJDIR)/marker.c $(OBJDIR)/decor.c \
	    $(OBJDIR)/event.c $(OBJDIR)/process.c $(OBJDIR)/process_table.c \
	    $(OBJDIR)/regex.c fe/tiny-regex-c/re.c
FUZZBIN_DIRLOCALS = $(TESTDIR)/fuzz_dirlocals
FUZZBIN_REGEX    = $(TESTDIR)/fuzz_regex
FUZZBIN_LOCALVARS = $(TESTDIR)/fuzz_localvars
FUZZBIN_COMPILE_PARSE = $(TESTDIR)/fuzz_compile_parse
FUZZBINS = $(FUZZBIN) $(FUZZBIN_DIRLOCALS) $(FUZZBIN_REGEX) $(FUZZBIN_LOCALVARS) $(FUZZBIN_COMPILE_PARSE)
FUZZ_SEEDS = $(TESTDIR)/fuzz-seeds
FUZZ_SEEDS_REGEX = $(FUZZ_SEEDS)/regex
# The working corpus is gitignored, so a fresh checkout starts each target
# from nothing unless the tracked seeds are copied in first.  Every target
# has seeds now: keypress, dirlocals and localvars used to start empty, so
# `-runs=50` on an empty corpus explored almost nothing.
FUZZ_CORPUS = $(TESTDIR)/fuzz-corpus
# Smoke runs are a time budget, not a run count: 50 runs is a different
# amount of work on every machine and on every build of the target, while
# 5 s is the thing CI actually has to pay.  The rest of these mirror the
# names both subprojects already use, so one habit works everywhere.
FUZZ_MAX_TOTAL_TIME ?= 5
FUZZ_MAX_LEN ?= 4096
FUZZ_TIMEOUT ?= 10
FUZZ_RSS_LIMIT_MB ?= 2048
FUZZ_VERBOSITY ?= 0
# -print_final_stats reports execs/s, features and peak RSS per target,
# which is the drift a smoke run can otherwise only fail to notice.
FUZZ_SMOKE_ARGS ?= -max_total_time=$(FUZZ_MAX_TOTAL_TIME) \
		   -max_len=$(FUZZ_MAX_LEN) -timeout=$(FUZZ_TIMEOUT) \
		   -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		   -verbosity=$(FUZZ_VERBOSITY) -print_final_stats=1
# Crash, OOM and timeout inputs libFuzzer saves.  Without a prefix it
# writes them into the working directory, where they are neither ignored
# nor obviously related to a fuzz run; test/fuzz-artifacts/ is already
# gitignored.  The trailing slash is what makes libFuzzer treat the value
# as a directory, and the directory has to exist first.
FUZZ_ARTIFACTS = $(TESTDIR)/fuzz-artifacts
REGEX_DIFF_BIN = $(TESTDIR)/regex_differential
REGEX_DIFF_CASES ?= 2000
REGEX_DIFF_SEED ?= 20260729
EMACS ?= emacs
# The PTY harness needs pexpect and PyYAML, which are not always installed
# for the same interpreter everywhere: CI images ship them for python3,
# while the developer box this grew up on has them only for its own
# `python`.  Pick the first interpreter that can import both, and fall back
# to python3 so a machine with neither reports the missing module rather
# than a missing binary.  Override with `make PYTHON=...`.
PYTHON ?= $(shell for p in python3 python; do \
		if command -v $$p >/dev/null 2>&1 && \
		   $$p -c 'import pexpect, yaml' >/dev/null 2>&1; then \
			echo $$p; exit 0; \
		fi; \
	done; echo python3)
# Passed to the harness as --emacs when set, so `make check
# KG_PTY_EMACS=/path/to/emacs` reaches it (a make-level variable is not in
# the child's environment).  Unset, the harness searches $KG_PTY_EMACS and
# then PATH itself.
KG_PTY_EMACS ?=
PTY_TESTS = $(sort $(wildcard $(TESTDIR)/pty/*.yaml))
# Source objects needed by tests (subset of OBJS, no main/tty/display/etc.)
# process.o and process_table.o are here, not just on the EXTRA_ lists that
# used to list process.o alone, because event.o now calls
# kg_process_table_resolves() unconditionally (event_resolution()'s process
# arm) -- every test that links event.o needs process_table.o, which in
# turn needs process.o.  $^ in the link rule below dedupes, so an EXTRA_
# list that also names process.o separately is harmless.
TEST_SRCS_OBJS = $(OBJDIR)/undo.o $(OBJDIR)/buffer.o $(OBJDIR)/syntax.o \
                 $(OBJDIR)/width.o $(OBJDIR)/marker.o $(OBJDIR)/decor.o \
                 $(OBJDIR)/cmdstate.o $(OBJDIR)/event.o \
                 $(OBJDIR)/process.o $(OBJDIR)/process_table.o
TEST_RUNNER ?=
KG_RUNNER ?=
# Per-run machine-readable test results (gitignored).  Both layers write
# here so a CI stage, or utils/quality_report.py, can read counts and
# per-case durations instead of scraping the summary block.
CHECK_RESULTS_DIR ?= $(TESTDIR)/.results
PTY_ACCEPT_ARGS ?=
PTY_TIMEOUT ?= 15.0
PTY_STARTUP_DELAY_ADD ?=
PTY_KEY_DELAY_ADD ?=
PTY_JOBS ?=
FUZZ_CFLAGS ?= -Wall -Wextra -pedantic -std=c23 -O1 -g \
	       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DKG_FUZZ=1 \
	       -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
FE_FUZZ_CFLAGS ?= $(FUZZ_CFLAGS)

ifeq ($(WITH_LISP),1)
override FUZZ_CFLAGS += -DKG_USE_LISP=1
endif

# Project metrics
SCC ?= scc
SCC_PATHS ?= src test
SCC_COMPLEXITY_PATHS ?= src
# Deliberately re-baselined 4208 -> 4280 (+1.7%) as the one sanctioned
# ratchet raise of the completed 2026-07-30 review program.
# Measured 4193 after the program's extraction pass, leaving 87 points of
# headroom for that completed campaign; nothing raises this again without a
# reviewed exception.  (History: 4199 -> 4208 for the
# path picker's literal-accept answers; 4280 -> 4223 -> 4221 -> 4217 ->
# 4193 -> 4127 as the follow-up program's slices funded themselves and
# banked what was left.)
#
# Raised 4127 -> 4152 by explicit decision for Plan 01 Phase 6, the four
# describe commands.  That slice is additive by nature: it adds a way to
# ask the command table and the keymaps what they hold, and there is
# nothing it replaces to pay for it.  The funding candidates that remain
# are parser state machines whose rewrite would be invented work, and the
# three picker loops, which are a Plan 05/06-shaped refactor.  Measured
# 4151 after the slice, so this is the ceiling and not a target -- and it
# is still 71 below the 4223 this follow-up program started from.
#
# Lowered 4152 -> 4144 to bank what wave 1 left behind: the merged
# tracks measured 4150, and deleting the three undo opcodes nothing can
# produce and the row primitive the rectangle migration orphaned took
# that to 4144.  The cap is the measurement again, as it was before the
# describe slice.
#
# Raised 4144 -> 4200 for Plan 01's decoder flag day (wave 2): C has no
# switch on a struct, so every switch(legacy int) the flag day touched
# (tty.c's parse_escape() escape-key names, search.c's
# isearch_handoff_key(), bufmgr.c's minibuf_edit_key(), kbd.c's
# CTRL_G/shift-select dispatch) became comparisons instead, and several
# sites also gained a `mods == 0` guard the old encoding's disjoint
# numeric ranges used to give for free (Ctrl-a's key_event base is the
# same 'a' the bare letter uses, unlike the legacy int).  Table-driven
# dispatch (the shift_motions[]-shaped pattern already in kbd.c) funded
# most of it back -- isearch_handoff_key() and minibuf_edit_key() both
# gained a second lookup table for exactly this -- but not all of it.
# Measured 4161 after the key-event slice.  Plan 03's standalone marker
# store and buffer-mark adapter add 43 (4243 measured); 4250 banks that
# named module with six points of rounding/tool-version room, not room for
# unrelated command growth.
# Raised 5500 -> 5660 by Phase 7 sub-plan 07A (2026-08-06), funding the
# interactive metadata, prefix/argument builder, nested command execution and
# prompt seam in 07D/07E. The idle-tree measurement is 5489, so the raise is
# 171 points above the floor against the audited +110..170 scc price; the
# per-file and pmccabe ratchets remain unchanged and are reported per slice.
SCC_COMPLEXITY_MAX ?= 5660
SCC_FILE_COMPLEXITY_MAX ?= 520
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(addprefix $(OBJDIR)/,$(SRCS))
# Lowered 120 -> 110 after editor_process_keypress() shed its kill-lines,
# repeated-yank, shift-motion, recenter and end-of-keypress bookkeeping
# into helpers: that function measures 85 and the worst function in the
# tree is now localvars_parse_footer at 100, so 110 is the budget later work
# has to stay under.  Lower it, do not raise it.  (History: 130 ->
# 120 when the C-x and C-x r prefix dispatch moved out; 130 dates from
# when a missing pmccabe binary silently disabled this gate.)
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 110
# The ceiling above is a backstop; the ratchet is per symbol.  Every
# function's measured complexity is recorded in this manifest, no symbol
# may exceed its entry, and a function with no entry is new and has to
# come in at or under PMCCABE_NEW_FUNCTION_MAX.  Regenerate with
# `make pmccabe-baseline` -- which is also how a decrease is banked, so
# the diff shows what moved and in which direction.
PMCCABE_BASELINE ?= .ci/pmccabe-baseline.json
PMCCABE_NEW_FUNCTION_MAX ?= 15
# Census of everything that still changes buffer text outside the edit
# edit-gateway follow-up is shrinking: raw row primitives, hand-written undo
# records, the ambient suppress_undo flag and direct writes to a row's
# text fields.  A ratchet, not a ban -- no count may rise, and `make
# gateway-baseline` is how a migrated caller is banked.
GATEWAY_MANIFEST ?= .ci/mutation-gateway.json
COVERAGE_DIR ?= coverage
COVERAGE_CFLAGS ?= -Wall -W -pedantic -std=c23 -O0 -g --coverage
# --branch-coverage matches what both subprojects already collect; the
# root reported lines and functions only.  The ignore list stays: lcov 2.x
# calls a gcno/gcda pair that disagrees "inconsistent" and refuses the
# file otherwise.
COVERAGE_LCOV_ARGS ?= --quiet --branch-coverage --ignore-errors inconsistent,gcov
COVERAGE_GENHTML_ARGS ?= --quiet
COVERAGE_BASELINE ?= .ci/coverage-baseline.json
COVERAGE_BASELINE_HOW ?= 'make coverage PTY_JOBS=8 PTY_ACCEPT_ARGS=--require-tools (gcc -O0 --coverage, lcov --branch-coverage), src/*.c after lcov --extract'
COVERAGE_BASELINE_NOTE ?= 'Per-file floors: no file may cover a smaller share of its lines or functions than it does here. Measured with the recipe in "how"; PTY_JOBS does not change the result (verified: PTY_JOBS=1 and two PTY_JOBS=8 runs agreed file for file), so the lane keeps the fast setting.'
CLANG_FORMAT ?= clang-format
FUZZ_CC ?= clang
FORMAT_FILES = $(wildcard $(OBJDIR)/*.[ch] $(TESTDIR)/*.[ch])
BEAR ?= bear
CLANG_CC ?= clang
COMPILE_DB_FILE ?= compile_commands.json
# Every other tool here is a bare name that PATH resolves; these two used
# to be absolute paths into one developer box's /opt-3, so `make iwyu`
# could not run anywhere else without being told where to look (the hosted
# workflow sets both in its env).  Find them on PATH, fall back to that
# box's layout the way utils/pty_accept.py falls back for Emacs, and let
# the recipe say which tool is missing rather than dying as "no such file".
IWYU_FALLBACK_DIR ?= /opt-3/iwyu-21/bin
IWYU ?= $(shell command -v include-what-you-use 2>/dev/null || \
	echo $(IWYU_FALLBACK_DIR)/include-what-you-use)
IWYU_TOOL ?= $(shell command -v iwyu_tool.py 2>/dev/null || \
	echo $(IWYU_FALLBACK_DIR)/iwyu_tool.py)
IWYU_ARGS ?= -Xiwyu --error=1
IWYU_FILES = $(addprefix $(CURDIR)/$(OBJDIR)/,$(SRCS))

all: $(TARGET)

$(LISP_CONFIG):
	rm -f $(OBJDIR)/.with-lisp-0 $(OBJDIR)/.with-lisp-1
	touch $@

$(OBJS): $(LISP_CONFIG)

$(TARGET): $(OBJS) $(FE_OBJ) $(REGEX_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(OBJDIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/tiny_regex.o: fe/tiny-regex-c/re.c fe/tiny-regex-c/re.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

$(OBJDIR)/regex.o: src/regex.c src/regex.h $(HDRS) fe/tiny-regex-c/re.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/lisp_core.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp.h
$(OBJDIR)/lisp_prelude.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/lisp_string.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/lisp_buffer.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/lisp_word.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/lisp_io.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/lisp_cmd.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/lisp_obj.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_obj.h
$(OBJDIR)/lisp_process.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_obj.h $(OBJDIR)/lisp_process.h $(OBJDIR)/process.h $(OBJDIR)/process_table.h
$(OBJDIR)/lisp_require.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/main.o: $(OBJDIR)/lisp.h

$(OBJDIR)/fe.o: fe/fe.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

$(OBJDIR)/fe_eval.o: fe/fe_eval.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

check: header-check lisp-include-check docs-check lisp-compat-check lisp-prelude-check check-unit check-pty

# Cheap documentation drift: every key the built-in help table names has
# to be spelled somewhere in kg(1).  Not a substitute for reading either
# one -- it is what catches a binding added to src/help.c and nowhere else.
docs-check:
	@$(PYTHON) utils/check_help_drift.py

# Phase 0 sub-plan 00C's manifest: every Fe primitive (fe/fe.c), kg native
# and kg prelude definition (src/lisp_prelude.c) must appear in exactly one
# of fe/compat/features.json or test/lisp-compat/features.json.  Pure
# structure -- it runs no Fe, no kg, no Emacs -- docs-check's closest
# analogue, so it sits next to it.  Snapshot regeneration/verification
# against a real Emacs is the separate `lisp-compat-oracle` target below,
# not part of this or ordinary `make check`.
lisp-compat-check:
	@$(PYTHON) utils/check_lisp_compat.py

# Phase 1 sub-plan 01A: lisp/prelude.el is the canonical prelude source and
# src/lisp_prelude_generated.inc is a checked-in, byte-for-byte copy of it,
# so an ordinary build needs no Python.  These two targets are the drift
# check that keeps the pair honest -- the same structural no-drift shape as
# docs-check and header-check, which is why they sit beside them in `check`.
# Regeneration writes into a temporary file and compares, so the check never
# rewrites the tree it is checking.
lisp-prelude-generate:
	@$(PYTHON) utils/embed_lisp.py lisp/prelude.el \
		src/lisp_prelude_generated.inc

lisp-prelude-check:
	@tmp=$$(mktemp) && trap 'rm -f "$$tmp"' EXIT && \
	$(PYTHON) utils/embed_lisp.py lisp/prelude.el "$$tmp" >/dev/null && \
	if cmp -s "$$tmp" src/lisp_prelude_generated.inc; then \
		echo "lisp-prelude-check: src/lisp_prelude_generated.inc matches lisp/prelude.el"; \
	else \
		echo "lisp-prelude-check: src/lisp_prelude_generated.inc is stale" >&2; \
		echo "  lisp/prelude.el changed without running 'make lisp-prelude-generate'." >&2; \
		diff -u src/lisp_prelude_generated.inc "$$tmp" | head -20 >&2; \
		exit 1; \
	fi

# Regenerates/verifies test/lisp-compat/oracle/*.json against the resolved
# Emacs, reusing fe/utils/run-emacs-oracle.py directly rather than copying
# it (00B's runner takes the corpus root as its first argument specifically
# so kg does not need its own copy).  Same resolution order as the PTY
# harness: --emacs, then $KG_PTY_EMACS, then `emacs` on PATH, then the
# /opt-3 pin; missing Emacs SKIPs unless --require-tools is passed via
# LISP_COMPAT_ORACLE_ARGS.  A regeneration/verification target, not part of
# ordinary `make check` -- checked-in snapshots keep the normal suite
# Emacs-free, exactly as in fe/.
#
# Restricted with --case to exactly the comparison=emacs entries: the
# runner (generic over any corpus root, per 00B) processes every
# cases/*.json file it is given regardless of the manifest's comparison
# field, and a kg-policy case has no oracle snapshot by design (this
# manifest's own README), so an unrestricted run would create dozens of
# snapshot files nothing ever checks or wants committed.
lisp-compat-oracle:
	$(PYTHON) fe/utils/run-emacs-oracle.py test/lisp-compat \
		$(if $(KG_PTY_EMACS),--emacs $(KG_PTY_EMACS),) \
		$$($(PYTHON) -c "import json; d = json.load(open('test/lisp-compat/features.json')); \
			print(' '.join('--case ' + f['cases'][0] for f in d['features'] if f['comparison'] == 'emacs'))") \
		$(LISP_COMPAT_ORACLE_ARGS)

# Compile each src/*.h as the first thing in its own translation unit.
# The trailing declaration is there so a header that legitimately expands
# to nothing (perf.h with the counters off) is not an empty translation
# unit, which -pedantic rejects.  Prints the count it checked, because "no
# output" is also what a glob that matched nothing looks like.
header-check:
	@n=0; for h in $(ALL_HDRS); do \
		printf '#include "%s"\nextern int kg_header_check;\n' "$$h" | \
			$(CC) $(CFLAGS) -fsyntax-only -x c - || exit 1; \
		n=$$((n + 1)); \
	done; \
	test "$$n" -gt 0 || { echo "header-check: no headers found" >&2; exit 1; }; \
	echo "header-check: $$n header(s) compile standalone"

# The interpreter must stay behind the adapter seam: only the src/lisp_*.c
# implementation files may include fe.h or the private lisp_internal.h
# (which includes fe.h itself for standalone header-check compilation), so
# every other module and the public lisp.h stay Fe-free.  A plain grep of
# the checked-in sources, not the object dir.
lisp-include-check:
	@bad=$$(grep -l '#include.*fe\.h' src/*.c src/*.h 2>/dev/null | \
		while read f; do \
			case "$$f" in \
				src/lisp_*.c | src/lisp_internal.h) ;; \
				*) echo "$$f" ;; \
			esac; \
		done || true); \
	if [ -n "$$bad" ]; then \
		echo "lisp-include-check: fe.h reached outside the Lisp adapter:" >&2; \
		echo "$$bad" | sed 's/^/  /' >&2; \
		exit 1; \
	fi; \
	bad=$$(grep -l '#include.*lisp_internal\.h' src/*.c src/*.h 2>/dev/null | \
		while read f; do \
			case "$$f" in \
				src/lisp_*.c) ;; \
				*) echo "$$f" ;; \
			esac; \
		done || true); \
	if [ -n "$$bad" ]; then \
		echo "lisp-include-check: lisp_internal.h reached outside src/lisp_*.c:" >&2; \
		echo "$$bad" | sed 's/^/  /' >&2; \
		exit 1; \
	fi; \
	bad=$$(grep -l '#include.*fe_internal\.h' src/*.c src/*.h 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "lisp-include-check: fe_internal.h is private to fe/, reached from src/:" >&2; \
		echo "$$bad" | sed 's/^/  /' >&2; \
		exit 1; \
	fi; \
	echo "lisp-include-check: fe.h/lisp_internal.h only in src/lisp_*.c, fe_internal.h nowhere in src/"

check-unit: $(TESTBINS)
	@$(PYTHON) utils/run_unit_tests.py --runner "$(TEST_RUNNER)" \
		--json $(CHECK_RESULTS_DIR)/unit.json $(TESTBINS)

check-pty: $(TARGET) $(PTY_TESTS)
	@$(PYTHON) utils/pty_accept.py $(PTY_ACCEPT_ARGS) \
		--json $(CHECK_RESULTS_DIR)/pty.json \
		$(if $(PTY_TIMEOUT),--timeout $(PTY_TIMEOUT),) \
		$(if $(PTY_STARTUP_DELAY_ADD),--startup-delay-add $(PTY_STARTUP_DELAY_ADD),) \
		$(if $(PTY_KEY_DELAY_ADD),--key-delay-add $(PTY_KEY_DELAY_ADD),) \
		$(if $(PTY_JOBS),--jobs $(PTY_JOBS),) \
		$(if $(KG_PTY_EMACS),--emacs $(KG_PTY_EMACS),) \
		--kg $(TARGET) --kg-runner "$(KG_RUNNER)" $(PTY_TESTS)

fuzz-keypress: $(FUZZBIN)

fuzz-keypress-seed:
	mkdir -p $(FUZZ_CORPUS)/keypress
	cp -f $(FUZZ_SEEDS)/keypress/* $(FUZZ_CORPUS)/keypress/

fuzz-keypress-smoke: $(FUZZBIN) fuzz-keypress-seed
	mkdir -p $(FUZZ_ARTIFACTS)/keypress
	./$(FUZZBIN) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/keypress/ \
		$(FUZZ_CORPUS)/keypress

fuzz-dirlocals: $(FUZZBIN_DIRLOCALS)

fuzz-dirlocals-seed:
	mkdir -p $(FUZZ_CORPUS)/dirlocals
	cp -f $(FUZZ_SEEDS)/dirlocals/* $(FUZZ_CORPUS)/dirlocals/

fuzz-dirlocals-smoke: $(FUZZBIN_DIRLOCALS) fuzz-dirlocals-seed
	mkdir -p $(FUZZ_ARTIFACTS)/dirlocals
	./$(FUZZBIN_DIRLOCALS) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/dirlocals/ \
		$(FUZZ_CORPUS)/dirlocals

fuzz-regex: $(FUZZBIN_REGEX)

fuzz-regex-seed:
	mkdir -p $(FUZZ_CORPUS)/regex
	cp -f $(FUZZ_SEEDS_REGEX)/* $(FUZZ_CORPUS)/regex/

fuzz-regex-smoke: $(FUZZBIN_REGEX) fuzz-regex-seed
	mkdir -p $(FUZZ_ARTIFACTS)/regex
	./$(FUZZBIN_REGEX) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/regex/ \
		$(FUZZ_CORPUS)/regex

# Replay every tracked seed once, without mutation: a fast check that the
# checked-in regression inputs still compile, run and stay clean.
fuzz-regex-seed-replay: $(FUZZBIN_REGEX)
	mkdir -p $(FUZZ_ARTIFACTS)/regex
	./$(FUZZBIN_REGEX) -runs=0 \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/regex/ \
		$(FUZZ_SEEDS_REGEX)/*

fuzz-localvars: $(FUZZBIN_LOCALVARS)

fuzz-localvars-seed:
	mkdir -p $(FUZZ_CORPUS)/localvars
	cp -f $(FUZZ_SEEDS)/localvars/* $(FUZZ_CORPUS)/localvars/

fuzz-localvars-smoke: $(FUZZBIN_LOCALVARS) fuzz-localvars-seed
	mkdir -p $(FUZZ_ARTIFACTS)/localvars
	./$(FUZZBIN_LOCALVARS) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/localvars/ \
		$(FUZZ_CORPUS)/localvars

fuzz-compile-parse: $(FUZZBIN_COMPILE_PARSE)

fuzz-compile-parse-seed:
	mkdir -p $(FUZZ_CORPUS)/compile_parse
	cp -f $(FUZZ_SEEDS)/compile_parse/* $(FUZZ_CORPUS)/compile_parse/

fuzz-compile-parse-smoke: $(FUZZBIN_COMPILE_PARSE) fuzz-compile-parse-seed
	mkdir -p $(FUZZ_ARTIFACTS)/compile_parse
	./$(FUZZBIN_COMPILE_PARSE) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/compile_parse/ \
		$(FUZZ_CORPUS)/compile_parse

fuzz-seed: fuzz-keypress-seed fuzz-dirlocals-seed fuzz-regex-seed fuzz-localvars-seed fuzz-compile-parse-seed

fuzz-smoke: fuzz-keypress-smoke fuzz-dirlocals-smoke fuzz-regex-smoke fuzz-localvars-smoke fuzz-compile-parse-smoke

# Randomised differential test against Emacs' own matcher.  Not part of
# `check`: it needs emacs on PATH, and skips itself with a message when it
# is missing.  Bump REGEX_DIFF_CASES to hunt, keep the default quick.
check-regex-differential: $(REGEX_DIFF_BIN)
	@$(PYTHON) utils/regex_differential.py --driver $(REGEX_DIFF_BIN) \
		--emacs $(EMACS) --cases $(REGEX_DIFF_CASES) \
		--seed $(REGEX_DIFF_SEED)

complexity:
	$(SCC) --ci --by-file --sort complexity $(SCC_PATHS)

complexity-check:
	$(SCC) --ci --by-file --format json $(SCC_COMPLEXITY_PATHS) | \
		$(PYTHON) utils/check_scc_complexity.py \
			--max-total $(SCC_COMPLEXITY_MAX) \
			--max-file $(SCC_FILE_COMPLEXITY_MAX)

pmccabe:
	$(PMCCABE) $(PMCCABE_PATHS) | sort -nr

pmccabe-check:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		$(PYTHON) utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX) \
			--max-new-function $(PMCCABE_NEW_FUNCTION_MAX) \
			--baseline $(PMCCABE_BASELINE)

pmccabe-baseline:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		$(PYTHON) utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX) \
			--max-new-function $(PMCCABE_NEW_FUNCTION_MAX) \
			--write-baseline $(PMCCABE_BASELINE)

gateway-check:
	$(PYTHON) utils/check_mutation_gateway.py \
		--manifest $(GATEWAY_MANIFEST) $(OBJDIR)

gateway-baseline:
	$(PYTHON) utils/check_mutation_gateway.py \
		--manifest $(GATEWAY_MANIFEST) --write $(OBJDIR)

coverage: coverage-clean
	$(MAKE) clean
	mkdir -p $(COVERAGE_DIR)
	$(MAKE) $(TARGET) $(TESTBINS) CFLAGS="$(COVERAGE_CFLAGS)" \
		FE_CFLAGS="$(COVERAGE_CFLAGS)"
	lcov $(COVERAGE_LCOV_ARGS) --capture --initial --directory . \
		--output-file $(COVERAGE_DIR)/base.info
	$(MAKE) check CFLAGS="$(COVERAGE_CFLAGS)" \
		FE_CFLAGS="$(COVERAGE_CFLAGS)"
	lcov $(COVERAGE_LCOV_ARGS) --capture --directory . \
		--output-file $(COVERAGE_DIR)/run.info
	lcov $(COVERAGE_LCOV_ARGS) \
		--add-tracefile $(COVERAGE_DIR)/base.info \
		--add-tracefile $(COVERAGE_DIR)/run.info \
		--output-file $(COVERAGE_DIR)/kg.info
	lcov $(COVERAGE_LCOV_ARGS) --extract $(COVERAGE_DIR)/kg.info \
		'$(CURDIR)/src/*.c' --output-file $(COVERAGE_DIR)/src.info
	genhtml $(COVERAGE_GENHTML_ARGS) $(COVERAGE_DIR)/src.info \
		--output-directory $(COVERAGE_DIR)/html
	lcov --summary $(COVERAGE_DIR)/src.info
	$(MAKE) coverage-check

# Hold every file to the rate it had when the baseline was recorded.  Run
# it separately to re-check an existing tracefile without a 60 s rebuild.
coverage-check:
	@$(PYTHON) utils/check_coverage.py $(COVERAGE_DIR)/src.info \
		--baseline $(COVERAGE_BASELINE) --root $(CURDIR)

coverage-baseline:
	@$(PYTHON) utils/check_coverage.py $(COVERAGE_DIR)/src.info \
		--baseline $(COVERAGE_BASELINE) --root $(CURDIR) \
		--write-baseline --note $(COVERAGE_BASELINE_NOTE) \
		--how $(COVERAGE_BASELINE_HOW)

coverage-clean:
	rm -rf $(COVERAGE_DIR)
	find $(OBJDIR) $(TESTDIR) \( -name '*.gcda' -o -name '*.gcno' \) -delete

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

compile-db:
	$(BEAR) -- $(MAKE) CC="$(CLANG_CC)" -B

iwyu:
	@test -f $(COMPILE_DB_FILE) || { \
		echo "$(COMPILE_DB_FILE) missing; run 'make compile-db' first"; \
		exit 2; \
	}
	@command -v "$(IWYU)" >/dev/null 2>&1 || { \
		echo "include-what-you-use not found (tried '$(IWYU)');" \
		     "install it, or set IWYU=/path/to/include-what-you-use" >&2; \
		exit 2; \
	}
	@command -v "$(IWYU_TOOL)" >/dev/null 2>&1 || { \
		echo "iwyu_tool.py not found (tried '$(IWYU_TOOL)');" \
		     "install it, or set IWYU_TOOL=/path/to/iwyu_tool.py" >&2; \
		exit 2; \
	}
	PATH="$$(dirname "$(IWYU)"):$${PATH}" \
		$(IWYU_TOOL) -p . $(IWYU_FILES) -- $(IWYU_ARGS)

# Per-test linker prerequisites beyond the common test_%.o + test.o.
# The static pattern rule below pulls these in via secondary expansion.
EXTRA_undo         := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(OBJDIR)/cmdstate.o
EXTRA_buffer       := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
EXTRA_syntax       := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS)
EXTRA_yank         := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(OBJDIR)/cmdstate.o
EXTRA_autocomplete := $(TESTDIR)/stubs.o $(TESTDIR)/stubs_extra.o $(OBJDIR)/autocomplete.o $(TEST_SRCS_OBJS)
EXTRA_word         := $(TESTDIR)/stubs_noyank.o $(TESTDIR)/stubs_extra.o $(OBJDIR)/word.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(OBJDIR)/cmdstate.o
EXTRA_basic        := $(TESTDIR)/stubs.o          $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(OBJDIR)/vgeom.o $(TEST_SRCS_OBJS) $(OBJDIR)/cmdstate.o
# The geometry index's own unit tests need exactly what test_basic needs
# to reach get_visual_row()/find_visual_row()/goto_visual_row_col(): real
# basic.o (editor_cursor_goto(), editor_row_insert_char()), mode.o (the
# per-row primitives the index rebuilds from) and vgeom.o itself.
EXTRA_vgeom        := $(EXTRA_basic)
EXTRA_region       := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(OBJDIR)/cmdstate.o
EXTRA_shell        := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/shell.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o
EXTRA_complete     := $(TESTDIR)/stubs.o          $(OBJDIR)/path.o $(TEST_SRCS_OBJS)
EXTRA_lisp         := $(TESTDIR)/stubs_noyank.o $(TESTDIR)/stubs_lispobj.o $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(OBJDIR)/vgeom.o $(OBJDIR)/word.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(LISP_OBJS) $(OBJDIR)/keybind.o $(FE_OBJ) $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o $(OBJDIR)/keymap.o $(REGEX_OBJS)
EXTRA_regex        := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS) $(REGEX_OBJS)
EXTRA_localvars    := $(TESTDIR)/stubs.o          $(OBJDIR)/localvars.o $(TEST_SRCS_OBJS)
EXTRA_compile     := $(TESTDIR)/stubs_noyank.o  $(OBJDIR)/compile.o $(OBJDIR)/process.o
# compile_nav.c is compile.c's optional hook implementation: it needs a real
# buffer to attach markers/decorations to, so it links like EXTRA_buffer
# (real bufmgr.o, TEST_SRCS_OBJS' marker.o/decor.o) rather than EXTRA_compile's
# streaming-only set.  editor_next_error()/editor_previous_error() also call
# editor_goto_line_direct(), which stubs_buffer.c stubs as a no-op -- the
# native suite exercises the record/cursor state machine only, per Plan 05
# Bundle D; the point-placement half is PTY-only.
EXTRA_compile_nav := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(OBJDIR)/compile_parse.o $(OBJDIR)/compile_nav.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
# compile_parse.c is pure: no editor state, nothing beyond def.h's checked
# arithmetic/ASCII helpers.  Same minimal baseline as EXTRA_localvars.
EXTRA_compile_parse := $(TESTDIR)/stubs.o       $(OBJDIR)/compile_parse.o $(TEST_SRCS_OBJS)
EXTRA_tty         := $(TESTDIR)/stubs.o          $(OBJDIR)/tty.o $(OBJDIR)/fileio.o $(OBJDIR)/keyevent.o $(TEST_SRCS_OBJS)
EXTRA_minibuf     := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
EXTRA_dired       := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
# The command table is only reachable by linking cmd.o, which reaches
# most of the editor; the same everything-but-main.c link test_perf uses
# is cheaper than stubbing 74 handlers.
EXTRA_cmd         := $(TESTDIR)/stubs_main.o $(filter-out $(OBJDIR)/main.o,$(OBJS)) $(REGEX_OBJS) $(FE_OBJ)
# The binding inventory reads both the command table and kbd.c's key-level
# read-only verdict, so it links the same everything-but-main.c set.
EXTRA_keys        := $(EXTRA_cmd)
# The key module answers questions about keys and reaches for nothing but
# the UTF-8 decoder it shares with the display; the harness itself needs
# the editor globals the other stub links provide.
EXTRA_keymap      := $(EXTRA_cmd)
# describe-bindings renders the real maps into a real buffer, so it needs
# the same everything-but-main.c set the command table does.
EXTRA_describe    := $(EXTRA_cmd)
EXTRA_keyevent    := $(TESTDIR)/stubs.o $(OBJDIR)/keyevent.o $(TEST_SRCS_OBJS)
EXTRA_winmgr      := $(TESTDIR)/stubs_buffer.o   $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(OBJDIR)/winmgr.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
EXTRA_marker      := $(EXTRA_buffer)
EXTRA_decor       := $(EXTRA_buffer)
EXTRA_event       := $(EXTRA_buffer) $(OBJDIR)/event.o
# The register table stores markers into real buffers and its commands
# reach the region text and the insertion path, so it links the same
# buffer-backed set as EXTRA_marker plus its own object.
EXTRA_register    := $(EXTRA_buffer) $(OBJDIR)/register.o
# The process table's drain subscriber commits into a real buffer via
# kg_buffer_replace(), so this needs the same buffer-backed set EXTRA_event
# does; process_table.o itself is already pulled in by TEST_SRCS_OBJS (see
# its comment), named again here only for readability.
EXTRA_process_table := $(EXTRA_buffer) $(OBJDIR)/event.o $(OBJDIR)/process_table.o

.SECONDEXPANSION:
$(filter-out $(TESTDIR)/test_perf,$(TESTBINS)): $(TESTDIR)/test_%: $(TESTDIR)/test_%.o $(TESTDIR)/test.o $$(EXTRA_$$*)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERFOBJDIR):
	mkdir -p $@

$(PERFOBJDIR)/%.o: $(OBJDIR)/%.c $(HDRS) $(OBJDIR)/perf.h | $(PERFOBJDIR)
	$(CC) $(PERF_CFLAGS) -c $< -o $@

$(PERFOBJDIR)/%.o: $(TESTDIR)/%.c $(HDRS) $(OBJDIR)/perf.h | $(PERFOBJDIR)
	$(CC) $(PERF_CFLAGS) -I$(OBJDIR) -c $< -o $@

$(PERFOBJDIR)/lisp_core.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp.h
$(PERFOBJDIR)/lisp_prelude.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/lisp_string.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/lisp_buffer.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/lisp_word.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/lisp_io.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/lisp_cmd.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/lisp_obj.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_obj.h
$(PERFOBJDIR)/lisp_process.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_obj.h $(OBJDIR)/lisp_process.h $(OBJDIR)/process.h $(OBJDIR)/process_table.h
$(PERFOBJDIR)/lisp_require.o: $(OBJDIR)/lisp_internal.h
$(PERFOBJDIR)/regex.o: $(OBJDIR)/regex.h fe/tiny-regex-c/re.h

$(TESTDIR)/test_perf: $(PERF_TEST_OBJS) $(FE_OBJ) $(REGEX_ENGINE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERF_KG): $(PERF_SRC_OBJS) $(FE_OBJ) $(REGEX_ENGINE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Wall-clock benchmarks, deliberately not a CI gate: see the note in
# CLAUDE.md.  The gates that hold are the counter assertions in
# test/test_perf.c, which run inside every `make check`.
bench: $(PERF_KG)
	@mkdir -p $(dir $(BENCH_OUT))
	@$(PYTHON) utils/bench.py --kg $(PERF_KG) --json $(BENCH_OUT) \
		$(BENCH_ARGS)

# Not part of `bench` above, and not folded into it: this clean-rebuilds
# both WITH_LISP configurations (restoring WITH_LISP=1, the default,
# afterward), so it is slower and more disruptive than the fast path
# `bench` is, and clobbers whatever src/*.o a plain `make` had left --
# `bench` itself never touches src/*.o, only test/perfobj/*.o. It reports
# the two baseline items `bench` cannot: kg startup time with vs. without
# Lisp (wall time only -- a WITH_LISP=0 binary has no Lisp counters and
# neither side here is a counting build, so it is not comparable to
# `bench`'s own "startup" case; see utils/bench.py's --kg-no-lisp help)
# and binary size in both configurations.
BENCH_TOGGLE_DIR = $(TESTDIR)/.bench
BENCH_TOGGLE_OUT ?= $(TESTDIR)/.results/bench-lisp-toggle.json

bench-lisp-toggle:
	@mkdir -p $(BENCH_TOGGLE_DIR) $(dir $(BENCH_TOGGLE_OUT))
	$(MAKE) clean
	$(MAKE) $(TARGET)
	cp $(TARGET) $(BENCH_TOGGLE_DIR)/kg-with-lisp
	$(MAKE) clean
	$(MAKE) WITH_LISP=0 $(TARGET)
	cp $(TARGET) $(BENCH_TOGGLE_DIR)/kg-no-lisp
	$(MAKE) clean
	$(MAKE) $(PERF_KG)
	$(PYTHON) utils/bench.py --kg $(PERF_KG) --runs 3 \
		--case startup --case startup-no-lisp \
		--kg-no-lisp $(BENCH_TOGGLE_DIR)/kg-no-lisp \
		--binary-size $(BENCH_TOGGLE_DIR)/kg-with-lisp=$(BENCH_TOGGLE_DIR)/kg-no-lisp \
		--json $(BENCH_TOGGLE_OUT)
	$(MAKE) $(TARGET)

$(TESTDIR)/%.o: $(TESTDIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -I$(OBJDIR) -c $< -o $@

$(TESTDIR)/test_lisp.o: $(OBJDIR)/lisp.h

$(FUZZBIN): $(FUZZ_SRCS) $(HDRS) $(FUZZ_FE_OBJ) $(LISP_CONFIG)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -Ife/tiny-regex-c -o $@ $(FUZZ_SRCS) \
		$(FUZZ_FE_OBJ) $(LDLIBS)

$(FUZZBIN_DIRLOCALS): $(TESTDIR)/fuzz_dirlocals.c $(OBJDIR)/localvars.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_dirlocals.c $(OBJDIR)/localvars.c

$(FUZZBIN_REGEX): $(TESTDIR)/fuzz_regex.c $(OBJDIR)/regex.c $(OBJDIR)/regex.h fe/tiny-regex-c/re.c fe/tiny-regex-c/re.h
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -Ife/tiny-regex-c -o $@ \
		$(TESTDIR)/fuzz_regex.c $(OBJDIR)/regex.c fe/tiny-regex-c/re.c

$(REGEX_DIFF_BIN): $(TESTDIR)/regex_differential.c $(OBJDIR)/regex.c $(OBJDIR)/regex.h fe/tiny-regex-c/re.c fe/tiny-regex-c/re.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/regex_differential.c $(OBJDIR)/regex.c \
		fe/tiny-regex-c/re.c

$(FUZZBIN_LOCALVARS): $(TESTDIR)/fuzz_localvars.c $(OBJDIR)/localvars.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_localvars.c $(OBJDIR)/localvars.c

$(FUZZBIN_COMPILE_PARSE): $(TESTDIR)/fuzz_compile_parse.c $(OBJDIR)/compile_parse.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_compile_parse.c $(OBJDIR)/compile_parse.c

$(TESTDIR)/fe_fuzz.o: fe/fe.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

$(TESTDIR)/fe_eval_fuzz.o: fe/fe_eval.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(FE_OBJ) $(REGEX_OBJS) $(OBJDIR)/.with-lisp-* $(TESTDIR)/*.o \
	      $(TESTBINS) $(FUZZBINS) $(REGEX_DIFF_BIN)
	rm -rf $(PERFOBJDIR)

distclean: clean
	rm -f $(TARGET) $(TESTBINS)
	find . -name '*~' -o -name '*.orig' -o -name '*.rej' \
	       -o -name '*.bak' -o -name '*.swp' -o -name '.*.swp' | xargs rm -f
	rm -f core DEADJOE

deb:
	dpkg-buildpackage -b -us -uc

release:
	sh utils/mkrel.sh

install: $(TARGET)
	install -d $(DESTDIR)$(bindir)
	install -m 755 -s $(TARGET) $(DESTDIR)$(bindir)/$(PROG)
	install -d $(DESTDIR)$(man1dir)
	install -m 644 $(MAN1) $(DESTDIR)$(man1dir)/$(PROG).1

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(PROG)
	rm -f $(DESTDIR)$(man1dir)/$(PROG).1

.PHONY: all clean distclean check header-check lisp-include-check docs-check lisp-compat-check lisp-compat-oracle lisp-prelude-generate lisp-prelude-check check-unit check-pty check-regex-differential \
	bench bench-lisp-toggle complexity complexity-check \
	pmccabe pmccabe-check pmccabe-baseline gateway-check gateway-baseline coverage coverage-check coverage-baseline coverage-clean format format-check compile-db iwyu \
	fuzz-keypress fuzz-keypress-seed fuzz-keypress-smoke \
	fuzz-dirlocals fuzz-dirlocals-seed fuzz-dirlocals-smoke \
	fuzz-regex fuzz-regex-seed fuzz-regex-smoke fuzz-regex-seed-replay \
	fuzz-localvars fuzz-localvars-seed fuzz-localvars-smoke \
	fuzz-compile-parse fuzz-compile-parse-seed fuzz-compile-parse-smoke \
	fuzz-seed fuzz-smoke \
	deb release install uninstall
