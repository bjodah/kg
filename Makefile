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
ifeq ($(wildcard fe/fe_unwind.c),)
FE_SPLIT_MISSING = 1
endif
ifeq ($(FE_SPLIT_MISSING),1)
ifeq ($(filter-out clean distclean coverage-clean,$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),)
SKIP_FE_CHECK = 1
endif
endif
ifneq ($(SKIP_FE_CHECK),1)
$(error one of fe/fe.c, fe/fe_eval.c, fe/fe_run.c, fe/fe_unwind.c is missing; run 'git submodule update --init --recursive' or build with 'WITH_LISP=0')
endif
endif
override CFLAGS += -DKG_USE_LISP=1
override LDLIBS += -lm
# The evaluator lives in its own translation unit since Fe sub-plan 03B
# (fe.c -> fe.c + fe_eval.c, behind a private fe/fe_internal.h), the run
# driver plus the public FeEvaluate*/FeCall* surface in a third since Fe
# sub-plan 11B, and the completion machinery -- the condition hierarchy, the
# cleanup registry and every raise -- in a fourth since Fe's Phase 20; both
# splits are how fe kept its 520 per-file cap binding on the evaluator. A
# list so every consumer below is a one-line change.
FE_OBJ = $(OBJDIR)/fe.o $(OBJDIR)/fe_eval.o $(OBJDIR)/fe_run.o \
	 $(OBJDIR)/fe_unwind.o
FUZZ_FE_OBJ = $(TESTDIR)/fe_fuzz.o $(TESTDIR)/fe_eval_fuzz.o \
	      $(TESTDIR)/fe_run_fuzz.o $(TESTDIR)/fe_unwind_fuzz.o
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
# The default is this development environment's release prefix, read from
# $TREE_SITTER_ROOT when the box exports one so that moving the install
# needs no repo edit; the {debug,asan,msan} siblings are what a sanitizer
# lane would point at.  $(or ...) rather than plain `?=` because `?=` only
# defers to an environment variable of the SAME name, and TREE_SITTER_ROOT
# is the box's name for this while TREE_SITTER_PREFIX is kg's.
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
# name substituted, which lets one entry cover a whole
# one-prefix-per-grammar tree.  This install is flat -- every grammar .so
# beside the core in the prefix's lib -- so the default is one plain
# directory, read from $TREE_SITTER_GRAMMAR_DIR on the same terms as the
# prefix above.
WITH_TREE_SITTER ?= 0
TREE_SITTER_PREFIX ?= $(or $(TREE_SITTER_ROOT),/opt-2/tree-sitter-v0.26.12-release)
TS_GRAMMAR_PATH ?= $(or $(TREE_SITTER_GRAMMAR_DIR),/opt-2/tree-sitter-v0.26.12-release/lib)

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

# DAP is the optional debugger client (doc/plans/2026-08-11-dap.md), and it
# is ON by default for WITH_LSP's reason exactly: adapters are found at run
# time, so there is nothing to install for it and no prefix or guard file to
# check, only the 0/1 validation.  .ci/ci-16-with-dap-0.sh keeps the
# disabled build honest, together with its orthogonality runs against
# WITH_LSP=0 and WITH_LISP=0.
WITH_DAP ?= 1

ifneq ($(WITH_DAP),0)
ifneq ($(WITH_DAP),1)
$(error WITH_DAP must be 0 or 1)
endif
endif
ifeq ($(WITH_DAP),1)
override CFLAGS += -DKG_USE_DAP=1
endif

# One stamp for the whole feature configuration, so an object compiled
# under one set of -D flags is never mistaken for up to date under another.
# It replaced LISP_CONFIG when the tree-sitter axis arrived, and gained the
# LSP axis with it: independent axes need one stamp between them, not one
# each, or the sequence `make WITH_LISP=0; make WITH_LISP=0
# WITH_TREE_SITTER=1` looks unchanged to make.  Lives in $(OBJDIR) beside
# the objects it guards.
FEATURE_CONFIG = \
    $(OBJDIR)/.features-lisp-$(WITH_LISP)-ts-$(WITH_TREE_SITTER)-lsp-$(WITH_LSP)-dap-$(WITH_DAP)

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
#
# grep-buffer.el (Phase 17) is the milestone package: the first one that
# could not have been written before the elisp wave.  It stands alone --
# it requires nothing but the prelude.
#
# help-fns.el (Phase 19) is the discovery surface: describe-function,
# describe-variable and apropos, written in Lisp over the reflection the
# phase added.  It stands alone too.
LISP_PACKAGES = lisp/auto-fill.el lisp/grep-buffer.el lisp/help-fns.el \
                lisp/pipeline.el lisp/pipeline-text.el

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
             lisp_motion.c lisp_io.c lisp_cmd.c lisp_obj.c lisp_search.c \
             lisp_hooks.c lisp_process.c lisp_require.c lisp_prompt.c \
             lisp_locals.c
endif
LISP_OBJS = $(addprefix $(OBJDIR)/,$(LISP_SRCS:.c=.o))

# Content-Length framing belongs to neither protocol client.  It was
# extracted for exactly that reason (doc/plans/dap/00-infrastructure.md
# stage A), and src/dap_transport.c composes it as src/lsp_transport.c
# does, so it is built whenever EITHER axis is on rather than being named
# in both lists -- where a WITH_LSP=0 WITH_DAP=1 build would link a
# debugger with no framing under it.
#
# The JSON layer is here for the same reason and since the same stage: it
# was renamed out of lsp_json.c because both protocols' bodies are JSON,
# and src/dap_client.c (stage 3) is the second client to parse one, so a
# WITH_LSP=0 WITH_DAP=1 build needs it as much as the default build does.
PROTOCOL_SRCS =
ifneq ($(filter 1,$(WITH_LSP) $(WITH_DAP)),)
PROTOCOL_SRCS += framed_io.c json.c
endif
PROTOCOL_OBJS = $(addprefix $(OBJDIR)/,$(PROTOCOL_SRCS:.c=.o))

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
LSP_SRCS += announce.c lsp_transport.c lsp_uri.c lsp_client.c lsp_server.c \
            lsp_sync.c
endif
# The two WITH_LSP=1 modules that reach the whole editor -- lsp_req.c takes
# a request's position from the current buffer, lsp_edit.c applies a
# WorkspaceEdit to buffers and opens files.  They are NOT in LSP_OBJS,
# which every test binary links (TEST_SRCS_OBJS below): a test that links
# them would have to link the buffer table, the window table and the
# command layer with them.  They are src/xref.c's situation one axis over,
# and the axis is why they are a list rather than a row in SRCS: xref.c is
# built in both configurations and says so in a #else, while these two
# exist only where the protocol does.
ifeq ($(WITH_LSP),1)
LSP_EDITOR_SRCS = lsp_req.c lsp_edit.c
endif
LSP_OBJS = $(addprefix $(OBJDIR)/,$(LSP_SRCS:.c=.o))
# Named so `make clean` removes what THIS configuration did not build,
# the way SYNTAX_BACKEND_ALL does for the syntax backends: without it a
# `make; make WITH_LSP=0 clean` would leave src/lsp_transport.o and the
# transport's test binary behind.
#
# src/visit.c, src/xref.c, src/lsp_log.c, src/lsp_diag.c and
# src/lsp_hover.c are outside all of this on
# purpose.  visit.c is
# the navigation primitive next-error and xref share, so it is built in every
# configuration; xref.c is built in every configuration too, for lsp_core.c's
# reason -- the command table's `xref-find-definitions` row and the M-.
# binding are unconditional, and a WITH_LSP=0 kg answers them by saying the
# feature was not compiled in.  lsp_log.c is the third: it owns the
# *lsp-log* buffer, so it reaches the editor exactly as xref.c does, and
# main.c installs it unconditionally.  lsp_diag.c and lsp_hover.c are the
# fourth and fifth, for xref.c's reason exactly: their command-table rows
# are unconditional, and both reach buffers, windows and the echo area.
# None of them is in LSP_OBJS, because
# LSP_OBJS is
# also what every test binary links (see TEST_SRCS_OBJS) and xref.c reaches
# the whole editor: the suite that does link it says so itself, below.
LSP_ALL = $(TESTDIR)/test_xref $(TESTDIR)/test_lsp_log \
          $(TESTDIR)/test_lsp_diag \
          $(OBJDIR)/framed_io.o $(OBJDIR)/announce.o $(OBJDIR)/lsp_transport.o \
          $(TESTDIR)/test_framed_io $(TESTDIR)/test_announce $(TESTDIR)/test_lsp_transport \
          $(OBJDIR)/json.o $(TESTDIR)/test_lsp_json \
          $(OBJDIR)/lsp_uri.o \
          $(OBJDIR)/lsp_client.o $(OBJDIR)/lsp_server.o \
          $(TESTDIR)/test_lsp_client \
          $(OBJDIR)/lsp_sync.o $(TESTDIR)/test_lsp_sync \
          $(OBJDIR)/lsp_req.o \
          $(OBJDIR)/lsp_edit.o $(TESTDIR)/test_lsp_edit

# The debugger client's one editor-facing header is src/dap.h, and its
# implementation is split across two objects by what they may drag into a
# link, not by what they do:
#
#   dap_core.c   dap_shutdown/dap_poll/dap_wait_fds -- the facade legs the
#                editor calls from editor_cleanup() and from the two poll
#                sites.  It reaches nothing but the protocol, so it joins
#                TEST_SRCS_OBJS below, which every test binary links.
#   dap_keymap.c dap_init(), which creates the three debugger maps.  It
#                reaches the keymap layer, and keymap.c resolves command
#                names through cmd.c -- so a test binary linking it would
#                have to link the whole command table with it.  That is
#                lsp_req.c/lsp_edit.c's situation exactly, and the reason
#                those are outside LSP_OBJS.
#
# Both are compiled in every configuration, the LISP_SRCS/LSP_SRCS shape:
# the facade's entry points exist either way, so no caller grows a
# KG_USE_DAP conditional.  Everything BEHIND the facade (transport, client,
# session, breakpoints) is WITH_DAP=1 only: dap_transport.c (stage 2),
# dap_client.c (stage 3), dap_config.c/dap_session.c (stage 4) and
# dap_breakpoint.c (stage 5).  The first four link against process.o and
# $(PROTOCOL_OBJS) and against nothing else in the editor; the breakpoint
# table reaches three more -- marker.o, event.o and the buffer table -- and
# every one of those is in TEST_SRCS_OBJS or the stub set already, which is
# what keeps it inside DAP_OBJS too rather than out on DAP_EDITOR_SRCS.
# dap_core.c reaches dap_session.c for its poll and wait legs and
# dap_breakpoint.c to drop the table at shutdown, which is why both are on
# this list and not beside the keymap.
DAP_SRCS = dap_core.c
ifeq ($(WITH_DAP),1)
DAP_SRCS += dap_transport.c dap_client.c dap_config.c dap_session.c \
            dap_breakpoint.c dap_exec.c dap_decor.c
endif
# dap_commands.c joins dap_keymap.c for the same reason and in both
# configurations: it is the one debugger file that prompts, opens buffers
# and arranges windows, so a test binary linking it would link the whole
# editor -- and its WITH_DAP=0 half is what keeps the cmdtable rows honest.
DAP_EDITOR_SRCS = dap_keymap.c dap_commands.c
# The panes, the layout and the row metadata join them, and for the same
# reason: dap_ui.c writes special buffers through the edit gateway and
# arranges windows.  WITH_DAP=1 only, unlike its two neighbours, because it
# has no disabled half to compile -- a build whose `kg -V` says `-dap`
# answers every command in dap_commands.c's own `#else` half and never
# reaches a pane.
ifeq ($(WITH_DAP),1)
DAP_EDITOR_SRCS += dap_ui.c
endif
DAP_OBJS = $(addprefix $(OBJDIR)/,$(DAP_SRCS:.c=.o))
# What `make clean` must remove because THIS configuration did not build
# it, LSP_ALL's reason exactly: without it a `make; make WITH_DAP=0 clean`
# leaves src/dap_transport.o and the transport's suite behind.
DAP_ALL = $(OBJDIR)/dap_transport.o $(TESTDIR)/test_dap_transport \
          $(OBJDIR)/dap_client.o $(TESTDIR)/test_dap_client \
          $(OBJDIR)/dap_config.o $(TESTDIR)/test_dap_config \
          $(OBJDIR)/dap_session.o $(TESTDIR)/test_dap_session \
          $(OBJDIR)/dap_breakpoint.o $(TESTDIR)/test_dap_breakpoint \
          $(OBJDIR)/dap_exec.o $(TESTDIR)/test_dap_exec \
          $(OBJDIR)/dap_decor.o $(TESTDIR)/test_dap_commands \
          $(OBJDIR)/dap_ui.o $(TESTDIR)/test_dap_ui

# Source files
SRCS = main.c tty.c async.c syntax.c $(SYNTAX_BACKEND_SRCS) autocomplete.c buffer.c fileio.c \
       display.c search.c basic.c word.c kbd.c yank.c undo.c help.c describe.c bufmgr.c winmgr.c winconfig.c cmd.c cmdstate.c keyevent.c keymap.c macro.c \
       shell.c path.c rect.c $(LISP_SRCS) $(PROTOCOL_SRCS) $(LSP_SRCS) $(DAP_SRCS) $(DAP_EDITOR_SRCS) keybind.c mode.c vgeom.c localvars.c compile.c compile_parse.c \
       compile_nav.c next_error.c occur.c register.c visit.c fileline.c xref.c lsp_log.c \
       lsp_diag.c lsp_hover.c $(LSP_EDITOR_SRCS) lsp_rename.c lsp_complete.c \
       dabbrev.c \
       width.c dired.c perf.c platform.c process.c process_table.c marker.c decor.c event.c \
       mouse.c showparen.c prompt.c gitdiag.c

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
           $(TESTDIR)/test_tty $(TESTDIR)/test_async $(TESTDIR)/test_minibuf \
           $(TESTDIR)/test_dired $(TESTDIR)/test_winmgr \
           $(TESTDIR)/test_cmd $(TESTDIR)/test_keys \
           $(TESTDIR)/test_keyevent $(TESTDIR)/test_keymap \
           $(TESTDIR)/test_describe $(TESTDIR)/test_marker \
           $(TESTDIR)/test_decor $(TESTDIR)/test_event \
           $(TESTDIR)/test_register $(TESTDIR)/test_process_table \
           $(TESTDIR)/test_vgeom $(TESTDIR)/test_dabbrev \
           $(TESTDIR)/test_showparen $(TESTDIR)/test_fileline \
           $(TESTDIR)/test_occur $(TESTDIR)/test_gitdiag \
           $(TESTDIR)/test_readonly \
           $(TESTDIR)/test_perf
# Each backend's own suite exists only where that backend does: it links
# that backend's object and asserts what it paints, so neither is a suite
# the other configuration can run at all.  test_syntax_legacy asserts the
# bespoke scanners; test_syntax_tree_sitter asserts grammar loading, query
# compilation and capture-to-render-offset painting, and hard-requires the
# C grammar, since a WITH_TREE_SITTER=1 build only happens on a box that
# has one.
# That suite's ABI guard needs a grammar this tree-sitter refuses, and
# every grammar the install ships is in range, so the mismatch is built
# rather than found: TS_FAKE_GRAMMARS, whose rules are down beside the
# other test-binary rules because a rule up here would become the default
# goal ahead of `all`.
TS_FAKE_GRAMMAR_DIR = $(TESTDIR)/.ts-fake-grammar
TS_FAKE_GRAMMARS = $(TS_FAKE_GRAMMAR_DIR)/libtree-sitter-kgfakeold.so \
                   $(TS_FAKE_GRAMMAR_DIR)/libtree-sitter-kgfakenew.so
ifeq ($(WITH_TREE_SITTER),0)
TESTBINS += $(TESTDIR)/test_syntax_legacy
else
TESTBINS += $(TESTDIR)/test_syntax_tree_sitter
endif
# The two shared layers' suites exist wherever the layers do, which is
# either protocol axis: they are those layers' own regression proof and a
# WITH_LSP=0 WITH_DAP=1 tree needs them as much as the default build does.
# test_lsp_json keeps its name from before json.c was renamed out of
# lsp_json.c; the suite is the JSON layer's, whichever client parses with
# it.
ifneq ($(PROTOCOL_SRCS),)
TESTBINS += $(TESTDIR)/test_framed_io $(TESTDIR)/test_lsp_json
endif
# Same per-axis rule: the transport's suite links src/lsp_transport.o,
# which only a WITH_LSP=1 build has.  A WITH_LSP=0 tree has no transport
# to test -- the facade it does have is three no-ops that every other
# binary already links.
ifeq ($(WITH_LSP),1)
TESTBINS += $(TESTDIR)/test_announce $(TESTDIR)/test_lsp_transport \
            $(TESTDIR)/test_lsp_client $(TESTDIR)/test_lsp_sync \
            $(TESTDIR)/test_lsp_log $(TESTDIR)/test_xref \
            $(TESTDIR)/test_lsp_diag $(TESTDIR)/test_lsp_edit
endif
# And the debugger's, on its own axis for the same reason: they link
# src/dap_transport.o and src/dap_client.o, which only a WITH_DAP=1 build
# has.
ifeq ($(WITH_DAP),1)
TESTBINS += $(TESTDIR)/test_dap_transport $(TESTDIR)/test_dap_client \
            $(TESTDIR)/test_dap_config $(TESTDIR)/test_dap_session \
            $(TESTDIR)/test_dap_breakpoint $(TESTDIR)/test_dap_exec \
            $(TESTDIR)/test_dap_commands $(TESTDIR)/test_dap_ui
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
FUZZBIN_SYNTAX = $(TESTDIR)/fuzz_syntax
FUZZ_SRCS = $(TESTDIR)/fuzz_keypress.c $(TESTDIR)/fuzz_stubs.c \
	    $(OBJDIR)/kbd.c $(OBJDIR)/buffer.c $(OBJDIR)/basic.c \
	    $(OBJDIR)/word.c $(OBJDIR)/autocomplete.c $(OBJDIR)/yank.c \
	    $(OBJDIR)/undo.c $(OBJDIR)/rect.c $(OBJDIR)/syntax.c \
	    $(addprefix $(OBJDIR)/,$(SYNTAX_BACKEND_SRCS)) \
	    $(OBJDIR)/tty.c $(OBJDIR)/async.c $(OBJDIR)/macro.c $(OBJDIR)/mouse.c \
	    $(addprefix $(OBJDIR)/,$(LISP_SRCS)) \
	    $(addprefix $(OBJDIR)/,$(PROTOCOL_SRCS)) \
	    $(addprefix $(OBJDIR)/,$(LSP_SRCS)) \
	    $(addprefix $(OBJDIR)/,$(DAP_SRCS)) \
	    $(OBJDIR)/keybind.c $(OBJDIR)/width.c $(OBJDIR)/cmdstate.c $(OBJDIR)/keyevent.c \
	    $(OBJDIR)/keymap.c $(OBJDIR)/marker.c $(OBJDIR)/decor.c \
	    $(OBJDIR)/event.c $(OBJDIR)/process.c $(OBJDIR)/process_table.c \
	    $(OBJDIR)/regex.c fe/tiny-regex-c/re.c
FUZZ_SYNTAX_SRCS = $(TESTDIR)/fuzz_syntax.c \
		  $(filter-out $(TESTDIR)/fuzz_keypress.c,$(FUZZ_SRCS))
FUZZBIN_DIRLOCALS = $(TESTDIR)/fuzz_dirlocals
FUZZBIN_REGEX    = $(TESTDIR)/fuzz_regex
FUZZBIN_LOCALVARS = $(TESTDIR)/fuzz_localvars
FUZZBIN_COMPILE_PARSE = $(TESTDIR)/fuzz_compile_parse
FUZZBIN_LSP_JSON = $(TESTDIR)/fuzz_lsp_json
FUZZBIN_WIDTH = $(TESTDIR)/fuzz_width
FUZZBIN_KEYBIND = $(TESTDIR)/fuzz_keybind
FUZZBIN_FRAMES = $(TESTDIR)/fuzz_frames
FUZZBIN_DAP_DISPATCH = $(TESTDIR)/fuzz_dap_dispatch
FUZZBINS = $(FUZZBIN) $(FUZZBIN_SYNTAX) $(FUZZBIN_DIRLOCALS) $(FUZZBIN_REGEX) $(FUZZBIN_LOCALVARS) $(FUZZBIN_COMPILE_PARSE) $(FUZZBIN_LSP_JSON) $(FUZZBIN_WIDTH) $(FUZZBIN_KEYBIND) $(FUZZBIN_FRAMES)
# The debugger's dispatcher exists only on its own axis, so its target
# joins the aggregates only there -- `make fuzz-smoke WITH_DAP=0` must not
# ask clang to build a client that configuration does not have.
DAP_FUZZ_TARGETS =
ifeq ($(WITH_DAP),1)
FUZZBINS += $(FUZZBIN_DAP_DISPATCH)
DAP_FUZZ_TARGETS = dap-dispatch
endif
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
# $(DAP_OBJS) is here for the same reason and with the same shape: the
# debugger facade's poll legs are called from src/async.c and its shutdown
# from editor_cleanup().  dap_keymap.o is deliberately NOT here (see
# DAP_EDITOR_SRCS above).
# $(PROTOCOL_OBJS) is what both of those stand on: LSP_OBJS used to carry
# framed_io.o for everyone, and a WITH_LSP=0 WITH_DAP=1 test binary would
# otherwise link dap_transport.o with nothing under it.
TEST_SRCS_OBJS = $(OBJDIR)/undo.o $(OBJDIR)/buffer.o $(OBJDIR)/syntax.o \
                 $(SYNTAX_BACKEND_OBJS) \
                 $(OBJDIR)/width.o $(OBJDIR)/marker.o $(OBJDIR)/decor.o \
                 $(OBJDIR)/cmdstate.o $(OBJDIR)/event.o \
                 $(OBJDIR)/process.o $(OBJDIR)/process_table.o \
                 $(PROTOCOL_OBJS) $(LSP_OBJS) $(DAP_OBJS)
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
ifeq ($(WITH_DAP),1)
override FUZZ_CFLAGS += -DKG_USE_DAP=1
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
# The whole-tree complexity ceiling.  `make complexity-check` sums scc's
# per-file complexity over $(SCC_COMPLEXITY_PATHS) and fails above
# SCC_COMPLEXITY_MAX; a single file above SCC_FILE_COMPLEXITY_MAX fails
# it too (the worst today is src/bufmgr.c at 517).
#
# The ceiling equals the measured actual, with no slack: it is a ratchet,
# not an allowance, so new complexity has to be paid for rather than
# grown into.  Raising it is an explicit decision, lowering it banks work
# already done, and BOTH carry their rationale and their measured proof
# -- the per-file bisect, the before/after `make complexity-check
# SCC_COMPLEXITY_MAX=...` pair, what pmccabe said -- in the COMMIT
# MESSAGE.  The history lives in `git log`; this comment describes only
# what the knobs mean today.
SCC_COMPLEXITY_MAX ?= 10304
SCC_FILE_COMPLEXITY_MAX ?= 520
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(addprefix $(OBJDIR)/,$(SRCS))
# Per-function backstop for `make pmccabe-check`: no function in the tree
# may measure above this.  The worst today is draw_window_rows at 67.
# Lower it, do not raise it -- and a move in either direction
# carries its rationale and measured proof in the commit message.
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
# Every other tool here is a bare name that PATH resolves; these two are
# resolved here instead, falling back to one developer box's /opt-3
# layout the way utils/pty_accept.py falls back for Emacs (the hosted
# workflow sets both in its env).  The recipe then checks the result and
# names the tool that is missing rather than dying as "no such file".
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
$(OBJDIR)/lisp_locals.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_locals.h
$(OBJDIR)/lisp_process.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_obj.h $(OBJDIR)/lisp_process.h $(OBJDIR)/process.h $(OBJDIR)/process_table.h
$(OBJDIR)/lisp_require.o: $(OBJDIR)/lisp_internal.h
$(OBJDIR)/main.o: $(OBJDIR)/lisp.h

$(OBJDIR)/fe.o: fe/fe.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

$(OBJDIR)/fe_eval.o: fe/fe_eval.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

$(OBJDIR)/fe_run.o: fe/fe_run.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

$(OBJDIR)/fe_unwind.o: fe/fe_unwind.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

check: header-check lisp-include-check docs-check lisp-compat-check lisp-prelude-check lisp-package-check forecast-check lisp-oracle-check lisp-gc-stress-check forecast-init-check check-unit-decoding check-pty-tokens check-unit check-pty

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

# Phase 15's forecast audit: which names does the Lisp we WANT to write
# reach for, and which of those does kg not have?  The corpus is
# utils/forecast/ plus kg's own lisp/*.el, the implemented-name set is
# parsed out of fe.c, src/lisp_prelude.c and lisp/*.el, and the ranked
# MISSING/COVERED partition is the checked-in utils/forecast/AUDIT.md.
#
# `forecast-check' is the regenerate-and-diff gate, and it sits here --
# beside docs-check, lisp-compat-check and lisp-prelude-check -- rather
# than in a .ci step of its own for the reason those three are here: it
# runs no fe, no kg and no Emacs, needs no build, and is meaningful in
# both WITH_LISP configurations.  Joining `make check' therefore gates it
# in every CI lane that runs the suite, which is the smallest honest
# option available.
forecast-audit:
	@$(PYTHON) utils/forecast_audit.py

forecast-check:
	@$(PYTHON) utils/forecast_audit.py --check

# The corpus's floor, and the one part of it that is not structural:
# utils/forecast/target-init.el is a realistic user init file and Phase
# 15's definition of done requires it to LOAD CLEAN, so it is run rather
# than read.  `-b' gives the process a buffer, since the file's commands
# are defined (not called) at load time but `with-current-buffer' resolves
# a buffer either way.  Ordered after lisp-gc-stress-check for that
# target's own reason: three `check' prerequisites build test/kgbatch
# through a sub-make, and a run that links it while another is exec'ing it
# dies with EACCES.
forecast-init-check: lisp-gc-stress-check
ifeq ($(WITH_LISP),1)
	@$(MAKE) --no-print-directory $(TESTDIR)/kgbatch
	@$(TESTDIR)/kgbatch -b utils/forecast/target-init.el >/dev/null \
		&& echo "forecast-init-check: utils/forecast/target-init.el" \
			"loads clean"
else
	@echo "# forecast-init-check: WITH_LISP=0, no evaluator to load it"
endif

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

# Phase 13.5's kg half of fe's FE_GC_STRESS knob.  Fe's `MakeObject()`
# collects before every allocation when fe.c is built with
# -DFE_GC_STRESS=1, so an object live only through an unrooted C local
# dies at the first opportunity rather than at whatever unrelated
# allocation happens to empty the free list.  This target runs one
# heavy-allocation script through kg's own evaluator twice -- an ordinary
# kgbatch and one linked against a stress-built fe -- and requires the
# same answer from both, with the stress build's arena-stats collection
# count far above the ordinary one's.  Same shape as test/perfobj: the
# instrumented fe object gets its own name under test/ and never mixes
# with src/*.o, so turning the knob on leaves no stale object behind.
# fe_eval.c, fe_run.c and fe_unwind.c are NOT rebuilt: the macro lives
# inside fe.c and nothing in fe.h moves with it.
GC_STRESS_KGBATCH = $(TESTDIR)/kgbatch-gcstress
GC_STRESS_FE_OBJ = $(TESTDIR)/fe_gcstress.o

$(TESTDIR)/fe_gcstress.o: fe/fe.c fe/fe.h fe/fe_internal.h
	$(CC) $(FE_CFLAGS) -DFE_GC_STRESS=1 -c $< -o $@

$(GC_STRESS_KGBATCH): test/kgbatch.c $(TESTDIR)/stubs_main.o \
	$(FEATURE_CONFIG) $(filter-out $(OBJDIR)/main.o,$(OBJS)) \
	$(GC_STRESS_FE_OBJ) $(OBJDIR)/fe_eval.o $(OBJDIR)/fe_run.o \
	$(OBJDIR)/fe_unwind.o $(REGEX_OBJS)
	$(CC) $(CFLAGS) -I$(OBJDIR) -o $@ $< \
		$(TESTDIR)/stubs_main.o $(filter-out $(OBJDIR)/main.o,$(OBJS)) \
		$(GC_STRESS_FE_OBJ) $(OBJDIR)/fe_eval.o $(OBJDIR)/fe_run.o \
		$(OBJDIR)/fe_unwind.o $(REGEX_OBJS) $(LDLIBS)

# Ordered after lisp-oracle-check rather than beside it: `check`'s
# prerequisites run in parallel under -j, both targets build
# $(TESTDIR)/kgbatch through a sub-make, and a run that links it while the
# other is exec'ing it dies with EACCES on the half-written binary.
# Measured: .ci/ci-03 failed exactly that way.  The dependency is
# scheduling, not meaning -- neither check needs the other's result.
lisp-gc-stress-check: lisp-oracle-check
ifeq ($(WITH_LISP),1)
	@$(MAKE) --no-print-directory $(TESTDIR)/kgbatch $(GC_STRESS_KGBATCH)
	@$(PYTHON) utils/check_lisp_gc_stress.py \
		--kgbatch $(TESTDIR)/kgbatch \
		--stress-kgbatch $(GC_STRESS_KGBATCH)
else
	@echo "# lisp-gc-stress-check: WITH_LISP=0, no evaluator to stress"
endif

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

check-pty-tokens:
	@$(PYTHON) test/pty_tokens_test.py

# The native layer's report has to survive the bytes a failing test
# prints.  A test that fails on a fixture prints the fixture, fixtures
# are often raw bytes, and one invalid UTF-8 sequence used to take the
# results of all 46 binaries with it -- the whole report replaced by a
# UnicodeDecodeError traceback.  This drives the real script over a
# command that prints 0xC3 0x28 and exits 1, and requires one FAIL line
# with the byte escaped.  `sh -c` as the runner is what lets a shell
# command stand in for a test binary here.
check-unit-decoding:
	@out=$$($(PYTHON) utils/run_unit_tests.py --runner "sh -c" \
		"printf 'raw: \303\050\n' >&2; exit 1" 2>&1 || true); \
	case "$$out" in \
	*Traceback*) echo "check-unit-decoding: the runner died on the byte" >&2; \
		printf '%s\n' "$$out" >&2; exit 1;; \
	esac; \
	case "$$out" in \
	*'raw: \xc3('*) echo "check-unit-decoding: one FAIL line, byte escaped";; \
	*) echo "check-unit-decoding: the escaped byte is not in the report" >&2; \
		printf '%s\n' "$$out" >&2; exit 1;; \
	esac

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

fuzz-syntax: $(FUZZBIN_SYNTAX)

fuzz-syntax-seed:
	mkdir -p $(FUZZ_CORPUS)/syntax
	cp -f $(FUZZ_SEEDS)/syntax/* $(FUZZ_CORPUS)/syntax/

fuzz-syntax-smoke: $(FUZZBIN_SYNTAX) fuzz-syntax-seed
	mkdir -p $(FUZZ_ARTIFACTS)/syntax
	./$(FUZZBIN_SYNTAX) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/syntax/ \
		$(FUZZ_CORPUS)/syntax

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

fuzz-lsp-json: $(FUZZBIN_LSP_JSON)

fuzz-lsp-json-seed:
	mkdir -p $(FUZZ_CORPUS)/lsp_json
	cp -f $(FUZZ_SEEDS)/lsp_json/* $(FUZZ_CORPUS)/lsp_json/

fuzz-lsp-json-smoke: $(FUZZBIN_LSP_JSON) fuzz-lsp-json-seed
	mkdir -p $(FUZZ_ARTIFACTS)/lsp_json
	./$(FUZZBIN_LSP_JSON) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/lsp_json/ \
		$(FUZZ_CORPUS)/lsp_json

fuzz-width: $(FUZZBIN_WIDTH)

fuzz-width-seed:
	mkdir -p $(FUZZ_CORPUS)/width
	cp -f $(FUZZ_SEEDS)/width/* $(FUZZ_CORPUS)/width/

fuzz-width-smoke: $(FUZZBIN_WIDTH) fuzz-width-seed
	mkdir -p $(FUZZ_ARTIFACTS)/width
	./$(FUZZBIN_WIDTH) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/width/ \
		$(FUZZ_CORPUS)/width

fuzz-keybind: $(FUZZBIN_KEYBIND)

fuzz-keybind-seed:
	mkdir -p $(FUZZ_CORPUS)/keybind
	cp -f $(FUZZ_SEEDS)/keybind/* $(FUZZ_CORPUS)/keybind/

fuzz-keybind-smoke: $(FUZZBIN_KEYBIND) fuzz-keybind-seed
	mkdir -p $(FUZZ_ARTIFACTS)/keybind
	./$(FUZZBIN_KEYBIND) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/keybind/ \
		$(FUZZ_CORPUS)/keybind

fuzz-frames: $(FUZZBIN_FRAMES)

fuzz-frames-seed:
	mkdir -p $(FUZZ_CORPUS)/frames
	cp -f $(FUZZ_SEEDS)/frames/* $(FUZZ_CORPUS)/frames/

fuzz-frames-smoke: $(FUZZBIN_FRAMES) fuzz-frames-seed
	mkdir -p $(FUZZ_ARTIFACTS)/frames
	./$(FUZZBIN_FRAMES) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/frames/ \
		$(FUZZ_CORPUS)/frames

fuzz-dap-dispatch: $(FUZZBIN_DAP_DISPATCH)

fuzz-dap-dispatch-seed:
	mkdir -p $(FUZZ_CORPUS)/dap_dispatch
	cp -f $(FUZZ_SEEDS)/dap_dispatch/* $(FUZZ_CORPUS)/dap_dispatch/

fuzz-dap-dispatch-smoke: $(FUZZBIN_DAP_DISPATCH) fuzz-dap-dispatch-seed
	mkdir -p $(FUZZ_ARTIFACTS)/dap_dispatch
	./$(FUZZBIN_DAP_DISPATCH) $(FUZZ_SMOKE_ARGS) \
		-artifact_prefix=$(FUZZ_ARTIFACTS)/dap_dispatch/ \
		$(FUZZ_CORPUS)/dap_dispatch

fuzz-seed: fuzz-keypress-seed fuzz-syntax-seed fuzz-dirlocals-seed fuzz-regex-seed fuzz-localvars-seed fuzz-compile-parse-seed fuzz-lsp-json-seed fuzz-width-seed fuzz-keybind-seed fuzz-frames-seed $(addsuffix -seed,$(addprefix fuzz-,$(DAP_FUZZ_TARGETS)))

fuzz-smoke: fuzz-keypress-smoke fuzz-syntax-smoke fuzz-dirlocals-smoke fuzz-regex-smoke fuzz-localvars-smoke fuzz-compile-parse-smoke fuzz-lsp-json-smoke fuzz-width-smoke fuzz-keybind-smoke fuzz-frames-smoke $(addsuffix -smoke,$(addprefix fuzz-,$(DAP_FUZZ_TARGETS)))

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
# test_basic.c #includes src/display.c to reach the drawing helpers, so it
# needs everything display.c calls -- showparen.o and gitdiag.o among them,
# since the repaint is where the paren highlight and the git diagnostics
# are recomputed.
EXTRA_basic        := $(TESTDIR)/stubs.o          $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(OBJDIR)/vgeom.o $(OBJDIR)/showparen.o $(OBJDIR)/gitdiag.o $(TEST_SRCS_OBJS) $(OBJDIR)/cmdstate.o
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
EXTRA_compile_nav := $(TESTDIR)/stubs_buffer.o $(TESTDIR)/stubs_win.o $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(OBJDIR)/compile_parse.o $(OBJDIR)/compile_nav.o $(OBJDIR)/next_error.o $(OBJDIR)/visit.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
# compile_parse.c is pure: no editor state, nothing beyond def.h's checked
# arithmetic/ASCII helpers.  Same minimal baseline as EXTRA_localvars.
EXTRA_compile_parse := $(TESTDIR)/stubs.o       $(OBJDIR)/compile_parse.o $(TEST_SRCS_OBJS)
# occur.c builds a real listing buffer out of a real source buffer, so it
# needs everything compile_nav's records need plus the regex engine it
# searches with.  win_display_buffer_other_window()/win_position_at_row()
# come from stubs_win.o and editor_goto_line_direct() from stubs_buffer.o,
# both no-ops: what this suite observes is the store, the listing's text
# and the next-error handover, never point moving.
EXTRA_occur       := $(EXTRA_compile_nav) $(OBJDIR)/occur.o $(REGEX_OBJS)
EXTRA_tty         := $(TESTDIR)/stubs.o          $(OBJDIR)/tty.o $(OBJDIR)/async.o $(OBJDIR)/fileio.o $(OBJDIR)/keyevent.o $(TEST_SRCS_OBJS)
# The aggregate itself is editor-free: its enabled build drives the real LSP
# registry and its disabled build reaches the facade's no-op half.
EXTRA_async       := $(TESTDIR)/stubs.o          $(OBJDIR)/async.o $(TEST_SRCS_OBJS)
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
EXTRA_winmgr      := $(TESTDIR)/stubs_buffer.o   $(OBJDIR)/dired.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/fileio.o $(OBJDIR)/bufmgr.o $(OBJDIR)/compile.o $(OBJDIR)/winmgr.o $(OBJDIR)/winconfig.o $(OBJDIR)/mode.o $(OBJDIR)/vgeom.o $(TEST_SRCS_OBJS) $(OBJDIR)/process.o $(OBJDIR)/cmdstate.o $(OBJDIR)/keyevent.o
# The scanner is pure, but the command around it replaces a byte range of
# a live buffer and reports through the echo area, so this links the same
# buffer-and-stubs set the word commands do, plus its own object.
EXTRA_dabbrev     := $(EXTRA_word) $(OBJDIR)/dabbrev.o
# The matcher is pure over a row array, but the update seam publishes
# decorations into a real buffer and reads the current window, so this
# links the buffer-backed set (TEST_SRCS_OBJS' marker.o/decor.o) plus
# mode.o, which is where chars_to_render_col() lives.
EXTRA_showparen   := $(EXTRA_buffer) $(OBJDIR)/mode.o $(OBJDIR)/vgeom.o \
                     $(OBJDIR)/showparen.o
# The git diagnostics link the same set and for the same reasons: the pure
# half reads rows and mode.o's render_to_chars_col(), and the update seam
# publishes decorations into a real buffer.  Not a backend suite -- nothing
# here names a scanner or a parser -- so it builds and runs in both
# WITH_TREE_SITTER configurations, which is the point of the module.
EXTRA_gitdiag     := $(EXTRA_buffer) $(OBJDIR)/mode.o $(OBJDIR)/vgeom.o \
                     $(OBJDIR)/gitdiag.o
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
# The framing layer is pure POSIX byte-stream machinery.  Its own direct
# suite uses the common harness baseline; framed_io.o also arrives through
# TEST_SRCS_OBJS' LSP_OBJS and is named here for readability.
EXTRA_framed_io := $(TESTDIR)/stubs.o $(OBJDIR)/framed_io.o $(TEST_SRCS_OBJS)
# The endpoint announce scanner is a libc-only parser.  Its direct suite uses
# the common harness baseline; announce.o also arrives through LSP_OBJS.
EXTRA_announce := $(TESTDIR)/stubs.o $(OBJDIR)/announce.o $(TEST_SRCS_OBJS)
# The transport depends on process.h and POSIX and on nothing else in the
# editor, so this is the minimal link: its own object, plus the baseline
# every test binary needs for test.o's harness globals.  process.o comes
# from TEST_SRCS_OBJS.
EXTRA_lsp_transport := $(TESTDIR)/stubs.o $(OBJDIR)/lsp_transport.o $(TEST_SRCS_OBJS)
# The debugger's transport is the same minimal link one axis over: its own
# object plus the baseline test.o's harness globals need.  framed_io.o and
# process.o both arrive through TEST_SRCS_OBJS (see $(PROTOCOL_OBJS)
# there); dap_transport.o itself does too, through $(DAP_OBJS), and is
# named for readability as every suite above does.
EXTRA_dap_transport := $(TESTDIR)/stubs.o $(OBJDIR)/dap_transport.o $(TEST_SRCS_OBJS)
# The protocol brain one layer up, and the same minimal link: it holds no
# editor state at all, so its suite needs the transport, the JSON layer and
# the baseline test.o's harness globals reach for, and nothing else.  Both
# dap_*.o arrive through TEST_SRCS_OBJS' $(DAP_OBJS) and json.o through
# $(LSP_OBJS); all three are named for readability, as the suites above do.
EXTRA_dap_client := $(TESTDIR)/stubs.o $(OBJDIR)/dap_client.o \
                    $(OBJDIR)/dap_transport.o $(OBJDIR)/json.o \
                    $(TEST_SRCS_OBJS)
# The configuration reader: src/json.o and the C library, which is the whole
# of what a file parser needs.  The baseline is here for test.o's harness
# globals, as every suite above.
EXTRA_dap_config := $(TESTDIR)/stubs.o $(OBJDIR)/dap_config.o \
                    $(OBJDIR)/json.o $(TEST_SRCS_OBJS)
# The session on top of both: the client, the transport, the JSON layer and
# the configuration records it starts an adapter from.  Still no editor
# object -- a session holds no buffer, window or command -- which is what
# lets this suite drive a real adapter with no editor linked.
EXTRA_dap_session := $(TESTDIR)/stubs.o $(OBJDIR)/dap_session.o \
                     $(OBJDIR)/dap_config.o $(OBJDIR)/dap_client.o \
                     $(OBJDIR)/dap_transport.o $(OBJDIR)/json.o \
                     $(TEST_SRCS_OBJS)
# The breakpoint table is the first DAP module that IS editor state, so its
# suite is the first that cannot use the minimal baseline above: it needs
# real buffers with real files behind them (realpath() is the table's key),
# real markers to anchor to, real buf_open_path()/buf_kill() so the two
# lifecycle events are published by the editor rather than by the test, and
# the session stack underneath so a case can drive one against the fake
# adapter.  That is EXTRA_winmgr's link -- the one other suite that opens
# and kills real buffers -- plus dap_breakpoint.o, which also arrives
# through TEST_SRCS_OBJS' $(DAP_OBJS) and is named here for readability as
# every suite above does.
EXTRA_dap_breakpoint := $(EXTRA_winmgr) $(OBJDIR)/dap_breakpoint.o
# The stop model sits on the session and holds no editor state either, so
# its suite is the session's link plus dap_exec.o and the breakpoint table
# it routes `breakpoint` events and temporary breakpoints through.  Both
# arrive through TEST_SRCS_OBJS' $(DAP_OBJS); named here for readability,
# as every suite above does.
EXTRA_dap_exec := $(EXTRA_dap_session) $(OBJDIR)/dap_exec.o \
                  $(OBJDIR)/dap_breakpoint.o
# The commands are the one debugger file that reaches the command table,
# so their suite links the same everything-but-main.c set EXTRA_cmd does --
# which is also the only link in which src/dap_commands.o exists at all.
EXTRA_dap_commands := $(EXTRA_cmd)
# The panes are the other file in that link and are reachable only from it:
# they write special buffers, arrange windows and are bound to keys, so this
# suite is the everything-but-main.c set too.
EXTRA_dap_ui := $(EXTRA_cmd)
# The JSON layer depends on the C library and nothing else -- not even
# process.h -- so its own object would link on its own; the baseline is
# here because test.o's harness reaches the editor globals stubs.o and
# TEST_SRCS_OBJS provide, exactly as the transport's suite does.
# json.o itself arrives through TEST_SRCS_OBJS' $(LSP_OBJS), and is
# named again for readability.
EXTRA_lsp_json := $(TESTDIR)/stubs.o $(OBJDIR)/json.o $(TEST_SRCS_OBJS)
# The client and the registry above it: the protocol state machine, the
# server specs and the workspace-root walk, plus the two modules below them.
# Still no editor object -- lsp_server.c reaches syntax.h for `enum
# kg_mode_id` and nothing else in it, so syntax.o is not needed.  All four
# lsp_*.o arrive through TEST_SRCS_OBJS' $(LSP_OBJS); named here for
# readability, as the two suites above do.
EXTRA_lsp_client := $(TESTDIR)/stubs.o $(OBJDIR)/lsp_client.o \
                    $(OBJDIR)/lsp_server.o $(OBJDIR)/lsp_transport.o \
                    $(OBJDIR)/json.o $(OBJDIR)/lsp_uri.o \
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
                  $(OBJDIR)/lsp_transport.o $(OBJDIR)/json.o
# The log buffer is the other LSP module that writes to a buffer, so its
# suite links what lsp_sync's does and for the same reason: real bufmgr.o,
# so `*lsp-log*` is a buffer the editor made rather than a stand-in.  Its
# own object is named because lsp_log.c is outside LSP_OBJS (see LSP_ALL),
# and lsp_client.o -- whose log hook it installs -- arrives through
# TEST_SRCS_OBJS.
EXTRA_lsp_log := $(EXTRA_buffer) $(OBJDIR)/lsp_log.o
# xref.c is the command layer: buffers, windows, the echo area, the command
# table and the whole LSP stack underneath it.  Stubbing that is stubbing the
# editor, so this links the same everything-but-main.c set EXTRA_cmd does --
# and it is the only test binary that links src/xref.o at all, which is why
# xref.c is not in LSP_OBJS.  What it actually drives is the pure half,
# xref_location_of(): a location parser is where a server's three answer
# shapes are either read right or navigated wrong.
EXTRA_xref        := $(EXTRA_cmd)
# Applying a WorkspaceEdit reaches the buffer table, the edit gateway and
# the undo stack, and completion's prefix scanner reaches dabbrev's -- so
# this links the same everything-but-main.c set EXTRA_cmd does, for
# EXTRA_xref's reason.
EXTRA_lsp_edit    := $(EXTRA_cmd)
# The diagnostics store and the hover renderer, together: both are the
# command layer, both reach buffers, decorations and the echo area, and
# the store's position conversion is only observable against a real
# buffer's rows -- so this links the same everything-but-main.c set
# EXTRA_xref does, and for the same reason.
EXTRA_lsp_diag    := $(EXTRA_cmd)
# fileline.c answers "what does line N of that file say" from an open
# buffer or from disk, so its suite needs real buffers holding real files:
# EXTRA_buffer's set (bufmgr.o for buf_open_path(), fileio.o to load one)
# plus the module itself.
EXTRA_fileline    := $(EXTRA_buffer) $(OBJDIR)/fileline.o
# The visit-time write-protection verdict is a def.h inline over a path
# and the file system: no editor module is involved in deciding whether a
# file may be written, so this links the same minimal baseline
# EXTRA_localvars does -- stubs for the globals test.o itself reaches, and
# nothing else.
EXTRA_readonly    := $(TESTDIR)/stubs.o $(TEST_SRCS_OBJS)

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
$(PERFOBJDIR)/lisp_locals.o: $(OBJDIR)/lisp_internal.h $(OBJDIR)/lisp_locals.h
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

# The feature stamp guards these for $(PERF_SRC_OBJS)' reason exactly: a
# test object is compiled with CFLAGS, feature defines and all, and the
# suites that read them (test/test_async.c, test/test_keymap.c) assert
# DIFFERENT things per configuration.  Without this, `make; make WITH_DAP=0
# check-unit` links a stale test object against freshly disabled src/*.o
# and fails in a way that is neither configuration.
$(TESTDIR)/%.o: $(TESTDIR)/%.c $(HDRS) $(FEATURE_CONFIG)
	$(CC) $(CFLAGS) -I$(OBJDIR) -c $< -o $@

$(TESTDIR)/test_lisp.o: $(OBJDIR)/lisp.h

# The deliberately-unloadable grammars test_syntax_tree_sitter's ABI guard
# is asserted against: one source built twice, once below the ABI floor and
# once above the ceiling, under the sonames the loader looks for.  An
# order-only prerequisite, so neither .so reaches the suite's link line --
# they are dlopen'd, like every other grammar -- and compiled with none of
# $(CFLAGS), because what they stand in for is a third-party binary.
$(TS_FAKE_GRAMMAR_DIR)/libtree-sitter-kgfakeold.so: KG_FAKE_ABI = 6
$(TS_FAKE_GRAMMAR_DIR)/libtree-sitter-kgfakenew.so: KG_FAKE_ABI = 999
$(TS_FAKE_GRAMMARS): $(TESTDIR)/fake_ts_grammar.c
	@mkdir -p $(@D)
	$(CC) -shared -fPIC \
		-DKG_FAKE_GRAMMAR=$(patsubst libtree-sitter-%.so,%,$(@F)) \
		-DKG_FAKE_ABI=$(KG_FAKE_ABI) -o $@ $<

$(TESTDIR)/test_syntax_tree_sitter: | $(TS_FAKE_GRAMMARS)

$(FUZZBIN): $(FUZZ_SRCS) $(HDRS) $(FUZZ_FE_OBJ) $(FEATURE_CONFIG)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -Ife/tiny-regex-c -o $@ $(FUZZ_SRCS) \
		$(FUZZ_FE_OBJ) $(LDLIBS)

$(FUZZBIN_SYNTAX): $(FUZZ_SYNTAX_SRCS) $(HDRS) $(FUZZ_FE_OBJ) $(FEATURE_CONFIG)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -Ife/tiny-regex-c -o $@ $(FUZZ_SYNTAX_SRCS) \
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

$(FUZZBIN_LSP_JSON): $(TESTDIR)/fuzz_lsp_json.c $(OBJDIR)/json.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_lsp_json.c $(OBJDIR)/json.c -lm

$(FUZZBIN_WIDTH): $(TESTDIR)/fuzz_width.c $(OBJDIR)/width.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_width.c $(OBJDIR)/width.c

$(FUZZBIN_KEYBIND): $(TESTDIR)/fuzz_keybind.c $(OBJDIR)/keybind.c $(OBJDIR)/keymap.c $(OBJDIR)/keyevent.c $(OBJDIR)/width.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_keybind.c $(OBJDIR)/keybind.c \
		$(OBJDIR)/keymap.c $(OBJDIR)/keyevent.c $(OBJDIR)/width.c

# framed_io is independent of LSP policy; process.c supplies only the pipe
# helper used by the harness.  Each delivered frame goes to the same JSON
# parser a protocol client calls: four translation units, no stubs or def.h.
$(FUZZBIN_FRAMES): $(TESTDIR)/fuzz_frames.c $(OBJDIR)/framed_io.c \
                   $(OBJDIR)/json.c $(OBJDIR)/process.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_frames.c $(OBJDIR)/framed_io.c \
		$(OBJDIR)/json.c $(OBJDIR)/process.c

# The whole protocol stack under the harness, and nothing else: the client
# holds no editor state, so this is the same four files its unit suite
# links.
$(FUZZBIN_DAP_DISPATCH): $(TESTDIR)/fuzz_dap_dispatch.c \
                   $(OBJDIR)/dap_client.c $(OBJDIR)/dap_transport.c \
                   $(OBJDIR)/framed_io.c $(OBJDIR)/json.c \
                   $(OBJDIR)/process.c $(HDRS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ \
		$(TESTDIR)/fuzz_dap_dispatch.c $(OBJDIR)/dap_client.c \
		$(OBJDIR)/dap_transport.c $(OBJDIR)/framed_io.c \
		$(OBJDIR)/json.c $(OBJDIR)/process.c

$(TESTDIR)/fe_fuzz.o: fe/fe.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

$(TESTDIR)/fe_eval_fuzz.o: fe/fe_eval.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

$(TESTDIR)/fe_run_fuzz.o: fe/fe_run.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

$(TESTDIR)/fe_unwind_fuzz.o: fe/fe_unwind.c fe/fe.h fe/fe_internal.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(FE_OBJ) $(REGEX_OBJS) $(SYNTAX_BACKEND_ALL) $(LSP_ALL) \
	      $(DAP_ALL) \
	      $(OBJDIR)/.features-* $(OBJDIR)/.with-lisp-* $(TESTDIR)/*.o \
	      $(TESTBINS) $(TESTDIR)/kgbatch $(GC_STRESS_KGBATCH) \
	      $(FUZZBINS) $(TESTDIR)/fuzz_lsp_frames $(REGEX_DIFF_BIN)
	rm -rf $(PERFOBJDIR) $(TS_FAKE_GRAMMAR_DIR)

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

.PHONY: all clean distclean check header-check lisp-include-check docs-check lisp-compat-check lisp-compat-oracle lisp-prelude-generate lisp-prelude-check lisp-package-check lisp-oracle-check forecast-audit forecast-check forecast-init-check check-unit check-pty-tokens check-pty check-regex-differential \
	bench bench-lisp-toggle complexity complexity-check \
	pmccabe pmccabe-check pmccabe-baseline gateway-check gateway-baseline coverage coverage-check coverage-baseline coverage-clean format format-check compile-db iwyu \
	fuzz-keypress fuzz-keypress-seed fuzz-keypress-smoke \
	fuzz-syntax fuzz-syntax-seed fuzz-syntax-smoke \
	fuzz-lsp-json fuzz-lsp-json-seed fuzz-lsp-json-smoke \
	fuzz-width fuzz-width-seed fuzz-width-smoke \
	fuzz-keybind fuzz-keybind-seed fuzz-keybind-smoke \
	fuzz-dirlocals fuzz-dirlocals-seed fuzz-dirlocals-smoke \
	fuzz-regex fuzz-regex-seed fuzz-regex-smoke fuzz-regex-seed-replay \
	fuzz-localvars fuzz-localvars-seed fuzz-localvars-smoke \
	fuzz-compile-parse fuzz-compile-parse-seed fuzz-compile-parse-smoke \
	fuzz-frames fuzz-frames-seed fuzz-frames-smoke \
	fuzz-seed fuzz-smoke \
	deb release install uninstall
