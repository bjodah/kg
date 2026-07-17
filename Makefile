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
       shell.c path.c rect.c lisp.c keybind.c mode.c localvars.c compile.c

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
           $(TESTDIR)/test_tty
FUZZBIN = $(TESTDIR)/fuzz_keypress
FUZZ_SRCS = $(TESTDIR)/fuzz_keypress.c $(TESTDIR)/fuzz_stubs.c \
	    $(OBJDIR)/kbd.c $(OBJDIR)/buffer.c $(OBJDIR)/basic.c \
	    $(OBJDIR)/word.c $(OBJDIR)/autocomplete.c $(OBJDIR)/yank.c \
	    $(OBJDIR)/undo.c $(OBJDIR)/rect.c $(OBJDIR)/syntax.c \
	    $(OBJDIR)/tty.c $(OBJDIR)/macro.c $(OBJDIR)/lisp.c \
	    $(OBJDIR)/keybind.c
PTY_TESTS = $(sort $(wildcard $(TESTDIR)/pty/*.yaml))
# Source objects needed by tests (subset of OBJS, no main/tty/display/etc.)
TEST_SRCS_OBJS = $(OBJDIR)/undo.o $(OBJDIR)/buffer.o $(OBJDIR)/syntax.o
TEST_RUNNER ?=
KG_RUNNER ?=
PTY_ACCEPT_ARGS ?=
PTY_TIMEOUT ?= 15.0
PTY_STARTUP_DELAY_ADD ?=
PTY_KEY_DELAY_ADD ?=
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
SCC_COMPLEXITY_MAX ?= 3400
SCC_FILE_COMPLEXITY_MAX ?= 420
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(addprefix $(OBJDIR)/,$(SRCS))
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 125
COVERAGE_DIR ?= coverage
COVERAGE_CFLAGS ?= -Wall -W -pedantic -std=c23 -O0 -g --coverage
COVERAGE_LCOV_ARGS ?= --quiet --ignore-errors inconsistent,gcov
COVERAGE_GENHTML_ARGS ?= --quiet
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

$(OBJDIR)/regex.o: src/regex.c src/regex.h fe/tiny-regex-c/re.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/lisp.o: $(OBJDIR)/lisp.c $(OBJDIR)/lisp.h
$(OBJDIR)/main.o: $(OBJDIR)/lisp.h

$(OBJDIR)/fe.o: fe/fe.c fe/fe.h
	$(CC) $(FE_CFLAGS) -c $< -o $@

check: check-unit check-pty

check-unit: $(TESTBINS)
	@pass=0; fail=0; \
	for t in $(TESTBINS); do \
		name=$$(basename $$t); \
		log=$$(mktemp); \
		if $(TEST_RUNNER) $$t >$$log 2>&1; then \
			echo "PASS: $$name"; pass=$$((pass+1)); \
		else \
			echo "FAIL: $$name"; fail=$$((fail+1)); \
			cat $$log; \
		fi; \
		rm -f $$log; \
	done; \
	total=$$((pass+fail)); \
	echo ""; \
	echo "============================================================================"; \
	echo "Testsuite summary for kg"; \
	echo "============================================================================"; \
	printf "# TOTAL: %d\n# PASS:  %d\n# SKIP:  0\n# XFAIL: 0\n# FAIL:  %d\n# XPASS: 0\n# ERROR: 0\n" \
	       $$total $$pass $$fail; \
	echo "============================================================================"; \
	[ $$fail -eq 0 ]

check-pty: $(TARGET) $(PTY_TESTS)
	@python utils/pty_accept.py $(PTY_ACCEPT_ARGS) \
		$(if $(PTY_TIMEOUT),--timeout $(PTY_TIMEOUT),) \
		$(if $(PTY_STARTUP_DELAY_ADD),--startup-delay-add $(PTY_STARTUP_DELAY_ADD),) \
		$(if $(PTY_KEY_DELAY_ADD),--key-delay-add $(PTY_KEY_DELAY_ADD),) \
		--kg $(TARGET) --kg-runner "$(KG_RUNNER)" $(PTY_TESTS)

fuzz-keypress: $(FUZZBIN)

fuzz-keypress-smoke: $(FUZZBIN)
	mkdir -p $(TESTDIR)/fuzz-corpus/keypress
	./$(FUZZBIN) -runs=1000 $(TESTDIR)/fuzz-corpus/keypress

complexity:
	$(SCC) --ci --by-file --sort complexity $(SCC_PATHS)

complexity-check:
	$(SCC) --ci --by-file --format json $(SCC_COMPLEXITY_PATHS) | \
		python3 utils/check_scc_complexity.py \
			--max-total $(SCC_COMPLEXITY_MAX) \
			--max-file $(SCC_FILE_COMPLEXITY_MAX)

pmccabe:
	$(PMCCABE) $(PMCCABE_PATHS) | sort -nr

pmccabe-check:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		python3 utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX)

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
EXTRA_undo         := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS)
EXTRA_buffer       := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_syntax       := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS)
EXTRA_yank         := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_autocomplete := $(TESTDIR)/stubs.o $(TESTDIR)/stubs_extra.o $(OBJDIR)/autocomplete.o $(TEST_SRCS_OBJS)
EXTRA_word         := $(TESTDIR)/stubs.o $(TESTDIR)/stubs_extra.o $(OBJDIR)/word.o $(TEST_SRCS_OBJS)
EXTRA_basic        := $(TESTDIR)/stubs.o          $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(TEST_SRCS_OBJS)
EXTRA_region       := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(TEST_SRCS_OBJS)
EXTRA_shell        := $(TESTDIR)/stubs_noyank.o   $(OBJDIR)/shell.o $(OBJDIR)/yank.o $(OBJDIR)/rect.o $(OBJDIR)/buffer.o $(OBJDIR)/undo.o $(OBJDIR)/syntax.o
EXTRA_complete     := $(TESTDIR)/stubs.o          $(OBJDIR)/path.o $(TEST_SRCS_OBJS)
EXTRA_lisp         := $(TESTDIR)/stubs.o          $(OBJDIR)/basic.o $(OBJDIR)/mode.o $(TEST_SRCS_OBJS) $(OBJDIR)/lisp.o $(OBJDIR)/keybind.o $(FE_OBJ)
EXTRA_regex        := $(TESTDIR)/stubs.o          $(TEST_SRCS_OBJS) $(REGEX_OBJS)
EXTRA_localvars    := $(TESTDIR)/stubs.o          $(OBJDIR)/localvars.o $(TEST_SRCS_OBJS)
EXTRA_compile     := $(TESTDIR)/stubs_noyank.o  $(OBJDIR)/compile.o
EXTRA_tty         := $(TESTDIR)/stubs.o          $(OBJDIR)/tty.o $(TEST_SRCS_OBJS)

.SECONDEXPANSION:
$(TESTBINS): $(TESTDIR)/test_%: $(TESTDIR)/test_%.o $(TESTDIR)/test.o $$(EXTRA_$$*)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TESTDIR)/%.o: $(TESTDIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -I$(OBJDIR) -c $< -o $@

$(TESTDIR)/test_lisp.o: $(OBJDIR)/lisp.h

$(FUZZBIN): $(FUZZ_SRCS) $(HDRS) $(FUZZ_FE_OBJ) $(LISP_CONFIG)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(OBJDIR) -o $@ $(FUZZ_SRCS) \
		$(FUZZ_FE_OBJ) $(LDLIBS)

$(TESTDIR)/fe_fuzz.o: fe/fe.c fe/fe.h
	$(FUZZ_CC) $(FE_FUZZ_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJDIR)/fe.o $(REGEX_OBJS) $(OBJDIR)/.with-lisp-* $(TESTDIR)/*.o \
	      $(TESTBINS) $(FUZZBIN)

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

.PHONY: all clean distclean check check-unit check-pty complexity complexity-check \
	pmccabe pmccabe-check coverage coverage-clean format format-check compile-db iwyu \
	fuzz-keypress fuzz-keypress-smoke deb release install uninstall
