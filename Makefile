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
ifeq ($(wildcard fe/fe_run.c),)
FE_SPLIT_MISSING = 1
endif
ifeq ($(FE_SPLIT_MISSING),1)
ifeq ($(filter-out clean distclean coverage-clean,$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),)
SKIP_FE_CHECK = 1
endif
endif
ifneq ($(SKIP_FE_CHECK),1)
$(error one of fe/fe.c, fe/fe_eval.c, fe/fe_run.c is missing; run 'git submodule update --init --recursive' or build with 'WITH_LISP=0')
endif
endif
override CFLAGS += -DKG_USE_LISP=1
override LDLIBS += -lm
# The evaluator lives in its own translation unit since Fe sub-plan 03B
# (fe.c -> fe.c + fe_eval.c, behind a private fe/fe_internal.h), and the run
# driver plus the public FeEvaluate*/FeCall* surface in a third since Fe
# sub-plan 11B (fe_eval.c -> fe_eval.c + fe_run.c, which is how fe kept its
# 520 per-file cap binding while dynamic binding landed); a list so every
# consumer below is a one-line change.
FE_OBJ = $(OBJDIR)/fe.o $(OBJDIR)/fe_eval.o $(OBJDIR)/fe_run.o
FUZZ_FE_OBJ = $(TESTDIR)/fe_fuzz.o $(TESTDIR)/fe_eval_fuzz.o \
	      $(TESTDIR)/fe_run_fuzz.o
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

# Tree-sitter is the optional second highlighting backend, and it is off by
# default on purpose: kg has to keep building on a box with no package
# manager, so WITH_TREE_SITTER=0 stays the dependency-free configuration and
# the legacy scanners stay a first-class, permanently maintained backend
# (doc/plans/kg-tree-sitter-plan.md, Refinement decision 3).
#
# The dependency model is a prebuilt PREFIX, not a submodule (decision 1).
# TREE_SITTER_PREFIX names an install whose include/tree_sitter/api.h is the
# guard file; the core library is then an ordinary link-time dependency
# found under $(TREE_SITTER_PREFIX)/lib, with no pkg-config requirement.
# The default is this development environment's release prefix; the
# {debug,asan,msan} siblings are what a sanitizer lane would point at.
# -rpath is not decoration: the release and debug prefixes ship only a
# shared libtree-sitter, in a directory on no default loader path, so
# without it the editor links and then fails to start.  A static prefix
# (the asan and msan ones ship libtree-sitter.a) links from the same three
# flags and simply ignores the rpath.
#
# Grammars are NOT linked: they are dlopen'd at runtime by soname, Emacs'
# own model (decision 1).  TS_GRAMMAR_PATH is the compiled-in search path
# the loader falls back to when $KG_TS_GRAMMAR_PATH is unset or empty:
# colon-separated entries, each a directory holding
# libtree-sitter-<name>.so, and an entry containing `%s` has the grammar
# name substituted -- which is how /opt-9's one-prefix-per-grammar layout
# is a single entry rather than twenty.
WITH_TREE_SITTER ?= 0
TREE_SITTER_PREFIX ?= /opt-9/tree-sitter-v0.26.11-release
TS_GRAMMAR_PATH ?= /opt-9/tree-sitter-grammar-%s/lib

ifneq ($(WITH_TREE_SITTER),0)
ifneq ($(WITH_TREE_SITTER),1)
$(error WITH_TREE_SITTER must be 0 or 1)
endif
endif
ifeq ($(WITH_TREE_SITTER),1)
ifeq ($(wildcard $(TREE_SITTER_PREFIX)/include/tree_sitter/api.h),)
ifeq ($(filter-out clean distclean coverage-clean,$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),)
SKIP_TREE_SITTER_CHECK = 1
endif
endif
ifneq ($(SKIP_TREE_SITTER_CHECK),1)
$(error $(TREE_SITTER_PREFIX)/include/tree_sitter/api.h is missing; point TREE_SITTER_PREFIX=... at a tree-sitter install prefix, or build with 'WITH_TREE_SITTER=0')
endif
endif
override CFLAGS += -DKG_USE_TREE_SITTER=1 -I$(TREE_SITTER_PREFIX)/include \
		   -DKG_TS_GRAMMAR_DEFAULT_PATH='"$(TS_GRAMMAR_PATH)"'
# In LDLIBS rather than LDFLAGS because two link lines (test/kgbatch and
# the keypress fuzz target) pass LDLIBS and not LDFLAGS; -L before -l in
# one variable resolves the same way, and LDLIBS is last on every line.
# -ldl for the grammar loader's dlopen().  Redundant on a glibc >= 2.34,
# where libdl was folded into libc and libdl.so is a stub kept for exactly
# this line; still required on older glibc and on the BSDs' libc split.
override LDLIBS += -L$(TREE_SITTER_PREFIX)/lib \
		   -Wl,-rpath,$(TREE_SITTER_PREFIX)/lib -ltree-sitter -ldl
endif

# Which implementation of the private src/syntax_backend.h contract this
# build compiles.  A source list, never an #ifdef: the backend that is not
# selected is not compiled at all, which is the property
# `test ! -e src/syntax_legacy.o` in .ci/ci-13-with-tree-sitter.sh asserts.
ifeq ($(WITH_TREE_SITTER),1)
SYNTAX_BACKEND_SRCS = syntax_tree_sitter.c syntax_tree_sitter_lang.c
else
SYNTAX_BACKEND_SRCS = syntax_legacy.c
endif
SYNTAX_BACKEND_OBJS = $(addprefix $(OBJDIR)/,$(SYNTAX_BACKEND_SRCS:.c=.o))
# Both backends' objects and test binaries, named so `make clean` removes
# the one this configuration did not build.  $(OBJS)/$(TESTBINS) only ever
# name the selected backend, so without this a `make WITH_TREE_SITTER=1;
# make clean` would leave src/syntax_tree_sitter.o behind for a later
# default build's linker to trip over.
SYNTAX_BACKEND_ALL = $(OBJDIR)/syntax_legacy.o $(PERFOBJDIR)/syntax_legacy.o \
		     $(OBJDIR)/syntax_tree_sitter.o \
		     $(PERFOBJDIR)/syntax_tree_sitter.o \
		     $(OBJDIR)/syntax_tree_sitter_lang.o \
		     $(PERFOBJDIR)/syntax_tree_sitter_lang.o \
		     $(TESTDIR)/test_syntax_legacy \
		     $(TESTDIR)/test_syntax_tree_sitter

# LSP is the optional language-server client (doc/plans/2026-08-08-lsp.md),
# and unlike tree-sitter it is ON by default: it has no build-time
# dependency of any kind -- servers are found at run time -- so WITH_LSP=1
# is still the dependency-free configuration, and defaulting it off would
# mean the main lanes never compiled the client at all.  There is
# correspondingly no prefix or guard file to check, only the 0/1
# validation; .ci/ci-14-with-lsp-0.sh is what keeps the disabled build
# honest, together with one orthogonality run against WITH_LISP=0.
WITH_LSP ?= 1

ifneq ($(WITH_LSP),0)
ifneq ($(WITH_LSP),1)
$(error WITH_LSP must be 0 or 1)
endif
endif
ifeq ($(WITH_LSP),1)
override CFLAGS += -DKG_USE_LSP=1
endif

# One stamp for the whole feature configuration, so an object compiled
# under one set of -D flags is never mistaken for up to date under another.
# It replaced LISP_CONFIG when the tree-sitter axis arrived, and gained the
# LSP axis with it: independent axes need one stamp between them, not one
# each, or the sequence `make WITH_LISP=0; make WITH_LISP=0
# WITH_TREE_SITTER=1` looks unchanged to make.  Lives in $(OBJDIR) beside
# the objects it guards.
FEATURE_CONFIG = \
    $(OBJDIR)/.features-lisp-$(WITH_LISP)-ts-$(WITH_TREE_SITTER)-lsp-$(WITH_LSP)

prefix  = /usr/local
bindir  = $(prefix)/bin
mandir  = $(prefix)/share/man
man1dir = $(mandir)/man1
datadir = $(prefix)/share
lispdir = $(datadir)/kg/lisp

# The Lisp `make install` ships, decided by sub-plan 10D Part 1 against
# how the *prelude* ships.  lisp/prelude.el is not here on purpose: it is
# compiled into the binary (utils/embed_lisp.py ->
# src/lisp_prelude_generated.inc, kept honest by `make
# lisp-prelude-check`), so it can never be missing, stale, or a version
# behind the editor that evaluates it.  A file on disk cannot have that
# property, and installing a second copy of the prelude beside the
# compiled-in one would only invite someone to edit the copy nothing
# reads.
#
# The three files below are the opposite kind of thing: optional packages
# a user chooses to (require ...), which by definition have to be files
# the load-path can reach.  README.md and doc/kg.1 tell users to require
# them, so they are installed, and both say where -- $(lispdir), which is
# NOT on the default load-path (that is $XDG_CONFIG_HOME/kg/lisp, a
# per-user directory), so the docs also say the (add-to-load-path ...)
# line that reaches it.  Baking an install prefix into the binary's
# default load-path was the alternative and was rejected: it is C in the
# editor for a path a one-line init file states better.
#
# pipeline.el and pipeline-text.el ship as a pair on purpose:
# pipeline-text.el's first form is (require 'pipeline), so installing one
# without the other would install something that cannot load.
LISP_PACKAGES = lisp/auto-fill.el lisp/pipeline.el lisp/pipeline-text.el

# Show a leading "~" on lines past end-of-buffer (vim/kilo style).
# Off by default for an Emacs-like presentation.  Override on the make
# command line, e.g. `make KG_SHOW_TILDE=1`.
KG_SHOW_TILDE ?= 0
override CFLAGS += -DKG_SHOW_TILDE=$(KG_SHOW_TILDE)

# Required for POSIX/GNU interfaces when source files include system headers
# directly, as enforced by Include What You Use.  Only glibc (Linux/Cygwin)
# needs the nudge -- src/def.h guards its own copy of these defines the same
# way.  BSD libc's feature_test macro chain never re-enables __BSD_VISIBLE
# once _POSIX_C_SOURCE is defined (there is no _DEFAULT_SOURCE rescue as on
# glibc), so requesting it there hides BSD-only declarations kg needs
# (SIGWINCH, gettimeofday) with nothing to bring them back; BSD's unset-macro
# default already exposes the full POSIX + XSI + BSD surface kg uses.
ifneq (,$(filter Linux CYGWIN%,$(shell uname -s)))
override CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

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

# The LSP client's one editor-facing header is src/lsp.h, and lsp_core.c is
# always built -- the LISP_SRCS shape above, for the same reason: the
# facade's entry points exist in both configurations, so no caller grows a
# KG_USE_LSP conditional.  Stage 0 of doc/plans/2026-08-08-lsp.md ships the
# facade inert, so lsp_core.c is the same no-ops either way, and that is
# when lsp_core.c's halves will start to differ.  Everything BEHIND the
# facade is WITH_LSP=1 only, and lsp_transport.c (Stage 1) is the first of
# it: the JSON, client and server-registry files join the same list.
LSP_SRCS = lsp_core.c
ifeq ($(WITH_LSP),1)
LSP_SRCS += lsp_transport.c lsp_json.c lsp_uri.c lsp_client.c lsp_server.c \
            lsp_sync.c
endif
LSP_OBJS = $(addprefix $(OBJDIR)/,$(LSP_SRCS:.c=.o))
# Named so `make clean` removes what THIS configuration did not build,
# the way SYNTAX_BACKEND_ALL does for the syntax backends: without it a
# `make; make WITH_LSP=0 clean` would leave src/lsp_transport.o and the
# transport's test binary behind.
LSP_ALL = $(OBJDIR)/lsp_transport.o $(TESTDIR)/test_lsp_transport \
          $(OBJDIR)/lsp_json.o $(TESTDIR)/test_lsp_json \
          $(OBJDIR)/lsp_uri.o \
          $(OBJDIR)/lsp_client.o $(OBJDIR)/lsp_server.o \
          $(TESTDIR)/test_lsp_client \
          $(OBJDIR)/lsp_sync.o $(TESTDIR)/test_lsp_sync

# Source files
SRCS = main.c tty.c syntax.c $(SYNTAX_BACKEND_SRCS) autocomplete.c buffer.c fileio.c \
       display.c search.c basic.c word.c kbd.c yank.c undo.c help.c describe.c bufmgr.c winmgr.c cmd.c cmdstate.c keyevent.c keymap.c macro.c \
       shell.c path.c rect.c $(LISP_SRCS) $(LSP_SRCS) keybind.c mode.c vgeom.c localvars.c compile.c compile_parse.c \
       compile_nav.c register.c \
       width.c dired.c perf.c platform.c process.c process_table.c marker.c decor.c event.c

# Object and header files
OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
REGEX_ENGINE_OBJ = $(OBJDIR)/tiny_regex.o
REGEX_WRAPPER_OBJ = $(OBJDIR)/regex.o
REGEX_OBJS = $(REGEX_ENGINE_OBJ) $(REGEX_WRAPPER_OBJ)
# Every header in src/ is checked for compiling on its own (see
# `header-check`): a module header that only works once def.h has been
# included is def.h with extra steps.
ALL_HDRS = $(sort $(wildcard $(OBJDIR)/*.h))
# Every object depends on every src header.  HDRS used to name def.h alone,
# and every other header reached an object only through a hand-maintained
# per-object line -- the exact defect the lisp_obj.h fix wrote out (a header
# deciding a struct layout every TU reaches for, with make never told), found
# again nine headers wide: 611 undeclared edges across 54 objects, measured.
# The price is a full rebuild on any header edit; the alternative was the
# silent-ABI-skew class of bug, which costs more.
HDRS = $(ALL_HDRS)

# Test infrastructure
TESTDIR  = test
TESTBINS = $(TESTDIR)/test_undo $(TESTDIR)/test_buffer \
           $(TESTDIR)/test_syntax \
           $(TESTDIR)/test_yank \
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
# Each backend's own suite exists only where that backend does: it links
# that backend's object and asserts what it paints, so neither is a suite
# the other configuration can run at all.  test_syntax_legacy asserts the
# bespoke scanners; test_syntax_tree_sitter asserts grammar loading, query
# compilation and capture-to-render-offset painting, and hard-requires the
# C grammar, since a WITH_TREE_SITTER=1 build only happens on a box that
# has one.
ifeq ($(WITH_TREE_SITTER),0)
TESTBINS += $(TESTDIR)/test_syntax_legacy
else
TESTBINS += $(TESTDIR)/test_syntax_tree_sitter
endif
# Same per-axis rule: the transport's suite links src/lsp_transport.o,
# which only a WITH_LSP=1 build has.  A WITH_LSP=0 tree has no transport
# to test -- the facade it does have is three no-ops that every other
# binary already links.
ifeq ($(WITH_LSP),1)
TESTBINS += $(TESTDIR)/test_lsp_transport $(TESTDIR)/test_lsp_json \
            $(TESTDIR)/test_lsp_client $(TESTDIR)/test_lsp_sync
endif
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
	    $(addprefix $(OBJDIR)/,$(SYNTAX_BACKEND_SRCS)) \
	    $(OBJDIR)/tty.c $(OBJDIR)/macro.c \
	    $(addprefix $(OBJDIR)/,$(LISP_SRCS)) \
	    $(addprefix $(OBJDIR)/,$(LSP_SRCS)) \
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
# $(LSP_OBJS) is here for the same reason: the LSP facade is called from
# tty.c's idle poll and from editor_cleanup() (src/bufmgr.c), so every test
# binary linking either one needs it.  It is one object of no-ops today.
TEST_SRCS_OBJS = $(OBJDIR)/undo.o $(OBJDIR)/buffer.o $(OBJDIR)/syntax.o \
                 $(SYNTAX_BACKEND_OBJS) \
                 $(OBJDIR)/width.o $(OBJDIR)/marker.o $(OBJDIR)/decor.o \
                 $(OBJDIR)/cmdstate.o $(OBJDIR)/event.o \
                 $(OBJDIR)/process.o $(OBJDIR)/process_table.o \
                 $(LSP_OBJS)
# The tree-sitter backend converts a capture's chars-space columns into
# render-byte offsets with chars_to_render_col() (src/mode.c), so in that
# configuration every test binary that links a backend needs mode.o too.
# It costs nothing: mode.o reaches only for utf8_width_at(), which width.o
# above already provides, and $^ in the link rule dedupes it against the
# EXTRA_ lists that name it themselves.
ifeq ($(WITH_TREE_SITTER),1)
TEST_SRCS_OBJS += $(OBJDIR)/mode.o
endif
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
# Minimum seconds a tmux case waits after its last key before the harness
# may call the pane settled.  Empty here (a plain `make check` does not
# need it); .ci/ci-env.sh sets it, and higher under --parallel.
PTY_SETTLE_FLOOR ?=
PTY_JOBS ?=
FUZZ_CFLAGS ?= -Wall -Wextra -pedantic -std=c23 -O1 -g \
	       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DKG_FUZZ=1 \
	       -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
FE_FUZZ_CFLAGS ?= $(FUZZ_CFLAGS)

ifeq ($(WITH_LISP),1)
override FUZZ_CFLAGS += -DKG_USE_LISP=1
endif
ifeq ($(WITH_LSP),1)
override FUZZ_CFLAGS += -DKG_USE_LSP=1
endif
# FUZZ_CFLAGS is a complete flag set of its own rather than CFLAGS plus
# extras, so the feature defines have to be repeated here; the link side
# needs nothing, the keypress target already passing $(LDLIBS).
ifeq ($(WITH_TREE_SITTER),1)
override FUZZ_CFLAGS += -DKG_USE_TREE_SITTER=1 \
			-I$(TREE_SITTER_PREFIX)/include \
			-DKG_TS_GRAMMAR_DEFAULT_PATH='"$(TS_GRAMMAR_PATH)"'
# Same reason TEST_SRCS_OBJS gains it above: the tree-sitter backend calls
# chars_to_render_col(), which lives in src/mode.c.
FUZZ_SRCS += $(OBJDIR)/mode.c
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
# Raised 5660 -> 5730 by the Phase 7 review-fix cycle (2026-08-06). 07A's
# 5660 funded the interactive work as planned; it did not fund repairing it,
# and the phase had already spent 5489 -> 5656 of it (07D +89, 07E +78
# against its own +30..50 price). The fixes are structural rather than
# additive -- a per-activation command scope that survives an Fe unwind, a
# real wrong-type-argument raise instead of prose, a non-allocating numeric
# classifier, an uncapped raw universal list, one buffer-name policy enum
# where three booleans had merged, and a truthful commandp -- and each
# replaces a wrong cheap thing with a right dearer one. Helpers were split
# before this was raised (lisp_command_activate, universal_raw_value,
# cmd_scope_save/restore, scan_digits/scan_exponent, buf_name_take,
# expand_dir_or_empty, dired_prefill_expanded). Measured actual at the end
# of the cycle: 5714, so this leaves 16 points of rounding room, not room
# for unrelated growth. The per-file cap 520 and the pmccabe ceilings are
# unchanged.
# Raised 5730 -> 5804 by Phase 8 sub-plan 08A (2026-08-06), funding the
# measured +40..90 scc C-side band for 08D/08E. The re-measured idle tree is
# 5714, so 5804 is the top of that band above the actual floor, not a general
# allowance for unrelated growth. Proof on the same tree: setting
# SCC_COMPLEXITY_MAX=5713 makes complexity-check fail on 5714, while the
# raised 5804 setting passes. This temporary-lowering proof is deliberately
# uncommitted; the command and its output are recorded in 08A's funding note.
# Raised 5804 -> 5860 by Phase 9 sub-plan 09A Decision 7 (2026-08-06), funding
# 09D's arena-diagnostics command, its renderer and the exhaustion coverage at
# the audited +5..15 scc price. The re-measured idle tree is 5798, so +15
# breaches at 5813 and 5860 leaves the band's top plus rounding room, not a
# general allowance for unrelated growth. Proof on the same tree:
# SCC_COMPLEXITY_MAX=5797 makes complexity-check fail on 5798, while 5798
# passes. 09D re-sets this to the measured actual at the phase close.
# Re-set 5860 -> 5800 at the Phase 9 close (2026-08-07), the convention the
# Phase 8 fix cycle established and fe's own 09B/09C pair repeated (820 ->
# 757, 1120 -> 1065). The phase spent +2 of the 62 points the raise made
# available: one cmdtable row and one 15-line renderer, against a +5..15
# price. The other 60 go back rather than sitting as unearned headroom for
# work nobody has priced. Proof on the same tree: SCC_COMPLEXITY_MAX=5799
# fails with "total complexity 5800 exceeds limit 5799", 5800 passes.
# Raised 5800 -> 5820 by Phase 10 sub-plan 10A Decision 8 (2026-08-07), funding
# 10C's two `src/*.c` touches at the audited +3..10 scc price: the
# lisp_require.c `.el`-suffix conditional and the two Sec.15 perf-counter sites.
# The tree sits at exactly 5800/5800 -- zero headroom, so even +1 breaches --
# which is why this is a Decision and not routine growth. Proof on the same
# tree, at the pin and before the funded work:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5799
#   FAIL: total complexity 5800 exceeds limit 5799
#   $ make complexity-check SCC_COMPLEXITY_MAX=5800
#   scc total complexity: 5800 (limit 5800)
# Re-set 5820 -> 5802 at the Phase 10 close (2026-08-07), the convention
# every phase since 08 has kept.  The phase spent +2 of the 20 points the
# raise made available, against a +3..10 price: the lisp_require.c suffix
# conditional cost 0 (a ternary scc does not count) and the three
# perf-counter sites cost 2, all of it the outermost-require test the
# nesting fix needed.  The other 18 go back rather than sitting as
# unearned headroom for work nobody has priced.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5801
#   FAIL: total complexity 5802 exceeds limit 5801
#   $ make complexity-check SCC_COMPLEXITY_MAX=5802
#   scc total complexity: 5802 (limit 5802)
# Raised 5802 -> 5825 by Phase 11 sub-plan 11A Decision 8 (2026-08-07),
# funding 11D's `src/*.c` share at the audited +8..20 scc price: the two
# capture/restore natives the `save-excursion`/`with-current-buffer`
# prelude macros are built on, the loader seam moving onto
# `FeTryEvaluateStringWithOptions` with its own bookkeeping unwind and
# `FeResignal`, and `load` returning `t`. Everything else Phase 11 adds on
# the kg side -- the prelude, the compat cases, the PTY cases, the docs --
# is outside the scan. The re-measured idle tree is 5802/5802: zero
# headroom, so even +1 breaches, which is why this is a Decision and not
# routine growth. Proof on the same tree, at the pin and before the funded
# work:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5801
#   FAIL: total complexity 5802 exceeds limit 5801
#   $ make complexity-check SCC_COMPLEXITY_MAX=5802
#   scc total complexity: 5802 (limit 5802)
# Re-set 5825 -> 5799 at the Phase 11 close (2026-08-07), the convention
# every phase since 08 has kept. The phase spent NOTHING of the 23 points
# the raise made available and gave three back: the loader seam cost +1
# (the resignal branch), replacing the save-excursion/with-current-buffer
# natives -- two natives, two malloc'd records and two cleanup callbacks
# -- with one capture/restore pair over GC-managed objects returned 3, and
# deleting lisp_call_body(), whose only two callers those natives were,
# returned 1 more. The rest of Phase 11's kg work is prelude, compat
# cases, PTY cases and docs, none of which the scan covers. Setting the
# cap BELOW where the phase started is deliberate and is the ratchet
# working as intended; it is not a claim that the pre-phase 5802 was
# wrong. Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5798
#   FAIL: total complexity 5799 exceeds limit 5798
#   $ make complexity-check SCC_COMPLEXITY_MAX=5799
#   scc total complexity: 5799 (limit 5799)
# Raised 5799 -> 5806 by the Phase 11 fix cycle (2026-08-07) for the
# acceptance review's BLOCKER B1, the only src/ change in the cycle: the
# excursion restore hands its pool record straight back
# (lisp_marker_release, and the wrapper-identity check in
# lisp_object_gc that early release makes load-bearing), plus the
# tolerance that keeps a spent state object from becoming a type error.
# +7, still 19 under the 5825 Phase 11 funded and did not spend, so this
# raise draws on that phase's own budget rather than opening a new one --
# and the close below re-sets at the measured actual, as the convention
# requires. Proof on the same tree, before the fix:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5798
#   FAIL: total complexity 5799 exceeds limit 5798
# and after it:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5805
#   FAIL: total complexity 5806 exceeds limit 5805
#   $ make complexity-check SCC_COMPLEXITY_MAX=5806
#   scc total complexity: 5806 (limit 5806)
# Re-confirmed at the fix cycle's close, on the finished tree, and NOT
# lowered: 5806 is the measured actual with zero headroom, so there is
# nothing to give back the way the Phase 11 close gave back three.
#   $ make complexity-check SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 5806 (limit 99999)
#   $ make complexity-check SCC_COMPLEXITY_MAX=5805
#   FAIL: total complexity 5806 exceeds limit 5805
# SCC_FILE_COMPLEXITY_MAX stays 520 (worst file src/bufmgr.c at 479,
# unchanged by this phase), and the pmccabe ceilings stay 110/15 (worst
# function 91, one new symbol at 2).
# Raised 5806 -> 5830 by Phase 12 sub-plan 12A Decision 7 (2026-08-07),
# funding 12D's `src/*.c` share at the audited +10..24 scc price: the
# incremental reader native `internal--read-form' that prelude `load'
# loops over (it keeps C ownership of path resolution, depth bookkeeping
# and load-buffer lifetime, and has to distinguish end-of-file from a
# form that reads as nil), and the two condition sites that start raising
# `file-missing' with Emacs' (OPERATION STRERROR PATH) data instead of a
# prose `error'.  The prelude loop DELETES C at the same time, so the net
# is recorded honestly at the close rather than assumed; 12E re-sets this
# at the measured actual, as every phase since 08 has.  Everything else
# Phase 12 adds on the kg side -- the prelude `load', the compat cases,
# the pool constant, the docs -- is outside the scan or costs zero.  The
# re-measured tree is 5806/5806: zero headroom, so even +1 breaches,
# which is why this is a Decision and not routine growth.  Proof on the
# same tree, before the funded work:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5805
#   FAIL: total complexity 5806 exceeds limit 5805
#   $ make complexity-check SCC_COMPLEXITY_MAX=5806
#   scc total complexity: 5806 (limit 5806)
# Re-set 5830 -> 5826 at the Phase 12 close, the convention every phase
# since 08 has kept.  The phase spent +20 of the 24 the raise made
# available, against a +10..24 price: the file-condition raise helper and
# its two sites, and -- unpriced, and recorded as such in its commit --
# the renderer that keeps kg's missing-file diagnostic naming the file
# instead of printing the bare condition name fe's `signal' would leave.
# The pool went to 256 for zero, being a #define value, and the crash fix
# in src/syntax.c cost zero because its guard is an offset rather than a
# branch.  The four points left over go back rather than sitting as
# unearned headroom: the one item of the phase that did NOT land -- the
# prelude `load' loop, blocked on an fe entry point that does not exist,
# see doc/TODO.md -- needs an fe pin move and will fund its own raise
# with a measurement of its own.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5825
#   FAIL: total complexity 5826 exceeds limit 5825
#   $ make complexity-check SCC_COMPLEXITY_MAX=5826
#   scc total complexity: 5826 (limit 5826)
# Raised 5826 -> 5845 for Phase 12's fix cycle, the phase's second
# funded Decision: the acceptance reviews rejected the close, so the
# remainder is priced here rather than smuggled in.  It buys (a) the
# review fixes, measured +7 on this tree -- the empty-OPERATION
# separator rule and the checked memcpy bound in
# render_file_condition(), and the two guarded sibling writes in
# generic_keyword_scan() -- and (b) the prelude `load' loop the
# paragraph above left unfunded, now unblocked by fe's input-unit trio
# (FE_API_VERSION 8), whose C half is a read-form native beside the
# eval loop.  Proof at the raise, review fixes in, loader not yet:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5832
#   FAIL: total complexity 5833 exceeds limit 5832
#   $ make complexity-check SCC_COMPLEXITY_MAX=5833
#   scc total complexity: 5833 (limit 5833)
# 5845 is a ceiling, not a spend; the close re-sets it at the measured
# actual, as every phase since 08 has.
# The fix cycle's close: the measured actual IS 5845 -- the review fixes
# spent 7 and the loader rebuild the remaining 12 (its C stream natives,
# minus the deleted native_load/native_require/lisp_eval_file bodies) --
# so the ceiling and the actual coincide and the re-set is a proof, not
# an edit.  Proof at the close:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5844
#   FAIL: total complexity 5845 exceeds limit 5844
#   $ make complexity-check SCC_COMPLEXITY_MAX=5845
#   scc total complexity: 5845 (limit 5845)
# Raised 5845 -> 5879 for the kill-ring-in-prompts plan
# (doc/plans/2026-08-07-kill-ring-in-prompts.md), a funded Decision
# priced at landing: +34 buys the minibuffer prompts' kills and yanks
# retargeted from the private minibuf_kill slot to the real ring with a
# prompt-local yank-pop (bufmgr.c: minibuf_kill_span/minibuf_yank/
# minibuf_yank_pop/minibuf_prompt_paint, and the per-keystroke
# kill-class boundary in cmdstate.c), and isearch's
# yank-kill/yank-pop-only (search.c: isearch_yank) -- the plan priced
# comparable features at +5..24 and this lands two prompts plus the
# coalescing discipline neither had.  Cap equals the measured actual,
# no slack.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5878
#   FAIL: total complexity 5879 exceeds limit 5878
#   $ make complexity-check SCC_COMPLEXITY_MAX=5879
#   scc total complexity: 5879 (limit 5879)
# Lowered 5879 -> 5847 by the CX campaign's sub-plan A
# (doc/plans/2026-08-07_complexity-reduction-campaign.md), which is the
# other direction: the modeline and footer envelopes were each carrying
# their own copy of the same span scanner -- trim, find the unquoted
# colon, lowercase the name, skip to the value -- and now share one.
# Dedup, not extraction, is what moves this number down; the footer's
# stage decomposition in the same sub-plan measured exactly zero here.
# Cap equals the measured actual, no slack.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5846
#   FAIL: total complexity 5847 exceeds limit 5846
#   $ make complexity-check SCC_COMPLEXITY_MAX=5847
#   scc total complexity: 5847 (limit 5847)
# Lowered 5847 -> 5844 by the same campaign's sub-plan B: reflow's eight
# hand-unwound allocation failures and three copies of "grow the line
# buffer" became one accumulator with a sticky `failed` flag.  Cap equals
# the measured actual.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5843
#   FAIL: total complexity 5844 exceeds limit 5843
#   $ make complexity-check SCC_COMPLEXITY_MAX=5844
#   scc total complexity: 5844 (limit 5844)
# Lowered 5844 -> 5837 by the same campaign's sub-plan C: editor_move_cursor()
# split by MODE (visual-line first, then one helper per direction), and
# four screen-cursor primitives it was open-coding -- retreat a column,
# step a screen row up or down, place a column -- collapsed into shared
# ones.  Cap equals the measured actual.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5836
#   FAIL: total complexity 5837 exceeds limit 5836
#   $ make complexity-check SCC_COMPLEXITY_MAX=5837
#   scc total complexity: 5837 (limit 5837)
# Raised 5837 -> 5841 for the same campaign's sub-plan D, a funded
# Decision and the only one it takes: the campaign's other three
# sub-plans returned 42 points and this one spends 4 of them back,
# because on these two functions scc and pmccabe disagree about which
# way the change went.
#   generic_keyword_scan: three near-identical arms for 0b/0o/0x became
#   one table of prefixes and their digit sets.  pmccabe 54 -> 48, 50
#   lines deleted -- and scc +3, measured on this tree by adding the
#   table alone with the arms untouched: the deletion itself scores
#   exactly 0, so the whole cost is the helper's own existence.  (The
#   switch-per-radix shape the same dedup can take measured +7, and an
#   if-chain +21, which is why the table is the shape that landed.)
#   do_isearch: the search step and the ESC restore became
#   isearch_advance() and isearch_restore_point().  pmccabe 54 -> 40,
#   and scc +1, the one `if` a helper needs to report out-of-memory
#   that two inline `return`s did not.
# Both are dedup and naming, which is what this campaign is for; scc
# counts a helper's braces and its own branch keywords and gives no
# credit for the copies that went away.  Cap equals the measured actual,
# no slack.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5840
#   FAIL: total complexity 5841 exceeds limit 5840
#   $ make complexity-check SCC_COMPLEXITY_MAX=5841
#   scc total complexity: 5841 (limit 5841)
# Raised 5841 -> 5996 for the Windows support commit (6475c0c,
# 2026-08-08), which landed without a pre-priced Decision; this entry
# banks it after the fact instead of before. +155 buys the new
# src/platform.c (+137: the POSIX shims Windows lacks -- kg_poll,
# kg_realpath, kg_console_enable/disable, the opendir/readdir/closedir
# trio, and the rest kg_dirent.h declares), the new stdckdint.h (+19: a
# <stdckdint.h> polyfill for toolchains that predate the C23 header), the
# #ifdef _WIN32 branch dired_collect_flagged()/dired_delete_verified()
# picked up beside the fstatat identity path (+8, doc/coordinates.md-style
# platform split, not a behaviour change on POSIX), tty.c's console-mode
# handling (+8), process_table.c's poll_entry() Windows branch (+4), and
# +1 each in compile.c and main.c. src/process.c's own scc score DROPPED
# 26 despite gaining the CreateProcess-based spawn_process() (pmccabe 5 ->
# 19): scc and pmccabe disagree on which way this file moved, the same
# split the CX campaign's sub-plan D note above already records for
# generic_keyword_scan/do_isearch -- more lines, fewer scc-countable
# branches. Cap equals the measured actual, no slack. Proof on the same
# tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=5995
#   FAIL: total complexity 5996 exceeds limit 5995
#   $ make complexity-check SCC_COMPLEXITY_MAX=5996
#   scc total complexity: 5996 (limit 5996)
# The pmccabe baseline is banked the same way (make pmccabe-baseline
# PMCCABE_BASELINE_ARGS=--allow-regressions, since these are increases,
# not the improvements that command exists for): kg_poll is a new
# function at 20 against the 15 new-function budget; spawn_process 5 ->
# 19; compilation_poll 22 -> 23; poll_entry 14 -> 16; main 10 -> 11;
# dired_collect_flagged 7 -> 8 and dired_delete_verified 4 -> 5, one
# extra branch apiece from the #ifdef _WIN32 identity path above.
# SCC_FILE_COMPLEXITY_MAX stays 520 (worst file is still src/bufmgr.c,
# unchanged by this commit at 498).
# Raised 5996 -> 6072 for the syntax backend seam, slice 2 (2026-08-08):
# src/syntax.c split into a backend-neutral facade plus src/syntax_legacy.c,
# the bespoke scanners, behind the private contract in
# src/syntax_backend.h (doc/plans/kg-tree-sitter-plan.md, Phase 2).  Most
# of the +76 is NOT new complexity, and the split is measured two ways
# rather than one because scc's own number is not additive across a file
# split:
#   - The behaviour-preserving move adds +5 by per-function measurement
#     (score each function on its own, same method on both sides: 444 ->
#     449).  The three additions are legacy_spec_for() (+3, the mode-id
#     lookup that replaces four columns on struct editor_syntax),
#     syntax_backend_update_row()'s scanner-or-generic branch (+1), and
#     syntax_git_rebase_action_name() (+1, the accessor that lets the
#     legacy scanner read the facade's rebase action table instead of
#     keeping a second copy of it).  pmccabe, an independent tool, agrees
#     in direction and size: 2 + 3 + 2 = +7 across those three symbols,
#     and NO existing symbol's measured complexity changed -- every moved
#     function re-keys at exactly its old number (25 re-keyed symbols,
#     `make pmccabe-baseline PMCCABE_BASELINE_ARGS=--allow-regressions`,
#     which is what the flag is for here: a re-key, not a regression).
#   - The other ~71 is scc counting more of the same bytes.  scc 3.7.0's
#     C complexity counter loses count part-way through a long file after
#     certain character-literal/comment sequences, and where the file is
#     cut changes what it sees.  Reproducible on the UNCHANGED pre-split
#     file: partitioning HEAD's src/syntax.c into 20 contiguous pieces at
#     top-level function boundaries sums to 416, while scc reports 267 for
#     the whole file (a 2-piece cut at line 1089 sums to exactly 267, so
#     this is position-dependent, not a size rule).  Splitting the file
#     moved the facade's functions out from behind the scanners and they
#     are now counted; nothing was added to them.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the two halves measure 112 (src/syntax.c, was 267) and 231
# (src/syntax_legacy.c), and the worst file is still src/bufmgr.c at 498.
# Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6071
#   FAIL: total complexity 6072 exceeds limit 6071
#   $ make complexity-check SCC_COMPLEXITY_MAX=6072
#   scc total complexity: 6072 (limit 6072)
# Raised 6072 -> 6082 for the syntax backend seam, slice 3 (2026-08-08):
# edit-granular syntax notification (doc/plans/kg-tree-sitter-plan.md,
# Phase 3).  kg_buffer_replace() now tells the syntax layer about an edit
# exactly ONCE, with a byte-and-point description of it
# (src/syntax.h's kg_syntax_edit, field for field tree-sitter's
# TSInputEdit), instead of re-running the highlighter -- and its downstream
# propagation -- once per rebuilt row.  The +10 is all new branch points,
# and unlike slice 2's it is genuinely new code rather than scc counting
# more of the same bytes:
#   - src/syntax.c 112 -> 118 (+6).  syntax_after_edit() is +4 (its two
#     range guards and the loop over the replacement's rows),
#     syntax_propagate_below() is net +1 (it takes over the loop
#     syntax_propagate_downstream() was, and its "did this row change"
#     test moved out of the loop condition into editor_update_syntax(),
#     which is the other +1).  syntax_rebuild() adds none.
#   - src/buffer.c 316 -> 320 (+4).  splice_last_line_len() is +2 (the
#     backwards scan for the last separator, which is where the new end
#     point's column comes from), edit_describe()'s newline/no-newline
#     branch is +1, and the editor_update_row() split into
#     editor_render_row() + the notification accounts for the last one.
#     No function grew: pmccabe agrees, with every new symbol at or below
#     8 against the 15 new-function budget (editor_render_row 8 -- the old
#     editor_update_row body, re-keyed -- syntax_after_edit 5,
#     syntax_propagate_below 3, splice_last_line_len 3, edit_describe 2,
#     syntax_rebuild 1) and exactly one existing symbol moving:
#     editor_update_syntax 1 -> 2, banked with `make pmccabe-baseline
#     PMCCABE_BASELINE_ARGS=--allow-regressions`.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520; the worst file is still src/bufmgr.c at 498.  Proof on the same
# tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6081
#   FAIL: total complexity 6082 exceeds limit 6081
#   $ make complexity-check SCC_COMPLEXITY_MAX=6082
#   scc total complexity: 6082 (limit 6082)
# Raised 6082 -> 6089 for the syntax backend seam, slice 4 (2026-08-08):
# backend-preparable staged loads and opaque per-buffer syntax state
# (doc/plans/kg-tree-sitter-plan.md, Phase 4).  A file load now renders
# every staged row and then prepares the backend against the WHOLE staged
# document in one pass, instead of rendering-and-colouring a row at a time;
# what that pass derives is an opaque `struct kg_syntax_state *` the
# adopting buffer owns.  The +7 is new branch points, one per new failure
# mode or ownership decision, and scc's file numbers are additive here (no
# file moved that did not gain code):
#   - src/fileio.c 141 -> 144 (+3).  load_stage_rows() is the whole of it:
#     it had one call and no branch, and now has two steps that can fail
#     separately -- a row that will not render, and a preparation that runs
#     out of memory -- plus the state handover in commit_load_result() and
#     the release in free_load_result(), neither of which branches
#     (pmccabe: load_stage_rows 1 -> 3, and it is the only fileio.c symbol
#     that moved).
#   - src/syntax.c 118 -> 120 (+2).  editor_set_syntax_commit() gains the
#     "does the mode actually change" test that guards the mode-change
#     release point (pmccabe 3 -> 4); the four new facade functions
#     (syntax_prepare_rows, syntax_state_adopt/release/discard) are
#     straight-line and measure 1 each.
#   - src/bufmgr.c 498 -> 499 and src/dired.c 129 -> 130 (+1 each): the
#     second failure branch buf_open_special() and dired_fill_current()
#     each acquire, for the same reason load_stage_rows() does (pmccabe
#     3 -> 4 apiece).
#   - src/buffer.c 320 -> 320 and src/syntax_legacy.c 231 -> 231 (+0),
#     even though both changed: kg_row_builder_highlight() became
#     kg_row_builder_render() at the same measured complexity (pmccabe 4,
#     unchanged -- it lost the highlight call and gained nothing), and
#     syntax_backend_prepare() (pmccabe 3) is the loop that function gave
#     up.  The work moved; it did not grow.
# pmccabe agrees in direction and size: +5 across four existing symbols,
# and all seven new symbols are at or below 4 against the 15 new-function
# budget (kg_row_builder_render 4, syntax_backend_prepare 3,
# syntax_prepare_rows/state_adopt/state_release/state_discard 1,
# syntax_backend_state_free 1).  One symbol goes away
# (kg_row_builder_highlight).  Banked with `make pmccabe-baseline
# PMCCABE_BASELINE_ARGS=--allow-regressions`, the flag being for the four
# +1/+2 increases above.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520; the worst file is still src/bufmgr.c, at 499.  Proof on the same
# tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6088
#   FAIL: total complexity 6089 exceeds limit 6088
#   $ make complexity-check SCC_COMPLEXITY_MAX=6089
#   scc total complexity: 6089 (limit 6089)
# Raised 6089 -> 6092 for the WITH_TREE_SITTER build axis, slice 5
# (2026-08-08): the feature flag, the feature stamp, the backend source
# lists and the `+tree-sitter` feature word
# (doc/plans/kg-tree-sitter-plan.md, Phase 5).  Almost all of that slice is
# Makefile and .ci, which scc does not scan (SCC_COMPLEXITY_PATHS is src);
# the whole +3 is one new file, src/syntax_tree_sitter.c, the SKELETON
# backend that the tree-sitter configuration compiles in place of
# src/syntax_legacy.c.  It counts against the cap even though no default
# build compiles it, because scc reads the directory and not the build.
#   - src/syntax_tree_sitter.c 0 -> 3 (+3), the file's whole measurement:
#     syntax_backend_prepare()'s loop over the staged rows and its
#     out-of-memory guard, which is the same shape the legacy backend's
#     prepare has.  The other two contract functions are empty.
#   - src/main.c 16 -> 16 (+0): -V gained a third printf argument and a
#     #ifdef-selected string constant, neither of which is a branch point.
# Bisected on this tree: with src/syntax_tree_sitter.c moved out of src/,
# `make complexity-check SCC_COMPLEXITY_MAX=6089` passes at exactly 6089,
# so nothing else in the slice moved the number.
# pmccabe agrees: three new symbols, all far under the 15 new-function
# budget (syntax_backend_prepare 3, syntax_backend_update_row 1,
# syntax_backend_state_free 1), no existing symbol changed, and no
# baseline rewrite is needed -- the new file is only in PMCCABE_PATHS
# when WITH_TREE_SITTER=1, where `make pmccabe-check` reports the three as
# new and under budget.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520; the worst file is still src/bufmgr.c, at 499.  Proof on the same
# tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6091
#   FAIL: total complexity 6092 exceeds limit 6091
#   $ make complexity-check SCC_COMPLEXITY_MAX=6092
#   scc total complexity: 6092 (limit 6092)
# Raised 6092 -> 6179 for the tree-sitter C backend, slice 6 (2026-08-08):
# the real backend behind src/syntax_backend.h -- a row-backed TSInput, a
# per-buffer parser and tree, dlopen'd grammars and kg's own highlight
# query, and the capture-to-render-offset painting
# (doc/plans/kg-tree-sitter-plan.md, Phase 6 and Phase 8's query policy).
# The +87 is almost entirely two files that no default build compiles, and
# scc reads the directory rather than the build:
#   - src/syntax_tree_sitter.c 3 -> 51 (+48).  The skeleton's 3 becomes the
#     whole backend: the TSInput reader's row walk (ts_input_read, pmccabe
#     7), the ranged per-row query and its capture loop
#     (syntax_backend_update_row 8, paint_capture 6), the chars->render
#     conversion and precedence write (paint_span 7), and the state
#     lifetime (syntax_backend_prepare 6, syntax_backend_rebuild 6,
#     ts_state_new 5, syntax_backend_state_free 5).
#   - src/syntax_tree_sitter_lang.c 0 -> 41 (+41), the whole new file: the
#     grammar search path and loader (path_next_entry 6, grammar_so_path 6,
#     grammar_try 6, kg_ts_grammar_load 4), the query compile and capture
#     mapping (query_compile 7, capture_face 4), and the cached, once-per
#     -process resolution (kg_ts_language_for_mode 6).
#   - src/syntax.c 120 -> 118 (-2).  syntax_after_edit() gave its row loop
#     and its end-row clamp to the backend and kept only the counter, the
#     range guard and the coordinate assertion (pmccabe 5 -> 3, the one
#     improvement in the baseline).
#   - src/syntax_legacy.c 231 -> 231 (+0), even though it GAINED that row
#     loop verbatim plus syntax_backend_rebuild().  scc's C counter is
#     position-dependent in a long file (the slice-2 note above documents
#     the same effect at length); pmccabe, which is not, reports the two
#     new symbols at 4 and 1 and no existing symbol moved.
# No new symbol anywhere in the slice exceeds 8 against the 15
# new-function budget.  Bisected on this tree: with src/syntax_tree_sitter.c
# and src/syntax_tree_sitter_lang.c moved out of src/, the total is 6087 --
# which is HEAD's 6092, minus the 3 the skeleton contributed, minus
# syntax.c's 2 -- so nothing else in the slice moved the number:
#   $ mv src/syntax_tree_sitter*.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=6086
#   FAIL: total complexity 6087 exceeds limit 6086
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the two new files measure 51 and 41, and the worst file is still
# src/bufmgr.c at 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6178
#   FAIL: total complexity 6179 exceeds limit 6178
#   $ make complexity-check SCC_COMPLEXITY_MAX=6179
#   scc total complexity: 6179 (limit 6179)
# Raised 6179 -> 6198 for incremental parsing and ranged highlighting,
# slice 7 (2026-08-08): the edit becomes a TSInputEdit against the old
# tree, the parse reuses it, and the repaint shrinks from every row to the
# damaged ones (doc/plans/kg-tree-sitter-plan.md, Phase 7).  The whole +19
# is one file, which no default build compiles:
#   - src/syntax_tree_sitter.c 51 -> 71 (+20): the parse split into
#     ts_state_parse()/ts_state_reparse() so a parse can be handed an old
#     tree, the edit translation (ts_edit_from, pmccabe 1), the damage set
#     as a sorted merge with a high-water mark (damage_paint 5,
#     range_last_row 3, damage_repaint 5), the no-tree case
#     (ts_after_edit_untreed 3), syntax_backend_after_edit() itself going
#     from "call rebuild" to the tree-edit/parse/changed-ranges sequence
#     (2 -> 7), and one loop in ts_input_read (7 -> 8): an incremental
#     parse seeks backwards constantly, and walking the row cursor back
#     instead of restarting it at row zero is 4.5M row steps and 11 ms per
#     keystroke turned into 36k steps and under 0.1 ms on a 38 768-row
#     file.  That one is bought with a measurement, not with taste.
#   - src/perf.h +0 and src/perf.c +0: four counter enumerators and four
#     names are declarations, not branch points.
# Bisected on this tree: with src/syntax_tree_sitter.c moved out of src/,
# the total is 6128, which is HEAD's 6179 minus the 51 that file measured
# there, so nothing outside it moved:
#   $ mv src/syntax_tree_sitter.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6128 (limit 99999)
# No new symbol exceeds 7 against the 15 new-function budget, and the only
# existing symbol that moved is ts_input_read, 7 -> 8, priced above
# (`make pmccabe-check WITH_TREE_SITTER=1`).
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the file measures 71, and the worst is still src/bufmgr.c at 499.
# Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6198
#   FAIL: total complexity 6199 exceeds limit 6198
#   $ make complexity-check SCC_COMPLEXITY_MAX=6199
#   scc total complexity: 6199 (limit 6199)
# Raised 6199 -> 6203 for grammar batch 1, slice 8 (2026-08-08): Python,
# YAML and Markdown (block) join C in the tree-sitter registry, and a
# query carrying predicates is rejected at load time
# (doc/plans/kg-tree-sitter-plan.md, Phase 8 and Refinement decision 2).
# The whole +4 is one file that no default build compiles:
#   - src/syntax_tree_sitter_lang.c 41 -> 45 (+4), and NOT where the
#     diff is biggest.  The three new queries are 98 lines of C string
#     literal and three registry rows, and they measure ZERO: scc's C
#     counter does not look inside a string literal or a comment.
#     Bisected on this tree, with the queries and their comments cut out
#     of the file, it still measures 45.  What the +4 buys is executable:
#     +3 for the predicate guard (query_has_predicates, pmccabe 3, and
#     the arm that rejects in query_compile, 7 -> 8) and +1 for the
#     testing seam kg_ts_query_accepts (pmccabe 2) that lets a test hand
#     a deliberately-predicated query to exactly the registry's own
#     compile path.
# The guard is the reason this slice costs anything at all, and it is
# bought deliberately: tree-sitter's C library PARSES predicates and then
# matches the pattern regardless, so an unexecuted #eq? is not a filter
# that fails, it is a filter that silently passes.  Refinement decision 2
# ("no predicate engine") was a policy a reviewer had to enforce; it is
# now a load-time check.
# Bisected on this tree: with src/syntax_tree_sitter_lang.c moved out of
# src/, the total is 6158, which is HEAD's 6199 minus the 41 that file
# measured there, so nothing outside it moved:
#   $ mv src/syntax_tree_sitter_lang.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6158 (limit 99999)
# pmccabe agrees: two new symbols at 3 and 2 against the 15 new-function
# budget, one existing symbol moved (query_compile 7 -> 8), and
# `make pmccabe-check WITH_TREE_SITTER=1` needs no baseline rewrite.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX
# stays 520: the file measures 45, and the worst is still src/bufmgr.c at
# 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6202
#   FAIL: total complexity 6203 exceeds limit 6202
#   $ make complexity-check SCC_COMPLEXITY_MAX=6203
#   scc total complexity: 6203 (limit 6203)
# Raised 6203 -> 6212 for grammar batch 2, slice 9 (2026-08-08): JavaScript,
# React, TypeScript, TSX, Java, Rust, HTML, Emacs Lisp and Makefile join the
# tree-sitter registry -- thirteen rows over eleven grammars
# (doc/plans/kg-tree-sitter-plan.md, Phase 8, batch 2).  The whole +9 is one
# file that no default build compiles, and NOT the part of it that grew:
#   - src/syntax_tree_sitter_lang.c 45 -> 54 (+9).  The nine new queries are
#     ~230 lines of C string literal and nine registry rows, and they
#     measure ZERO, for the reason slice 8 recorded: scc's C counter does
#     not look inside a string literal or a comment.  What the +9 buys is
#     the two things a thirteen-row table needs that a four-row one did not:
#     a mode -> first-row INDEX, because ts_after_edit_untreed() walks the
#     registry once per edit that lands on a buffer with no tree and a
#     linear scan of thirteen rows per keystroke is not free
#     (registry_index, pmccabe 2; kg_mode_id is dense from zero, so the
#     index is an array subscript); and per-file-name grammar VARIANTS,
#     because kg's one TypeScript mode covers .ts and .tsx and tree-sitter
#     makes those two different grammars with different node inventories
#     (row_covers, pmccabe 4, and registry_find 3 -> 7, which is the
#     suffix walk over a mode's rows).
# Nothing else in src moved: src/syntax.c stays 118 and
# src/syntax_tree_sitter.c stays 71, even though both changed --
# syntax_prepare_rows() gained a filename parameter it stores, and
# ts_language_for() passes b->filename, and neither is a branch point.
# Bisected on this tree: with src/syntax_tree_sitter_lang.c moved out of
# src/, the total is 6158, which is HEAD's 6203 minus the 45 that file
# measured there, so nothing outside it moved:
#   $ mv src/syntax_tree_sitter_lang.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6158 (limit 99999)
# pmccabe agrees: two new symbols at 4 and 2 against the 15 new-function
# budget, one existing symbol moved (registry_find 3 -> 7), and
# `make pmccabe-check WITH_TREE_SITTER=1` needs no baseline rewrite.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX
# stays 520: the file measures 54 -- the ratchet a nine-language slice was
# most likely to hit is the one it moved least -- and the worst is still
# src/bufmgr.c at 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6211
#   FAIL: total complexity 6212 exceeds limit 6211
#   $ make complexity-check SCC_COMPLEXITY_MAX=6212
#   scc total complexity: 6212 (limit 6212)
# Raised 6212 -> 6318 for the LSP transport, Stage 1 (2026-08-08): a
# bidirectional child process and the Language Server Protocol's
# base-protocol framing over its two pipes
# (doc/plans/2026-08-08-lsp.md, Stage 1).  The whole +106 is one new file,
# and it is a first-of-its-kind module: there was no framing parser and no
# incremental non-blocking writer in the tree to extend.
#   - src/lsp_transport.c 0 -> 106, the file's entire measurement.  A byte
#     parser and two non-blocking pipes are what a comparison-dense
#     number looks like.  The framing half: parse_length (pmccabe 13, a
#     digit run with an overflow guard, so a server's absurd
#     Content-Length is refused rather than wrapped),
#     headers_content_length (7, skip unknown fields, reject a line with
#     no colon), header_block_end (7, "\r\n\r\n" with a bare "\n\n"
#     tolerated), next_header_line (6), line_names (6, field names are
#     case-insensitive), inbox_take_message (6) and inbox_fill (6).  The
#     buffering half: buf_reserve (7), buf_append (2), buf_consume (2).
#     The I/O half: lsp_transport_send (8), lsp_transport_next_message
#     (6), outbox_write (5), lsp_transport_flush (4), lsp_transport_close
#     (4), lsp_transport_child_alive (4), lsp_transport_start (3),
#     io_would_block (3), transport_fail (2), and five accessors at 1.
#   - src/process.c 12 -> 12 (+0), though it gained kg_process_spawn_bidi()
#     and a Windows refusal of it.  The new entry point is COMPOSED out of
#     kg_process_spawn() rather than placed beside it: the child's stdin is
#     a CLOEXEC pipe handed to the request's own stdin_fd, which is what
#     shell.c already does with that field, so the fork, the exec, the
#     process group, the /dev/null fallbacks and the CLOEXEC discipline are
#     reached and not copied.  Its three guards do not register with scc's
#     C counter, which is the honest reading of a function that adds no
#     branch shape the file did not already have.
# The file measured 111 before io_would_block() was pulled out of the two
# places that spell "EAGAIN or EWOULDBLOCK or EINTR"; that is the only
# funding a module with no predecessor had available, and it was taken.
# Bisected on this tree: with src/lsp_transport.c moved out of src/, the
# total is 6212, the previous cap exactly, so nothing outside it moved:
#   $ mv src/lsp_transport.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6212 (limit 99999)
# pmccabe agrees: 24 new symbols for this slice, the worst 13, all under
# the 15 new-function budget, and no existing symbol moved -- so `make
# pmccabe-check` passes with no baseline rewrite.  (It reads src/process.c
# preprocessor-blind and reports only the _WIN32 half, so the POSIX
# kg_process_spawn_bidi() -- three guards, 4 counted by hand -- does not
# appear in its output; it is under budget either way.)
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the new file measures 106, and the worst is still src/bufmgr.c at
# 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6317
#   FAIL: total complexity 6318 exceeds limit 6317
#   $ make complexity-check SCC_COMPLEXITY_MAX=6318
#   scc total complexity: 6318 (limit 6318)
# Raised 6318 -> 6413 for the LSP JSON layer, Stage 2 (2026-08-09): the
# parser and writer JSON-RPC needs, and no more than that
# (doc/plans/2026-08-08-lsp.md, Stage 2).  The whole +95 is one new file,
# and the reason it is not smaller is that there was no JSON anywhere in
# src/ or in vendored code to extend, and the alternative -- a third-party
# parser -- is the new dependency this repo does not take.
#   - src/lsp_json.c 0 -> 95, the file's entire measurement.  A strict
#     parser is a refusal per grammar rule, and the refusals ARE the
#     feature: what a lenient JSON reader saves in branches it spends
#     turning a server's typo into a client that navigates somewhere
#     wrong.  The decoding half: parse_unicode_escape (pmccabe 12,
#     surrogate pairs and the lone-surrogate refusal), sbuf_push_utf8
#     (10, the four UTF-8 lengths), parse_string (8), hex_digit (7),
#     read_hex4 (4), arena_string (3) and parse_escape (3 modified, 11
#     traditional -- a flat switch over the nine legal escapes).  The
#     grammar half: parse_element (9), scan_number_tail (8, the fraction
#     and exponent must each have a digit), parse_number (6),
#     parse_container (6), scan_int_part (5, where the leading-zero rule
#     lives), parse_literal (4), skip_ws (6), take_children (3),
#     parse_value (2, a seven-way switch) and enter (2).  The storage
#     half: arena_alloc (7), grow (6), stack_push (2), sbuf_push (2).
#     The reading API: lsp_json_get (7), lsp_json_int (5, the
#     out-of-range refusal that keeps the cast defined), lsp_json_key_at
#     (4), lsp_json_str (4), lsp_json_at (3) and five more at 3 or less
#     -- every one of them NULL-tolerant, which is one branch each and
#     is what lets a client chain them and check once.  The writer:
#     w_escaped (4), lsp_jsonw_finish (4), lsp_jsonw_int (3), w_append
#     (3), lsp_jsonw_bool (3), and eleven appenders at 1, which is the
#     shape a sticky failure flag buys -- no appender has an error path.
# Nothing outside the new file changed at all: this stage adds no call
# site, since Stage 3 is what will have one.
# Bisected on this tree: with src/lsp_json.c moved out of src/, the total
# is 6318, the previous cap exactly, so nothing outside it moved:
#   $ mv src/lsp_json.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6318 (limit 99999)
# pmccabe agrees: 57 new symbols for this slice, the worst 12, all under
# the 15 new-function budget, and no existing symbol moved -- so `make
# pmccabe-check` passes with no baseline rewrite.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the new file measures 95, and the worst is still src/bufmgr.c at
# 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6412
#   FAIL: total complexity 6413 exceeds limit 6412
#   $ make complexity-check SCC_COMPLEXITY_MAX=6413
#   scc total complexity: 6413 (limit 6413)
# Raised 6413 -> 6582 for the JSON-RPC client and the server registry,
# Stage 3 (2026-08-09): the protocol state machine over one server, and the
# policy that decides which server and where (doc/plans/2026-08-08-lsp.md,
# Stage 3).  The whole +169 is two new files; nothing outside them moved,
# and src/lsp_core.c gained its KG_USE_LSP split at a measured cost of 0,
# because a preprocessor branch is not a run-time one.
#   - src/lsp_client.c 0 -> 111.  It is a conversation, and the count is
#     the shape of one: three states, sixteen pending slots, a queue that
#     exists only before READY, and a death path that has to run every
#     callback exactly once so no caller's context is orphaned.  The
#     dispatch half: lsp_client_poll (pmccabe 8, drain, dispatch, notice a
#     dead child), handle_response (6, match by id or drop),
#     dispatch_message (5, the three things a message can be),
#     capture_caps (5, textDocumentSync in both its number and object
#     forms), id_value (6, an integer id, or the digits of a server that
#     stringified it), refuse_server_request (3) and the two handshake
#     callbacks at 2 and 1.  The sending half: lsp_client_start (7),
#     build_initialize (6, the request's own shape), build_call (5),
#     client_write (5, send now or hold until READY), client_request (4),
#     flush_queued/drop_queued/lsp_client_notify at 2.  The life-cycle
#     half: wait_for_exit (5), pending_fail_all (4), client_die (3),
#     lsp_client_dispose (3), lsp_client_shutdown_begin (3),
#     pending_alloc (3) and six accessors at 1.  The URI half:
#     uri_plain_byte (11, one comparison per RFC 3986 unreserved class,
#     which is a table written as an expression) and path_to_file_uri (6).
#   - src/lsp_server.c 0 -> 58.  A table, a walk and four slots.  The
#     walk: start_dir_of (6, the one realpath, on the directory rather
#     than a file that may not exist yet), lsp_workspace_root (5, mode
#     markers, then .git, then the file's directory), ancestor_matching
#     (5, bounded so a symlinked path cannot outlast it), dir_has_c_markers
#     (4, the four things a C build system leaves), marker_contains (4,
#     pyproject.toml read for `[tool.ty` rather than parsed), marker_exists
#     (3), parent_dir (3), dir_has_python_markers (2), dir_has_git (1).
#     The registry: lsp_server_for (6), instance_find (5, where a dead
#     instance's slot is reclaimed), lsp_server_shutdown_all (5, the
#     grace budget split between the live ones), spec_start (3, argv for
#     a built-in and /bin/sh for the environment override), spec_for (3),
#     instance_free_slot (3), and the accessors at 3 or less.
# Bisected on this tree: with both new files moved out of src/, the total
# is 6413, the previous cap exactly, so nothing outside them moved:
#   $ mv src/lsp_client.c src/lsp_server.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6413 (limit 99999)
# pmccabe agrees: 49 new symbols for this slice, the worst 11, all under
# the 15 new-function budget, and no existing symbol moved -- so `make
# pmccabe-check` passes with no baseline rewrite.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the new files measure 111 and 58, and the worst is still
# src/bufmgr.c at 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6581
#   FAIL: total complexity 6582 exceeds limit 6581
#   $ make complexity-check SCC_COMPLEXITY_MAX=6582
#   scc total complexity: 6582 (limit 6582)
# Raised 6582 -> 6700 for document synchronisation and positions, Stage 4
# (2026-08-09): the shadow snapshots and the one contiguous replacement they
# yield, the UTF-8/UTF-16 position conversions, and the file: URIs both the
# client and the sync layer write (doc/plans/2026-08-08-lsp.md, Stage 4).
# The whole +118 is two new files minus a deletion, and the deletion is the
# point of the second one:
#   - src/lsp_sync.c 0 -> 84.  The boundary module -- the only one in the
#     LSP stack that reads a buffer -- and the count is three ideas.  The
#     positions: utf8_seq_len (pmccabe 8, the four sequence lengths and
#     every way a byte fails to start one, since kg never refuses to show a
#     file and so must never refuse to say where a position in one is),
#     lsp_pos_utf16_from_byte (4), lsp_pos_byte_from_utf16 (4),
#     lsp_pos_encode (3), lsp_pos_decode (3), point_at (3).  The diff:
#     diff_of (11, two scans and the two boundary walks that keep a range
#     from starting or ending inside a character), is_utf8_cont (1).  The
#     table and the notifications: lsp_sync_before_request (7, the four
#     states a document can be in), doc_open (6), abs_path_of (6, a buffer
#     stores the name it was opened with and a relative URI is one the
#     server resolves somewhere else), lsp_sync_close_buffer (4), doc_find
#     (4), language_id_of (3), doc_free_slot (3), doc_change (3),
#     lsp_sync_drop_client (3), same_buffer (3), and eleven builders and
#     accessors at 2 or 1 -- which is what a sticky-failure JSON writer
#     buys: no appender has an error path, so a whole didOpen is one
#     branch-free function.
#   - src/lsp_uri.c 0 -> 52, of which 17 is not new: uri_plain_byte (11)
#     and the encoder (lsp_uri_from_path, 8, was 6) MOVED here out of
#     src/lsp_client.c, which drops 111 -> 92 for exactly that reason.  The
#     genuinely new half is the decoder Stage 4 needs and Stage 3 did not:
#     lsp_uri_to_path (13, the percent-decode with its two refusals -- a
#     malformed escape, and %00, which is how a check on a whole path ends
#     up applied to a prefix of it), uri_path_start (7, the scheme and the
#     empty-or-localhost authority), hex_value (7), ascii_prefix_ieq (3),
#     ascii_lower (3).  A separate module and not a corner of the sync
#     because two callers need it and only one of them may see the editor.
#   - src/lsp_server.c 58 -> 59 (+1): instance_drop (3), the one funnel
#     every path that ends a client now goes through, so that whoever kept
#     per-client state hears about it.  The hook is a function pointer the
#     registry is handed rather than a call into src/lsp_sync.h, which
#     would put an editor dependency underneath the registry and cost every
#     test binary that links it a buffer table.
#   - src/lsp_core.c 0 -> 0: lsp_init() went from empty to one call.
# Bisected on this tree: with both new files moved out of src/, the total is
# 6564, which is the previous cap minus the 19 src/lsp_client.c gave up and
# plus the 1 src/lsp_server.c took on, so nothing else moved:
#   $ mv src/lsp_uri.c src/lsp_sync.c /tmp && make complexity-check \
#         SCC_COMPLEXITY_MAX=99999
#   scc total complexity: 6564 (limit 99999)
# pmccabe agrees: 38 new symbols for this slice, the worst 13, all under
# the 15 new-function budget, and no existing symbol moved -- so `make
# pmccabe-check` passes with no baseline rewrite.
# Cap equals the measured actual, no slack.  SCC_FILE_COMPLEXITY_MAX stays
# 520: the new files measure 84 and 52, and the worst is still
# src/bufmgr.c at 499.  Proof on the same tree:
#   $ make complexity-check SCC_COMPLEXITY_MAX=6699
#   FAIL: total complexity 6700 exceeds limit 6699
#   $ make complexity-check SCC_COMPLEXITY_MAX=6700
#   scc total complexity: 6700 (limit 6700)
SCC_COMPLEXITY_MAX ?= 6700
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

# Removing every other stamp is what makes this work: the file that exists
# names the configuration the objects beside it were compiled for, so a
# build in any other configuration finds its own stamp absent, recreates
# it, and every object rebuilds.  The .with-lisp-* names are the shape this
# stamp had before the tree-sitter axis; a tree that still has one is a
# tree whose objects predate the rename.
$(FEATURE_CONFIG):
	rm -f $(OBJDIR)/.features-* $(OBJDIR)/.with-lisp-*
	touch $@

$(OBJS): $(FEATURE_CONFIG)
# The counting build is a second set of objects from the same sources and
# needs the same guard: PERF_CFLAGS is CFLAGS, feature defines and all, so
# a perfobj compiled under one configuration is exactly as wrong under
# another as an src/*.o is.  Without this, `make WITH_TREE_SITTER=1 check`
# followed by `make check` relinks test_perf from objects that still think
# tree-sitter is the backend -- which test_perf can now tell, since the
# two backends' syntax-notification shapes differ (BACKEND_KEEPS_STATE).
$(PERF_SRC_OBJS) $(PERF_TEST_OBJS): $(FEATURE_CONFIG)
# LISP_SRCS is a subset of SRCS, so a second `$(LISP_OBJS): $(FEATURE_CONFIG)`
# says nothing the line above has not.  What was missing is this: the
# embedded prelude is #included, and nothing told make so, which is how a
# `make lisp-prelude-generate` followed by `make` could relink an editor
# still carrying the previous prelude.
$(OBJDIR)/lisp_prelude.o: $(OBJDIR)/lisp_prelude_generated.inc
ifeq ($(WITH_LISP),1)
# lisp_internal.h includes Fe's public header, but this Makefile does not
# generate compiler dependency files.  Keep both ordinary and performance
# copies honest explicitly: a version/API edit in fe.h must rebuild every
# adapter object, especially lisp_core.o's static_assert tripwires.
$(LISP_OBJS): fe/fe.h
$(addprefix $(PERFOBJDIR)/,$(LISP_SRCS:.c=.o)): fe/fe.h
endif

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

$(OBJDIR)/fe_run.o: fe/fe_run.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

check: header-check lisp-include-check docs-check lisp-compat-check lisp-prelude-check lisp-package-check lisp-oracle-check check-unit check-pty

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

# Sub-plan 10D Part 1's drift gate, in lisp-prelude-check's mold: a PTY
# case's config_files: is inline YAML, so a case exercising a tracked
# lisp/ package carries a *copy* of it.  lisp/auto-fill.el claimed in a
# comment that the copy was byte-for-byte identical and it was not, for
# two phases.  Structural, runs nothing, needs no build -- so it sits
# beside the other no-drift checks and runs in both WITH_LISP
# configurations.
lisp-package-check:
	@$(PYTHON) utils/check_lisp_package_drift.py

# Sub-plan 10C Part 3: kg's half of the milestone gate's oracle item.
# Runs every `comparison: emacs` case through test/kgbatch and compares it
# with the checked-in snapshot, with the XPASS rule fe's own runner lacks
# (a recorded divergence that starts agreeing FAILS).  No Emacs is invoked
# -- the snapshots are the oracle -- so this belongs in ordinary `make
# check` rather than in a .ci step, and it is cheap enough to be there:
# 113 cases, one kg process each, **0.29 s measured** against `make
# check`'s 85 s, i.e. 0.3%.  The self-test is run first and is the reason
# "0 failed" means anything: it builds a corpus in a temp directory whose
# snapshot says 4 where kg answers 3, and requires the run to fail.
# WITH_LISP=0 has no evaluator to compare, so the whole target reports
# that and does nothing, exactly as the lisp-gated PTY cases skip.
lisp-oracle-check:
ifeq ($(WITH_LISP),1)
	@$(MAKE) --no-print-directory $(TESTDIR)/kgbatch
	@$(PYTHON) utils/check_lisp_oracle.py --self-test
	@$(PYTHON) utils/check_lisp_oracle.py $(LISP_ORACLE_ARGS)
else
	@echo "# lisp-oracle-check: WITH_LISP=0, no evaluator to compare"
endif

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

# Audit utility for feeding a fixed init corpus to an external driver: it
# links the editor's own objects and calls kg_lisp_eval_string(), so it
# really is kg's evaluator, minus the terminal.  It is intentionally
# outside TESTBINS all the same -- it asserts nothing, prints whatever
# each file evaluates to, and exists so a question about kg's Lisp is a
# one-second experiment rather than a PTY case.  What it cannot answer is
# anything that needs a real buffer, window or keystroke; those are
# test/pty/*.yaml.
kgbatch: test/kgbatch

test/kgbatch: test/kgbatch.c $(TESTDIR)/stubs_main.o $(FEATURE_CONFIG) \
	$(filter-out $(OBJDIR)/main.o,$(OBJS)) $(FE_OBJ) $(REGEX_OBJS)
	$(CC) $(CFLAGS) -I$(OBJDIR) -o $@ $< \
		$(TESTDIR)/stubs_main.o $(filter-out $(OBJDIR)/main.o,$(OBJS)) \
		$(FE_OBJ) $(REGEX_OBJS) $(LDLIBS)

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
			print(' '.join('--case ' + c for f in d['features'] if f['comparison'] == 'emacs' for c in f['cases']))") \
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
		$(if $(PTY_SETTLE_FLOOR),--settle-floor $(PTY_SETTLE_FLOOR),) \
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
			--write-baseline $(PMCCABE_BASELINE) $(PMCCABE_BASELINE_ARGS)

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
# The backend's own suite links exactly what the facade's does: the
# scanners reach the buffer through the same row primitives.
EXTRA_syntax_legacy := $(TESTDIR)/stubs.o         $(TEST_SRCS_OBJS)
# Likewise for the tree-sitter backend, whose extra dependencies (the core
# library, dl) are already on every link line in that configuration
# (LDLIBS) and whose mode.o is already in TEST_SRCS_OBJS there.
EXTRA_syntax_tree_sitter := $(TESTDIR)/stubs.o    $(TEST_SRCS_OBJS)
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
# The transport depends on process.h and POSIX and on nothing else in the
# editor, so this is the minimal link: its own object, plus the baseline
# every test binary needs for test.o's harness globals.  process.o comes
# from TEST_SRCS_OBJS.
EXTRA_lsp_transport := $(TESTDIR)/stubs.o $(OBJDIR)/lsp_transport.o $(TEST_SRCS_OBJS)
# The JSON layer depends on the C library and nothing else -- not even
# process.h -- so its own object would link on its own; the baseline is
# here because test.o's harness reaches the editor globals stubs.o and
# TEST_SRCS_OBJS provide, exactly as the transport's suite does.
# lsp_json.o itself arrives through TEST_SRCS_OBJS' $(LSP_OBJS), and is
# named again for readability.
EXTRA_lsp_json := $(TESTDIR)/stubs.o $(OBJDIR)/lsp_json.o $(TEST_SRCS_OBJS)
# The client and the registry above it: the protocol state machine, the
# server specs and the workspace-root walk, plus the two modules below them.
# Still no editor object -- lsp_server.c reaches syntax.h for `enum
# kg_mode_id` and nothing else in it, so syntax.o is not needed.  All four
# lsp_*.o arrive through TEST_SRCS_OBJS' $(LSP_OBJS); named here for
# readability, as the two suites above do.
EXTRA_lsp_client := $(TESTDIR)/stubs.o $(OBJDIR)/lsp_client.o \
                    $(OBJDIR)/lsp_server.o $(OBJDIR)/lsp_transport.o \
                    $(OBJDIR)/lsp_json.o $(OBJDIR)/lsp_uri.o \
                    $(TEST_SRCS_OBJS)
# Document sync is the one LSP module that reads buffers, so its suite is
# the first that cannot use the minimal baseline above: it builds real
# buffers, edits them through the edit transaction and watches the shadow
# follow the content generation.  That is EXTRA_buffer's link exactly --
# real bufmgr.o, so buf_handle()/buf_resolve() are the editor's rather than
# test.c's weak stand-ins -- and every lsp_*.o it needs on top of that
# already arrives through TEST_SRCS_OBJS' $(LSP_OBJS), named here for
# readability as the three suites above do.
EXTRA_lsp_sync := $(EXTRA_buffer) $(OBJDIR)/lsp_sync.o $(OBJDIR)/lsp_uri.o \
                  $(OBJDIR)/lsp_client.o $(OBJDIR)/lsp_server.o \
                  $(OBJDIR)/lsp_transport.o $(OBJDIR)/lsp_json.o

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

$(FUZZBIN): $(FUZZ_SRCS) $(HDRS) $(FUZZ_FE_OBJ) $(FEATURE_CONFIG)
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

$(TESTDIR)/fe_run_fuzz.o: fe/fe_run.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(FE_OBJ) $(REGEX_OBJS) $(SYNTAX_BACKEND_ALL) $(LSP_ALL) \
	      $(OBJDIR)/.features-* $(OBJDIR)/.with-lisp-* $(TESTDIR)/*.o \
	      $(TESTBINS) $(TESTDIR)/kgbatch $(FUZZBINS) $(REGEX_DIFF_BIN)
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
	install -d $(DESTDIR)$(lispdir)
	install -m 644 $(LISP_PACKAGES) $(DESTDIR)$(lispdir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(PROG)
	rm -f $(DESTDIR)$(man1dir)/$(PROG).1
	rm -f $(addprefix $(DESTDIR)$(lispdir)/,$(notdir $(LISP_PACKAGES)))
	-rmdir $(DESTDIR)$(lispdir) $(DESTDIR)$(datadir)/kg

.PHONY: all clean distclean check header-check lisp-include-check docs-check lisp-compat-check lisp-compat-oracle lisp-prelude-generate lisp-prelude-check lisp-package-check lisp-oracle-check check-unit check-pty check-regex-differential \
	bench bench-lisp-toggle complexity complexity-check \
	pmccabe pmccabe-check pmccabe-baseline gateway-check gateway-baseline coverage coverage-check coverage-baseline coverage-clean format format-check compile-db iwyu \
	fuzz-keypress fuzz-keypress-seed fuzz-keypress-smoke \
	fuzz-dirlocals fuzz-dirlocals-seed fuzz-dirlocals-smoke \
	fuzz-regex fuzz-regex-seed fuzz-regex-smoke fuzz-regex-seed-replay \
	fuzz-localvars fuzz-localvars-seed fuzz-localvars-smoke \
	fuzz-compile-parse fuzz-compile-parse-seed fuzz-compile-parse-smoke \
	fuzz-seed fuzz-smoke \
	deb release install uninstall
