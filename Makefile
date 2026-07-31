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
ifeq ($(filter-out clean distclean coverage-clean,$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),)
SKIP_FE_CHECK = 1
endif
endif
ifneq ($(SKIP_FE_CHECK),1)
$(error fe/fe.c is missing; run 'git submodule update --init --recursive' or build with 'WITH_LISP=0')
endif
endif
override CFLAGS += -DKG_USE_LISP=1
override LDLIBS += -lm
FE_OBJ = $(OBJDIR)/fe.o
FUZZ_FE_OBJ = $(TESTDIR)/fe_fuzz.o
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

# Source files
SRCS = main.c tty.c syntax.c autocomplete.c buffer.c fileio.c \
       display.c search.c basic.c word.c kbd.c yank.c undo.c help.c bufmgr.c winmgr.c cmd.c macro.c \
       shell.c path.c rect.c lisp.c keybind.c mode.c localvars.c compile.c \
       width.c dired.c perf.c

# Object and header files
OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
REGEX_ENGINE_OBJ = $(OBJDIR)/tiny_regex.o
REGEX_WRAPPER_OBJ = $(OBJDIR)/regex.o
REGEX_OBJS = $(REGEX_ENGINE_OBJ) $(REGEX_WRAPPER_OBJ)
HDRS = $(OBJDIR)/def.h

# Test infrastructure
TESTDIR  = test
TESTBINS = $(TESTDIR)/test_undo $(TESTDIR)/test_buffer \
           $(TESTDIR)/test_syntax $(TESTDIR)/test_yank \
           $(TESTDIR)/test_autocomplete $(TESTDIR)/test_word \
           $(TESTDIR)/test_basic $(TESTDIR)/test_region \
           $(TESTDIR)/test_shell $(TESTDIR)/test_complete \
           $(TESTDIR)/test_lisp $(TESTDIR)/test_regex \
           $(TESTDIR)/test_localvars $(TESTDIR)/test_compile \
           $(TESTDIR)/test_tty $(TESTDIR)/test_minibuf \
           $(TESTDIR)/test_dired $(TESTDIR)/test_winmgr \
           $(TESTDIR)/test_cmd $(TESTDIR)/test_perf
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
# were instrumented (fe.o).
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
	    $(OBJDIR)/tty.c $(OBJDIR)/macro.c $(OBJDIR)/lisp.c \
	    $(OBJDIR)/keybind.c $(OBJDIR)/width.c
FUZZBIN_DIRLOCALS = $(TESTDIR)/fuzz_dirlocals
FUZZBIN_REGEX    = $(TESTDIR)/fuzz_regex
FUZZBIN_LOCALVARS = $(TESTDIR)/fuzz_localvars
FUZZBINS = $(FUZZBIN) $(FUZZBIN_DIRLOCALS) $(FUZZBIN_REGEX) $(FUZZBIN_LOCALVARS)
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
TEST_SRCS_OBJS = $(OBJDIR)/undo.o $(OBJDIR)/buffer.o $(OBJDIR)/syntax.o \
                 $(OBJDIR)/width.o
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
# ratchet raise of the 2026-07-30 review program, per
# doc/reviews/2026-07-30/plans/p0-evidence-baselines-and-budgets.md phase 3.
# Measured 4193 after that plan's extraction pass, so the program starts
# with 87 points of headroom to spend across plans 01-15; nothing raises
# this again without a reviewed exception.  (History: 4199 -> 4208 for the
# path picker's literal-accept answers.)
SCC_COMPLEXITY_MAX ?= 4245
SCC_FILE_COMPLEXITY_MAX ?= 520
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(addprefix $(OBJDIR)/,$(SRCS))
# Lowered 120 -> 110 after editor_process_keypress() shed its kill-lines,
# repeated-yank, shift-motion, recenter and end-of-keypress bookkeeping
# into helpers: that function measures 85 and the worst function in the
# tree is now localvars_parse_footer at 100, so 110 is the budget plans
# 01-15 have to stay under.  Lower it, do not raise it.  (History: 130 ->
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
# gateway plan 10 is growing: raw row primitives, hand-written undo
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
IWYU ?= /opt-3/iwyu-21/bin/include-what-you-use
IWYU_TOOL ?= /opt-3/iwyu-21/bin/iwyu_tool.py
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

$(OBJDIR)/lisp.o: $(OBJDIR)/lisp.c $(OBJDIR)/lisp.h
$(OBJDIR)/main.o: $(OBJDIR)/lisp.h

$(OBJDIR)/fe.o: fe/fe.c fe/fe.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

check: check-unit check-pty

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

fuzz-seed: fuzz-keypress-seed fuzz-dirlocals-seed fuzz-regex-seed fuzz-localvars-seed

fuzz-smoke: fuzz-keypress-smoke fuzz-dirlocals-smoke fuzz-regex-smoke fuzz-localvars-smoke

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
	PATH="$$(dirname "$(IWYU)"):$${PATH}" \
		$(IWYU_TOOL) -p . $(IWYU_FILES) -- $(IWYU_ARGS)

# Per-test linker prerequisites beyond the common test_%.o + test.o.
# The static pattern rule below pulls these in via secondary expansion.
EXTRA_undo         := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_buffer       := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(TEST_SRCS_OBJS)
EXTRA_syntax       := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS)
EXTRA_yank         := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_autocomplete := $(TESTDIR)/stubs.o $(TESTDIR)/stubs_extra.o $(OBJDIR)/autocomplete.o $(TEST_SRCS_OBJS)
EXTRA_word         := $(TESTDIR)/stubs_noyank.o $(TESTDIR)/stubs_extra.o $(OBJDIR)/word.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_basic        := $(TESTDIR)/stubs.o          $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(TEST_SRCS_OBJS)
EXTRA_region       := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_shell        := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/shell.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/buffer.o $(OBJDIR)/undo.o $(OBJDIR)/syntax.o $(OBJDIR)/width.o
EXTRA_complete     := $(TESTDIR)/stubs.o          $(OBJDIR)/path.o $(TEST_SRCS_OBJS)
EXTRA_lisp         := $(TESTDIR)/stubs_noyank.o          $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(OBJDIR)/word.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS) $(OBJDIR)/lisp.o $(OBJDIR)/keybind.o $(FE_OBJ)
EXTRA_regex        := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS) $(REGEX_OBJS)
EXTRA_localvars    := $(TESTDIR)/stubs.o          $(OBJDIR)/localvars.o $(TEST_SRCS_OBJS)
EXTRA_compile     := $(TESTDIR)/stubs_noyank.o  $(OBJDIR)/compile.o
EXTRA_tty         := $(TESTDIR)/stubs.o          $(OBJDIR)/tty.o $(OBJDIR)/fileio.o $(TEST_SRCS_OBJS)
EXTRA_minibuf     := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(TEST_SRCS_OBJS)
EXTRA_dired       := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(TEST_SRCS_OBJS)
# The command table is only reachable by linking cmd.o, which reaches
# most of the editor; the same everything-but-main.c link test_perf uses
# is cheaper than stubbing 74 handlers.
EXTRA_cmd         := $(TESTDIR)/stubs_main.o $(filter-out $(OBJDIR)/main.o,$(OBJS)) $(REGEX_OBJS) $(FE_OBJ)
EXTRA_winmgr      := $(TESTDIR)/stubs_buffer.o   $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(OBJDIR)/winmgr.o $(TEST_SRCS_OBJS)

.SECONDEXPANSION:
$(filter-out $(TESTDIR)/test_perf,$(TESTBINS)): $(TESTDIR)/test_%: $(TESTDIR)/test_%.o $(TESTDIR)/test.o $$(EXTRA_$$*)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERFOBJDIR):
	mkdir -p $@

$(PERFOBJDIR)/%.o: $(OBJDIR)/%.c $(HDRS) $(OBJDIR)/perf.h | $(PERFOBJDIR)
	$(CC) $(PERF_CFLAGS) -c $< -o $@

$(PERFOBJDIR)/%.o: $(TESTDIR)/%.c $(HDRS) $(OBJDIR)/perf.h | $(PERFOBJDIR)
	$(CC) $(PERF_CFLAGS) -I$(OBJDIR) -c $< -o $@

$(PERFOBJDIR)/lisp.o: $(OBJDIR)/lisp.h
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

$(TESTDIR)/%.o: $(TESTDIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -I$(OBJDIR) -c $< -o $@

$(TESTDIR)/test_lisp.o: $(OBJDIR)/lisp.h

$(FUZZBIN): $(FUZZ_SRCS) $(HDRS) $(FUZZ_FE_OBJ) $(LISP_CONFIG)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ $(FUZZ_SRCS) \
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

$(TESTDIR)/fe_fuzz.o: fe/fe.c fe/fe.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJDIR)/fe.o $(REGEX_OBJS) $(OBJDIR)/.with-lisp-* $(TESTDIR)/*.o \
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

.PHONY: all clean distclean check check-unit check-pty check-regex-differential \
	bench complexity complexity-check \
	pmccabe pmccabe-check pmccabe-baseline gateway-check gateway-baseline coverage coverage-check coverage-baseline coverage-clean format format-check compile-db iwyu \
	fuzz-keypress fuzz-keypress-seed fuzz-keypress-smoke \
	fuzz-dirlocals fuzz-dirlocals-seed fuzz-dirlocals-smoke \
	fuzz-regex fuzz-regex-seed fuzz-regex-smoke fuzz-regex-seed-replay \
	fuzz-localvars fuzz-localvars-seed fuzz-localvars-smoke \
	fuzz-seed fuzz-smoke \
	deb release install uninstall
