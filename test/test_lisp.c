/* test_lisp.c - Fe interpreter lifecycle regression tests */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "../src/keybind.h"
#include "../src/keymap.h"
#include "../src/lisp.h"
#include "../src/process_table.h"
#include "../src/word.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char test_status_message[512];
extern char test_command_name[128];
extern int test_command_calls;
/* stubs_noyank.c's cmd_invoke() reaches Lisp commands through these,
 * because it is also linked into binaries with no Lisp objects at all.
 * Installed by main() below, which is what makes (command-execute
 * 'lisp-cmd) a real nested activation in these tests rather than the
 * CMD_UNKNOWN the stub answered before. */
extern int (*test_lisp_command_exists)(const char *name);
extern int (*test_lisp_command_run)(const char *name, int fd);
extern int test_run_command_with_prefix(
    const char *name, struct command_prefix prefix);

/* word.o is linked for forward-word/backward-word, and basic.o for the
 * window-cycle commands; their interactive primitives drag in terminal
 * entry points the Lisp tests never reach. */
void editor_refresh_screen(void) { }
void probe_window_size(void) { }
int tty_write(const void *buf, size_t n)
{
	(void)buf;
	(void)n;
	return 0;
}

int editor_read_raw_byte(int fd)
{
	(void)fd;
	return 0;
}

static void setup_editor(void)
{
	free_all_rows();
	undo_free();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	winlist[0].active = 1;
	wcur()->buf = buf_handle(buf_current);
	bcur()->filename = "/tmp/bridge.txt";
	/* The current slot counts, exactly as buf_load_args() makes it count
	 * in the editor.  Without this the harness believed it held one
	 * buffer fewer than it does, and buf_kill_buffer()'s
	 * keep-the-last-one rule refused a kill that the editor allows --
	 * the same omission test/kgbatch.c had. */
	buf_count = 1;
	undo_init();
	test_status_message[0] = '\0';
	test_command_name[0] = '\0';
	test_command_calls = 0;
}

static void teardown_editor(void)
{
	free_all_rows();
	bcur()->row = nullptr;
	bcur()->numrows = 0;
	undo_free();
}

static void test_disabled(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() != 0);
	CHECK(kg_lisp_eval_string("(+ 1 2)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not compiled in") != nullptr);
	CHECK(kg_lisp_load_file("unused.el") != 0);
	CHECK(strstr(kg_lisp_last_error(), "not compiled in") != nullptr);
	CHECK(kg_lisp_load_init() == 0);
	/* No variables exist here, so the startup path's question always
	 * answers "not asked for" and the startup screen shows. */
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") == 0);
	kg_lisp_set_interrupt_check(nullptr);
	kg_lisp_shutdown();
	kg_lisp_shutdown();
}

static void test_lifecycle(void)
{
	char result[128] = "";

	kg_lisp_shutdown();
	CHECK(kg_lisp_eval_string("1", 1, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not initialized") != nullptr);
	kg_lisp_shutdown();
	CHECK(strstr(kg_lisp_last_error(), "not initialized") != nullptr);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_init() == 0);
	kg_lisp_shutdown();
	kg_lisp_shutdown();
}

/* ------------------------- $KG_LISP_ARENA_BYTES ------------------------
 *
 * Phase B of doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md:
 * the arena size stopped being a constant of the build.  Three tests --
 * what kg accepts as a size, what it refuses and how, and whether the
 * floor it refuses against still matches the measurement it was derived
 * from.
 */
static const char arena_env[] = "KG_LISP_ARENA_BYTES";

/* `reachable_live_objects' from .ci/prelude-startup-census.json -- what
 * survives the forced collection kg_lisp_init() ends with, which is the
 * measurement the arena floor is three times.  Scraped rather than
 * parsed: the file is one flat object and a JSON parser in a test would
 * be more machinery than the question needs, exactly as
 * test_prelude_source_file() below scans the prelude's own text.  The
 * editor never reads this file; only this test does. */
static size_t census_reachable_live_objects(void)
{
	static const char *const paths[] = {
		".ci/prelude-startup-census.json",
		"../.ci/prelude-startup-census.json",
	};
	static const char key[] = "\"reachable_live_objects\":";
	static char text[8192];
	const char *found;
	FILE *fp = nullptr;
	size_t i, len;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fp = fopen(paths[i], "r");
		if (fp != nullptr) {
			break;
		}
	}
	CHECK(fp != nullptr);
	if (fp == nullptr) {
		return 0;
	}
	len = fread(text, 1, sizeof(text) - 1, fp);
	fclose(fp);
	text[len] = '\0';
	/* Twice in the file: once in "measured_with" as a description, once
	 * in "ceiling" as the number.  The last one is the number. */
	found = strstr(text, key);
	CHECK(found != nullptr);
	while (found != nullptr) {
		const char *next = strstr(found + 1, key);

		if (next == nullptr) {
			break;
		}
		found = next;
	}
	if (found == nullptr) {
		return 0;
	}
	return strtoul(found + sizeof(key) - 1, nullptr, 10);
}

/* The floor, learned the way a user learns it: from the refusal.  Nothing
 * here knows the constant in src/lisp_core.c, so a floor that moves moves
 * these assertions with it rather than past them. */
static size_t refused_arena_floor(void)
{
	static const char marker[] = "below the ";
	const char *found;

	setenv(arena_env, "1", 1);
	CHECK(kg_lisp_init() != 0);
	/* A no-op after a failed init, and the one thing that keeps a
	 * surprise success here from leaking a context into every test
	 * after it. */
	kg_lisp_shutdown();
	unsetenv(arena_env);
	found = strstr(kg_lisp_last_error(), marker);
	CHECK(found != nullptr);
	return found == nullptr
	    ? 0
	    : strtoul(found + sizeof(marker) - 1, nullptr, 10);
}

/* One size, three spellings, and everything that is not a size at all.
 * The size is above the compiled default on purpose: the assertion that
 * makes this more than a parser test is that the slots follow the bytes,
 * and that reads clearest upwards.  Which number is above the default is
 * Phase B's to keep -- it was 2M while the default was 1 MiB. */
static void test_arena_env_parsing(void)
{
	static const char *const twenty_mib[] = { "20M", "20480K", "20971520" };
	/* Empty, blank, signed, hex, trailing text, a suffix kg does not
	 * know, and two values that overflow size_t -- before scaling and
	 * after it.  None of them is an answer, so each leaves the compiled
	 * default alone, as an unreadable $KG_LSP_TIMEOUT_MS does. */
	static const char *const not_a_size[] = { "", " ", "1 ", " 1", "abc",
		"12x", "0x10", "-1", "+1", "1K2", "1MB", "1.5M", "M",
		"18446744073709551616", "18014398509481984M" };
	struct kg_lisp_arena_stats compiled, measured;
	size_t i;

	unsetenv(arena_env);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&compiled) == 0);
	kg_lisp_shutdown();

	for (i = 0; i < sizeof(twenty_mib) / sizeof(twenty_mib[0]); i++) {
		setenv(arena_env, twenty_mib[i], 1);
		CHECK(kg_lisp_init() == 0);
		CHECK(kg_lisp_arena_stats(&measured) == 0);
		CHECKF(measured.arena_bytes == 20U * 1024U * 1024U,
		    "%s opened %zu bytes", twenty_mib[i], measured.arena_bytes);
		/* A bigger arena is a bigger arena: the slots follow the
		 * bytes, which is the whole point of the knob. */
		CHECK(measured.total_slots > compiled.total_slots);
		kg_lisp_shutdown();
	}
	for (i = 0; i < sizeof(not_a_size) / sizeof(not_a_size[0]); i++) {
		setenv(arena_env, not_a_size[i], 1);
		CHECK(kg_lisp_init() == 0);
		CHECK(kg_lisp_arena_stats(&measured) == 0);
		CHECKF(measured.arena_bytes == compiled.arena_bytes,
		    "\"%s\" opened %zu bytes, not the compiled %zu",
		    not_a_size[i], measured.arena_bytes, compiled.arena_bytes);
		kg_lisp_shutdown();
	}
	unsetenv(arena_env);
}

/* A size kg understands and refuses: it says so, naming the variable and
 * the floor, and starts nothing.  The one thing that must never happen
 * here is a smaller arena opened in silence. */
static void test_arena_env_floor_refused(void)
{
	struct kg_lisp_arena_stats stats;
	char result[128] = "";

	setenv(arena_env, "1024", 1);
	CHECK(kg_lisp_init() != 0);
	CHECK(kg_lisp_config_refused() == 1);
	CHECK(strstr(kg_lisp_last_error(), "KG_LISP_ARENA_BYTES=1024")
	    != nullptr);
	CHECK(strstr(kg_lisp_last_error(), "minimum Lisp arena") != nullptr);
	/* Nothing was opened, so the session is the Lisp-less one every
	 * caller already knows how to answer for. */
	CHECK(kg_lisp_arena_stats(&stats) != 0);
	CHECK(kg_lisp_eval_string("1", 1, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not initialized") != nullptr);

	/* A size so large that aligning it up overflows size_t is a failure
	 * rather than a refusal: kg understood the value and could not use
	 * it, which is the pre-existing "cannot start" answer and stays
	 * fatal where a refusal does not. */
	setenv(arena_env, "18446744073709551615", 1);
	CHECK(kg_lisp_init() != 0);
	CHECK(kg_lisp_config_refused() == 0);
	CHECK(strstr(kg_lisp_last_error(), "overflow") != nullptr);
	kg_lisp_shutdown();

	/* The refusal is about the configuration it was handed, not a state
	 * kg stays in: a later init on a sound one clears it. */
	unsetenv(arena_env);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_config_refused() == 0);
	kg_lisp_shutdown();
}

/* The floor is a byte count in src/lisp_core.c and the margin it exists to
 * keep is a slot count in .ci/prelude-startup-census.json, measured by a
 * different tool at a different time.  Nothing in C ties the two together
 * -- fe decides how many slots a byte count buys, and it has moved that
 * partition at several pins already -- so this test opens a real arena at
 * exactly the floor and asks the arena itself, against the census's own
 * number.  A prelude that grows past a third of the floor's slots fails
 * here, which is the drift this is for; so does a floor raised so far past
 * the measurement that it has stopped tracking it. */
static void test_arena_floor_matches_census(void)
{
	size_t floor_bytes = refused_arena_floor();
	size_t reachable = census_reachable_live_objects();
	struct kg_lisp_arena_stats stats;
	char text[32];

	CHECK(floor_bytes > 0);
	CHECK(reachable > 0);
	if (floor_bytes == 0 || reachable == 0) {
		return;
	}
	/* The floor is a floor: one byte under it is refused. */
	snprintf(text, sizeof(text), "%zu", floor_bytes - 1);
	setenv(arena_env, text, 1);
	CHECK(kg_lisp_init() != 0);
	kg_lisp_shutdown();

	snprintf(text, sizeof(text), "%zu", floor_bytes);
	setenv(arena_env, text, 1);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	CHECK(stats.arena_bytes == floor_bytes);
	CHECKF(stats.total_slots >= reachable * 3,
	    "the %zu-byte floor opens %zu slots, under the %zu the census's "
	    "%zu reachable objects need three of",
	    floor_bytes, stats.total_slots, reachable * 3, reachable);
	CHECKF(stats.total_slots < reachable * 6,
	    "the %zu-byte floor opens %zu slots, more than twice the %zu it "
	    "is supposed to be tracking",
	    floor_bytes, stats.total_slots, reachable * 3);
	/* And the prelude really does fit in it, with the margin above. */
	CHECK(stats.peak_live_objects <= reachable * 3);
	CHECK(stats.allocation_failures == 0);
	kg_lisp_shutdown();
	unsetenv(arena_env);
}

static void test_sized_input(void)
{
	char result[128] = "";
	static const char embedded_nul[] = { '1', '\0', '2' };
	static const char truncated[] = "(+ 1 2) ignored";

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(
		  embedded_nul, sizeof(embedded_nul), result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "eval:") != nullptr);
	CHECK(strstr(result, "embedded NUL byte") != nullptr);
	CHECK(kg_lisp_eval_string(truncated, 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "3") == 0);
	CHECK(kg_lisp_eval_string(truncated, 6, result, sizeof(result)) != 0);
	CHECK(strstr(result, "unclosed list") != nullptr);
	CHECK(kg_lisp_eval_string("9", 1, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "9") == 0);
	kg_lisp_shutdown();
}

static void test_eval_and_recovery(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(+ 1 2)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "3") == 0);
	CHECK(kg_lisp_eval_string("(car 1)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, "eval:") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 3 4)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);
	kg_lisp_shutdown();
}

static void test_load_file(void)
{
	char path[] = "/tmp/kg-lisp-XXXXXX";
	char result[128] = "";
	static const char source[] = "(setq loaded-value 41)\n"
				     "(+ loaded-value 1)\n";
	int fd;
	FILE *file;

	fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0) {
		return;
	}
	file = fdopen(fd, "w");
	CHECK(file != nullptr);
	if (file == nullptr) {
		(void)close(fd);
		(void)unlink(path);
		return;
	}
	CHECK(
	    fwrite(source, 1, sizeof(source) - 1, file) == sizeof(source) - 1);
	CHECK(fclose(file) == 0);

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_load_file(path) == 0);
	CHECK(kg_lisp_last_error()[0] == '\0');
	CHECK(kg_lisp_eval_string("loaded-value", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "41") == 0);
	kg_lisp_shutdown();
	CHECK(unlink(path) == 0);
}

static void test_load_file_error(void)
{
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_load_file("/tmp/kg-lisp-missing/no-file.el") != 0);
	CHECK(strstr(kg_lisp_last_error(), "cannot open") != nullptr);
	CHECK(strstr(kg_lisp_last_error(), "no-file.el") != nullptr);
	kg_lisp_shutdown();
}

/* Write SOURCE to a fresh temporary .el file, returning its path in
 * PATH (which must be a writable "...XXXXXX" template) or false. */
static bool write_temp_lisp(char *path, const char *source)
{
	int fd = mkstemp(path);
	size_t length = strlen(source);
	FILE *file;
	bool written;

	if (fd < 0) {
		return false;
	}
	file = fdopen(fd, "w");
	if (file == nullptr) {
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	/* Close unconditionally: short-circuiting past fclose on a failed
	 * write leaks the stream, which gcc's -fanalyzer says out loud. */
	written = fwrite(source, 1, length, file) == length;
	if (fclose(file) != 0 || !written) {
		(void)unlink(path);
		return false;
	}
	return true;
}

/* Sub-plan 11D Part 3: what a completion raised inside a loaded file can
 * and cannot reach.  The compat corpus pins the same two answers against
 * the pinned Emacs (load-error-condition-reachability, closed here, and
 * load-throw-reachability, opened here); this runs them in-process and
 * adds the parts the corpus has no way to ask about -- that `load'
 * answers t, that the loader's own bookkeeping is unwound rather than
 * merely abandoned, and that a second load after a caught error still
 * works, which is what a leaked depth counter or a leaked load_buffers
 * slot would break. */
static void test_load_error_condition_reachability(void)
{
	char raiser[] = "/tmp/kg-lisp-raise-XXXXXX";
	char thrower[] = "/tmp/kg-lisp-throw-XXXXXX";
	char plain[] = "/tmp/kg-lisp-plain-XXXXXX";
	char form[512];
	char result[256] = "";

	CHECK(write_temp_lisp(raiser, "(car 1)\n"));
	CHECK(write_temp_lisp(thrower, "(throw 'load-tag 99)\n"));
	CHECK(write_temp_lisp(plain, "(setq loaded-marker 'ran)\n"));

	CHECK(kg_lisp_init() == 0);

	/* `load' answers t, as Emacs' does. */
	(void)snprintf(form, sizeof(form), "(load \"%s\")", plain);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "t") == 0);
	CHECK(kg_lisp_eval_string("loaded-marker", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "ran") == 0);

	/* The flip: an ordinary error inside the loaded file reaches an
	 * enclosing condition-case with its condition symbol intact,
	 * instead of transferring past it to kg's outermost barrier. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (error (car e)))", raiser);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "wrong-type-argument") == 0);
	/* Named narrowly, not just as `error': the condition object is
	 * replayed, not re-manufactured. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (wrong-type-argument 'narrow))",
	    raiser);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "narrow") == 0);

	/* The bookkeeping was unwound, not abandoned: loading again after
	 * a caught error still works.  A leaked load_depth or a leaked
	 * load_buffers slot shows up here and nowhere else. */
	(void)snprintf(form, sizeof(form), "(load \"%s\")", plain);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "t") == 0);

	/* The half that was divergent until Phase 12's fix cycle: `load' is
	 * now a prelude loop `eval'ing each form in the CURRENT run (fe's
	 * input-unit trio, FE_API_VERSION 8), so there is no containment
	 * barrier and no throw wall -- the catch receives the thrown 99,
	 * which is Emacs' answer.  The condition-case is kept from the old
	 * shape of this probe so a regression to (no-catch load-tag 99)
	 * fails on the value rather than aborting the test. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (catch 'load-tag (load \"%s\"))"
	    " (error (car e)))",
	    thrower);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "99") == 0);

	/* A missing file raises Emacs' `file-missing' (sub-plan 12D Part 2),
	 * where it used to raise a plain `error' whose message carried the
	 * path as prose.  A handler naming the narrow class catches it, and
	 * so does one naming its parent or `error'. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"/tmp/kg-lisp-missing/x.el\")"
	    " (error (car e)))");
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "file-missing") == 0);
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"/tmp/kg-lisp-missing/x.el\")"
	    " (file-missing (cdr e)))");
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result,
		  "(\"Cannot open load file\" \"No such file or directory\""
		  " \"/tmp/kg-lisp-missing/x.el\")")
	    == 0);
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"/tmp/kg-lisp-missing/x.el\")"
	    " (file-error 'by-parent))");
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "by-parent") == 0);
	/* Uncaught, the diagnostic is Emacs' error-message-string rendering
	 * of that data rather than the bare condition name fe's `signal'
	 * would otherwise leave. */
	(void)snprintf(
	    form, sizeof(form), "(load \"/tmp/kg-lisp-missing/x.el\")");
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    != 0);
	CHECK(strstr(result,
		  "Cannot open load file: No such file or directory,"
		  " /tmp/kg-lisp-missing/x.el")
	    != nullptr);
	/* Emacs drops the ": " separator when OPERATION is the empty
	 * string: (file-missing "" "d" "p") renders "d, p", not ": d, p".
	 * User-reachable via `signal' only; kg's own raises always fill
	 * OPERATION.  The "eval:1: " ahead of it is the position prefix
	 * the renderer preserves -- asserting through it is what catches
	 * a regression to "eval:1: : d, p". */
	(void)snprintf(
	    form, sizeof(form), "(signal 'file-missing '(\"\" \"d\" \"p\"))");
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "eval:1: d, p") != nullptr);

	kg_lisp_shutdown();
	CHECK(unlink(raiser) == 0);
	CHECK(unlink(thrower) == 0);
	CHECK(unlink(plain) == 0);
}

/* Phase 12 fix cycle: `load' is a prelude read-eval loop over fe's
 * input-unit trio, and these are the loop's own guarantees -- the ones
 * the barrier-shaped loader could not give or gave differently.  The
 * compat corpus pins the Emacs-comparable half (load-dynamic-extent);
 * this pins the kg-side mechanics: per-form diagnostic labels, error
 * timing, cleanup-on-cross-throw, and the depth limit surviving the
 * rebuild. */
static void test_load_incremental_loop(void)
{
	char forms[] = "/tmp/kg-lisp-forms-XXXXXX";
	char timing[] = "/tmp/kg-lisp-timing-XXXXXX";
	char cleaner[] = "/tmp/kg-lisp-clean-XXXXXX";
	char form[512];
	char result[256] = "";

	/* Form 4 sits past a blank line and a comment; a form spanning
	 * lines would drift without the per-form latch (the fe acceptance
	 * test's ablation), so the error must name line 4, not 1. */
	CHECK(write_temp_lisp(forms, "(setq lf-probe 1)\n\n;; c\n(car 6)\n"));
	/* Form 1 must RUN before form 2's reader error surfaces: Emacs
	 * reads and evaluates a load incrementally, and the audit measured
	 * kg's whole-buffer reader diverging on exactly this shape. */
	CHECK(write_temp_lisp(timing, "(setq lt-probe 41)\n(unclosed\n"));
	CHECK(write_temp_lisp(cleaner,
	    "(unwind-protect (throw 'lc-tag 5) (setq lc-cleaned 'yes))\n"));

	CHECK(kg_lisp_init() == 0);

	(void)snprintf(form, sizeof(form), "(load \"%s\")", forms);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    != 0);
	CHECK(strstr(result, forms) != nullptr);
	CHECK(strstr(result, ":4:") != nullptr);
	/* ...and the abandoned unit's label did not leak into the next
	 * diagnostic (the fe fix cycle's B1, seen from kg). */
	CHECK(kg_lisp_eval_string("(car 7)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, forms) == nullptr);

	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (error lt-probe))", timing);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "41") == 0);

	/* A cleanup in the loading frame runs while a throw crosses the
	 * load on its way to the caller's catch. */
	(void)snprintf(form, sizeof(form),
	    "(progn (setq lc-cleaned 'no)"
	    " (list (catch 'lc-tag (load \"%s\")) lc-cleaned))",
	    cleaner);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "(5 yes)") == 0);

	/* The depth limit survived the rebuild: a self-loading file stops
	 * at LISP_MAX_LOAD_DEPTH with the loader's own diagnostic, not in
	 * the C stack.  The file loads itself by its final name, so the
	 * source is written after mkstemp fixed it. */
	{
		char self[] = "/tmp/kg-lisp-self-XXXXXX";
		char source[128];
		FILE *file;

		CHECK(write_temp_lisp(self, ";; placeholder\n"));
		(void)snprintf(source, sizeof(source), "(load \"%s\")\n", self);
		file = fopen(self, "w");
		CHECK(file != nullptr);
		CHECK(
		    fwrite(source, 1, strlen(source), file) == strlen(source));
		CHECK(fclose(file) == 0);
		(void)snprintf(form, sizeof(form), "(load \"%s\")", self);
		CHECK(kg_lisp_eval_string(
			  form, strlen(form), result, sizeof(result))
		    != 0);
		CHECK(strstr(result, "load depth limit exceeded") != nullptr);
		CHECK(unlink(self) == 0);
	}

	kg_lisp_shutdown();
	CHECK(unlink(forms) == 0);
	CHECK(unlink(timing) == 0);
	CHECK(unlink(cleaner) == 0);
}

static int interrupt_polls;

static int cancel_evaluation(void) { return ++interrupt_polls >= 2; }

static void test_interrupt(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() == 0);
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(kg_lisp_eval_string("(while t 1)", 11, result, sizeof(result))
	    != 0);
	CHECK(strcmp(result, "Quit") == 0);
	kg_lisp_set_interrupt_check(nullptr);
	CHECK(kg_lisp_eval_string("(+ 2 3)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);
	kg_lisp_shutdown();
}

/* The two cleanup-delivery divergences from Emacs the Phase 12 docs
 * review found (manifest row cleanup-raise-residuals; fe's twin row is
 * unwind-protect-cleanup-raise-residuals): both PRE-EXISTING, pinned
 * here so a change of answer is visible.  Emacs answers
 * (OUT (error "cleanup")) and (no-catch b 2) respectively; fe delivers
 * to frames the abandoned computation left on the stack. */
static void test_cleanup_raise_residuals(void)
{
	const char *body = "(condition-case o (catch 'tg (unwind-protect"
			   " (condition-case nil (throw 'tg 'body)"
			   " (error 'IN)) (error \"cleanup\")))"
			   " (error (list 'OUT o)))";
	const char *exited = "(catch 'a (unwind-protect (catch 'b"
			     " (throw 'a 1)) (throw 'b 2)))";
	char result[128] = "";

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(body, strlen(body), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "IN") == 0);
	CHECK(
	    kg_lisp_eval_string(exited, strlen(exited), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "2") == 0);
	kg_lisp_shutdown();
}

/* Quit during load is still a quit after the Phase 12 fix-cycle loader
 * rebuild: C-g inside a loaded form's evaluation cancels the whole
 * evaluation as KG_LISP_ERROR_QUIT, and the loader's bookkeeping is
 * unwound (the prelude loop's cleanup rode fe's drain), so loading
 * again afterwards works. */
static void test_load_quit(void)
{
	char looper[] = "/tmp/kg-lisp-loop-XXXXXX";
	char form[256];
	char result[128] = "";

	CHECK(write_temp_lisp(looper, "(while t 1)\n"));
	CHECK(kg_lisp_init() == 0);
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	(void)snprintf(form, sizeof(form), "(load \"%s\")", looper);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    != 0);
	CHECK(strcmp(result, "Quit") == 0);
	CHECK(kg_lisp_last_error_kind() == KG_LISP_ERROR_QUIT);
	kg_lisp_set_interrupt_check(nullptr);
	/* The depth counter and buffer slot were released on the way out. */
	{
		FILE *file = fopen(looper, "w");

		CHECK(file != nullptr);
		CHECK(fputs("(setq lq-after 'ok)\n", file) >= 0);
		CHECK(fclose(file) == 0);
	}
	(void)snprintf(form, sizeof(form), "(load \"%s\")", looper);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "t") == 0);
	kg_lisp_shutdown();
	CHECK(unlink(looper) == 0);
}

static void test_message_arity(void)
{
	char result[128] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(message)", 9, result, sizeof(result)) != 0);
	CHECK(strstr(result, "too few arguments") != nullptr);
	CHECK(kg_lisp_eval_string("(message 1)", 11, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "Wrong type argument") != nullptr);
	CHECK(kg_lisp_eval_string(
		  "(message \"ready\")", 17, result, sizeof(result))
	    == 0);
	CHECK(strcmp(test_status_message, "ready") == 0);
	/* message formats its argument, so a second one is no longer an
	 * arity error; test_format_natives covers what it does with it. */
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_insert_and_undo(void)
{
	static constexpr size_t payload_len = 2048;
	static const char prefix[] = "(insert \"";
	static const char suffix[] = "\")";
	char result[128] = "";
	char *source;
	size_t source_len;

	setup_editor();
	source_len = sizeof(prefix) - 1 + payload_len + sizeof(suffix) - 1;
	source = malloc(source_len + 1);
	CHECK(source != nullptr);
	if (!source) {
		teardown_editor();
		return;
	}
	memcpy(source, prefix, sizeof(prefix) - 1);
	memset(source + sizeof(prefix) - 1, 'x', payload_len);
	memcpy(
	    source + sizeof(prefix) - 1 + payload_len, suffix, sizeof(suffix));

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(source, source_len, result, sizeof(result))
	    == 0);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == (int)payload_len);
	CHECK(memcmp(bcur()->row[0].chars, source + sizeof(prefix) - 1,
		  payload_len)
	    == 0);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	CHECK(bcur()->row[0].size == 0);
	kg_lisp_shutdown();
	free(source);
	teardown_editor();
}

static void test_insert_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "original", 8);
	bcur()->readonly = 1;
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(
		  "(insert \"changed\")", 18, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "original", 8) == 0);
	CHECK(kg_lisp_eval_string("(+ 5 6)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "11") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_goto_line_and_buffer_name(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "zero", 4);
	editor_insert_row(bcur(), 1, "second", 6);
	editor_insert_row(bcur(), 2, "third", 5);
	CHECK(kg_lisp_init() == 0);
	/* Lines count from 1 and point lands at the beginning of the line, so
	 * line 2 of "zero\nsecond\nthird" is position 6. */
	CHECK(kg_lisp_eval_string("(goto-line 2)", 13, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("(point)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "6") == 0);
	/* A column is reached by moving on from there. */
	CHECK(kg_lisp_eval_string(
		  "(goto-char (+ (point) 2))", 25, result, sizeof(result))
	    == 0);
	CHECK(
	    kg_lisp_eval_string("(current-column)", 16, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "2") == 0);
	/* Out-of-range lines clamp to the buffer. */
	CHECK(
	    kg_lisp_eval_string("(goto-line 9999)", 16, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string(
		  "(line-number-at-pos)", 20, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "3") == 0);
	CHECK(kg_lisp_eval_string("(goto-line -9)", 14, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("(point)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "1") == 0);
	CHECK(kg_lisp_eval_string("(buffer-name)", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "bridge.txt") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static int write_text_file(const char *path, const char *content)
{
	FILE *file = fopen(path, "w");

	if (!file) {
		return 1;
	}
	if (fputs(content, file) == EOF) {
		(void)fclose(file);
		return 1;
	}
	return fclose(file) != 0;
}

/* Create <root>/kg and <root>/kg/lisp under a fresh temp dir and point
 * XDG_CONFIG_HOME at it. */
static int setup_config_root(char *root, size_t rootsize)
{
	char path[512];

	(void)snprintf(root, rootsize, "/tmp/kg-lisp-cfg-XXXXXX");
	if (!mkdtemp(root)) {
		return 1;
	}
	(void)snprintf(path, sizeof(path), "%s/kg", root);
	if (mkdir(path, 0700) != 0) {
		return 1;
	}
	(void)snprintf(path, sizeof(path), "%s/kg/lisp", root);
	if (mkdir(path, 0700) != 0) {
		return 1;
	}
	return setenv("XDG_CONFIG_HOME", root, 1) != 0;
}

static void remove_config_root(const char *root)
{
	static const char *const entries[] = {
		"%s/kg/init.fe",
		"%s/kg/init.el",
		"%s/kg/lisp/legacy.fe",
		"%s/kg/lisp/pkg.el",
		"%s/kg/lisp/pkg-a.el",
		"%s/kg/lisp/pkg-b.el",
		"%s/kg/lisp/pkg-x.el",
		"%s/kg/lisp/oldstyle.fe",
		"%s/kg/lisp/counted.el",
		"%s/kg/lisp/quiet.el",
		"%s/kg/lisp/after-cycle.el",
		"%s/kg/lisp/impl.el",
		"%s/kg/lisp/dup.el",
		"%s/kg/lisp/suffixed.el",
		"%s/kg/lisp/plainstem.el",
		"%s/kg/lisp/doubled.el.el",
		"%s/absolute-pkg.el",
		"%s/direct.fe",
		"%s/literal.el",
		"%s/defvar-a.el",
		"%s/defvar-b.el",
		"%s/kg/lisp",
		"%s/kg",
		"%s",
	};
	char path[512];
	size_t i;

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		(void)snprintf(path, sizeof(path), entries[i], root);
		(void)remove(path);
	}
	(void)unsetenv("XDG_CONFIG_HOME");
}

static void test_init_file(void)
{
	char root[64], path[512], result[128] = "";

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* Missing init file is normal. */
	CHECK(kg_lisp_load_init() == 0);

	/* An init.fe with no init.el alongside it is ignored: discovery is
	 * .el-only, with no probing of the retired extension. */
	(void)snprintf(path, sizeof(path), "%s/kg/init.fe", root);
	CHECK(write_text_file(path, "(setq init-loaded 99)\n") == 0);
	CHECK(kg_lisp_load_init() == 0);
	CHECK(kg_lisp_eval_string("init-loaded", 11, result, sizeof(result))
	    != 0);
	CHECK(strstr(kg_lisp_last_error(), "void-variable") != nullptr);

	/* Adding init.el is then loaded. */
	(void)snprintf(path, sizeof(path), "%s/kg/init.el", root);
	CHECK(write_text_file(path, "(setq init-loaded 42)\n") == 0);
	CHECK(kg_lisp_load_init() == 0);
	CHECK(kg_lisp_eval_string("init-loaded", 11, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "42") == 0);

	/* A broken init file reports its labelled error; forms evaluated
	 * before the failure remain applied. */
	CHECK(write_text_file(path, "(setq init-partial 1)\n(car 1)\n") == 0);
	CHECK(kg_lisp_load_init() != 0);
	CHECK(strstr(kg_lisp_last_error(), "init.el") != nullptr);
	CHECK(kg_lisp_eval_string("init-partial", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "1") == 0);

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_load_package(void)
{
	static const char load_error_form[]
	    = "(condition-case e (load \"caught-load-error\")"
	      " (error 'caught))";
	char root[64], path[512], source[600], result[128] = "";
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* A real NAME.fe on disk does not satisfy bare (load "NAME"): no
	 * fallback probes the retired extension, and the error names the
	 * .el path discovery actually looked for. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/legacy.fe", root);
	CHECK(write_text_file(path, "(setq legacy-value 9)\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"legacy\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "legacy.el") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 1 1)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "2") == 0);

	/* Bare names resolve to <config>/kg/lisp/NAME.el. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg.el", root);
	CHECK(write_text_file(path, "(setq pkg-value 7)\n") == 0);
	CHECK(kg_lisp_eval_string("(load \"pkg\")", 12, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("pkg-value", 9, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);

	/* The .el suffix may be written out, as in Emacs, whose `load`
	 * tries NAME.elc, NAME.el and then NAME so both spellings find the
	 * same file.  kg used to build pkg.el.el and report a path nobody
	 * asked for. */
	CHECK(
	    kg_lisp_eval_string("(load \"pkg.el\")", 15, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("pkg-value", 9, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);

	/* Missing packages raise an error naming the resolved path. */
	CHECK(
	    kg_lisp_eval_string("(load \"absent\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.el") != nullptr);
	/* ... and with the suffix written out, the same path, not
	 * absent.el.el. */
	CHECK(kg_lisp_eval_string(
		  "(load \"absent.el\")", 18, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.el") != nullptr);
	CHECK(strstr(result, "absent.el.el") == nullptr);
	CHECK(kg_lisp_eval_string("(+ 1 1)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "2") == 0);

	/* Packages may load packages. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-a.el", root);
	CHECK(
	    write_text_file(path, "(load \"pkg-b\")\n(setq a-after b-value)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.el", root);
	CHECK(write_text_file(path, "(setq b-value 5)\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"pkg-a\")", 14, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("a-after", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);

	/* Load cycles hit the depth limit and recover. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-x.el", root);
	CHECK(write_text_file(path, "(load \"pkg-x\")\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"pkg-x\")", 14, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "depth limit") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 2 2)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "4") == 0);

	/* Names containing '/' are literal paths, independent of extension:
	 * this is a bare .el literal, not the .fe regression -- see
	 * test_require_provide's direct.fe case for that. */
	(void)snprintf(path, sizeof(path), "%s/literal.el", root);
	CHECK(write_text_file(path, "(setq literal-value 3)\n") == 0);
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(
	    kg_lisp_eval_string(source, (size_t)length, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("literal-value", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "3") == 0);

	/* An evaluation error raised by a loaded file reaches a
	 * condition-case around the load, since sub-plan 11D Part 3 put
	 * lisp_eval_file() on fe's protected string entry.  It used to
	 * cross the nested evaluator's barrier and surface at top level,
	 * and this assertion is the inverted form of the one that pinned
	 * that: the same form now RETURNS, with the handler's value.
	 * The path resolution here is the bare-name one -- <config>/kg/lisp
	 * -- which the reachability fix had to leave alone. */
	(void)snprintf(
	    path, sizeof(path), "%s/kg/lisp/caught-load-error.el", root);
	CHECK(write_text_file(path, "(car 1)\n") == 0);
	CHECK(kg_lisp_eval_string(load_error_form, sizeof(load_error_form) - 1,
		  result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "caught") == 0);
	CHECK(kg_lisp_eval_string("(+ 2 3)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);

	kg_lisp_shutdown();
	remove_config_root(root);
}

/* Sub-plan 12D: the two-file probe the one-arg-`defvar' scope carrier
 * needed, run against kg's real `load'.  Emacs' measured rule is that a
 * one-argument `(defvar v)' makes `let' over v dynamic within the
 * reader/evaluation unit the defvar appeared in -- one file, or one
 * `eval' -- and nowhere else, with `special-variable-p' answering nil
 * throughout.  kg's mark used to be context-global, so file B saw file A's
 * declaration; since the pin, `EvaluateInput' stamps each mark with the
 * input unit that made it and `SymbolIsLetDynamic' compares for equality.
 *
 * The three answers below are byte-for-byte what the pinned Emacs 31.0.90
 * gives for the same two files under `lexical-binding: t' (measured
 * 2026-08-07: `(A from-A B unbound svp nil)').  The `let' is INSIDE each
 * file on purpose: fe consults the flag where the `let' runs and has
 * nowhere to record a closure's unit, so a function defined after the
 * defvar in A and CALLED from another unit answers lexically where Emacs
 * answers dynamically -- fe's own manifest row
 * one-arg-defvar-scope-carrier records that narrowing in full. */
static void test_phase12_one_arg_defvar_file_scope(void)
{
	char root[64], path[512], source[600], result[128] = "";
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	(void)snprintf(path, sizeof(path), "%s/defvar-a.el", root);
	CHECK(write_text_file(path,
		  "(defvar p12oav)\n"
		  "(defun p12-a-reader ()"
		  " (if (boundp 'p12oav) p12oav 'unbound))\n"
		  "(setq p12-a-in-file"
		  " (let ((p12oav 'from-A)) (p12-a-reader)))\n")
	    == 0);
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(
	    kg_lisp_eval_string(source, (size_t)length, result, sizeof(result))
	    == 0);

	(void)snprintf(path, sizeof(path), "%s/defvar-b.el", root);
	CHECK(write_text_file(path,
		  "(setq p12-b-in-file"
		  " (let ((p12oav 'from-B)) (p12-a-reader)))\n")
	    == 0);
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(
	    kg_lisp_eval_string(source, (size_t)length, result, sizeof(result))
	    == 0);

	/* A's own `let' is dynamic: the reader sees from-A. */
	CHECK(kg_lisp_eval_string("p12-a-in-file", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "from-A") == 0);
	/* B's is lexical: the reader sees the (unbound) global. */
	CHECK(kg_lisp_eval_string("p12-b-in-file", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "unbound") == 0);
	/* And the one-argument arity still marks nothing special. */
	CHECK(kg_lisp_eval_string(
		  "(special-variable-p 'p12oav)", 28, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "nil") == 0);

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_command_allow_list(void)
{
	char result[128] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(command-execute \"shell-command\")", 33,
		  result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "not allowed") != nullptr);
	CHECK(strstr(result, "shell-command") != nullptr);
	CHECK(test_command_calls == 0);
	CHECK(kg_lisp_eval_string(
		  "(command-execute \"version\")", 27, result, sizeof(result))
	    == 0);
	CHECK(test_command_calls == 1);
	CHECK(strcmp(test_command_name, "version") == 0);
	/* A quoted symbol names the command as it does in Emacs. */
	CHECK(kg_lisp_eval_string(
		  "(command-execute 'version)", 26, result, sizeof(result))
	    == 0);
	CHECK(test_command_calls == 2);
	CHECK(strcmp(test_command_name, "version") == 0);
	/* A read-only buffer refuses a command that edits it, and says so
	 * as that rather than as "not allowed" -- the two verdicts are
	 * different answers and a caller may want to tell them apart. */
	bcur()->readonly = 1;
	CHECK(kg_lisp_eval_string("(command-execute \"upcase-word\")", 31,
		  result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "read-only") != nullptr);
	CHECK(test_command_calls == 2);
	bcur()->readonly = 0;
	kg_lisp_shutdown();
	teardown_editor();
}

/* Defined below, beside the tests that use it most. */
static int eval_eq(const char *source, const char *expected);

static int eval_ok(const char *source)
{
	char result[256] = "";
	return kg_lisp_eval_string(
		   source, strlen(source), result, sizeof(result))
	    == 0;
}

static int eval_error_contains(const char *source, const char *fragment)
{
	char result[256] = "";

	if (kg_lisp_eval_string(source, strlen(source), result, sizeof(result))
	    == 0) {
		return 0;
	}
	return strstr(result, fragment) != nullptr;
}

/* kg_lisp_variable_non_nil(): the one channel by which an editor module
 * reads a user-settable variable, and the startup screen's two names are
 * its first caller (main.c).  What is pinned here is the channel's four
 * answers -- unbound, nil, non-nil, and a name too long for the form
 * buffer -- plus the two properties the prelude's `defvar's give the
 * names: bound and special before any init file runs.
 *
 * The alias is deliberately NOT a shared cell: Emacs makes
 * `inhibit-startup-message' a `defvaralias' of `inhibit-startup-screen',
 * kg has no variable aliases, and what makes both spellings work is
 * main.c asking for both.  Setting one leaves the other nil, and this
 * asserts that rather than leaving it to be discovered. */
static void test_variable_non_nil(void)
{
	/* A name no C buffer sizes: the read interns whatever it is handed
	 * and answers from `boundp`, so a long one is an unbound one. */
	char overlong[512];

	/* Before init, and after shutdown: no interpreter, no value. */
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") == 0);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(boundp 'inhibit-startup-screen)", "t"));
	CHECK(eval_eq("(special-variable-p 'inhibit-startup-message)", "t"));
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") == 0);
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-message") == 0);
	/* A name nothing ever declared is void, not nil; asking for its
	 * value would raise, and the contained read answers zero. */
	CHECK(eval_eq("(boundp 'kg-test-no-such-variable)", "nil"));
	CHECK(kg_lisp_variable_non_nil("kg-test-no-such-variable") == 0);

	CHECK(eval_ok("(setq inhibit-startup-message t)"));
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-message") != 0);
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") == 0);
	/* Non-nil, not "t": 0 and the empty string are values a user may
	 * write and Emacs treats as true. */
	CHECK(eval_ok("(setq inhibit-startup-screen 0)"));
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") != 0);
	CHECK(eval_ok("(setq inhibit-startup-screen nil)"));
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") == 0);

	memset(overlong, 'x', sizeof(overlong) - 1);
	overlong[sizeof(overlong) - 1] = '\0';
	CHECK(kg_lisp_variable_non_nil(overlong) == 0);
	CHECK(kg_lisp_variable_non_nil(nullptr) == 0);

	kg_lisp_shutdown();
	CHECK(kg_lisp_variable_non_nil("inhibit-startup-screen") == 0);
}

/* `fill-column' is a PRELUDE variable now, and the C fill reads it.  It
 * was `lisp/auto-fill.el''s until the frontier demand phase: a package's
 * `defvar' that no C ever consulted, while src/word.c wrapped M-q at a
 * hard-coded 72.  Both halves are asserted here -- the Lisp answer, and
 * what editor_fill_column() makes of it -- because the whole point of
 * the move is that they are the same number. */
static void test_fill_column_variable(void)
{
	setup_editor();
	/* No interpreter yet: the fill still has a column, and the
	 * fallback is the same 70 a WITH_LISP=0 build wraps at. */
	CHECK(editor_fill_column() == 70);
	CHECK(kg_lisp_init() == 0);

	/* Emacs' own default, and special -- so s-word-wrap's
	 * `(let ((fill-column len)) ...)' binds dynamically rather than
	 * lexically, which is the whole reason it is a `defvar'. */
	CHECK(eval_eq("fill-column", "70"));
	CHECK(eval_eq("(special-variable-p 'fill-column)", "t"));
	CHECK(eval_eq("(let ((fill-column 5)) fill-column)", "5"));
	CHECK(editor_fill_column() == 70);

	/* An init file moves it, and M-q moves with it. */
	CHECK(eval_ok("(setq fill-column 40)"));
	CHECK(editor_fill_column() == 40);

	/* And it is buffer-local aware: a `setq-local' in this buffer is
	 * what the fill in this buffer reads. */
	CHECK(eval_ok("(setq-local fill-column 25)"));
	CHECK(editor_fill_column() == 25);
	CHECK(eval_ok("(kill-local-variable 'fill-column)"));
	CHECK(editor_fill_column() == 40);

	/* A binding that is not an integer is not a column: the read is
	 * contained and answers the fallback rather than raising into an
	 * editor command.  (`fill-region' does raise -- it is Lisp calling
	 * Lisp, where Emacs raises too.) */
	CHECK(eval_ok("(setq fill-column \"wide\")"));
	CHECK(editor_fill_column() == 70);

	/* A `defvar' over a name a user has already set does NOT clobber
	 * it, which is what lets lisp/auto-fill.el (and any package) keep
	 * declaring a variable the prelude now owns. */
	CHECK(eval_eq("(progn (setq fill-column 33)"
		      " (defvar fill-column 70 \"doc\") fill-column)",
	    "33"));

	kg_lisp_shutdown();
	CHECK(editor_fill_column() == 70);
	teardown_editor();
}

/* `fill-region', the C contracts the oracle corpus cannot reach.  The
 * seven frozen shapes -- one paragraph, many, refill/squeeze, a long
 * word, the adaptive prefix, the empty region and the declined sentence
 * nobreak -- are asserted against Emacs itself in
 * test/lisp-compat/cases/frontier-fill-region-*, so what is here is the
 * rest: the argument rules, the partial region, the buffer-local column
 * and the two refusals. */
static void test_fill_region(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Order-insensitive, like every other region native. */
	CHECK(eval_eq("(with-temp-buffer (insert \"aa bb cc dd\")"
		      " (let ((fill-column 5))"
		      "  (list (fill-region (point-max) (point-min))"
		      "   (buffer-substring (point-min) (point-max)))))",
	    "(\"\" \"aa bb\ncc dd\")"));

	/* Point ends where the region's END moved to, not where it was:
	 * squeezing three spaces out shortens the buffer under it, and
	 * Emacs answers 9 for this one, measured. */
	CHECK(eval_eq("(with-temp-buffer (insert \"aa   bb  cc \")"
		      " (let ((fill-column 30))"
		      "  (fill-region (point-min) (point-max)) (point)))",
	    "9"));

	/* START is rounded back to the beginning of its own LINE and END
	 * is taken exactly -- Emacs' asymmetry, measured on 31.0.91:
	 * `(fill-region 4 12)' over "aa bb cc dd" refills the whole line,
	 * and a region ending inside a paragraph does not follow it. */
	CHECK(eval_eq("(with-temp-buffer (insert \"aa bb cc dd\")"
		      " (let ((fill-column 5))"
		      "  (fill-region 4 (point-max))"
		      "  (buffer-substring (point-min) (point-max))))",
	    "aa bb\ncc dd"));
	CHECK(eval_eq("(with-temp-buffer (insert \"aa bb\n\ncc dd\")"
		      " (let ((fill-column 2))"
		      "  (fill-region (point-min) 6)"
		      "  (buffer-substring (point-min) (point-max))))",
	    "aa\nbb\n\ncc dd"));

	/* A region holding no word is not a paragraph: nil, and the text
	 * is left alone (Emacs answers (nil "   ")). */
	CHECK(eval_eq("(with-temp-buffer (insert \"   \")"
		      " (let ((fill-column 5))"
		      "  (list (fill-region (point-min) (point-max))"
		      "   (buffer-substring (point-min) (point-max)))))",
	    "(nil \"   \")"));

	/* The column comes from the variable in force in the buffer being
	 * filled, so a `setq-local' in one buffer is not the other's. */
	CHECK(eval_eq("(with-temp-buffer (insert \"aa bb cc\")"
		      " (setq-local fill-column 2)"
		      "  (fill-region (point-min) (point-max))"
		      "  (buffer-substring (point-min) (point-max)))",
	    "aa\nbb\ncc"));
	CHECK(eval_eq("fill-column", "70"));

	/* And a column that is not a number is Emacs' own condition, with
	 * Emacs' own predicate in it. */
	CHECK(eval_eq("(condition-case e"
		      " (with-temp-buffer (insert \"aa bb\")"
		      "  (let ((fill-column \"x\"))"
		      "   (fill-region (point-min) (point-max))))"
		      " (wrong-type-argument (cdr e)))",
	    "(number-or-marker-p \"x\")"));

	/* A read-only buffer refuses, by name, before anything is
	 * rewritten -- the rule insert and delete-region already state. */
	bcur()->readonly = 1;
	CHECK(eval_error_contains(
	    "(fill-region (point-min) (point-max))", "read-only"));
	bcur()->readonly = 0;

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_define_and_run_command(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(kg_lisp_run_command("greet", 0) != 0);
	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hello\")))"));
	CHECK(kg_lisp_command_name(0) != nullptr);
	CHECK(strcmp(kg_lisp_command_name(0), "greet") == 0);
	CHECK(kg_lisp_command_name(1) == nullptr);
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "hello") == 0);

	/* Redefinition replaces the function (and releases the old root). */
	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"again\")))"));
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "again") == 0);
	CHECK(kg_lisp_command_name(1) == nullptr);

	/* Errors inside a command return to the caller with a message that
	 * names the command, as the trampoline's eval label did. */
	CHECK(eval_ok("(define-command \"boom\" (lambda () (car 1)))"));
	CHECK(kg_lisp_run_command("boom", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error") != nullptr);
	CHECK(strstr(test_status_message, "boom") != nullptr);
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "again") == 0);

	/* A runaway command hits the step budget and recovers. */
	CHECK(eval_ok("(define-command \"spin\" (lambda () (while t 1)))"));
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strstr(test_status_message, "step limit") != nullptr);
	CHECK(eval_ok("(+ 1 2)"));

	CHECK(eval_ok("(remove-command \"boom\")"));
	CHECK(kg_lisp_run_command("boom", 0) != 0);
	CHECK(eval_error_contains(
	    "(remove-command \"boom\")", "no such Lisp command"));

	CHECK(eval_error_contains(
	    "(define-command \"version\" (lambda () 1))", "built-in"));
	CHECK(eval_error_contains(
	    "(define-command \"x\" 1)", "requires a function"));
	/* The optional third and fourth arguments are validated too: a spec
	 * is nil, a string, or a zero-argument function, and documentation
	 * is nil or a string. */
	CHECK(eval_error_contains("(define-command \"x\" (lambda () 1) 5)",
	    "requires a string or function spec"));
	CHECK(eval_error_contains("(define-command \"x\" (lambda () 1) nil 5)",
	    "documentation requires a string"));
	/* ... and both optional arguments really are optional. */
	CHECK(eval_ok("(define-command \"two-arg\" (lambda () 1))"));
	CHECK(eval_ok("(remove-command \"two-arg\")"));

	/* lookup-key answers nil rather than raising for a map nothing is
	 * called, and for a sequence that does not parse. */
	CHECK(eval_eq("(lookup-key \"no-such-map\" \"C-c i\")", "nil"));
	CHECK(
	    eval_eq("(lookup-key \"global\" \"C-c C-c C-c C-c C-c\")", "nil"));

	/* global-unset-key tells an unbindable sequence from an unbound
	 * one, and names the sequence in both messages. */
	CHECK(eval_error_contains(
	    "(global-unset-key \"C-x i\")", "only \"C-c <key>\" is bindable"));
	CHECK(eval_error_contains(
	    "(global-unset-key \"C-c q\")", "key is not bound"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* 07D item 3: a failed registration leaves both the old command and the
 * old function cell intact, and the command root and the function cell
 * hold one closure rather than two evaluations that merely behave alike.
 *
 * The probe is the command table filling up.  `defun` used to expand to
 * defalias-then-define-command, reading the cell back with
 * (symbol-function 'name) for the second step, so a define-command that
 * could not find a slot had already replaced the function. */
/* 07E §2's numeric classifier, driven directly: it is pure, so this is
 * the seam a native test can reach.  The `n`/`N` reader used to hand the
 * typed text straight to strtoimax and then to strtod and read the answer
 * off `end` and `errno`, which accepted far too much -- every NONE case
 * below was a silently accepted number before, and the empty one was 0
 * (with the keystrokes after it falling into the buffer). */
static void test_number_token_classifier(void)
{
	/* Integers, including fe's `5.` spelling. */
	CHECK(kg_number_token_classify("0") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("42") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("-42") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("+42") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("5.") == KG_NUMBER_TOKEN_INTEGER);
	/* ... and only trailing/leading ASCII whitespace beside them. */
	CHECK(kg_number_token_classify("  7  ") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("7\t") == KG_NUMBER_TOKEN_INTEGER);

	/* Floats: fraction and exponent forms. */
	CHECK(kg_number_token_classify("5.0") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify(".5") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("-.5") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1e3") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1E3") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1e-3") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1.5e+10") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1.e3") == KG_NUMBER_TOKEN_FLOAT);

	/* Nothing at all, which used to be the integer 0. */
	CHECK(kg_number_token_classify("") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("   ") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("\t\n") == KG_NUMBER_TOKEN_NONE);

	/* Not numbers, all of which the two converters between them
	 * accepted: strtod reads inf/nan and hex, and strtoimax stopped at
	 * the `x` and reported a clean 0. */
	CHECK(kg_number_token_classify("inf") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("-inf") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("nan") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("NaN") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("infinity") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("0x10") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1e+INF") == KG_NUMBER_TOKEN_NONE);

	/* Trailing and interior junk, and a second token. */
	CHECK(kg_number_token_classify("12abc") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1.2.3") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1 2") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1e") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1e+") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("+") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("-") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify(".") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify(".e3") == KG_NUMBER_TOKEN_NONE);
	/* No Lisp is evaluated to decide: a form is not a number. */
	CHECK(kg_number_token_classify("(+ 1 2)") == KG_NUMBER_TOKEN_NONE);

	/* Overflow is still an integer *token*; the reader turns it into a
	 * double, as an integer literal past int64 does in fe's reader. */
	CHECK(kg_number_token_classify("99999999999999999999")
	    == KG_NUMBER_TOKEN_INTEGER);
}

/* 07E: prompting is refused outside a real key/M-x command context.
 *
 * These tests have no terminal, so cmd_prompt_fd() answers -1 -- which
 * is exactly the shape of loading an init file, running a hook, or
 * running a process filter: a live Lisp frame with no command fd behind
 * it.  Guessing from state.frame_active, which is true in all of those,
 * is what the slice was told not to do, and this is the observable
 * difference.  The command body must not run either. */
static void test_interactive_prompt_context_refusal(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq ran nil)"));
	CHECK(eval_ok("(defun asks (v) (interactive \"sV: \") (setq ran v))"));
	CHECK(kg_lisp_run_command("asks", 0) == 0);
	CHECK(strstr(test_status_message,
		  "interactive prompt is not available here")
	    != nullptr);
	CHECK(eval_eq("ran", "nil"));

	/* Reached from Lisp, the same refusal is an ordinary condition the
	 * calling program can catch, and still does not run the body. */
	CHECK(eval_eq("(condition-case e (command-execute 'asks)"
		      "  (error 'refused))",
	    "refused"));
	CHECK(eval_eq("ran", "nil"));

	/* A command that needs no prompt is unaffected. */
	CHECK(eval_ok("(defun quiet () (interactive) (setq ran 'yes))"));
	CHECK(kg_lisp_run_command("quiet", 0) == 0);
	CHECK(eval_eq("ran", "yes"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 16: the public read forms' argument contracts, and the same
 * refusal the interactive codes get.
 *
 * Everything here happens BEFORE a prompt would open, which is why it is
 * a native test at all: this binary has no terminal, cmd_prompt_fd()
 * answers -1, and the value delivery, cancellation and picker policy are
 * PTY cases (test/pty/lisp-read-*.yaml, lisp-completing-read.yaml,
 * lisp-yes-no-prompts.yaml, lisp-prompt-rendering.yaml).  What is pinned
 * here is that a bad argument is a real `wrong-type-argument' rather than
 * a prompt nobody can answer, that an unsupported PREDICATE says so out
 * loud instead of being silently dropped, and that a HISTORY argument is
 * accepted -- so the Emacs spelling of an ordinary call reaches the
 * prompt refusal rather than an arity error. */
static void test_phase16_read_forms(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* All seven exist, and all seven refuse to prompt from here. */
	CHECK(eval_eq(
	    "(condition-case nil (read-string \"P: \") (error 'no))", "no"));
	CHECK(eval_eq(
	    "(condition-case nil (read-number \"P: \") (error 'no))", "no"));
	CHECK(eval_eq(
	    "(condition-case nil (read-file-name \"P: \") (error 'no))", "no"));
	CHECK(eval_eq(
	    "(condition-case nil (read-buffer \"P: \") (error 'no))", "no"));
	CHECK(eval_eq(
	    "(condition-case nil (y-or-n-p \"P: \") (error 'no))", "no"));
	CHECK(eval_eq(
	    "(condition-case nil (yes-or-no-p \"P: \") (error 'no))", "no"));
	CHECK(eval_eq("(condition-case nil (completing-read \"P: \" '(\"a\"))"
		      "  (error 'no))",
	    "no"));

	/* A non-string prompt is Emacs' condition, not a prompt. */
	CHECK(eval_eq("(condition-case nil (read-string 5)"
		      "  (wrong-type-argument 'wrong-type))",
	    "wrong-type"));
	CHECK(eval_eq("(condition-case nil (completing-read \"P: \" 5)"
		      "  (wrong-type-argument 'wrong-type))",
	    "wrong-type"));
	CHECK(eval_eq("(condition-case nil (completing-read \"P: \" '(1 2))"
		      "  (wrong-type-argument 'wrong-type))",
	    "wrong-type"));
	CHECK(eval_eq("(condition-case nil (read-number \"P: \" \"x\")"
		      "  (wrong-type-argument 'wrong-type))",
	    "wrong-type"));
	CHECK(eval_eq("(condition-case nil (read-string \"P: \" nil nil 5)"
		      "  (wrong-type-argument 'wrong-type))",
	    "wrong-type"));

	/* An unsupported argument is refused by name, and is refused before
	 * the prompt seam is consulted -- so this says PREDICATE even here,
	 * where every read would fail anyway. */
	CHECK(eval_ok("(setq report nil)"));
	CHECK(eval_eq("(condition-case e"
		      "    (completing-read \"P: \" '(\"a\") 'stringp)"
		      "  (error (setq report (car (cdr e))) 'refused))",
	    "refused"));
	CHECK(eval_eq("(string-match \"PREDICATE\" report)", "16"));
	CHECK(eval_eq("(condition-case nil (read-buffer \"P: \" nil nil 'x)"
		      "  (error 'refused))",
	    "refused"));
	CHECK(eval_eq(
	    "(condition-case nil (read-file-name \"P: \" nil nil nil nil 'x)"
	    "  (error 'refused))",
	    "refused"));

	/* HISTORY is accepted and ignored: a full Emacs argument list gets
	 * as far as the prompt refusal rather than "too many arguments". */
	CHECK(eval_eq("(condition-case nil"
		      "    (read-string \"P: \" \"init\" 'my-history \"def\")"
		      "  (error 'no))",
	    "no"));
	CHECK(eval_eq("(condition-case nil (read-number \"P: \" 3 'my-history)"
		      "  (error 'no))",
	    "no"));
	CHECK(eval_eq("(condition-case nil"
		      "    (completing-read \"P: \" '(\"a\") nil t \"a\""
		      "                     'my-history \"a\")"
		      "  (error 'no))",
	    "no"));

	/* One past Emacs' argument list is still an arity error. */
	CHECK(
	    eval_eq("(condition-case nil (y-or-n-p \"P: \" 1) (error 'arity))",
		"arity"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_defun_redefinition_is_atomic(void)
{
	char name[32];
	char form[128];
	int i;

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* An ordinary, non-interactive function first. */
	CHECK(eval_ok("(defun old-fn () 111)"));
	CHECK(eval_eq("(old-fn)", "111"));
	CHECK(kg_lisp_command_exists("old-fn") == 0);

	/* Fill the 32-slot command table with other names. */
	for (i = 0; i < 32; i++) {
		(void)snprintf(name, sizeof(name), "filler-%d", i);
		(void)snprintf(form, sizeof(form),
		    "(define-command \"%s\" (lambda () 0))", name);
		CHECK(eval_ok(form));
	}
	CHECK(eval_error_contains(
	    "(define-command \"one-more\" (lambda () 0))", "too many"));

	/* Redefining old-fn interactively must now fail -- and fail whole. */
	CHECK(eval_error_contains(
	    "(defun old-fn () (interactive) 222)", "too many"));
	CHECK(eval_eq("(old-fn)", "111"));
	CHECK(kg_lisp_command_exists("old-fn") == 0);

	/* With a slot free again it succeeds, and the command and the
	 * function cell are the same object -- `eq`, not merely equal. */
	CHECK(eval_ok("(remove-command \"filler-0\")"));
	CHECK(eval_ok("(defun old-fn () (interactive) 222)"));
	CHECK(eval_eq("(old-fn)", "222"));
	CHECK(kg_lisp_command_exists("old-fn") == 1);
	CHECK(kg_lisp_run_command("old-fn", 0) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_run_command_interrupt(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(define-command \"spin\" (lambda () (while t 1)))"));
	/* C-g reaches the command's loop through the same interrupt check
	 * eval-expression uses. */
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strcmp(test_status_message, "Quit") == 0);
	kg_lisp_set_interrupt_check(nullptr);

	/* A cancelled run leaves the interpreter usable, and without the
	 * interrupt the same command now exhausts the step budget.  That
	 * report names the command, which is what tells a user *which* of
	 * their commands ran away; the quit above deliberately does not,
	 * because "Quit" is the whole message Emacs shows for a C-g. */
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strstr(test_status_message, "step limit") != nullptr);
	CHECK(strstr(test_status_message, "Lisp error: spin:") != nullptr);
	CHECK(eval_ok("(+ 1 1)"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_run_command_reentrancy(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* A Lisp-defined command IS CMD_LISP_CALLABLE, so (command-execute
	 * 'inner) is a real nested activation.  The outer command has a live
	 * top-level frame; the nested one must build and call inside that
	 * evaluator rather than installing a frame of its own.  Overwriting
	 * the single state.frame here -- and clearing frame_active on the way
	 * out, while the outer command was still running -- is what made the
	 * outer command's next buffer operation abort(). */
	CHECK(eval_ok("(defun inner () (interactive) 1)"));
	CHECK(eval_ok("(defun outer ()"
		      "  (interactive)"
		      "  (setq outer-saw (command-execute 'inner))"
		      "  (insert \"after\")"
		      "  (message \"outer-done\"))"));
	CHECK(kg_lisp_run_command("outer", 0) == 0);
	/* command-execute returns the command's value, as Emacs does and as
	 * the checked-in oracle snapshot says. */
	CHECK(eval_eq("outer-saw", "1"));
	/* The outer command kept running after the nested one returned: a
	 * buffer operation past the nested call is exactly what used to
	 * raise "current buffer is dead" and abort. */
	CHECK(strcmp(test_status_message, "outer-done") == 0);
	CHECK(strstr(bcur()->row[0].chars, "after") != nullptr);

	/* A nested failure propagates into the surrounding evaluator, so the
	 * outer command's own condition-case catches it and the outer
	 * command completes normally. */
	CHECK(eval_ok("(defun boom () (interactive) (car 1 2))"));
	CHECK(eval_ok("(defun guarded ()"
		      "  (interactive)"
		      "  (setq guarded-saw"
		      "    (condition-case e (command-execute 'boom)"
		      "      (wrong-number-of-arguments 'caught)))"
		      "  (message \"guarded-done\"))"));
	CHECK(kg_lisp_run_command("guarded", 0) == 0);
	CHECK(eval_eq("guarded-saw", "caught"));
	CHECK(strcmp(test_status_message, "guarded-done") == 0);

	/* Uncaught, the nested failure is reported against the *outer*
	 * command, whose frame is the one that recovers. */
	CHECK(eval_ok("(defun bare () (interactive) (command-execute 'boom))"));
	CHECK(kg_lisp_run_command("bare", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error: bare:") != nullptr);
	/* and the interpreter is usable afterwards. */
	CHECK(eval_ok("(+ 1 1)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* 07D's prefix delivery, end to end through cmd_invoke: the raw form a
 * keystroke committed must reach `P`, `p` and current-prefix-arg.
 *
 * The regression these pin is that handle_pending_universal_arg() cleared
 * editor.prefix_raw_kind on the *commit* path, before the dispatcher
 * copied it, so every Lisp command saw raw nil however the argument was
 * spelled: P nil, p 1, current-prefix-arg nil. */
static void test_command_prefix_delivery(void)
{
	struct command_prefix none = { 0, 0, PREFIX_RAW_NONE, 0 };
	struct command_prefix five = { 1, 5, PREFIX_RAW_INTEGER, 0 };
	struct command_prefix minus = { 1, -1, PREFIX_RAW_MINUS, 0 };
	struct command_prefix one_cu = { 1, 4, PREFIX_RAW_UNIVERSAL, 1 };
	struct command_prefix two_cu = { 1, 16, PREFIX_RAW_UNIVERSAL, 2 };
	struct command_prefix five_cu = { 1, 1000, PREFIX_RAW_UNIVERSAL, 5 };

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(defun raw () (interactive \"P\") (setq seen p))"));
	CHECK(eval_ok("(defun num (n) (interactive \"p\") (setq seen n))"));
	/* `P` is the argument, and it is `eq` to current-prefix-arg during
	 * the body -- one rooted object, never two. */
	CHECK(eval_ok("(defun both (x)"
		      "  (interactive \"P\")"
		      "  (setq seen (eq x current-prefix-arg)))"));

	CHECK(eval_ok("(defun raw1 (x) (interactive \"P\") (setq seen x))"));
	CHECK(test_run_command_with_prefix("raw1", none) == CMD_RAN);
	CHECK(eval_eq("seen", "nil"));
	CHECK(test_run_command_with_prefix("raw1", five) == CMD_RAN);
	CHECK(eval_eq("seen", "5"));
	CHECK(test_run_command_with_prefix("raw1", minus) == CMD_RAN);
	CHECK(eval_eq("seen", "-"));
	CHECK(test_run_command_with_prefix("raw1", one_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "(4)"));
	CHECK(test_run_command_with_prefix("raw1", two_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "(16)"));
	/* The 1000 cap is the *effective* integer's, not the raw list's:
	 * five bare C-u is (1024) in Emacs and was (1000) here. */
	CHECK(test_run_command_with_prefix("raw1", five_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "(1024)"));

	/* Emacs would use a bignum past int64; the raw list saturates at the
	 * largest representable power of 4 instead, which needs 32
	 * consecutive C-u and is a recorded limit rather than a wrap. */
	{
		struct command_prefix many
		    = { 1, 1000, PREFIX_RAW_UNIVERSAL, 40 };

		CHECK(test_run_command_with_prefix("raw1", many) == CMD_RAN);
		CHECK(eval_eq("(car seen)", "4611686018427387904"));
	}

	/* `p` is prefix-numeric-value of the same raw form. */
	CHECK(test_run_command_with_prefix("num", none) == CMD_RAN);
	CHECK(eval_eq("seen", "1"));
	CHECK(test_run_command_with_prefix("num", five) == CMD_RAN);
	CHECK(eval_eq("seen", "5"));
	CHECK(test_run_command_with_prefix("num", minus) == CMD_RAN);
	CHECK(eval_eq("seen", "-1"));
	CHECK(test_run_command_with_prefix("num", two_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "16"));

	CHECK(test_run_command_with_prefix("both", two_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "t"));

	/* Each command builds its own raw list, so mutating one cannot be
	 * seen by the next invocation. */
	CHECK(eval_ok("(defun grab (x) (interactive \"P\") (setq kept x))"));
	CHECK(test_run_command_with_prefix("grab", one_cu) == CMD_RAN);
	CHECK(eval_ok("(setcar kept 99)"));
	CHECK(test_run_command_with_prefix("grab", one_cu) == CMD_RAN);
	CHECK(eval_eq("kept", "(4)"));

	/* current-prefix-arg is restored after a normal exit, and the prior
	 * global is read rather than assumed nil. */
	CHECK(eval_ok("(setq current-prefix-arg 'outside)"));
	CHECK(test_run_command_with_prefix("raw1", five) == CMD_RAN);
	CHECK(eval_eq("current-prefix-arg", "outside"));
	/* ... and after an error exit. */
	CHECK(eval_ok("(defun bang () (interactive) (car 1 2))"));
	CHECK(test_run_command_with_prefix("bang", five) == CMD_RAN);
	CHECK(eval_eq("current-prefix-arg", "outside"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_key_bindings(void)
{
	char spelled[32];

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(keybind_parse("C-c i", spelled, sizeof(spelled)) == 0
	    && strcmp(spelled, "C-c i") == 0);
	CHECK(keybind_parse("C-c C-y", spelled, sizeof(spelled)) == 0
	    && strcmp(spelled, "C-c C-y") == 0);
	CHECK(keybind_parse("C-x i", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c", spelled, sizeof(spelled)) != 0);
	/* Extra spaces are separators now, so this one is bindable; what
	 * matters is that it means exactly what "C-c i" means. */
	CHECK(keybind_parse("C-c  i", spelled, sizeof(spelled)) == 0
	    && strcmp(spelled, "C-c i") == 0);
	CHECK(keybind_parse("C-c ii", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c C-g", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c C-c", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c C-x", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c M-f", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c SPC", spelled, sizeof(spelled)) != 0);

	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hey\")))"));
	CHECK(eval_ok("(global-set-key \"C-c i\" \"greet\")"));
	CHECK(keybind_lookup("C-c i") != nullptr);
	CHECK(strcmp(keybind_lookup("C-c i"), "greet") == 0);
	CHECK(eval_error_contains(
	    "(global-set-key \"C-x i\" \"greet\")", "invalid key sequence"));
	CHECK(eval_ok("(global-unset-key \"C-c i\")"));
	CHECK(keybind_lookup("C-c i") == nullptr);
	CHECK(eval_error_contains("(global-unset-key \"C-c i\")", "not bound"));

	kg_lisp_shutdown();
	teardown_editor();
}

static int eval_eq(const char *source, const char *expected)
{
	char result[256] = "";

	if (kg_lisp_eval_string(source, strlen(source), result, sizeof(result))
	    != 0) {
		return 0;
	}
	return strcmp(result, expected) == 0;
}

/* provide/require/featurep: the bounded feature table, cycle detection,
 * and require's FILENAME argument.  Load-path order and a load-path full
 * of stale directories are separate tests below, since they need
 * add-to-load-path and a second temp directory outside the config
 * root. */
static void test_require_provide(void)
{
	char root[64], path[512], source[600];
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(featurep 'counted)", "nil"));

	/* A second require is a no-op: proven by a counter the file only
	 * bumps when it actually runs, not just by the absence of an
	 * error. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/counted.el", root);
	CHECK(write_text_file(path,
		  "(setq counted-runs "
		  "(if (boundp 'counted-runs) (+ counted-runs 1) 1))\n"
		  "(provide 'counted)\n")
	    == 0);
	CHECK(eval_eq("(require 'counted)", "counted"));
	CHECK(eval_eq("(featurep 'counted)", "t"));
	CHECK(eval_eq("counted-runs", "1"));
	CHECK(eval_eq("(require 'counted)", "counted"));
	CHECK(eval_eq("counted-runs", "1"));

	/* A file that never calls (provide ...) is an error naming the
	 * feature that did not appear. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/quiet.el", root);
	CHECK(write_text_file(path, "(setq quiet-ran t)\n") == 0);
	CHECK(eval_error_contains("(require 'quiet)", "quiet"));
	CHECK(eval_eq("quiet-ran", "t"));

	/* A real NAME.fe in a load-path directory does not satisfy bare
	 * (require 'NAME): the error names the feature, with no fallback
	 * to the retired extension. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/oldstyle.fe", root);
	CHECK(write_text_file(path, "(provide 'oldstyle)\n") == 0);
	CHECK(eval_error_contains("(require 'oldstyle)", "oldstyle"));
	CHECK(eval_eq("(featurep 'oldstyle)", "nil"));

	/* Cycle: pkg-a requires pkg-b, pkg-b requires pkg-a. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-a.el", root);
	CHECK(
	    write_text_file(path, "(require 'pkg-b)\n(provide 'pkg-a)\n") == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.el", root);
	CHECK(
	    write_text_file(path, "(require 'pkg-a)\n(provide 'pkg-b)\n") == 0);
	CHECK(eval_error_contains("(require 'pkg-a)", "pkg-a"));
	CHECK(eval_eq("(featurep 'pkg-a)", "nil"));
	/* Recovers cleanly: an unrelated eval afterwards still works. */
	CHECK(eval_eq("(+ 1 1)", "2"));
	/* And the abandoned requiring stack is actually reset, not merely
	 * unread: a later require of a fresh feature must load rather than
	 * trip the cycle check on a name left behind by the longjmp. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/after-cycle.el", root);
	CHECK(write_text_file(path, "(provide 'after-cycle)\n") == 0);
	CHECK(eval_eq("(require 'after-cycle)", "after-cycle"));
	CHECK(eval_eq("(featurep 'after-cycle)", "t"));

	/* A condition caught within the same evaluator run never reaches kg's
	 * outer frame recovery.  The require stack therefore needs an Fe
	 * cleanup of its own: retrying a missing feature must repeat the path
	 * error, not report a stale cycle left by the first non-local exit. */
	CHECK(eval_eq("(list "
		      " (condition-case e (require 'retry-absent)"
		      "  (file-missing (car (cdr (cdr (cdr e))))))"
		      " (condition-case e (require 'retry-absent)"
		      "  (file-missing (car (cdr (cdr (cdr e)))))))",
	    "(\"retry-absent\" \"retry-absent\")"));

	/* An explicit FILENAME resolves instead of the feature's own
	 * name. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/impl.el", root);
	CHECK(write_text_file(path, "(provide 'aliased)\n") == 0);
	CHECK(eval_ok("(require 'aliased \"impl\")"));
	CHECK(eval_eq("(featurep 'aliased)", "t"));

	/* Literal-path regression: an explicit '/'-containing filename is
	 * never extension-probed, so a real .fe file stays reachable through
	 * both load and require's own FILENAME argument -- the hard cut is
	 * discovery-only.  require runs first, while the feature is still
	 * absent: an earlier load would already provide it and require would
	 * return without exercising path resolution at all. */
	(void)snprintf(path, sizeof(path), "%s/direct.fe", root);
	CHECK(write_text_file(
		  path, "(setq direct-value 3)\n(provide 'direct-feature)\n")
	    == 0);
	CHECK(eval_eq("(featurep 'direct-feature)", "nil"));
	length = snprintf(
	    source, sizeof(source), "(require 'direct-feature \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_eq(source, "direct-feature"));
	CHECK(eval_eq("(featurep 'direct-feature)", "t"));
	CHECK(eval_eq("direct-value", "3"));
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_ok(source));

	/* NOERROR, the third slot (Phase 29 U.1a).  Emacs 31.0.91's own
	 * answers, recorded in test/lisp-compat/cases/u0-require-*.json:
	 * non-nil answers nil for a feature whose file is not there and
	 * provides NOTHING, nil raises `file-missing' with the data shape
	 * kg's one-argument require already raised, and all three arities
	 * answer the feature symbol for a feature already provided. */
	CHECK(eval_eq("(list (require 'guarded-absent nil t) "
		      "(featurep 'guarded-absent))",
	    "(nil nil)"));
	CHECK(eval_eq("(condition-case e (require 'guarded-absent nil nil) "
		      "(file-missing (car e)))",
	    "file-missing"));
	CHECK(eval_eq("(list (require 'counted) (require 'counted nil) "
		      "(require 'counted nil t) (featurep 'counted))",
	    "(counted counted counted t)"));
	/* NOERROR covers the missing file and nothing else: the file's own
	 * error still propagates through it. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/angry.el", root);
	CHECK(write_text_file(path, "(error \"boom\")\n") == 0);
	CHECK(eval_error_contains("(require 'angry nil t)", "boom"));
	/* And a fourth argument is an arity error, as in Emacs. */
	CHECK(eval_eq("(condition-case e (require 'counted nil t 4) "
		      "(error (cdr e)))",
	    "(require 4)"));

	kg_lisp_shutdown();
	remove_config_root(root);
}

/* require's FILENAME and load's NAME answer the same question about the
 * same input (sub-plan 10C Part 2, 10A Decision 7).
 *
 * Before this, `(load "impl.el")` worked and `(require 'f "impl.el")`
 * did not: load's lisp_package_path() asked lisp_has_el_suffix() and
 * require's candidate_readable() unconditionally appended, so require
 * looked for impl.el.el and reported a path the caller never wrote.  Two
 * loaders in one editor disagreeing about one string.
 *
 * Every other resolution rule is pinned here *unchanged*, because the
 * fix is one conditional inside the load-path candidate builder and
 * nothing else may have moved: a bare stem still gains .el, a stem whose
 * file is absent is still an error naming it, and a '/'-containing name
 * is still literal -- never suffixed, never searched. */
static void test_require_el_suffix(void)
{
	char root[64], path[512], source[600];
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* The fix: a FILENAME that already ends in .el resolves to that
	 * file. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/suffixed.el", root);
	CHECK(write_text_file(
		  path, "(setq suffixed-ran t)\n(provide 'suffixed-f)\n")
	    == 0);
	CHECK(eval_eq("(require 'suffixed-f \"suffixed.el\")", "suffixed-f"));
	CHECK(eval_eq("(featurep 'suffixed-f)", "t"));
	CHECK(eval_eq("suffixed-ran", "t"));
	/* And load, which already worked, still answers the same file --
	 * the symmetry is the point of the change. */
	CHECK(eval_ok("(load \"suffixed.el\")"));

	/* Pinned unchanged: a bare stem still gains the suffix. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/plainstem.el", root);
	CHECK(write_text_file(path, "(provide 'plain-f)\n") == 0);
	CHECK(eval_eq("(require 'plain-f \"plainstem\")", "plain-f"));

	/* Pinned unchanged, and the sharp end of the fix: the doubled name
	 * is no longer what require looks for.  This file is exactly what
	 * the old code resolved "doubled.el" to, and it must now be
	 * unreachable -- the require fails naming the feature, and the
	 * marker the file would set stays unbound. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/doubled.el.el", root);
	CHECK(write_text_file(
		  path, "(setq doubled-ran t)\n(provide 'doubled-f)\n")
	    == 0);
	CHECK(eval_error_contains("(require 'doubled-f \"doubled.el\")",
	    "Cannot open load file: No such file or directory, doubled.el"));
	CHECK(eval_eq("(featurep 'doubled-f)", "nil"));
	CHECK(eval_error_contains("doubled-ran", "doubled-ran"));

	/* Pinned unchanged: an absolute FILENAME is literal.  It contains a
	 * '/', so resolve_require_path never reaches the load-path or the
	 * suffix rule at all -- the file is taken exactly as written, .el
	 * suffix included. */
	(void)snprintf(path, sizeof(path), "%s/absolute-pkg.el", root);
	CHECK(write_text_file(
		  path, "(setq absolute-ran t)\n(provide 'absolute-f)\n")
	    == 0);
	length = snprintf(
	    source, sizeof(source), "(require 'absolute-f \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_eq(source, "absolute-f"));
	CHECK(eval_eq("absolute-ran", "t"));

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_load_path_order(void)
{
	char root[64], extra[64], path[512], source[600];
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	(void)snprintf(path, sizeof(path), "%s/kg/lisp/dup.el", root);
	CHECK(write_text_file(
		  path, "(setq dup-source 'default)\n(provide 'dup)\n")
	    == 0);

	(void)snprintf(extra, sizeof(extra), "/tmp/kg-lisp-extra-XXXXXX");
	CHECK(mkdtemp(extra) != nullptr);
	(void)snprintf(path, sizeof(path), "%s/dup.el", extra);
	CHECK(
	    write_text_file(path, "(setq dup-source 'extra)\n(provide 'dup)\n")
	    == 0);

	/* add-to-load-path prepends, so the directory just added is searched
	 * before the default <config>/kg/lisp/ one -- the order that decides
	 * which of the two same-named files (require 'dup) finds. */
	length = snprintf(
	    source, sizeof(source), "(add-to-load-path \"%s\")", extra);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_ok(source));
	CHECK(eval_ok("(require 'dup)"));
	CHECK(eval_eq("dup-source", "extra"));

	kg_lisp_shutdown();
	remove_config_root(root);
	(void)snprintf(path, sizeof(path), "%s/dup.el", extra);
	(void)remove(path);
	(void)rmdir(extra);
}

static void test_load_path_missing_dirs(void)
{
	char root[64];

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* A load-path entry that does not exist fails the same access()
	 * check a missing file would, so several stale directories in a row
	 * are just several misses, not a crash. */
	CHECK(eval_ok("(add-to-load-path \"/no/such/directory/at/all\")"));
	CHECK(eval_error_contains(
	    "(require 'never-provided-anywhere)", "never-provided-anywhere"));
	CHECK(eval_eq("(+ 3 4)", "7"));

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_math_natives(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(expt 2 8)"));
	CHECK(eval_ok("(expt 2 10)"));
	CHECK(eval_ok("(sin 0)"));
	CHECK(eval_ok("(cos 0)"));
	CHECK(eval_ok("(sqrt 16)"));
	CHECK(eval_ok("(log (exp 1))"));
	CHECK(eval_ok("(atan 1 1)"));

	CHECK(eval_ok("(floor 7 2)"));
	CHECK(eval_ok("(ceiling -7 2)"));
	CHECK(eval_ok("(truncate -7.5 2)"));

	CHECK(eval_eq("(round 2.5)", "2"));
	CHECK(eval_eq("(round -2.5)", "-2"));
	CHECK(eval_eq("(floor -7 2)", "-4"));

	/* Integer division by zero is an arith-error, as in Emacs; the
	 * float spelling (/ 1.0 0) is what produces the nonfinite values
	 * test_format_natives exercises. */
	CHECK(eval_error_contains("(/ 1 0)", "Arithmetic error"));
	CHECK(eval_error_contains("(/ 0)", "Arithmetic error"));
	CHECK(eval_error_contains("(/ 5 0)", "Arithmetic error"));
	CHECK(eval_eq("(/ 1.0 0)", "1.0e+INF"));

	kg_lisp_shutdown();
}

static int eval_eq_int(const char *source, int expected)
{
	char want[32];

	(void)snprintf(want, sizeof(want), "%d", expected);
	return eval_eq(source, want);
}

/* "abc" / "héllo" / "漢字": 1-based codepoint positions are
 * a=1 b=2 c=3 \n=4 h=5 é=6 l=7 l=8 o=9 \n=10 漢=11 字=12, so
 * (point-max) is 13 even though the buffer holds 15 bytes. */
static void setup_utf8_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "h\xc3\xa9llo", 6);
	editor_insert_row(bcur(), 2, "\xe6\xbc\xa2\xe5\xad\x97", 6);
}

static void test_point_offsets(void)
{
	char source[64];
	int n;

	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(point-min)", "1"));
	CHECK(eval_eq("(point-max)", "13"));

	/* Every position and count is an integer now, not a double. */
	CHECK(eval_eq("(type-of (point-min))", "integer"));
	CHECK(eval_eq("(type-of (point))", "integer"));
	CHECK(eval_eq("(type-of (point-max))", "integer"));
	CHECK(eval_eq("(type-of (line-number-at-pos))", "integer"));
	CHECK(eval_eq("(type-of (current-column))", "integer"));

	/* Every position round-trips, and offsets count codepoints even though
	 * the stored column is a byte index. */
	for (n = 1; n <= 13; n++) {
		(void)snprintf(source, sizeof(source), "(goto-char %d)", n);
		CHECK(eval_ok(source));
		CHECK(eval_eq_int("(point)", n));
	}
	/* Position 7 is the first "l" of "héllo", which the editor stores at
	 * byte column 3 of row 2. */
	CHECK(eval_ok("(goto-char 7)"));
	CHECK(eval_eq("(line-number-at-pos)", "2"));
	CHECK(eval_eq("(current-column)", "2"));
	CHECK(eval_ok("(goto-char 11)"));
	CHECK(eval_eq("(line-number-at-pos)", "3"));
	CHECK(eval_eq("(current-column)", "0"));

	/* Out-of-range positions clamp at both ends. */
	CHECK(eval_eq("(progn (goto-char -400) (point))", "1"));
	CHECK(eval_eq("(progn (goto-char 0) (point))", "1"));
	CHECK(eval_eq("(progn (goto-char 9999) (point))", "13"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_current_column_tab(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "\tx", 2);
	CHECK(kg_lisp_init() == 0);

	/* Display column, so the tab expands to the next tab stop (8),
	 * matching Emacs' current-column. */
	CHECK(eval_eq("(progn (goto-char 1) (current-column))", "0"));
	CHECK(eval_eq("(progn (goto-char 2) (current-column))", "8"));
	CHECK(eval_eq("(progn (goto-char 3) (current-column))", "9"));

	/* The native must see a setq earlier in the same form; waiting for a
	 * later repaint left C geometry one evaluation behind. */
	CHECK(eval_eq(
	    "(progn (setq tab-width 4) (goto-char 2) (current-column))", "4"));
	CHECK(strcmp(bcur()->row[0].render, "    x") == 0);
	CHECK(eval_eq(
	    "(progn (setq tab-width 9) (goto-char 2) (current-column))", "9"));
	CHECK(bcur()->row[0].hl_capacity >= bcur()->row[0].rsize);
	CHECK(eval_eq("(progn (setq tab-width 32) (goto-char 2) "
		      "(current-column))",
	    "32"));
	CHECK(bcur()->row[0].rsize == 33);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_char_after(void)
{
	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(char-after 1)", "97")); /* a */
	CHECK(eval_eq("(char-after 4)", "10")); /* row separator */
	CHECK(eval_eq("(char-after 6)", "233")); /* U+00E9 */
	CHECK(eval_eq("(char-after 11)", "28450")); /* U+6F22 */
	CHECK(eval_eq("(char-after 12)", "23383")); /* U+5B57 */
	CHECK(eval_eq("(char-after 13)", "nil")); /* end of buffer */
	CHECK(eval_eq("(char-after 9999)", "nil"));
	CHECK(eval_eq("(char-after 0)", "97"));
	CHECK(eval_eq("(progn (goto-char 6) (char-after))", "233"));
	CHECK(eval_eq("(progn (goto-char 6) (char-after nil))", "233"));
	CHECK(eval_eq("(progn (goto-char (point-max)) (char-after))", "nil"));
	/* A char code is an integer now, not a double. */
	CHECK(eval_eq("(type-of (char-after 1))", "integer"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_buffer_substring(void)
{
	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(buffer-substring 5 9)", "h\xc3\xa9ll"));
	/* Order-insensitive, like Emacs. */
	CHECK(eval_eq("(buffer-substring 9 5)", "h\xc3\xa9ll"));
	CHECK(eval_eq("(buffer-substring 3 3)", ""));
	CHECK(eval_eq("(buffer-substring 1 13)",
	    "abc\nh\xc3\xa9llo\n\xe6\xbc\xa2\xe5\xad\x97"));
	/* Both ends clamp. */
	CHECK(eval_eq("(buffer-substring -20 9999)",
	    "abc\nh\xc3\xa9llo\n\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_error_contains("(buffer-substring 1)", "too few arguments"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_mark_and_region(void)
{
	int dirty_before;

	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);
	dirty_before = bcur()->dirty;

	/* No mark: (mark) is nil and the region accessors signal. */
	CHECK(eval_eq("(mark)", "nil"));
	CHECK(eval_error_contains("(region-beginning)", "no region"));
	CHECK(eval_error_contains("(region-end)", "no region"));

	CHECK(eval_ok("(set-mark 5)"));
	CHECK(kg_mark_is_set(bcur()));
	CHECK(bcur()->mark_highlight == 1);
	CHECK(eval_eq("(mark)", "5"));
	CHECK(eval_eq("(progn (goto-char 9) (region-beginning))", "5"));
	CHECK(eval_eq("(region-end)", "9"));
	CHECK(eval_eq("(buffer-substring (region-beginning) (region-end))",
	    "h\xc3\xa9ll"));

	/* Mark after point still yields an ordered region. */
	CHECK(eval_ok("(progn (set-mark 9) (goto-char 5))"));
	CHECK(eval_eq("(region-beginning)", "5"));
	CHECK(eval_eq("(region-end)", "9"));

	/* Out-of-range marks clamp. */
	CHECK(eval_ok("(set-mark 9999)"));
	CHECK(eval_eq("(mark)", "13"));

	/* deactivate-mark drops the highlight but keeps the mark. */
	CHECK(eval_ok("(deactivate-mark)"));
	CHECK(bcur()->mark_highlight == 0);
	CHECK(kg_mark_is_set(bcur()));
	CHECK(eval_eq("(mark)", "13"));

	/* None of this touches the buffer. */
	CHECK(bcur()->dirty == dirty_before);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_word_motion(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one two three", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (forward-word) (point))", "4"));
	CHECK(eval_eq("(progn (goto-char 1) (forward-word 2) (point))", "8"));
	/* A huge count stops at the edge instead of spinning. */
	CHECK(
	    eval_eq("(progn (goto-char 1) (forward-word 9999) (point))", "14"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word) (point))", "9"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word 9999) (point))",
	    "1"));
	/* Negative counts reverse direction, as in Emacs. */
	CHECK(eval_eq("(progn (goto-char 1) (backward-word -1) (point))", "4"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (forward-word -1) (point))", "9"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* forward-word/backward-word cross row boundaries treating a row break --
 * even one crossing an empty row -- as a single separator, never a stop,
 * matching src/word.c's word_boundary_forward().  This also pins the
 * O(rows)-per-scanned-byte regression: the buggy lisp_flat_byte_at()
 * (since removed) re-derived (row, col) from a flat position on every
 * scanned byte, which this multi-row case exercises even though the test
 * itself only checks the result, not the cost. */
static void test_word_motion_crosses_rows(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one", 3);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "two three", 9);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (forward-word) (point))", "4"));
	CHECK(eval_eq("(progn (goto-char 1) (forward-word 2) (point))", "9"));
	CHECK(eval_eq("(progn (goto-char 1) (forward-word 3) (point))", "15"));
	CHECK(eval_eq("(point-max)", "15"));

	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word) (point))", "10"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word 2) (point))", "6"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word 3) (point))", "1"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_editor_bridge(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abc", 3);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(message \"bridged\")"));
	CHECK(strcmp(test_status_message, "bridged") == 0);
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));
	CHECK(eval_ok("(progn (goto-char 1) (insert \"z\"))"));
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "zabc"));
	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hey\")))"));
	CHECK(eval_ok("(global-set-key \"C-c i\" \"greet\")"));
	CHECK(keybind_lookup("C-c i") != nullptr);
	CHECK(eval_ok("(global-unset-key \"C-c i\")"));
	CHECK(keybind_lookup("C-c i") == nullptr);
	CHECK(
	    eval_error_contains("(load \"absent-package\")", "absent-package"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Buffer objects and the runtime execution context ---------------- */

/* Buffer undo depth, for the hidden-edit isolation checks below. */
static int undo_depth_of_buffer(struct editor_buffer *b)
{
	return b ? b->undostack.size : -1;
}

static void test_buffer_objects(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* current-buffer answers with the display buffer, an adapter object
	 * that is eq-identical across asks for the same buffer. */
	CHECK(eval_eq("(type-of (current-buffer))", "buffer"));
	CHECK(eval_eq("(buffer-live-p (current-buffer))", "t"));
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));
	CHECK(
	    eval_eq("(eq (current-buffer) (get-buffer \"bridge.txt\"))", "t"));
	CHECK(eval_eq("(get-buffer \"absent\")", "nil"));
	CHECK(eval_eq("(buffer-live-p 42)", "nil"));
	CHECK(eval_error_contains("(buffer-name 42)", "Wrong type argument"));
	CHECK(eval_eq("(condition-case e (buffer-name 42) "
		      "(wrong-type-argument (car (cdr e))))",
	    "bufferp"));

	/* get-buffer-create makes a clean hidden buffer, idempotent by name. */
	CHECK(eval_ok("(setq h (get-buffer-create \"hidden\"))"));
	CHECK(eval_eq("(buffer-name h)", "hidden"));
	CHECK(eval_eq("(eq h (get-buffer-create \"hidden\"))", "t"));
	CHECK(eval_eq("(buffer-live-p h)", "t"));

	/* buffer-list: current buffer first, every live buffer exactly once,
	 * the just-created hidden buffer present. */
	CHECK(eval_eq("(length (buffer-list))", "2"));
	CHECK(eval_eq("(eq (car (buffer-list)) (current-buffer))", "t"));
	CHECK(eval_eq("(if (memq h (buffer-list)) t nil)", "t"));
	CHECK(eval_eq(
	    "(if (memq (current-buffer) (cdr (buffer-list))) t nil)", "nil"));

	/* Hidden-buffer editing: insert lands there and the displayed
	 * window's point does not move; the edit is one undo record in the
	 * hidden buffer and none in the display buffer's chain. */
	CHECK(eval_eq("(progn (set-buffer h) (insert \"hello\")"
		      " (list (buffer-name) (point)"
		      " (buffer-substring (point-min) (point-max))))",
	    "(\"hidden\" 6 \"hello\")"));
	CHECK(wcur()->cx == 0 && wcur()->cy == 0);
	CHECK(bcur()->undostack.size == 0);
	CHECK(undo_depth_of_buffer(buf_resolve(buf_handle(1))) == 1);
	/* A second hidden edit is a second undo record. */
	CHECK(eval_ok("(progn (set-buffer h) (insert \"x\"))"));
	CHECK(undo_depth_of_buffer(buf_resolve(buf_handle(1))) == 2);
	/* The display buffer is untouched. */
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", ""));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_kill_buffer(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Killing a hidden buffer leaves its object dead. */
	CHECK(eval_ok("(setq h (get-buffer-create \"doomed\"))"));
	CHECK(eval_eq("(kill-buffer h)", "nil"));
	CHECK(eval_eq("(buffer-live-p h)", "nil"));
	CHECK(eval_error_contains("(buffer-name h)", "buffer is dead"));

	/* A modified buffer is refused without prompting. */
	CHECK(eval_ok("(setq h (get-buffer-create \"dirty\"))"));
	CHECK(eval_ok("(progn (set-buffer h) (insert \"x\"))"));
	CHECK(eval_error_contains("(kill-buffer h)", "modified"));
	CHECK(eval_eq("(buffer-live-p h)", "t"));

	/* Killing the exec buffer leaves later work on it an error. */
	CHECK(eval_ok("(setq h (get-buffer-create \"current\"))"));
	CHECK(eval_error_contains(
	    "(progn (set-buffer h) (kill-buffer (current-buffer)) (point))",
	    "current buffer is dead"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_buffer_stale_slot(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Kill a buffer, create another in the same slot; the old object
	 * must not alias the new buffer. */
	CHECK(eval_ok("(insert \"KEEP\")"));
	CHECK(eval_ok("(setq a (get-buffer-create \"slot-a\"))"));
	CHECK(eval_ok("(kill-buffer a)"));
	CHECK(eval_ok("(setq b (get-buffer-create \"slot-b\"))"));
	CHECK(eval_eq("(buffer-name b)", "slot-b"));
	CHECK(eval_eq("(buffer-live-p a)", "nil"));
	CHECK(eval_error_contains("(buffer-name a)", "buffer is dead"));
	CHECK(eval_error_contains("(set-buffer a)", "buffer is dead"));
	/* The display buffer's text is untouched by the slot reuse. */
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "KEEP"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* setup_editor() resets the display buffer alone, so buffers an earlier
 * case created still sit in their slots while buf_count says one.  A case
 * that needs the whole table hands them back first, through the harness's
 * own kill so their rows, names and stores go with them. */
static void release_other_buffers(void)
{
	int i, active = 0;

	for (i = 0; i < MAX_BUFFERS; i++) {
		active += buflist[i].active ? 1 : 0;
	}
	buf_count = active;
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (i == buf_current || !buflist[i].active) {
			continue;
		}
		buflist[i].dirty = 0;
		CHECK(buf_kill_buffer(buf_handle(i)));
	}
	CHECK(buf_count == 1);
}

/* The runtime point table holds one entry per buffer slot, so its last
 * entry is only reached when every slot is live and every one of them has
 * been selected.  Fill it that way: a table that did not grow with
 * MAX_BUFFERS raises "too many buffers with runtime point" partway, and
 * the buffer in the last slot is the one whose point proves it did. */
static void test_point_table_reaches_the_last_slot(void)
{
	char form[160];
	int i;

	setup_editor();
	release_other_buffers();
	CHECK(kg_lisp_init() == 0);

	/* Bounded by the iteration count as well as by buf_count, so a
	 * create that stops taking slots ends the loop instead of spinning.
	 * Each buffer gets a point of its own, which is what takes a table
	 * entry; the display buffer takes one at every frame's start. */
	for (i = 1; i < MAX_BUFFERS && buf_count < MAX_BUFFERS; i++) {
		(void)snprintf(form, sizeof(form),
		    "(progn (set-buffer (get-buffer-create \"cap-%d\"))"
		    " (insert \"abcdef\") (goto-char %d))",
		    i, 1 + i % 6);
		CHECK(eval_ok(form));
	}
	CHECK(buf_count == MAX_BUFFERS);

	/* One buffer past the table is refused with a message, not a slot. */
	CHECK(eval_error_contains(
	    "(get-buffer-create \"one-too-many\")", "too many open buffers"));

	/* The buffer in the last slot kept the point set in it, and so did
	 * the first one filled: no entry was evicted to make room. */
	(void)snprintf(form, sizeof(form),
	    "(progn (set-buffer (get-buffer \"cap-%d\")) (point))",
	    MAX_BUFFERS - 1);
	CHECK(eval_eq_int(form, 1 + (MAX_BUFFERS - 1) % 6));
	CHECK(eval_eq_int(
	    "(progn (set-buffer (get-buffer \"cap-1\")) (point))", 2));

	/* This suite's teardown only owns the display buffer, so the filled
	 * slots are handed back here. */
	for (i = 1; i < MAX_BUFFERS; i++) {
		(void)snprintf(form, sizeof(form),
		    "(progn (set-buffer (get-buffer \"cap-%d\"))"
		    " (set-buffer-modified-p nil)"
		    " (kill-buffer (current-buffer)))",
		    i);
		CHECK(eval_ok(form));
	}
	CHECK(buf_count == 1);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_buffer_point_sync(void)
{
	char result[128] = "";
	setup_editor();
	editor_insert_row(bcur(), 0, "one two", 7);
	CHECK(kg_lisp_init() == 0);

	/* A successful goto-char syncs the runtime point to the window. */
	CHECK(kg_lisp_eval_string("(goto-char 3)", sizeof("(goto-char 3)") - 1,
		  result, sizeof(result))
	    == 0);
	CHECK(wcur()->cx == 2);
	CHECK(wcur()->cy == 0);
	CHECK(eval_eq("(point)", "3"));

	/* A failed frame must not synchronize: the window stays put. */
	CHECK(eval_error_contains("(progn (goto-char 7) (car 1))", "pair"));
	CHECK(wcur()->cx == 2);
	CHECK(wcur()->cy == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* lisp_finite() is the seam every numeric argument of a kg native crosses,
 * and it used to hand the type check to FeToDouble, whose failure text is
 * "expected double, got string" -- fe's own wording, naming neither Emacs'
 * condition nor either of kg's two number types since 05D.  It now tags the
 * operand itself and raises Emacs' wrong-type-argument, message-level for
 * now (Phase 6 makes it a structured condition).  A NaN passes the tag test
 * and is still refused on its value, with its own text; nothing about the
 * accepted values moved. */
static void test_numeric_argument_seam(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one two", 7);
	CHECK(kg_lisp_init() == 0);

	/* Every native that reads a position or a count through the seam. */
	CHECK(eval_error_contains("(goto-char \"x\")", "Wrong type argument"));
	CHECK(eval_error_contains("(goto-line \"x\")", "Wrong type argument"));
	CHECK(
	    eval_error_contains("(forward-word \"x\")", "Wrong type argument"));
	CHECK(eval_error_contains(
	    "(char-to-string \"x\")", "Wrong type argument"));
	CHECK(eval_error_contains(
	    "(substring \"abcd\" \"1\" 2)", "Wrong type argument"));
	/* Not only strings: a symbol, a list, nil and an adapter object are
	 * the same verdict, where each used to name a raw fe type tag. */
	CHECK(eval_error_contains("(goto-char 'a)", "Wrong type argument"));
	CHECK(
	    eval_error_contains("(goto-char (list 1))", "Wrong type argument"));
	CHECK(eval_error_contains("(goto-char nil)", "Wrong type argument"));
	CHECK(eval_error_contains(
	    "(goto-char (make-marker))", "Wrong type argument"));
	/* The retired text is gone, not merely joined. */
	CHECK(eval_error_contains("(goto-char \"x\")", "Wrong type argument"));
	CHECK(!eval_error_contains("(goto-char \"x\")", "expected double"));

	/* A NaN keeps its own message, and an infinity still clamps rather
	 * than raising -- both unchanged by the tag check in front. */
	CHECK(
	    eval_error_contains("(goto-char (- (/ 1.0 0) (/ 1.0 0)))", "NaN"));
	CHECK(eval_ok("(goto-char (/ 1.0 0))"));

	/* Both number tags are accepted, and an integer still widens. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_eq("(point)", "3"));
	CHECK(eval_ok("(goto-char 5.0)"));
	CHECK(eval_eq("(point)", "5"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Point is a property of the buffer, not of the frame that happens to be
 * looking at it: (goto-char N), set-buffer away to a hidden buffer and
 * straight back within the same frame, must still see N.  Native-test
 * half of the PTY case lisp-point-survives-set-buffer.yaml. */
static void test_point_survives_set_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 4)"
		      " (set-buffer (get-buffer-create \"scratch\"))"
		      " (set-buffer (get-buffer \"bridge.txt\"))"
		      " (point))",
	    "4"));
	CHECK(eval_ok("(insert \"!\")"));
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "abc!def"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A hidden buffer's runtime point outlives the frame that created it: a
 * later, separate frame that set-buffers to it must pick up point where
 * the earlier frame's edit left it, not at point-min.  Native-test half
 * of the PTY case lisp-hidden-buffer-point-persists.yaml. */
static void test_hidden_buffer_point_persists_across_frames(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(progn (set-buffer (get-buffer-create \"scratch\"))"
		      " (insert \"hello\"))"));
	CHECK(eval_eq("(progn (set-buffer (get-buffer \"scratch\"))"
		      " (insert \"X\")"
		      " (buffer-substring (point-min) (point-max)))",
	    "helloX"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Frame entry is authoritative for the buffer the active window shows: it
 * overwrites that buffer's point-table entry from the window cursor on
 * every frame, rather than keeping whatever an earlier frame (or a stale
 * table entry) left there -- the window may have moved between commands
 * without going through Lisp at all. */
static void test_frame_entry_overwrites_from_window_cursor(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one two three", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 9) (point))", "9"));
	wcur()->cy = 0;
	wcur()->cx = 0;
	CHECK(eval_eq("(point)", "1"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The adapter object pool is bounded: distinct buffer handles each take
 * one record, so enough create/kill cycles without releasing the Lisp
 * values exhaust it, and the exhaustion is an error rather than a silent
 * alias to an earlier record.  The loop keeps every object alive (the
 * `held` list), so Fe's GC cannot release any record behind it. */
static void test_buffer_object_capacity(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Take the display buffer's object first: its record is what must
	 * survive the exhaustion below. */
	CHECK(eval_ok("(setq before (current-buffer))"));

	/* The adapter object pool is bounded: distinct buffer handles each
	 * take one record, so enough create/kill cycles while every Lisp
	 * value stays alive exhaust it, and the exhaustion is an error
	 * rather than a silent alias to an earlier record.  400 cycles, not
	 * the 100 this asserted before sub-plan 12D Part 3 raised
	 * LISP_MAX_OBJECTS 64 -> 256; the claim is the bound, not its
	 * value, and 100 no longer reaches it. */
	CHECK(eval_error_contains("(progn (setq held '()) (setq n 0)"
				  " (while (< n 400)"
				  "  (setq held (cons (get-buffer-create "
				  "(format \"cap-%d\" n)) held))"
				  "  (kill-buffer (car held))"
				  "  (setq n (+ n 1))))",
	    "too many buffer objects"));

	/* Exhaustion does not break existing objects: the display buffer's
	 * record was taken before the pool filled, so it still resolves and
	 * is still the same object a fresh ask returns (the pool is
	 * deduplicated per handle, not per ask). */
	CHECK(eval_eq("(buffer-live-p before)", "t"));
	CHECK(eval_eq("(eq before (current-buffer))", "t"));
	CHECK(eval_eq("(buffer-name before)", "bridge.txt"));
	/* Dropping the held list makes the wrappers unreachable again, so
	 * the pool has room for new distinct handles the next time Fe's GC
	 * actually collects. */
	CHECK(eval_ok("(setq held '())"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Editing natives: delete-region / replace-region ------------------ */

static void test_delete_region(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* Order-insensitive, like buffer-substring: (delete-region 8 3)
	 * deletes the same span as (delete-region 3 8). */
	CHECK(eval_ok("(delete-region 8 3)"));
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "heorld", 6) == 0);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_delete_region_utf8(void)
{
	setup_editor();
	/* "caf\xC3\xA9" is 4 codepoints ('c', 'a', 'f', e-acute) in 5
	 * bytes; deleting codepoint 4 removes both bytes of the e-acute
	 * and nothing else. */
	editor_insert_row(bcur(), 0, "caf\xC3\xA9", 5);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(delete-region 4 5)"));
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "caf", 3) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_delete_region_malformed_byte(void)
{
	setup_editor();
	/* A malformed lead byte counts as one codepoint of its own (see
	 * utf8_glyph_span_at() in def.h): "a\xFFb" is 3 codepoints in 3
	 * bytes, and deleting codepoint 2 removes exactly the bad byte. */
	editor_insert_row(bcur(), 0,
	    "a\xFF"
	    "b",
	    3);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(delete-region 2 3)"));
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "ab", 2) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_delete_region_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "original", 8);
	bcur()->readonly = 1;
	CHECK(kg_lisp_init() == 0);

	CHECK(kg_lisp_eval_string(
		  "(delete-region 1 4)", 20, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	/* Refused before any partial work: not one byte moved. */
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "original", 8) == 0);
	CHECK(bcur()->undostack.size == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_replace_region(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(replace-region 1 6 \"goodbye\")"));
	CHECK(bcur()->row[0].size == 13);
	CHECK(memcmp(bcur()->row[0].chars, "goodbye world", 13) == 0);
	/* One gateway call, one undo step -- never a delete plus an
	 * insert. */
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_replace_region_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "original", 8);
	bcur()->readonly = 1;
	CHECK(kg_lisp_init() == 0);

	CHECK(kg_lisp_eval_string(
		  "(replace-region 1 4 \"new\")", 27, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "original", 8) == 0);
	CHECK(bcur()->undostack.size == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* kg_edit_fail_alloc_after() is the seam test_buffer.c's own
 * refusal-is-atomic sweep uses to prove kg_buffer_replace() never
 * publishes a partial edit; this is the thin Lisp wrapper's half of that
 * proof, since neither native checks the gateway's return value (see
 * native_insert(), which this follows). */
static void test_replace_region_alloc_failure_is_atomic(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "alpha beta gamma", 17);
	CHECK(kg_lisp_init() == 0);

	kg_edit_fail_alloc_after(0);
	CHECK(eval_ok("(replace-region 7 11 \"BETA\")"));
	kg_edit_fail_alloc_after(-1);

	CHECK(bcur()->row[0].size == 17);
	CHECK(memcmp(bcur()->row[0].chars, "alpha beta gamma", 17) == 0);
	CHECK(bcur()->undostack.size == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Search natives ---------------------------------------------------- */

static void test_search_forward_backward(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "the cat sat on the mat", 22);
	CHECK(kg_lisp_init() == 0);

	/* search-forward moves point to the end of the match and returns
	 * the new point, as Emacs' does. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq("(search-forward \"cat\")", "8"));
	CHECK(eval_eq("(point)", "8"));
	/* No second "cat" ahead of point.  NOERROR `t' is written here
	 * because a bare two-argument failure RAISES `search-failed' since
	 * the frontier demand phase; the answer under `t' is the nil this
	 * line has always asserted, and point still does not move. */
	CHECK(eval_eq("(search-forward \"cat\" nil t)", "nil"));
	CHECK(eval_eq("(point)", "8"));

	/* search-backward moves to the start of the match. */
	CHECK(eval_ok("(goto-char (point-max))"));
	CHECK(eval_eq("(search-backward \"at\")", "21"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Emacs' NOERROR, all three answers and all four names, which is what
 * frontier-search-noerror-* froze against 31.0.91 before any of it
 * existed here.  The compat corpus asks the same questions of an Emacs
 * snapshot; this asks them of the editor's own objects, and adds the two
 * things a corpus case cannot see -- that the raise is catchable by the
 * NAME as well as by `error', and that COUNT is still refused. */
static void test_search_noerror_three_ways(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	CHECK(kg_lisp_init() == 0);

	/* NOERROR nil RAISES `search-failed', from all four names, carrying
	 * the PATTERN as its one data element, and point does not move. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq("(condition-case e (search-forward \"z\")"
		      " (error (cons (car e) (cdr e))))",
	    "(search-failed \"z\")"));
	CHECK(eval_eq("(point)", "1"));
	CHECK(eval_eq("(condition-case e (re-search-forward \"z\")"
		      " (search-failed 'by-name))",
	    "by-name"));
	CHECK(eval_ok("(goto-char (point-max))"));
	CHECK(eval_eq("(condition-case e (search-backward \"z\")"
		      " (error (car e)))",
	    "search-failed"));
	CHECK(eval_eq("(condition-case e (re-search-backward \"z\")"
		      " (error (car e)))",
	    "search-failed"));
	CHECK(eval_eq("(point)", "7"));

	/* NOERROR `t' answers nil and leaves point, from all four. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq("(list (search-forward \"z\" nil t)"
		      " (re-search-forward \"z\" nil t) (point))",
	    "(nil nil 1)"));
	CHECK(eval_ok("(goto-char (point-max))"));
	CHECK(eval_eq("(list (search-backward \"z\" nil t)"
		      " (re-search-backward \"z\" nil t) (point))",
	    "(nil nil 7)"));

	/* ANY OTHER value answers nil AND MOVES POINT TO THE LIMIT: to
	 * BOUND when one was given, and to point-max forward / point-min
	 * backward when it was not.  All four combinations, because a
	 * binding that moved to point-max in both directions would pass the
	 * two forward ones. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq(
	    "(list (re-search-forward \"z\" nil 'move) (point))", "(nil 7)"));
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq(
	    "(list (search-forward \"z\" 3 'move) (point))", "(nil 3)"));
	CHECK(eval_ok("(goto-char (point-max))"));
	CHECK(eval_eq(
	    "(list (re-search-backward \"z\" nil 'move) (point))", "(nil 1)"));
	CHECK(eval_ok("(goto-char (point-max))"));
	CHECK(eval_eq(
	    "(list (search-backward \"z\" 3 'move) (point))", "(nil 3)"));

	/* A SUCCESSFUL search is unchanged by any of the three: NOERROR
	 * decides only what a failure does. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq("(list (search-forward \"cd\") (point))", "(5 5)"));
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq(
	    "(list (search-forward \"cd\" nil 'move) (point))", "(5 5)"));

	/* Emacs' FOURTH argument, COUNT, stays refused BY NAME rather than
	 * accepted and ignored -- frontier-search-count-argument pins the
	 * condition and is written not to flip. */
	CHECK(eval_eq("(condition-case e (re-search-forward \"a\" nil t 2)"
		      " (wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_search_forward_bound(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "aaa bbb aaa", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-min))"));
	/* A bound before the second "aaa" hides it. */
	CHECK(eval_eq("(search-forward \"aaa\" 9)", "4"));
	CHECK(eval_eq("(search-forward \"aaa\" 9 t)", "nil"));
	CHECK(eval_eq("(point)", "4"));
	/* Without a bound the second occurrence is reachable. */
	CHECK(eval_eq("(search-forward \"aaa\")", "12"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Fe's string reader drops a lone backslash before a non-escape character
 * ('\n', '\r' and '\t' are the only real escapes; see Read()'s '"' case
 * in fe.c), so a literal backslash inside a Fe string literal takes two
 * source backslashes -- exactly like Emacs Lisp's own string syntax.
 * `pattern` is written the way kg_regex_compile() sees it, one backslash
 * per escape, e.g. "\\(a*\\)*b"; this doubles each of its backslashes
 * before splicing it into `fmt`'s one %s, producing the Fe source that
 * reads back to that same pattern -- so a test does not have to work out
 * the double escaping by hand. */
static void fe_regex_source(
    char *out, size_t outsize, const char *fmt, const char *pattern)
{
	char quoted[200];
	size_t i = 0, o = 0;

	while (pattern[i] != '\0' && o + 2 < sizeof(quoted)) {
		if (pattern[i] == '\\') {
			quoted[o++] = '\\';
		}
		quoted[o++] = pattern[i++];
	}
	quoted[o] = '\0';
	(void)snprintf(out, outsize, fmt, quoted);
}

static void test_re_search_and_match_data(void)
{
	char source[256];

	setup_editor();
	editor_insert_row(bcur(), 0, "foobar", 6);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-min))"));
	fe_regex_source(source, sizeof(source), "(re-search-forward \"%s\")",
	    "\\(foo\\)bar");
	CHECK(eval_eq(source, "7"));
	CHECK(eval_eq("(match-beginning 0)", "1"));
	CHECK(eval_eq("(match-end 0)", "7"));
	CHECK(eval_eq("(match-beginning 1)", "1"));
	CHECK(eval_eq("(match-end 1)", "4"));
	/* Group 2 was never in the pattern. */
	CHECK(eval_eq("(match-beginning 2)", "nil"));
	CHECK(eval_eq("(match-end 2)", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* re-search-backward: two possible matches on the row, searching from
 * point-max must pick the later one (kg_regex_match_backward()'s own
 * "last match ending at or before the limit" policy, src/regex.c), and a
 * no-match search afterwards must not disturb point or the previous
 * match data -- lisp_search() only writes state.match and moves the
 * marker on a found match. */
static void test_re_search_backward(void)
{
	char source[256];

	setup_editor();
	editor_insert_row(bcur(), 0, "foobar foobar", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-max))"));
	fe_regex_source(source, sizeof(source), "(re-search-backward \"%s\")",
	    "\\(foo\\)bar");
	/* The second "foobar" (0-based [7,13)), not the first: the closer
	 * match to the starting point wins. */
	CHECK(eval_eq(source, "8"));
	CHECK(eval_eq("(point)", "8"));
	CHECK(eval_eq("(match-beginning 0)", "8"));
	CHECK(eval_eq("(match-end 0)", "14"));
	CHECK(eval_eq("(match-beginning 1)", "8"));
	CHECK(eval_eq("(match-end 1)", "11"));

	/* No match: nil, and point plus the previous match data are left
	 * exactly as the successful search above left them. */
	fe_regex_source(
	    source, sizeof(source), "(re-search-backward \"%s\" nil t)", "zzz");
	CHECK(eval_eq(source, "nil"));
	CHECK(eval_eq("(point)", "8"));
	CHECK(eval_eq("(match-beginning 0)", "8"));
	CHECK(eval_eq("(match-end 0)", "14"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_regex_too_complex_and_bad_pattern(void)
{
	char source[256];

	setup_editor();
	/* 24 "a"s with no "b": the classic catastrophic-backtracking shape
	 * for "\(a*\)*b" trips this engine's step budget -- see
	 * test_regex.c's test_exhausted_budget_is_not_no_match(), which
	 * pins the exact boundary this reuses.  Too-complex is a distinct,
	 * truthful error, never folded into "no match". */
	editor_insert_row(bcur(), 0, "aaaaaaaaaaaaaaaaaaaaaaaa", 24);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-min))"));
	fe_regex_source(
	    source, sizeof(source), "(re-search-forward \"%s\")", "\\(a*\\)*b");
	CHECK(eval_error_contains(source, "too complex"));

	/* An unclosed group is a bad pattern: a distinct error again, not
	 * silently treated as "no match" either. */
	fe_regex_source(source, sizeof(source), "(re-search-forward \"%s\")",
	    "\\(unclosed");
	CHECK(eval_error_contains(source, "invalid regexp"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_search_cancellation(void)
{
	char result[128] = "";
	/* NOERROR `t': this case is about the QUIT, and a bare failure
	 * raises `search-failed' since the frontier demand phase, which
	 * would make the uninterrupted retry below a condition rather than
	 * the nil it is asserting. */
	const char *source = "(search-forward \"zzz\" nil t)";

	setup_editor();
	editor_insert_row(bcur(), 0, "xxxx", 4);
	editor_insert_row(bcur(), 1, "xxxx", 4);
	editor_insert_row(bcur(), 2, "xxxx", 4);
	CHECK(kg_lisp_init() == 0);
	CHECK(eval_ok("(goto-char (point-min))"));

	/* C-g reaches a multi-row search through the same interrupt check
	 * every long-running native polls; the row loop checks it once per
	 * row; two non-matching rows are enough to trip a threshold of 2. */
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(
	    kg_lisp_eval_string(source, strlen(source), result, sizeof(result))
	    != 0);
	CHECK(strcmp(result, "Quit") == 0);
	kg_lisp_set_interrupt_check(nullptr);

	/* Cancellation is distinct from "not found": an uninterrupted retry
	 * reports nil, not another error. */
	CHECK(eval_eq(source, "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Marker natives ----------------------------------------------------
 * (make-marker) answers a detached marker and (point-marker) one at
 * point, which is Emacs' split.  It answered point-marker's marker from
 * sub-plan 03 until Phase 17 added the name that meant it. */

static void test_markers(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* Emacs' make-marker points nowhere until something sets it. */
	CHECK(eval_eq("(marker-position (make-marker))", "nil"));
	CHECK(eval_eq("(marker-buffer (make-marker))", "nil"));
	CHECK(eval_eq("(type-of (make-marker))", "marker"));

	CHECK(eval_ok("(goto-char 7)"));
	CHECK(eval_ok("(setq m (point-marker))"));
	CHECK(eval_eq("(type-of m)", "marker"));
	CHECK(eval_eq("(marker-position m)", "7"));
	CHECK(eval_eq("(eq (marker-buffer m) (current-buffer))", "t"));
	/* Unlike buffer objects, two markers are never eq. */
	CHECK(eval_eq("(eq (make-marker) (make-marker))", "nil"));
	/* copy-marker takes a position, another marker, or nothing at all --
	 * and nothing at all detaches, measured on Emacs 31.0.90. */
	CHECK(eval_eq("(marker-position (copy-marker))", "nil"));
	CHECK(eval_eq("(marker-position (copy-marker 3))", "3"));
	CHECK(eval_eq("(marker-position (copy-marker m))", "7"));
	CHECK(eval_eq("(eq (copy-marker m) m)", "nil"));
	/* Copying a marker that points nowhere copies the nowhere, rather
	 * than falling back to point. */
	CHECK(eval_eq("(marker-position (copy-marker (make-marker)))", "nil"));
	/* A non-nil TYPE is Emacs' insertion type t -- kg's right gravity,
	 * so text inserted AT the marker carries it along, where the
	 * default leaves it behind. */
	CHECK(eval_eq("(with-temp-buffer (insert \"abcde\")"
		      " (let ((r (copy-marker 3 t)) (l (copy-marker 3)))"
		      " (goto-char 3) (insert \"XX\")"
		      " (list (marker-position l) (marker-position r))))",
	    "(3 5)"));

	/* An insertion strictly before the marker always pushes it
	 * forward, whatever its gravity. */
	CHECK(eval_ok("(goto-char 1)"));
	CHECK(eval_ok("(insert \"XXXXX\")"));
	CHECK(eval_eq("(marker-position m)", "12"));

	/* set-marker moves the same marker object, including to a
	 * different buffer, and returns it.  set-buffer only lasts for the
	 * top-level form that calls it (see the struct kg_lisp_exec_ctx
	 * comment in lisp_internal.h), so the buffer switch and the insert
	 * it enables have to be one form. */
	CHECK(eval_ok("(setq h (get-buffer-create \"scratch\"))"));
	CHECK(eval_ok("(progn (set-buffer h) (insert \"abcdef\"))"));
	CHECK(eval_eq("(eq (set-marker m 3 h) m)", "t"));
	CHECK(eval_eq("(marker-position m)", "3"));
	CHECK(eval_eq("(eq (marker-buffer m) h)", "t"));

	/* A nil position detaches it: no position, no buffer. */
	CHECK(eval_ok("(set-marker m nil)"));
	CHECK(eval_eq("(marker-position m)", "nil"));
	CHECK(eval_eq("(marker-buffer m)", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A marker outlives the buffer it named -- not an error, unlike a dead
 * buffer object -- and set-marker on one whose old buffer is gone still
 * works, by simply creating a fresh underlying marker in the new
 * buffer. */
static void test_marker_survives_buffer_kill(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq h (get-buffer-create \"doomed\"))"));
	/* set-buffer and make-marker have to share a form for the marker to
	 * land in "doomed" rather than the window's own buffer -- see the
	 * struct kg_lisp_exec_ctx comment in lisp_internal.h. */
	CHECK(eval_ok("(progn (set-buffer h) (setq m (point-marker)))"));
	CHECK(eval_ok("(kill-buffer h)"));

	CHECK(eval_eq("(marker-position m)", "nil"));
	CHECK(eval_eq("(marker-buffer m)", "nil"));

	CHECK(eval_ok("(set-marker m 1)"));
	CHECK(eval_eq("(marker-position m)", "1"));
	CHECK(eval_eq("(eq (marker-buffer m) (current-buffer))", "t"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Indices count codepoints, so "héllo" is 5 characters in 6 bytes and
 * "漢字" is 2 characters in 6 bytes. */
/* Emacs' `compare-strings', whose every rule was measured on 31.0.91 and
 * frozen as an oracle case (frontier-compare-strings-*) before the name
 * existed here.  This is the kg-side half: the same contract asked of the
 * editor's own objects, plus the bounds TABLE, which is where three of
 * the rules are not what the name suggests. */
/* Emacs' `assoc-string', the want behind s-format and s--aget, and the
 * one internal name the frontier demand phase added under it.  Every rule
 * was frozen as an oracle case first (frontier-assoc-string-*); what this
 * adds is the DESIGNATOR asked directly, and the two ends of the
 * asymmetry side by side, which is where a reimplementation goes wrong. */
/* Emacs' `regexp-opt', asked as a CONTRACT rather than as a spelling:
 * Emacs returns an optimized trie -- (regexp-opt '("a" "ab")) is
 * "\\(?:ab?\\)" there -- and matching that text would encode Emacs'
 * optimizer here.  So every assertion below compiles kg's answer with
 * kg's own engine and asks what it matches, which is what
 * frontier-regexp-opt-* froze. */
static void test_regexp_opt(void)
{
	CHECK(kg_lisp_init() == 0);

	/* LONGEST FIRST, whichever order the members arrive in.  This is
	 * the smallest input that tells a right implementation from a wrong
	 * one: an unsorted alternation matches only the "a". */
	CHECK(eval_eq("(let ((re (regexp-opt (list \"a\" \"ab\"))))"
		      " (list (string-match re \"ab\") (match-end 0)))",
	    "(0 2)"));
	CHECK(eval_eq("(let ((re (regexp-opt (list \"ab\" \"a\"))))"
		      " (list (string-match re \"ab\") (match-end 0)))",
	    "(0 2)"));
	/* Every member matches ENTIRELY, and the longest that fits wins,
	 * over three levels of shared prefix. */
	CHECK(eval_eq("(let* ((members (list \"cat\" \"car\" \"cart\" \"c\"))"
		      " (re (regexp-opt members)))"
		      " (mapcar (lambda (m) (list (string-match re m)"
		      " (match-end 0))) members))",
	    "((0 3) (0 3) (0 4) (0 1))"));

	/* MEMBERS ARE LITERAL TEXT: the regexp has to say so. */
	CHECK(eval_eq("(let ((re (regexp-opt (list \"a.c\" \"a+b\"))))"
		      " (list (string-match re \"axc\") (string-match re"
		      " \"a.c\") (match-end 0) (string-match re \"a+b\")))",
	    "(nil 0 3 0)"));

	/* NO MEMBERS is an UNMATCHABLE regexp and not the empty one, which
	 * would match everywhere.  Proved against four subjects including
	 * the regexp's OWN TEXT, since kg's engine misreads Emacs' shy
	 * group as ordinary characters and a wrong answer here would look
	 * like a match on the spelling. */
	CHECK(eval_eq("(let ((re (regexp-opt nil)))"
		      " (list (string-match re \"\") (string-match re \"x\")"
		      " (string-match re \"a\")"
		      " (string-match re \"\\\\`a\\\\`\")))",
	    "(nil nil nil nil)"));
	/* ONE member comes back BARE, with no group around it. */
	CHECK(eval_eq("(let ((re (regexp-opt (list \"x\"))))"
		      " (list (string-match re \"axb\") (match-end 0)))",
	    "(1 2)"));

	/* PAREN, the second argument (Phase 29 U.1a, reversing F.0's
	 * refuse-by-name decision after the Phase 28 census named a
	 * consumer).  It decides the WRAPPER and nothing else; every
	 * spelling below is Emacs 31.0.91's own, measured, and the
	 * behaviours are recorded in
	 * test/lisp-compat/cases/u0-regexp-opt-paren-*.json. */
	CHECK(
	    eval_eq("(regexp-opt (list \"a\" \"ab\") nil)", "\\(?:ab\\|a\\)"));
	CHECK(eval_eq("(regexp-opt (list \"a\" \"ab\") t)", "\\(ab\\|a\\)"));
	/* `words' and `symbols' ARE REFUSED, not produced: kg's engine has
	 * no \\< \\> \\_< or \\_>, so U.1a's producing Emacs' exact text
	 * gave callers a plausible-but-wrong match -- worse than a loud
	 * error.  The refusal names the engine gap that is Phase 29's
	 * U.2; the day the engine gains the four boundaries these two arms
	 * become wrappers again. */
	CHECK(eval_error_contains(
	    "(regexp-opt (list \"ab\") 'words)", "word boundaries"));
	CHECK(eval_error_contains(
	    "(regexp-opt (list \"ab\") 'symbols)", "symbol boundaries"));
	/* A STRING is the literal group OPENER and regexp-opt closes it. */
	CHECK(
	    eval_eq("(regexp-opt (list \"ab\") \"\\\\(?2:\")", "\\(?2:ab\\)"));
	/* Every other non-nil value behaves as t -- measured for `zzz',
	 * `WORDS', 5 and (1), so no table of accepted symbols is needed. */
	CHECK(eval_eq("(list (equal (regexp-opt (list \"a\" \"ab\") t)"
		      " (regexp-opt (list \"a\" \"ab\") 'zzz))"
		      " (equal (regexp-opt (list \"a\" \"ab\") t)"
		      " (regexp-opt (list \"a\" \"ab\") 5)))",
	    "(t t)"));
	/* The whole match is also group 1 under PAREN t -- the numbering
	 * shift the argument forces on the caller. */
	CHECK(
	    eval_eq("(let ((re (regexp-opt (list \"a\" \"ab\") t)))"
		    " (list (string-match re \"ab\") (match-string 1 \"ab\")))",
		"(0 \"ab\")"));
	/* No members stays unmatchable inside whichever wrapper is asked
	 * for. */
	CHECK(eval_eq("(let ((re (regexp-opt nil t)))"
		      " (list (string-match re \"a\") (string-match re \"\")))",
	    "(nil nil)"));
	/* WHAT THE REFUSAL ABOVE REPLACED: kg's engine has no \\< \\> or
	 * \\_< \\_>, and U.1a's words wrapper therefore matched a literal
	 * <ab> and not a word -- silently, with no error.  That assertion
	 * is gone with the production of the text; what stays pinned is
	 * that the refusal happens AT THE CALL, before any string exists
	 * to mis-match. */
	CHECK(eval_error_contains(
	    "(string-match (regexp-opt (list \"ab\") 'words) \"x ab y\")",
	    "word boundaries"));
	/* And an explicitly numbered group is refused by the engine
	 * outright, where the shy one beside it reads. */
	CHECK(eval_error_contains(
	    "(string-match (regexp-opt (list \"ab\") \"\\\\(?2:\") \"ab\")",
	    "invalid regexp"));

	/* The caller's own list is NOT rewritten, though kg's `sort' is
	 * destructive: `regexp-opt' copies first. */
	CHECK(eval_eq("(let ((members (list \"a\" \"abc\" \"ab\")))"
		      " (regexp-opt members) members)",
	    "(\"a\" \"abc\" \"ab\")"));

	kg_lisp_shutdown();
}

static void test_assoc_string_and_designator(void)
{
	CHECK(kg_lisp_init() == 0);

	/* The designator: a string is itself, a symbol is its name -- `nil'
	 * included -- and anything else answers nil, meaning `not
	 * comparable as a string'. */
	CHECK(eval_eq("(list (internal--string-designator \"a\")"
		      " (internal--string-designator 'a)"
		      " (internal--string-designator nil)"
		      " (internal--string-designator 1)"
		      " (internal--string-designator (list 1)))",
	    "(\"a\" \"a\" \"nil\" nil nil)"));

	/* THE ELEMENT COMES BACK, in every shape: a cons compared by its
	 * car, or a bare string or symbol compared as itself, with
	 * `symbol-name' coercing either side. */
	CHECK(eval_eq("(assoc-string \"b\" (list (cons \"a\" 1)"
		      " (cons \"b\" 2)))",
	    "(\"b\" . 2)"));
	CHECK(eval_eq("(assoc-string \"z\" (list (cons \"a\" 1)))", "nil"));
	CHECK(eval_eq("(assoc-string \"a\" (list \"a\" \"b\"))", "a"));
	CHECK(eval_eq("(assoc-string \"a\" (list 'a 'b))", "a"));
	CHECK(
	    eval_eq("(assoc-string 'a (list (cons \"a\" 1)))", "(\"a\" . 1)"));
	CHECK(eval_eq("(assoc-string \"a\" (list (cons 'a 1)))", "(a . 1)"));
	/* A nil KEY is the symbol `nil', so it is the string "nil" and
	 * finds an element -- measured on 31.0.91, and the one row that
	 * says the designator's nil answer is about TYPE and not about
	 * emptiness. */
	CHECK(eval_eq(
	    "(assoc-string nil (list (cons \"nil\" 1)))", "(\"nil\" . 1)"));

	/* THE ASYMMETRY.  A non-string ELEMENT is skipped silently and the
	 * scan CONTINUES past it; a non-string KEY raises.  An
	 * implementation that treated the two ends alike gets one of them
	 * wrong whichever way it chooses. */
	CHECK(eval_eq("(assoc-string \"a\" (list 1 2))", "nil"));
	CHECK(eval_eq("(assoc-string \"a\" (list (cons 1 2)))", "nil"));
	CHECK(eval_eq("(assoc-string \"a\" (list 1 (cons 2 3)"
		      " (cons \"a\" 4)))",
	    "(\"a\" . 4)"));
	CHECK(eval_eq("(assoc-string \"a\" nil)", "nil"));
	CHECK(eval_eq("(condition-case e (assoc-string 1 (list (cons \"1\" 2)))"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument stringp 1)"));

	/* Duplicates resolve to the FIRST match, and CASE-FOLD works in
	 * both directions -- ASCII only, `compare-strings'' own fold, which
	 * is why the É/é row beside these stays a divergence. */
	CHECK(eval_eq("(assoc-string \"b\" (list (cons \"a\" 1) (cons \"b\" 2)"
		      " (cons \"b\" 3)))",
	    "(\"b\" . 2)"));
	CHECK(eval_eq("(assoc-string \"A\" (list (cons \"a\" 1)))", "nil"));
	CHECK(eval_eq(
	    "(assoc-string \"A\" (list (cons \"a\" 1)) t)", "(\"a\" . 1)"));
	CHECK(eval_eq(
	    "(assoc-string \"a\" (list (cons \"A\" 1)) t)", "(\"A\" . 1)"));
	CHECK(eval_eq("(assoc-string \"\xc3\x89\" (list (cons \"\xc3\xa9\" 1))"
		      " t)",
	    "nil"));

	/* A FUNCTION REPLACER SEES THE MATCH DATA OF THE MATCHED TEXT, not
	 * of the subject -- Emacs' rule, and what s-format needs: its
	 * replacer reads `(match-string N MD)' where MD is the matched
	 * text, so subject offsets index the wrong string for every match
	 * after the first.  The second element is the one that answered
	 * "<a>-<>" before. */
	CHECK(eval_eq("(replace-regexp-in-string \"{\\\\([^}]+\\\\)}\""
		      " (lambda (md) (format \"<%s>\" (match-string 1 md)))"
		      " \"{a}-{b}\" t t)",
	    "<a>-<b>"));
	CHECK(eval_eq("(replace-regexp-in-string"
		      " \"\\\\$\\\\({\\\\([^}]+\\\\)}\\\\|[0-9]+\\\\)\""
		      " (lambda (md) (format \"<%s>\" (match-string 2 md)))"
		      " \"${a}-${b}\" t t)",
	    "<a>-<b>"));
	/* An EMPTY match keeps the old path, where kg already agreed with
	 * Emacs: both answer these two. */
	CHECK(eval_eq(
	    "(replace-regexp-in-string \"x*\" \"-\" \"abc\")", "-a-b-c"));
	CHECK(eval_eq("(replace-regexp-in-string \"x*\""
		      " (lambda (m) (format \"[%s]\" m)) \"axb\")",
	    "[]a[x][]b"));

	kg_lisp_shutdown();
}

static void test_compare_strings(void)
{
	CHECK(kg_lisp_init() == 0);

	/* THE RETURN CONTRACT: `t' for equal spans, else +/-(1 + the
	 * characters that compared EQUAL), signed by which sorts first.  A
	 * span that RUNS OUT is a mismatch too, which is the arithmetic
	 * s-shared-start recovers its prefix from. */
	CHECK(eval_eq("(compare-strings \"abc\" 0 3 \"abd\" 0 3)", "-3"));
	CHECK(eval_eq("(compare-strings \"abc\" 0 3 \"abc\" 0 3)", "t"));
	CHECK(eval_eq("(compare-strings \"abd\" 0 3 \"abc\" 0 3)", "3"));
	CHECK(eval_eq("(compare-strings \"ab\" 0 2 \"abc\" 0 3)", "-3"));
	CHECK(eval_eq("(compare-strings \"abc\" 0 3 \"ab\" 0 2)", "3"));
	/* The empty-span corners the index arithmetic has to survive. */
	CHECK(eval_eq("(compare-strings \"\" nil nil \"\" nil nil)", "t"));
	CHECK(eval_eq("(compare-strings \"\" nil nil \"a\" nil nil)", "-1"));

	/* NIL BOUNDS and an offset START, which is what s-ends-with?
	 * passes: nil START is 0, nil END is the length. */
	CHECK(eval_eq("(compare-strings \"xabc\" 1 nil \"abc\" nil nil)", "t"));
	CHECK(eval_eq("(compare-strings \"abc\" 1 2 \"abc\" 1 2)", "t"));

	/* THE BOUNDS TABLE.  An END past the string is CLIPPED SILENTLY --
	 * it is NOT the error this phase's plan predicted -- a START past
	 * it IS `args-out-of-range' whose data echoes the CLIPPED end, a
	 * NEGATIVE index counts from the end, and a nil END reports as nil
	 * rather than as the length it stands for. */
	CHECK(eval_eq("(compare-strings \"abc\" 0 10 \"abc\" 0 3)", "t"));
	CHECK(eval_eq("(compare-strings \"abc\" 0 4 \"abcd\" 0 4)", "-4"));
	CHECK(
	    eval_eq("(compare-strings \"abc\" -1 nil \"xbc\" nil nil)", "-1"));
	CHECK(eval_eq("(compare-strings \"abc\" 3 3 \"abc\" 0 3)", "-1"));
	CHECK(eval_eq("(condition-case e (compare-strings \"abc\" 5 6 \"abc\""
		      " 0 3) (error (cons (car e) (cdr e))))",
	    "(args-out-of-range \"abc\" 5 3)"));
	CHECK(eval_eq("(condition-case e (compare-strings \"abc\" -5 3 \"abc\""
		      " 0 3) (error (cons (car e) (cdr e))))",
	    "(args-out-of-range \"abc\" -5 3)"));
	CHECK(eval_eq("(condition-case e (compare-strings \"abc\" 2 1 \"abc\""
		      " 0 3) (error (cons (car e) (cdr e))))",
	    "(args-out-of-range \"abc\" 2 1)"));
	CHECK(eval_eq("(condition-case e (compare-strings \"abc\" 0 nil \"abc\""
		      " 9 nil) (error (cons (car e) (cdr e))))",
	    "(args-out-of-range \"abc\" 9 nil)"));
	/* The second string's span is checked the same way and names
	 * ITSELF, which is the whole reason this error carries three
	 * elements rather than two. */
	CHECK(eval_eq("(condition-case e (compare-strings \"abc\" 0 3 \"xy\""
		      " 5 6) (error (cons (car e) (cdr e))))",
	    "(args-out-of-range \"xy\" 5 2)"));

	/* THE TYPE ERRORS.  A symbol is NOT coerced here, where
	 * `assoc-string' does coerce one -- deliberately, and measured on
	 * both sides. */
	CHECK(eval_eq("(condition-case e (compare-strings 1 0 1 \"abc\" 0 3)"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument stringp 1)"));
	CHECK(eval_eq("(condition-case e (compare-strings \"abc\" 0 3 'abc 0 3)"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument stringp abc)"));

	/* THE INDEX IS IN CHARACTERS, not bytes: a byte-shaped
	 * implementation answers -3 for the first of these. */
	CHECK(eval_eq("(compare-strings \"\xc3\xa9"
		      "a\" nil nil \"\xc3\xa9"
		      "b\" nil nil)",
	    "-2"));
	CHECK(eval_eq("(compare-strings \"a\xc3\xa9\" nil nil \"a\xc3\xa9\""
		      " nil nil)",
	    "t"));

	/* IGNORE-CASE folds ASCII, and does so by UPCASING -- which only
	 * the SIGN of a mismatch shows: `(compare-strings "a" ... "_" ... t)'
	 * is -1 because the fold makes it "A" (0x41), before "_" (0x5F).
	 * The non-ASCII pair is the RECORDED DIVERGENCE this phase chose
	 * and did not close: kg answers -1 where Emacs answers t. */
	CHECK(eval_eq(
	    "(compare-strings \"ABC\" nil nil \"abc\" nil nil t)", "t"));
	CHECK(eval_eq(
	    "(compare-strings \"ABC\" nil nil \"abc\" nil nil nil)", "-1"));
	CHECK(eval_eq("(compare-strings \"a\" nil nil \"_\" nil nil t)", "-1"));
	CHECK(eval_eq("(compare-strings \"_\" nil nil \"a\" nil nil t)", "1"));
	CHECK(eval_eq("(compare-strings \"\xc3\x89\" nil nil \"\xc3\xa9\""
		      " nil nil t)",
	    "-1"));
	/* No multi-character case expansion on either side: Emacs answers
	 * -5 here too, because its fold does not turn German sharp s into
	 * two letters. */
	CHECK(eval_eq("(compare-strings \"STRASSE\" nil nil \"stra\xc3\x9f"
		      "e\" nil nil t)",
	    "-5"));

	kg_lisp_shutdown();
}

static void test_string_length_and_substring(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(string-length \"\")", "0"));
	CHECK(eval_eq("(string-length \"abc\")", "3"));
	CHECK(eval_eq("(string-length \"h\xc3\xa9llo\")", "5"));
	CHECK(eval_eq("(string-length \"\xe6\xbc\xa2\xe5\xad\x97\")", "2"));
	CHECK(eval_error_contains("(string-length 1)", "Wrong type argument"));
	/* Lengths and char codes are integers now, not doubles. */
	CHECK(eval_eq("(type-of (string-length \"abc\"))", "integer"));
	CHECK(eval_eq("(type-of (string-to-char \"abc\"))", "integer"));

	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 1)", "\xc3\xa9llo"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 1 3)", "\xc3\xa9l"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 0 2)", "h\xc3\xa9"));
	CHECK(eval_eq(
	    "(substring \"\xe6\xbc\xa2\xe5\xad\x97\" 1)", "\xe5\xad\x97"));
	CHECK(eval_eq(
	    "(substring \"\xe6\xbc\xa2\xe5\xad\x97\" 0 1)", "\xe6\xbc\xa2"));
	/* Negative indices count from the end, as in Emacs. */
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" -2)", "lo"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 1 -1)", "\xc3\xa9ll"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" -4 -3)", "\xc3\xa9"));
	/* nil means "the default end". */
	CHECK(eval_eq("(substring \"abc\" nil 2)", "ab"));
	CHECK(eval_eq("(substring \"abc\" 1 nil)", "bc"));
	/* Out of range clamps at both ends; a reversed range is empty. */
	CHECK(eval_eq("(substring \"abc\" 9)", ""));
	CHECK(eval_eq("(substring \"abc\" 0 99)", "abc"));
	CHECK(eval_eq("(substring \"abc\" -99)", "abc"));
	CHECK(eval_eq("(substring \"abc\" 2 1)", ""));
	CHECK(eval_eq("(substring \"abc\" 3 3)", ""));
	CHECK(eval_error_contains("(substring \"abc\")", "too few arguments"));

	kg_lisp_shutdown();
}

static void test_string_concat_and_equal(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(concat)", ""));
	CHECK(eval_eq("(concat \"a\")", "a"));
	CHECK(eval_eq("(concat \"a\" \"\" \"bc\" \"d\")", "abcd"));
	CHECK(eval_eq("(concat \"\xe6\xbc\xa2\" \"\xe5\xad\x97\")",
	    "\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_eq("(string-length (concat \"h\xc3\xa9\" \"llo\"))", "5"));
	CHECK(eval_error_contains("(concat \"a\" 1)", "Wrong type argument"));

	/* THE LIST-OF-CHARACTERS ARM, which exists because `s-reverse' is
	 * `(concat (nreverse (string-to-list s)))' and this raised
	 * `(wrong-type-argument stringp (99 98 97))' at it.  Each element
	 * is a character CODE and is encoded as UTF-8, so the round trip
	 * through `string-to-list' is a CHARACTER reversal and not a byte
	 * one -- the second and third of these are the whole want. */
	CHECK(eval_eq("(concat (list 97 98))", "ab"));
	CHECK(eval_eq("(concat (nreverse (string-to-list \"abc\")))", "cba"));
	CHECK(eval_eq("(concat (nreverse (string-to-list \"h\xc3\xa9llo\")))",
	    "oll\xc3\xa9h"));
	/* An empty list is the empty string, and the arms MIX. */
	CHECK(eval_eq("(concat nil)", ""));
	CHECK(eval_eq("(concat \"a\" (list 98) \"c\")", "abc"));
	/* A codepoint above ASCII round-trips through both halves. */
	CHECK(eval_eq("(string-to-list (concat (list 233)))", "(233)"));
	/* A NON-INTEGER element is `characterp', Emacs' own predicate for
	 * the element -- not `stringp', which is what the ARGUMENT gets
	 * when it is neither a string nor a list. */
	CHECK(eval_eq("(condition-case e (concat (list 97 \"b\"))"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument characterp \"b\")"));
	CHECK(eval_eq("(condition-case e (concat (list 97 -1))"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument characterp -1)"));
	CHECK(eval_eq("(condition-case e (concat 1)"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument stringp 1)"));

	/* `multibyte-string-p' answers nil for every string, ALWAYS -- the
	 * chosen answer, since `t' would send s-reverse into a branch that
	 * `(require 'ucs-normalize)'.  It still refuses a non-string: a
	 * predicate answering nil for 5 would be answering a different
	 * question. */
	CHECK(eval_eq("(list (multibyte-string-p \"abc\")"
		      " (multibyte-string-p \"\") (multibyte-string-p"
		      " \"h\xc3\xa9llo\"))",
	    "(nil nil nil)"));
	CHECK(eval_eq("(condition-case e (multibyte-string-p 5)"
		      " (error (cons (car e) (cdr e))))",
	    "(wrong-type-argument stringp 5)"));

	CHECK(eval_eq("(string= \"\" \"\")", "t"));
	CHECK(eval_eq("(string= \"abc\" \"abc\")", "t"));
	CHECK(eval_eq("(string= \"abc\" \"abd\")", "nil"));
	CHECK(eval_eq("(string= \"abc\" \"ab\")", "nil"));
	CHECK(eval_eq("(string= \"h\xc3\xa9llo\" \"h\xc3\xa9llo\")", "t"));
	CHECK(eval_eq("(string= \"h\xc3\xa9llo\" \"hello\")", "nil"));
	/* Recovery after a type error leaves the interpreter usable. */
	CHECK(eval_error_contains("(string= \"a\" 1)", "Wrong type argument"));
	CHECK(eval_eq("(concat \"ok\")", "ok"));

	kg_lisp_shutdown();
}

/* Every expectation here was measured against Emacs 31, except the two
 * noted places where kg has no bignums and no Emacs behaviour to copy. */
static void test_format_natives(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(format \"\")", ""));
	CHECK(eval_eq("(format \"plain\")", "plain"));
	CHECK(eval_eq("(format \"100%%\")", "100%"));

	/* %s and %S are fe's printer with the quoting flag flipped. */
	CHECK(eval_eq("(format \"%s\" 42)", "42"));
	CHECK(eval_eq("(format \"%s\" 42.5)", "42.5"));
	CHECK(eval_eq("(format \"%s\" \"hi\")", "hi"));
	CHECK(eval_eq("(format \"%S\" \"hi\")", "\"hi\""));
	CHECK(eval_eq("(format \"%s\" 'foo)", "foo"));
	CHECK(eval_eq("(format \"%s\" nil)", "nil"));
	CHECK(eval_eq("(format \"%s\" (list 1 2))", "(1 2)"));
	CHECK(eval_eq("(format \"%S\" (list 1 \"a\"))", "(1 \"a\")"));
	CHECK(eval_eq("(format \"%s=%S\" 'a \"b\")", "a=\"b\""));
	CHECK(eval_eq("(format \"h\xc3\xa9llo %s\" \"w\xc3\xb6rld\")",
	    "h\xc3\xa9llo w\xc3\xb6rld"));

	/* %d truncates toward zero. */
	CHECK(eval_eq("(format \"%d\" 42)", "42"));
	CHECK(eval_eq("(format \"%d\" 42.9)", "42"));
	CHECK(eval_eq("(format \"%d\" -3.7)", "-3"));
	CHECK(eval_eq("(format \"%d\" -0.5)", "0"));
	/* An integer argument prints its own value exactly -- the int64 path
	 * bypasses the double, so 2^53 + 1 is no longer rounded on the way
	 * in.  The double fast path's guard keeps the cast defined; the wide
	 * float values are exact, as they are in Emacs. */
	CHECK(eval_eq("(format \"%d\" 9007199254740993)", "9007199254740993"));
	CHECK(eval_eq("(format \"%d\" 1e19)", "10000000000000000000"));
	CHECK(eval_eq("(format \"%d\" -1e19)", "-10000000000000000000"));
	CHECK(eval_eq("(string-length (format \"%d\" 1e300))", "301"));
	CHECK(eval_eq("(substring (format \"%d\" 1e300) 0 4)", "1000"));
	/* kg has no bignums to print NaN or an infinity into, so unlike
	 * Emacs, which writes "nan" and "inf", %d refuses them.  The float
	 * is spelled (/ 1.0 0): (/ 1 0) is integer division by zero and an
	 * arith-error, so it never reaches format at all. */
	CHECK(eval_error_contains("(format \"%d\" (/ 1.0 0))", "finite"));
	CHECK(eval_error_contains(
	    "(format \"%d\" (- (/ 1.0 0) (/ 1.0 0)))", "finite"));
	/* %s and %S are fe's printer, so the exceptional floats now wear
	 * fe's readable spelling, the same "1.0e+INF"/"-0.0e+NaN" family
	 * Emacs writes; the C-style "inf" is what %d and the float
	 * conversions below produce. */
	CHECK(eval_eq("(format \"%s\" (/ 1.0 0))", "1.0e+INF"));
	CHECK(eval_eq("(format \"%S\" (/ 1.0 0))", "1.0e+INF"));
	CHECK(eval_eq("(format \"%s\" (- 0 (/ 1.0 0)))", "-1.0e+INF"));

	/* %e, %f and %g are C's conversions, which is exactly what Emacs
	 * uses; every one of these was checked against Emacs 31.  An integer
	 * argument widens through FeToDouble, so (format "%e" 42) formats
	 * the way it does in Emacs. */
	CHECK(eval_eq("(format \"%e\" 1.5)", "1.500000e+00"));
	CHECK(eval_eq("(format \"%e\" 42)", "4.200000e+01"));
	CHECK(eval_eq("(format \"%e\" 1e300)", "1.000000e+300"));
	CHECK(eval_eq("(format \"%f\" 1.5)", "1.500000"));
	CHECK(eval_eq("(format \"%f\" -2.25)", "-2.250000"));
	CHECK(eval_eq("(format \"%f\" 42)", "42.000000"));
	CHECK(eval_eq("(format \"%g\" 1.5)", "1.5"));
	CHECK(eval_eq("(format \"%g\" 0)", "0"));
	CHECK(eval_eq("(format \"%g\" 1e300)", "1e+300"));
	CHECK(eval_eq("(format \"%-5d|\" 3)", "3    |"));
	CHECK(eval_eq("(format \"%05.2f\" 1.5)", "01.50"));
	CHECK(eval_eq("(format \"%05d\" -3)", "-0003"));
	CHECK(eval_eq("(format \"%05.2d\" -3)", "  -03"));
	CHECK(eval_eq("(format \"%c\" 65)", "A"));
	CHECK(eval_eq("(format \"%c\" 233)", "\xc3\xa9"));
	CHECK(eval_eq("(format \"%c\" 128169)", "\xf0\x9f\x92\xa9"));
	CHECK(eval_eq("(format \"%x %X %o\" 255 255 15)", "ff FF 17"));
	CHECK(eval_eq("(format \"%x %X %o\" -15 -15 -15)", "-f -F -17"));
	CHECK(eval_eq("(format \"%-5s|\" \"x\")", "x    |"));
	CHECK(eval_eq("(format \"%.3s\" \"hello\")", "hel"));
	CHECK(eval_eq("(format \"%3.1s\" \"\xc3\xa9x\")", "  \xc3\xa9"));
	CHECK(eval_eq("(format \"%-3.1s\" \"\xc3\xa9x\")", "\xc3\xa9  "));
	CHECK(eval_error_contains(
	    "(format \"%999999999999d\" 1)", "field is too large"));
	/* Unlike %d, the float conversions have a rendering for the
	 * exceptional values, so they print them instead of raising. */
	CHECK(eval_eq("(format \"%e\" (/ 1.0 0))", "inf"));
	CHECK(eval_eq("(format \"%f\" (- 0 (/ 1.0 0)))", "-inf"));
	CHECK(eval_eq("(format \"%g\" (/ 1.0 0))", "inf"));
	CHECK(eval_error_contains("(format \"%f\" \"x\")", "argument type"));
	/* %f of DBL_MAX is the longest thing the buffer must hold. */
	CHECK(eval_eq("(string-length (format \"%f\" 1e300))", "308"));

	/* Extra arguments are ignored; missing ones are an error. */
	CHECK(eval_eq("(format \"%s\" 1 2)", "1"));
	CHECK(eval_error_contains("(format \"%s %s\" 1)", "not enough"));
	CHECK(eval_error_contains("(format \"%s\")", "not enough"));
	CHECK(eval_error_contains("(format \"%d\" \"x\")", "argument type"));
	CHECK(eval_error_contains(
	    "(format \"%q\" 1)", "invalid format operation %q"));
	CHECK(eval_error_contains("(format \"50%\")", "middle of format"));
	CHECK(eval_error_contains("(format 1)", "Wrong type argument"));
	/* The spellings Emacs accepts and kg refuses, recorded rather than
	 * quietly misread (manifest row phase8-format-strictness): Emacs'
	 * remaining flags and its N$ field numbers.  `%c' of 0 left this
	 * list in Phase 25 -- it writes a NUL byte here as it does in
	 * Emacs, and test_phase25_strings asserts the whole route. */
	CHECK(eval_error_contains(
	    "(format \"%+d\" 1)", "invalid format operation"));
	CHECK(eval_error_contains(
	    "(format \"% d\" 1)", "invalid format operation"));
	CHECK(eval_error_contains(
	    "(format \"%#x\" 255)", "invalid format operation"));
	CHECK(eval_error_contains(
	    "(format \"%1$s\" 1)", "invalid format operation"));
	/* Recovery after any of those leaves the interpreter usable. */
	CHECK(eval_eq("(format \"%s\" 'ok)", "ok"));

	/* A float rendering is bounded by nothing but the precision the
	 * caller wrote, and the conversion buffer is 512 bytes: kg used to
	 * ignore snprintf's return and answer the first 511 characters of
	 * (format "%.600f" 1.0), where Emacs 31.0.90 answers all 602.  The
	 * rows below straddle the buffer -- just under, just over, and over
	 * with a width and with left alignment, since those take different
	 * paths through the padding. */
	CHECK(eval_eq("(length (format \"%.500f\" 1.0))", "502"));
	CHECK(eval_eq("(length (format \"%.509f\" 1.0))", "511"));
	CHECK(eval_eq("(length (format \"%.510f\" 1.0))", "512"));
	CHECK(eval_eq("(length (format \"%.600f\" 1.0))", "602"));
	CHECK(eval_eq("(length (format \"%.600e\" 1.0))", "606"));
	CHECK(eval_eq("(length (format \"%700.600f\" 1.0))", "700"));
	CHECK(eval_eq("(length (format \"%-620.600f|\" 1.0))", "621"));
	CHECK(eval_eq("(substring (format \"%700.600f\" 1.0) 97 100)", " 1."));
	CHECK(eval_eq(
	    "(substring (format \"%-620.600f|\" 1.0) 617 621)", "   |"));
	/* A long rendering is still exact at its head. */
	CHECK(eval_eq("(substring (format \"%.600f\" 1.0) 0 6)", "1.0000"));

	/* message is format plus the status line. */
	CHECK(eval_ok("(message \"%s of %d\" \"one\" 3)"));
	CHECK(strcmp(test_status_message, "one of 3") == 0);
	CHECK(eval_ok("(message \"50%%\")"));
	CHECK(strcmp(test_status_message, "50%") == 0);
	CHECK(eval_error_contains("(message \"50%\")", "middle of format"));
	CHECK(eval_ok("(message \"a\" \"b\")"));
	CHECK(strcmp(test_status_message, "a") == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_char_string_round_trip(void)
{
	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(char-to-string 97)", "a"));
	CHECK(eval_eq("(char-to-string 10)", "\n"));
	CHECK(eval_eq("(char-to-string 233)", "\xc3\xa9"));
	CHECK(eval_eq("(char-to-string 28450)", "\xe6\xbc\xa2"));
	CHECK(eval_eq("(char-to-string 128169)", "\xf0\x9f\x92\xa9"));
	/* Emacs' own answer for a code point outside Unicode is
	 * (wrong-type-argument characterp 1114112), measured.  0 was kg's
	 * own rejection beside it until Phase 25; it is a one-byte string
	 * now, asserted in test_phase25_strings with the other three
	 * routes to one. */
	CHECK(eval_error_contains(
	    "(char-to-string 1114112)", "Wrong type argument"));
	CHECK(eval_eq("(condition-case e (char-to-string -1) (error e))",
	    "(wrong-type-argument characterp -1)"));
	CHECK(eval_error_contains("(char-to-string 55296)", "surrogate"));

	CHECK(eval_eq("(string-to-char \"abc\")", "97"));
	CHECK(eval_eq("(string-to-char \"\xc3\xa9x\")", "233"));
	CHECK(eval_eq("(string-to-char \"\")", "nil"));

	CHECK(eval_eq("(string-to-char (char-to-string 28450))", "28450"));
	CHECK(eval_eq("(string-length (char-to-string 28450))", "1"));
	/* char-after returns a number; char-to-string turns it back into text.
	 */
	CHECK(eval_eq("(char-to-string (char-after 6))", "\xc3\xa9"));
	CHECK(eval_eq("(char-to-string (char-after 11))", "\xe6\xbc\xa2"));
	CHECK(eval_eq("(string-to-char (buffer-substring 11 13))", "28450"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* "one  two" / "" / "héllo wörld" / "漢字" in 1-based codepoint positions:
 * o=1 n=2 e=3 ' '=4 ' '=5 t=6 w=7 o=8 \n=9, the empty row is 10, then
 * h=11 é=12 l=13 l=14 o=15 ' '=16 w=17 ö=18 r=19 l=20 d=21 \n=22, and
 * 漢=23 字=24 with point-max at 25. */
static void setup_thing_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one  two", 8);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "h\xc3\xa9llo w\xc3\xb6rld", 13);
	editor_insert_row(bcur(), 3, "\xe6\xbc\xa2\xe5\xad\x97", 6);
}

static void test_thing_at_point(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* An empty buffer has no thing at point, but a bad THING still
	 * raises rather than passing for "nothing here". */
	CHECK(eval_eq("(bounds-of-thing-at-point 'word)", "nil"));
	CHECK(eval_error_contains(
	    "(bounds-of-thing-at-point 'sexp)", "unsupported thing"));
	CHECK(eval_error_contains("(bounds-of-thing-at-point 'sexp)", "sexp"));
	CHECK(eval_error_contains(
	    "(bounds-of-thing-at-point \"word\")", "needs a symbol"));
	CHECK(eval_error_contains(
	    "(bounds-of-thing-at-point)", "too few arguments"));
	kg_lisp_shutdown();
	teardown_editor();

	setup_thing_buffer();
	CHECK(kg_lisp_init() == 0);

	/* Start, middle and end of a word all name the same word. */
	CHECK(eval_eq("(progn (goto-char 1) (bounds-of-thing-at-point 'word))",
	    "(1 . 4)"));
	CHECK(eval_eq("(progn (goto-char 2) (bounds-of-thing-at-point 'word))",
	    "(1 . 4)"));
	CHECK(eval_eq("(progn (goto-char 4) (bounds-of-thing-at-point 'word))",
	    "(1 . 4)"));
	CHECK(eval_eq("(progn (goto-char 6) (bounds-of-thing-at-point 'word))",
	    "(6 . 9)"));
	CHECK(eval_eq("(progn (goto-char 9) (bounds-of-thing-at-point 'word))",
	    "(6 . 9)"));
	/* Between two words, and on an empty line: no word. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (bounds-of-thing-at-point 'word))", "nil"));
	CHECK(eval_eq(
	    "(progn (goto-char 10) (bounds-of-thing-at-point 'word))", "nil"));

	/* Codepoints at or above U+0080 are word constituents here, unlike
	 * kg's ASCII-only interactive word motion. */
	CHECK(eval_eq("(progn (goto-char 11) (bounds-of-thing-at-point 'word))",
	    "(11 . 16)"));
	CHECK(eval_eq("(progn (goto-char 12) (bounds-of-thing-at-point 'word))",
	    "(11 . 16)"));
	CHECK(eval_eq("(progn (goto-char 18) (bounds-of-thing-at-point 'word))",
	    "(17 . 22)"));
	CHECK(eval_eq("(progn (goto-char 23) (bounds-of-thing-at-point 'word))",
	    "(23 . 25)"));
	CHECK(eval_eq("(progn (goto-char 24) (bounds-of-thing-at-point 'word))",
	    "(23 . 25)"));

	/* 'line takes in its line break, as in Emacs, so END is the start of
	 * the next row.  emacs -Q --batch on the same text returns (1 . 10),
	 * (10 . 11), (11 . 23) and (23 . 25). */
	CHECK(eval_eq("(progn (goto-char 5) (bounds-of-thing-at-point 'line))",
	    "(1 . 10)"));
	CHECK(eval_eq("(progn (goto-char 10) (bounds-of-thing-at-point 'line))",
	    "(10 . 11)"));
	CHECK(eval_eq("(progn (goto-char 15) (bounds-of-thing-at-point 'line))",
	    "(11 . 23)"));
	/* The last row has no next row, so its bounds stop at point-max. */
	CHECK(eval_eq("(progn (goto-char 23) (bounds-of-thing-at-point 'line))",
	    "(23 . 25)"));
	CHECK(eval_eq("(progn (goto-char 25) (bounds-of-thing-at-point 'line))",
	    "(23 . 25)"));

	/* car/cdr work on the result, as with any Emacs bounds pair. */
	CHECK(eval_eq(
	    "(car (progn (goto-char 12) (bounds-of-thing-at-point 'word)))",
	    "11"));
	CHECK(eval_eq(
	    "(cdr (progn (goto-char 12) (bounds-of-thing-at-point 'word)))",
	    "16"));

	/* The prelude's thing-at-point is the text of those bounds. */
	CHECK(eval_eq(
	    "(progn (goto-char 12) (thing-at-point 'word))", "h\xc3\xa9llo"));
	CHECK(eval_eq("(progn (goto-char 23) (thing-at-point 'word))",
	    "\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_eq("(progn (goto-char 5) (thing-at-point 'word))", "nil"));
	/* The line break is part of the text, so it is part of the string. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (thing-at-point 'line))", "one  two\n"));
	CHECK(eval_eq("(progn (goto-char 10) (thing-at-point 'line))", "\n"));
	CHECK(eval_eq("(progn (goto-char 23) (thing-at-point 'line))",
	    "\xe6\xbc\xa2\xe5\xad\x97"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The prelude's move-beginning-of-line / move-end-of-line: Emacs' line
 * motion pair as plain functions over (bounds-of-thing-at-point 'line),
 * whose cdr takes the line break in -- see test_thing_at_point for the
 * bounds themselves on this same buffer. */
static void test_line_motion(void)
{
	/* Empty buffer: both are quiet no-ops answering nil. */
	setup_editor();
	CHECK(kg_lisp_init() == 0);
	CHECK(eval_eq("(move-beginning-of-line)", "nil"));
	CHECK(eval_eq("(move-end-of-line)", "nil"));
	CHECK(eval_eq("(point)", "1"));
	kg_lisp_shutdown();
	teardown_editor();

	setup_thing_buffer();
	CHECK(kg_lisp_init() == 0);

	/* End of line is before the line's newline; the last line has no
	 * newline and ends at point-max.  An empty middle line's end is its
	 * own start, not the previous line's end. */
	CHECK(eval_eq("(progn (goto-char 5) (move-end-of-line) (point))", "9"));
	CHECK(
	    eval_eq("(progn (goto-char 10) (move-end-of-line) (point))", "10"));
	CHECK(
	    eval_eq("(progn (goto-char 15) (move-end-of-line) (point))", "22"));
	CHECK(
	    eval_eq("(progn (goto-char 24) (move-end-of-line) (point))", "25"));

	/* Beginning of line, from anywhere in the line. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (move-beginning-of-line) (point))", "1"));
	CHECK(eval_eq(
	    "(progn (goto-char 10) (move-beginning-of-line) (point))", "10"));
	CHECK(eval_eq(
	    "(progn (goto-char 22) (move-beginning-of-line) (point))", "11"));
	CHECK(eval_eq(
	    "(progn (goto-char 25) (move-beginning-of-line) (point))", "23"));

	/* N not nil or 1 first moves forward N - 1 lines; 0 names the
	 * previous line; nil and 1 are the current line; the buffer edges
	 * clamp, as goto-line's do. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (move-beginning-of-line 2) (point))", "10"));
	CHECK(eval_eq(
	    "(progn (goto-char 5) (move-end-of-line 2) (point))", "10"));
	CHECK(eval_eq(
	    "(progn (goto-char 10) (move-beginning-of-line 0) (point))", "1"));
	CHECK(eval_eq(
	    "(progn (goto-char 1) (move-end-of-line 4) (point))", "25"));
	CHECK(
	    eval_eq("(progn (goto-char 5) (move-end-of-line 1) (point))", "9"));
	CHECK(eval_eq(
	    "(progn (goto-char 5) (move-beginning-of-line 99) (point))", "23"));
	CHECK(eval_eq(
	    "(progn (goto-char 15) (move-beginning-of-line -99) (point))",
	    "1"));

	/* Both answer nil, as in Emacs. */
	CHECK(eval_eq("(move-end-of-line)", "nil"));

	kg_lisp_shutdown();
	teardown_editor();

	/* A trailing empty last line: its end is itself, not the previous
	 * line's newline -- the one case move-end-of-line's (< car cdr)
	 * guard exists for, since there car = cdr = point-max and the
	 * character before it IS a newline. */
	setup_editor();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "", 0);
	CHECK(kg_lisp_init() == 0);
	CHECK(eval_eq("(progn (goto-char 3) (move-end-of-line) (point))", "3"));
	CHECK(eval_eq("(progn (goto-char 1) (move-end-of-line) (point))", "2"));
	CHECK(eval_eq(
	    "(progn (goto-char 3) (move-beginning-of-line) (point))", "3"));
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_prelude_forms(void)
{
	CHECK(kg_lisp_init() == 0);

	/* cond, including a t fallthrough and an exhausted cond. */
	CHECK(eval_eq("(cond (t 1))", "1"));
	CHECK(eval_eq("(cond (nil 1) (nil 2) (t 3))", "3"));
	CHECK(
	    eval_eq("(cond ((is 1 2) \"a\") ((is 1 1) \"b\") (t \"c\"))", "b"));
	CHECK(eval_eq("(cond (nil 1))", "nil"));
	CHECK(eval_eq("(cond)", "nil"));
	/* An exhausted cond is really nil, not a nil-shaped truthy object. */
	CHECK(eval_eq("(if (cond (nil 1)) \"yes\" \"no\")", "no"));
	CHECK(eval_eq("(not (cond (nil 1)))", "t"));
	/* Clause bodies are implicit progns and only the taken one runs. */
	CHECK(eval_eq("(progn (setq seen 0) (cond (t (setq seen 1) 2) (t (setq "
		      "seen 9))) seen)",
	    "1"));

	CHECK(eval_eq("(when t 1 2)", "2"));
	CHECK(eval_eq("(when nil 1)", "nil"));
	CHECK(eval_eq("(progn (setq n 0) (when t (setq n 5)) n)", "5"));
	CHECK(eval_eq("(progn (setq n 0) (when nil (setq n 5)) n)", "0"));
	CHECK(eval_eq("(unless nil 1 2)", "2"));
	CHECK(eval_eq("(unless t 1)", "nil"));
	CHECK(eval_eq("(progn (setq n 0) (unless nil (setq n 7)) n)", "7"));
	CHECK(eval_eq("(progn (setq n 0) (unless t (setq n 7)) n)", "0"));

	/* dolist accumulates and leaves the loop variable local. */
	CHECK(eval_eq(
	    "(progn (setq acc \"\") (dolist (x (list \"a\" \"b\" \"c\"))"
	    " (setq acc (concat acc x))) acc)",
	    "abc"));
	CHECK(eval_eq(
	    "(progn (setq n 0) (dolist (x (list 1 2 3)) (setq n (+ n x))) n)",
	    "6"));
	CHECK(eval_eq("(progn (setq n 0) (dolist (x nil) (setq n 1)) n)", "0"));
	/* Nested loops do not capture each other's variable. */
	CHECK(eval_eq("(progn (setq acc \"\") (dolist (a (list \"1\" \"2\"))"
		      " (dolist (b (list \"x\" \"y\"))"
		      "  (setq acc (concat acc a b)))) acc)",
	    "1x1y2x2y"));

	CHECK(eval_eq("(string-empty-p \"\")", "t"));
	CHECK(eval_eq("(string-empty-p \"a\")", "nil"));
	CHECK(eval_eq("(string-empty-p (substring \"abc\" 1 1))", "t"));

	kg_lisp_shutdown();
}

/* Fe's `if` used to be an elif chain, so a four-armed `if` silently ran
 * the third form as a condition and never reached the fourth. */
static void test_elisp_if(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(if t 1 2)", "1"));
	CHECK(eval_eq("(if nil 1 2)", "2"));
	CHECK(eval_eq("(if nil 1)", "nil"));
	CHECK(eval_eq("(if t 1)", "1"));
	/* The ELSE forms are an implicit progn: every one of them runs and the
	 * last one is the value. */
	CHECK(eval_eq(
	    "(progn (setq probe 0) (if nil 1 (setq probe 2) (setq probe 3))"
	    " probe)",
	    "3"));
	CHECK(eval_eq("(if nil 1 2 3)", "3"));
	/* A false `if` with no ELSE is really nil, not a nil-shaped object. */
	CHECK(eval_eq("(not (if nil 1))", "t"));
	CHECK(eval_eq("(is (if nil 1) nil)", "t"));
	/* Only the taken branch runs. */
	CHECK(eval_eq(
	    "(progn (setq probe 0) (if t 1 (setq probe 9)) probe)", "0"));
	CHECK(eval_eq(
	    "(progn (setq probe 0) (if nil (setq probe 9) 1) probe)", "0"));
	/* A `let` in the ELSE forms binds, because they are a do-list. */
	CHECK(eval_eq("(if nil 1 (internal--let q 5) q)", "5"));

	kg_lisp_shutdown();
}

static void test_list_library(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(reverse (list 1 2 3))", "(3 2 1)"));
	CHECK(eval_eq("(reverse nil)", "nil"));
	CHECK(eval_eq("(length (list 1 2 3))", "3"));
	CHECK(eval_eq("(length nil)", "0"));
	/* length is polymorphic over strings, as in Emacs. */
	CHECK(eval_eq("(length \"h\xc3\xa9llo\")", "5"));
	CHECK(eval_eq("(nth 0 (list 'a 'b))", "a"));
	CHECK(eval_eq("(nth 1 (list 'a 'b))", "b"));
	CHECK(eval_eq("(nth 9 (list 'a))", "nil"));
	CHECK(eval_eq("(nthcdr 2 (list 1 2 3 4))", "(3 4)"));
	CHECK(eval_eq("(nthcdr 9 (list 1))", "nil"));
	CHECK(eval_eq("(last (list 1 2 3))", "(3)"));
	CHECK(eval_eq("(last nil)", "nil"));
	CHECK(eval_eq("(append (list 1 2) (list 3) (list 4))", "(1 2 3 4)"));
	CHECK(eval_eq("(append)", "nil"));
	CHECK(eval_eq("(append nil (list 1))", "(1)"));
	CHECK(eval_eq("(append (list 1) nil)", "(1)"));
	CHECK(eval_eq("(mapcar (lambda (x) (* x 2)) (list 1 2 3))", "(2 4 6)"));
	CHECK(eval_eq("(mapcar (lambda (x) x) nil)", "nil"));
	CHECK(eval_eq("(member 3 (list 1 2 3 4))", "(3 4)"));
	CHECK(eval_eq("(member 9 (list 1))", "nil"));
	CHECK(eval_eq("(memq 'c (list 'a 'b 'c))", "(c)"));
	CHECK(eval_eq("(assoc 'b (list (cons 'a 1) (cons 'b 2)))", "(b . 2)"));
	CHECK(eval_eq("(assoc 'z (list (cons 'a 1)))", "nil"));
	CHECK(eval_eq("(assoc 'a nil)", "nil"));
	/* equal is structural on lists, where Fe's `is` compares identity. */
	CHECK(eval_eq("(equal (list 1 (list 2)) (list 1 (list 2)))", "t"));
	CHECK(eval_eq("(equal (list 1) (list 1 2))", "nil"));
	CHECK(eval_eq("(equal (list 1) 1)", "nil"));
	CHECK(eval_eq("(equal \"ab\" \"ab\")", "t"));
	CHECK(eval_eq("(equal nil nil)", "t"));
	/* 05E replaced the fe-native `is` tail with an eql leaf rule, so a
	 * number leaf agrees only when it is the same type *and* the same
	 * value: (equal 1 1.0) is nil, where `is` said t.  Emacs 31.0.90
	 * answers the same, and test/lisp-compat's equal-number-leaves pins
	 * it against a snapshot of that. */
	CHECK(eval_eq("(equal 1 1.0)", "nil"));
	CHECK(eval_eq("(equal 1.0 1.0)", "t"));
	CHECK(eval_eq("(equal 1 1)", "t"));
	CHECK(eval_eq("(equal (list 1) (list 1.0))", "nil"));
	CHECK(eval_eq("(eq 'a 'a)", "t"));
	CHECK(eval_eq("(null nil)", "t"));
	CHECK(eval_eq("(1+ 1)", "2"));
	CHECK(eval_eq("(1- 1)", "0"));
	CHECK(eval_eq("(cadr (list 1 2 3))", "2"));
	CHECK(eval_eq("(cddr (list 1 2 3))", "(3)"));
	CHECK(eval_eq("(caar (list (list 1)))", "1"));

	/* The list functions are iterative: a long list must not blow the GC
	 * stack the way a recursive implementation would. */
	CHECK(eval_eq("(progn (setq big nil) (setq i 0)"
		      " (while (< i 500) (setq big (cons i big))"
		      "  (setq i (+ i 1)))"
		      " (length (append (reverse big)"
		      "  (mapcar (lambda (x) x) big))))",
	    "1000"));

	kg_lisp_shutdown();
}

static void test_type_predicates(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(type-of \"a\")", "string"));
	CHECK(eval_eq("(type-of 'a)", "symbol"));
	CHECK(eval_eq("(type-of 1)", "integer"));
	CHECK(eval_eq("(type-of 1.0)", "double"));
	CHECK(eval_eq("(type-of (cons 1 2))", "pair"));
	CHECK(eval_eq("(type-of nil)", "nil"));
	/* The callables live in function cells now, so the type probe reads
	 * the cell through a designator, never a bare value read. */
	CHECK(eval_eq("(type-of (symbol-function 'car))", "primitive"));
	CHECK(eval_eq("(type-of (symbol-function 'insert))", "native-fn"));
	CHECK(eval_eq("(type-of (lambda (x) x))", "lambda"));
	CHECK(eval_eq("(type-of (symbol-function 'cond))", "macro"));

	CHECK(eval_eq("(stringp \"a\")", "t"));
	CHECK(eval_eq("(stringp 'a)", "nil"));
	CHECK(eval_eq("(numberp 1)", "t"));
	CHECK(eval_eq("(numberp 1.5)", "t"));
	CHECK(eval_eq("(numberp \"1\")", "nil"));
	CHECK(eval_eq("(consp (cons 1 2))", "t"));
	CHECK(eval_eq("(consp nil)", "nil"));
	/* nil and t are symbols in Emacs, and here too. */
	CHECK(eval_eq("(symbolp 'a)", "t"));
	CHECK(eval_eq("(symbolp nil)", "t"));
	CHECK(eval_eq("(symbolp \"a\")", "nil"));
	CHECK(eval_eq("(functionp (lambda (x) x))", "t"));
	/* A symbol is a function designator: it resolves through its function
	 * cell, so (functionp 'car) is t, a macro is not a function, and a
	 * name with an empty cell is not one either. */
	CHECK(eval_eq("(functionp 'car)", "t"));
	CHECK(eval_eq("(functionp 'insert)", "t"));
	/* A native registers in the function namespace only: the value cell is
	 * never touched, so (boundp 'insert) is nil while (fboundp 'insert) is
	 * t -- the Lisp-2 split observable on a real kg native. */
	CHECK(eval_eq("(boundp 'insert)", "nil"));
	CHECK(eval_eq("(fboundp 'insert)", "t"));
	CHECK(eval_eq("(functionp 'cond)", "nil"));
	CHECK(eval_eq("(functionp 'no-such-function)", "nil"));
	CHECK(eval_eq("(functionp 1)", "nil"));
	/* Emacs' answer for a special form is nil -- `if' is not a function,
	 * however callable its name looks -- and the same for a macro
	 * whichever namespace it came from.  kg asks Fe's FeIsFunction,
	 * which is the classification funcall and apply reject an
	 * `invalid-function' operand by, so the predicate and the
	 * interpreter agree; the hand-rolled type check this replaced said t
	 * for every primitive, `if' included. */
	CHECK(eval_eq("(functionp 'if)", "nil"));
	CHECK(eval_eq("(functionp 'quote)", "nil"));
	CHECK(eval_eq("(functionp 'while)", "nil"));
	CHECK(eval_eq("(functionp 'when)", "nil"));
	CHECK(eval_eq("(functionp 'let)", "nil"));
	/* A cyclic alias chain is answered, not raised at: functionp
	 * resolves the designator with fe's host-facing FeGetFunction, which
	 * yields nil for a cycle, and nil is not a function.  Emacs answers
	 * nil here too -- by a different route, since its own fset refuses
	 * to build the cycle in the first place. */
	CHECK(eval_ok("(fset 'cyc 'cyc)"));
	CHECK(eval_eq("(functionp 'cyc)", "nil"));
	/* Contained, not merely quiet: the interpreter is still usable, and
	 * calling the name is still an error rather than a crash. */
	CHECK(eval_eq("(+ 1 2)", "3"));
	CHECK(eval_error_contains("(funcall 'cyc)",
	    "Symbol's chain of function indirections contains a loop"));
	/* fboundp reads the raw cell and follows nothing, so it answers for
	 * the same name without raising -- the "never errors" doc/lisp-api.md
	 * claims for it. */
	CHECK(eval_eq("(fboundp 'cyc)", "t"));
	CHECK(eval_eq("(listp nil)", "t"));
	CHECK(eval_eq("(listp (cons 1 2))", "t"));
	CHECK(eval_eq("(listp 1)", "nil"));
	CHECK(eval_error_contains("(stringp)", "too few arguments"));
	CHECK(eval_error_contains("(type-of 1 2)", "too many arguments"));

	kg_lisp_shutdown();
}

/* The fe-side numeric corrections kg's Phase 5 review put on the pin,
 * asserted through kg's own Lisp because kg is where some of them are
 * reachable at all: kg links core fe *without* Fex, so the `floor`,
 * `ceiling`, `round` and `truncate` running here are fe's own, where the
 * standalone fe interpreter's are Fex's shadowing definitions.  A NaN
 * operand used to reach an out-of-range double-to-int64 conversion on
 * exactly this path -- undefined behaviour observable only from a host
 * like kg -- so these cases are the evidence for that fix, not a
 * duplicate of fe's suite.  Every comparator expectation below was run
 * against Emacs 31.0.90, which answers the same. */
static void test_numeric_core_error_rules(void)
{
	CHECK(kg_lisp_init() == 0);

	/* Rounding: a NaN operand, and a zero divisor in the two-argument
	 * form, are arith-error rather than a UB conversion or a silent
	 * value.  The NaN is spelled (/ 0.0 0) because (/ 0 0) is integer
	 * division and raises before the rounding native ever runs. */
	CHECK(eval_error_contains("(floor (/ 0.0 0))", "Arithmetic error"));
	CHECK(eval_error_contains("(ceiling (/ 0.0 0))", "Arithmetic error"));
	CHECK(eval_error_contains("(round (/ 0.0 0))", "Arithmetic error"));
	CHECK(eval_error_contains("(truncate (/ 0.0 0))", "Arithmetic error"));
	CHECK(eval_error_contains("(floor 0 0)", "Arithmetic error"));
	CHECK(eval_error_contains("(round 0 0)", "Arithmetic error"));
	/* Contained, not merely quiet: the ordinary answers are unchanged
	 * and the interpreter is still usable afterwards. */
	CHECK(eval_eq("(floor 3.7)", "3"));
	CHECK(eval_eq("(floor 7 2)", "3"));
	CHECK(eval_eq("(ceiling 3.2)", "4"));
	CHECK(eval_eq("(round 3.5)", "4"));

	/* One operand is t with no type check at all -- Emacs' pair loop has
	 * no pair to run, so it never asks what the operand is. */
	CHECK(eval_eq("(= \"a\")", "t"));
	CHECK(eval_eq("(< t)", "t"));
	CHECK(eval_eq("(= 3)", "t"));
	/* A chain stops at the first false pair, so an operand past a
	 * settled answer is never type-checked; one that is still reachable
	 * is.  Both operand forms were evaluated before either arm ran, so
	 * short-circuiting erases no side effect. */
	CHECK(eval_eq("(< 2 1 \"a\")", "nil"));
	CHECK(eval_error_contains("(< 1 2 \"a\")", "Wrong type argument"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* `is` is fe's own broad comparator, not an Emacs form: it regained
	 * its epsilon tolerance across the two number tags, where `eql` --
	 * which is what `equal`'s leaves use -- stays type-honest. */
	CHECK(eval_eq("(is 1 1.0)", "t"));
	CHECK(eval_eq("(eql 1 1.0)", "nil"));

	/* The reader wants an explicit sign in a nonfinite exponent, so this
	 * is a symbol again rather than a number. */
	CHECK(eval_eq("(type-of '1eINF)", "symbol"));
	CHECK(eval_eq("(type-of '1.0e+INF)", "double"));

	kg_lisp_shutdown();
}

static void test_binding_forms(void)
{
	CHECK(kg_lisp_init() == 0);

	/* let is parallel: the value forms see the outer bindings. */
	CHECK(eval_eq("(progn (setq y 100) (let ((y 1) (z y)) z))", "100"));
	CHECK(eval_eq("(progn (setq y 100) (let* ((y 1) (z y)) z))", "1"));
	CHECK(eval_eq("(let ((a 1) (b 2)) (+ a b))", "3"));
	CHECK(eval_eq(
	    "(let* ((p 1) (q (+ p 1)) (r (+ q 1))) (list p q r))", "(1 2 3)"));
	/* Bare symbols and value-less bindings are nil. */
	CHECK(eval_eq("(let ((a 1) b (c)) (list a b c))", "(1 nil nil)"));
	CHECK(eval_eq("(let () 42)", "42"));
	CHECK(eval_eq("(let* () 42)", "42"));
	/* Nested lets shadow. */
	CHECK(eval_eq("(let ((a 1)) (let ((a 2) (b a)) (list a b)))", "(2 1)"));
	/* A while loop with setq inside a let body. */
	CHECK(eval_eq("(let ((acc 0) (k 0))"
		      " (while (< k 5) (setq acc (+ acc k)) (setq k (+ k 1)))"
		      " acc)",
	    "10"));
	/* setq inside a let reaches an outer variable. */
	CHECK(eval_eq(
	    "(progn (setq outer 0) (let ((q 1)) (setq outer 7)) outer)", "7"));
	/* Closures capture the let bindings.  The closure is a value, so the
	 * call goes through funcall: call position resolves function cells,
	 * and a bare (mk 5) would now be void-function. */
	CHECK(eval_eq("(progn (setq mk (let ((n 10)) (lambda (x) (+ x n))))"
		      " (funcall mk 5))",
	    "15"));

	/* setq takes any number of pairs and returns the last value. */
	CHECK(eval_eq("(progn (setq a 1 b 2 c 3) (list a b c))", "(1 2 3)"));
	CHECK(eval_eq("(setq a 5)", "5"));
	CHECK(eval_eq("(setq a 1 b 2)", "2"));
	CHECK(eval_eq("(setq)", "nil"));

	CHECK(eval_eq("(prog1 1 2 3)", "1"));
	CHECK(eval_eq(
	    "(progn (setq l (list 1 2 3)) (list (pop l) l))", "(1 (2 3))"));
	CHECK(eval_eq("(progn (setq l (list 2)) (push 1 l) l)", "(1 2)"));
	CHECK(eval_eq(
	    "(progn (setq n 0) (dotimes (i 4) (setq n (+ n i))) n)", "6"));
	CHECK(eval_eq("(dotimes (i 2) i)", "nil"));
	CHECK(eval_eq("(dotimes (i 2 'done) i)", "done"));
	CHECK(eval_eq("(dolist (x (list 1 2) 'done) x)", "done"));

	/* The phase's headline assertion: the value and function namespaces
	 * coexist under one name, as in Emacs -- defun writes the function
	 * cell and leaves the value cell alone. */
	CHECK(
	    eval_eq("(progn (setq f 7) (defun f () 9) (list f (f)))", "(7 9)"));

	kg_lisp_shutdown();
}

/* Phase 11's semantics grid, through kg's own entry points.
 *
 * test/lisp-compat/cases/phase11-dynamic-*.json compare the same probes
 * against the pinned Emacs 31.0.90 and are the authority on what the
 * answers should be; this runs them in-process, where a failure names a
 * line instead of a case, and adds the shapes the compat corpus cannot
 * reach -- the interaction with kg's own binding forms, and the
 * degenerate `let' shapes that have nothing to do with Emacs
 * disagreement and everything to do with not regressing kg.
 *
 * The grid probe each assertion is is named in the comment above it.
 * Six of the twenty-one probes ALREADY agreed before Phase 11 and are
 * guards rather than targets; they are marked as such, because an
 * implementation that binds specials dynamically at every
 * parameter-binding site passes every target here and fails A4. */
static void test_phase11_dynamic_binding(void)
{
	CHECK(kg_lisp_init() == 0);

	/* A9a: the predicate exists, and answers the *special* flag.  The
	 * three constants answer t, which is Emacs' answer for a name
	 * nothing can ever bind. */
	CHECK(eval_eq("(special-variable-p t)", "t"));
	CHECK(eval_eq("(special-variable-p nil)", "t"));
	CHECK(eval_eq("(special-variable-p :kw)", "t"));
	CHECK(eval_eq("(special-variable-p 'p11-never-declared)", "nil"));
	CHECK(eval_error_contains(
	    "(special-variable-p 3)", "expected symbol, got integer"));

	/* A7d / A8a: a two-argument defvar and a defconst mark full;
	 * A7a: a one-argument defvar marks let-dynamic only, so the
	 * predicate still answers nil for it. */
	CHECK(eval_eq("(progn (defvar p11-two 1) (special-variable-p"
		      " 'p11-two))",
	    "t"));
	CHECK(eval_eq("(progn (defconst p11-const 1) (special-variable-p"
		      " 'p11-const))",
	    "t"));
	CHECK(eval_eq("(progn (defvar p11-one) (special-variable-p"
		      " 'p11-one))",
	    "nil"));
	/* A10b (guard): a one-argument defvar binds nothing. */
	CHECK(eval_eq("(boundp 'p11-one)", "nil"));
	/* A7b: and yet `let' over it is dynamic -- WITHIN THE INPUT UNIT
	 * that declared it.  Every kg_lisp_eval_string() is its own unit
	 * (one FeEvaluateStringWithOptions, one EvaluateInput), so since the
	 * fe scope carrier this has to declare and bind in one call, exactly
	 * as Emacs answers `unbound' for `(eval '(defvar v) t)' followed by a
	 * separate `(eval '(let ((v 5)) ...) t)'.  The next assertion is the
	 * other half: a later unit does NOT see the mark. */
	CHECK(eval_eq("(progn (defvar p11-one2) (defun p11-one-r ()"
		      " p11-one2) (let ((p11-one2 5)) (p11-one-r)))",
	    "5"));
	/* A7b, the scope half: p11-one was declared in an earlier unit, so
	 * `let' over it here is lexical and the reader sees the (unbound)
	 * global -- the divergence the compat row
	 * phase11-one-arg-defvar-file-scope used to record and this pin
	 * closes. */
	CHECK(eval_error_contains("(progn (defun p11-one-r2 () p11-one)"
				  " (let ((p11-one 5)) (p11-one-r2)))",
	    "void-variable"));

	/* A1 / A11: the temporary-setting idiom, the headline. */
	CHECK(eval_eq("(progn (defvar p11-hkv nil) (defun p11-callee ()"
		      " p11-hkv) (defun p11-caller () (let ((p11-hkv t))"
		      " (p11-callee))) (p11-caller))",
	    "t"));
	/* A3a: nested extents, global outside and bound value inside. */
	CHECK(eval_eq("(progn (defvar p11-nv 1) (defun p11-nvf () p11-nv)"
		      " (list (p11-nvf) (let ((p11-nv 2)) (p11-nvf))"
		      " (p11-nvf)))",
	    "(1 2 1)"));
	/* A2a: setq inside the binding writes the binding, not the value
	 * the binding hides. */
	CHECK(eval_eq("(progn (defvar p11-sv 1) (defun p11-svf () p11-sv)"
		      " (list (let ((p11-sv 2)) (setq p11-sv 3) (p11-svf))"
		      " p11-sv))",
	    "(3 1)"));
	/* A2b (guard): setq outside any let still writes the global. */
	CHECK(eval_eq("(progn (setq p11-sv 4) p11-sv)", "4"));
	/* A3b: let* binds sequentially, and the path is special-aware. */
	CHECK(eval_eq("(progn (defvar p11-ls 1) (defun p11-lsf () p11-ls)"
		      " (let* ((p11-ls 2) (o (p11-lsf))) (list p11-ls o)))",
	    "(2 2)"));
	/* A5: a closure reads the value in force when it is CALLED. */
	CHECK(eval_eq("(progn (defvar p11-cv 1)"
		      " (let ((f (lambda () p11-cv)))"
		      " (let ((p11-cv 2)) (funcall f))))",
	    "2"));
	/* A8b: defconst declares, it does not enforce -- still
	 * let-rebindable, and the rebinding is dynamic. */
	CHECK(eval_eq("(progn (defun p11-cf () p11-const)"
		      " (list (let ((p11-const 2)) (p11-cf)) (p11-cf)))",
	    "(2 1)"));
	/* A10a: binding an unbound special binds it, and the restore puts
	 * back the ABSENCE of a value rather than nil. */
	CHECK(eval_eq("(progn (defvar p11-uv 1) (defun p11-uvf ()"
		      " (if (boundp 'p11-uv) (list 'bound p11-uv) 'unbound))"
		      " (makunbound 'p11-uv)"
		      " (list (let ((p11-uv 3)) (p11-uvf)) (p11-uvf)))",
	    "((bound 3) unbound)"));

	/* The restore runs on every completion kind.  A6a is the throw,
	 * A6b (guard -- kg and Emacs agreed before Phase 11 too) the
	 * error; quit has no in-process probe and is covered by the
	 * exhaustion/quit cases elsewhere in this file. */
	CHECK(eval_eq("(progn (defvar p11-tv 1) (defun p11-tvf () p11-tv)"
		      " (list (catch 'tag (let ((p11-tv 2))"
		      " (throw 'tag (p11-tvf)))) (p11-tvf)))",
	    "(2 1)"));
	CHECK(eval_eq("(progn (defvar p11-ev 1) (defun p11-evf () p11-ev)"
		      " (list (condition-case nil (let ((p11-ev 2))"
		      " (error \"boom\")) (error (p11-evf))) (p11-evf)))",
	    "(1 1)"));
	/* And on the budget wall, which no compat case can reach and
	 * `ignore-errors' cannot contain -- Budget is uncatchable by
	 * design, so the eval below is expected to FAIL, and the claim is
	 * about what the arena looks like afterwards: the binding must
	 * not have survived the wall. */
	CHECK(eval_ok("(defvar p11-bv 1)"));
	CHECK(!eval_ok("(let ((p11-bv 2)) (while t (setq p11-bv 2)))"));
	CHECK(eval_eq("p11-bv", "1"));

	/* A4 (guard, and the probe that falsified the phase's first
	 * draft): a defun PARAMETER named after a special binds
	 * LEXICALLY, in Emacs 31 under lexical-binding: t and here. */
	CHECK(eval_eq("(progn (defvar p11-pv 1) (defun p11-reader ()"
		      " p11-pv) (defun p11-taker (p11-pv)"
		      " (list p11-pv (p11-reader))) (p11-taker 9))",
	    "(9 1)"));
	/* The same for a lambda's parameter. */
	CHECK(eval_eq("(funcall (lambda (p11-pv) (list p11-pv"
		      " (p11-reader))) 8)",
	    "(8 1)"));
	/* A13 (guard): a name no defvar ever named stays lexical. */
	CHECK(eval_eq("(progn (setq p11-plain 1) (defun p11-plainf ()"
		      " p11-plain) (list (let ((p11-plain 2)) (p11-plainf))"
		      " (p11-plainf)))",
	    "(1 1)"));
	/* A12 (guard): defvar is first-wins, and marking is idempotent
	 * and one-way -- a second, one-argument defvar over a fully
	 * marked name neither unmarks it nor reassigns it. */
	CHECK(eval_eq("(progn (defvar p11-two 99) (list p11-two"
		      " (special-variable-p 'p11-two)))",
	    "(1 t)"));
	CHECK(eval_eq("(progn (defvar p11-two) (special-variable-p"
		      " 'p11-two))",
	    "t"));
	/* Marking upgrades the other way: let-dynamic-only, then full. */
	CHECK(eval_eq("(progn (defvar p11-up) (defvar p11-up 1)"
		      " (special-variable-p 'p11-up))",
	    "t"));
	/* Constants cannot be marked at all. */
	CHECK(eval_error_contains(
	    "(defvar t 1)", "Attempt to set a constant symbol"));

	/* Not grid probes: kg's own `let' shapes, which the switch onto
	 * fe's core bindings-list form had to preserve exactly.  The
	 * one-element binding (a) is why the prelude keeps a normalising
	 * macro instead of deleting it -- fe's core form raises
	 * wrong-type-argument for it, Emacs and kg answer nil. */
	CHECK(eval_eq("(let ((a)) a)", "nil"));
	CHECK(eval_eq("(let (a b) (list a b))", "(nil nil)"));
	CHECK(eval_eq("(let () 42)", "42"));
	CHECK(eval_eq("(let ((a 1)))", "nil"));
	/* kg's long-standing reading of a shape Emacs rejects; unchanged
	 * behaviour, recorded rather than fixed by this phase. */
	CHECK(eval_eq("(let ((a 1 2)) a)", "1"));
	/* The constant refusals survive the switch, with one message. */
	CHECK(eval_error_contains(
	    "(let ((t 1)) t)", "Attempt to set a constant symbol"));
	CHECK(eval_error_contains(
	    "(let ((nil 1)) 1)", "Attempt to set a constant symbol"));
	CHECK(eval_error_contains(
	    "(let ((:k 1)) 1)", "Attempt to set a constant symbol"));
	CHECK(eval_error_contains(
	    "(let* ((t 1)) t)", "Attempt to set a constant symbol"));
	/* Parallel evaluation of the initializers survives it too. */
	CHECK(eval_eq("(progn (setq p11-par 100)"
		      " (let ((p11-par 1) (z p11-par)) z))",
	    "100"));

	kg_lisp_shutdown();
}

static void test_definition_forms(void)
{
	CHECK(kg_lisp_init() == 0);
	CHECK(eval_eq("(prefix-numeric-value nil)", "1"));
	CHECK(eval_eq("(prefix-numeric-value '(16))", "16"));
	CHECK(eval_eq("(prefix-numeric-value '-)", "-1"));
	CHECK(eval_error_contains(
	    "(prefix-numeric-value '(1 2))", "Wrong type argument"));

	/* defun returns the symbol, as Emacs does. */
	CHECK(eval_eq("(defun square (x) (* x x))", "square"));
	CHECK(eval_eq("(square 7)", "49"));
	/* Argument lists reach Fe as written: its binder reads &optional and
	 * &rest itself, so kg no longer lowers them to a dotted list. */
	CHECK(eval_ok("(defun bare args args)"));
	CHECK(eval_eq("(bare 1 2)", "(1 2)"));
	CHECK(eval_ok("(defun dotted (a . r) (list a r))"));
	CHECK(eval_eq("(dotted 1 2 3)", "(1 (2 3))"));
	CHECK(eval_ok("(defun none () 7)"));
	CHECK(eval_eq("(none)", "7"));
	CHECK(eval_ok("(defun both (a &optional b &rest r) (list a b r))"));
	CHECK(eval_eq("(both 1)", "(1 nil nil)"));
	CHECK(eval_eq("(both 1 2 3 4)", "(1 2 (3 4))"));

	CHECK(eval_ok("(defun opt (a &optional b) (list a b))"));
	CHECK(eval_eq("(opt 1)", "(1 nil)"));
	CHECK(eval_eq("(opt 1 2)", "(1 2)"));
	CHECK(eval_ok("(defun rst (a &rest r) (list a r))"));
	CHECK(eval_eq("(rst 1)", "(1 nil)"));
	CHECK(eval_eq("(rst 1 2 3)", "(1 (2 3))"));

	/* A docstring is inert when a body follows it and is the value when it
	 * is the whole body, exactly as in Emacs. */
	CHECK(eval_ok("(defun documented (x) \"Doc.\" (+ x 1))"));
	CHECK(eval_eq("(documented 1)", "2"));
	CHECK(eval_ok("(defun onlydoc (x) \"Just a doc.\")"));
	CHECK(eval_eq("(onlydoc 1)", "Just a doc."));

	CHECK(eval_eq(
	    "(defmacro twice (form) (list 'progn form form))", "twice"));
	CHECK(eval_eq("(progn (setq n 0) (twice (setq n (+ n 1))) n)", "2"));
	CHECK(eval_ok("(defmacro my-list (&rest body) (cons 'list body))"));
	CHECK(eval_eq("(my-list 1 2)", "(1 2)"));

	/* defvar initialises only an unbound variable; defconst always
	 * assigns.  A variable holding nil is bound, so defvar leaves it. */
	CHECK(eval_eq("(defvar dv 5)", "dv"));
	CHECK(eval_ok("(defvar dv 9)"));
	CHECK(eval_eq("dv", "5"));
	CHECK(eval_ok("(setq dvn nil)"));
	CHECK(eval_ok("(defvar dvn 9)"));
	CHECK(eval_eq("dvn", "nil"));
	CHECK(eval_eq("(defconst dc 5)", "dc"));
	CHECK(eval_ok("(defconst dc 9)"));
	CHECK(eval_eq("dc", "9"));

	/* A stray top-level (interactive) is inert and really nil. */
	CHECK(eval_eq("(interactive)", "nil"));
	CHECK(eval_eq("(not (interactive))", "t"));

	/* (interactive) in a defun registers the command under its symbol and
	 * is stripped from the body. */
	setup_editor();
	CHECK(eval_ok("(defun greet-lisp () (interactive) (message \"hi\"))"));
	CHECK(kg_lisp_command_name(0) != nullptr);
	CHECK(strcmp(kg_lisp_command_name(0), "greet-lisp") == 0);
	CHECK(kg_lisp_run_command("greet-lisp", 0) == 0);
	CHECK(strcmp(test_status_message, "hi") == 0);
	CHECK(eval_eq("(defun greet-lisp () 9)", "greet-lisp"));
	CHECK(!kg_lisp_command_exists("greet-lisp"));
	CHECK(eval_ok("(defun late-lisp () 7 (interactive))"));
	CHECK(!kg_lisp_command_exists("late-lisp"));
	CHECK(eval_ok("(defun doc-empty-lisp () \"Doc\" (interactive))"));
	CHECK(kg_lisp_command_exists("doc-empty-lisp"));
	CHECK(kg_lisp_run_command("doc-empty-lisp", 0) == 0);
	/* A docstring plus a declaration and no executable form is nil, not
	 * the docstring: the declaration is removed after the docstring and
	 * the empty body becomes (nil).  07D item 1, Emacs-confirmed. */
	CHECK(eval_eq("(doc-empty-lisp)", "nil"));
	/* A declaration with no docstring behaves the same way ... */
	CHECK(eval_ok("(defun decl-only-lisp () (interactive))"));
	CHECK(eval_eq("(decl-only-lisp)", "nil"));
	/* ... and a string *after* the declaration is an ordinary body form,
	 * so it is the value.  Only the string in the Emacs docstring
	 * position, with a form after it, is documentation. */
	CHECK(eval_ok("(defun decl-then-lisp () (interactive) \"tail\")"));
	CHECK(eval_eq("(decl-then-lisp)", "tail"));

	/* commandp answers the command registry's question: only a name is
	 * a command here, and only the declaration in the Emacs position
	 * makes it one.  Every value below is Emacs' own answer, recorded
	 * in test/lisp-compat/oracle/interactive-command-predicate.json. */
	CHECK(eval_eq("(commandp 'greet-lisp)", "nil"));
	CHECK(eval_eq("(commandp 'decl-only-lisp)", "t"));
	CHECK(eval_eq("(commandp 'late-lisp)", "nil"));
	CHECK(eval_eq("(commandp 'quiet-lisp)", "nil"));
	/* A built-in editor command is one too, ... */
	CHECK(eval_eq("(commandp 'version)", "t"));
	/* ... and anything that is not a name at all is nil, as in Emacs. */
	CHECK(eval_eq("(commandp 5)", "nil"));
	CHECK(eval_eq("(commandp nil)", "nil"));
	CHECK(eval_eq("(commandp (lambda () 1))", "nil"));
	/* A defun without (interactive) registers nothing. */
	CHECK(eval_ok("(defun quiet-lisp () (message \"no\"))"));
	CHECK(!kg_lisp_command_exists("quiet-lisp"));

	/* `defalias' takes a DOCSTRING (Phase 29 U.1a), and the argument is
	 * storage: it lands on the symbol's `function-documentation'
	 * property, where `documentation' reads it back ahead of the
	 * definition registry.  Every answer here is Emacs 31.0.91's,
	 * recorded in test/lisp-compat/cases/u0-defalias-*.json. */
	CHECK(eval_eq(
	    "(defalias 'aliased-doc (lambda (x) x) \"Doc.\")", "aliased-doc"));
	CHECK(eval_eq("(aliased-doc 7)", "7"));
	CHECK(eval_eq("(documentation 'aliased-doc)", "Doc."));
	CHECK(eval_eq("(get 'aliased-doc 'function-documentation)", "Doc."));
	/* The docstring is the ALIAS's own, not the target's, and a symbol
	 * DEFINITION still resolves at call time. */
	CHECK(eval_ok("(defalias 'aliased-car 'car \"Take the first.\")"));
	CHECK(eval_eq("(aliased-car (list 1 2))", "1"));
	CHECK(eval_eq("(documentation 'aliased-car)", "Take the first."));
	/* Emacs does not type-check DOCSTRING: a non-string is stored
	 * verbatim, and `documentation' then answers nil for it because a
	 * non-string property indexes nothing. */
	CHECK(eval_ok("(defalias 'aliased-42 (lambda () 1) 42)"));
	CHECK(eval_eq("(get 'aliased-42 'function-documentation)", "42"));
	CHECK(eval_eq("(documentation 'aliased-42)", "nil"));
	/* A nil DOCSTRING stores nothing rather than clearing what is
	 * there, and neither does the two-argument form. */
	CHECK(eval_ok("(defalias 'aliased-keep (lambda () 1) \"Kept.\")"));
	CHECK(eval_ok("(defalias 'aliased-keep (lambda () 2) nil)"));
	CHECK(eval_eq("(documentation 'aliased-keep)", "Kept."));
	CHECK(eval_ok("(defalias 'aliased-keep (lambda () 3))"));
	CHECK(eval_eq("(documentation 'aliased-keep)", "Kept."));
	/* The fence: 2 or 3 arguments, never more. */
	CHECK(eval_eq("(condition-case e "
		      "(defalias 'aliased-4 (lambda () 1) \"D.\" 'extra) "
		      "(error (cdr e)))",
	    "(defalias 4)"));
	/* And a non-symbol target is still refused. */
	CHECK(eval_eq("(condition-case e (defalias 5 (lambda () 1)) "
		      "(error (car e)))",
	    "wrong-type-argument"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_strict_arity(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq(
	    "(condition-case e (funcall (lambda (x) x)) "
	    "(wrong-number-of-arguments (list (car e) (car (cdr (cdr e))))))",
	    "(wrong-number-of-arguments 0)"));
	CHECK(
	    eval_eq("(condition-case e "
		    "(progn (defun needs-two (a b) (list a b)) (needs-two 1)) "
		    "(wrong-number-of-arguments (car e)))",
		"wrong-number-of-arguments"));
	CHECK(eval_eq("(condition-case e "
		      "(progn (defmacro needs-form (x) x) (needs-form 1 2)) "
		      "(wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));
	CHECK(eval_eq("(progn (defun optional-rest (a &optional b &rest r) "
		      "(list a b r)) (optional-rest 1))",
	    "(1 nil nil)"));
	CHECK(eval_eq("(progn (defun optional-rest-2 (a &optional b &rest r) "
		      "(list a b r)) (optional-rest-2 1 2 3 4))",
	    "(1 2 (3 4))"));
	CHECK(eval_eq("(condition-case e (message) "
		      "(wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));
	CHECK(eval_error_contains("(message)", "too few arguments"));
	CHECK(eval_eq("(condition-case e (goto-char 1 2) "
		      "(wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));
	CHECK(eval_error_contains("(goto-char 1 2)", "too many arguments"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The writer abbreviates a two-element (quote X) list back to 'X, as
 * Emacs' printer does (fe sub-plan 11C, adopted at Phase 11's pin; this
 * was the recorded writer-quote-abbreviation divergence until then).  The
 * discrimination is the existing `function`/#' block's, measured against
 * Emacs in the same slice: exactly one element after the head, and the
 * form proper.  (quote x y), (quote) and (quote . x) all keep printing as
 * the pairs they are -- the answers on the right below are the pinned
 * Emacs 31.0.90's own.
 *
 * The reader's half stays here on purpose: 'x reads into the two-element
 * list (quote x) in kg exactly as it does in Emacs, which is what made
 * this the printer's gap and not the reader's, and what lets the compat
 * case build its operand with `list` and say the same thing to both
 * dialects. */
static void test_writer_quote_abbreviation(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(format \"%S\" (list 'quote 'x))", "'x"));
	CHECK(eval_eq("(format \"%S\" ''x)", "'x"));
	/* Recursive, so a quote inside a list comes out abbreviated too. */
	CHECK(eval_eq("(format \"%S\" (list 'a ''b 'c))", "(a 'b c)"));
	CHECK(eval_eq("(format \"%S\" '''x)", "''x"));
	/* Not a two-element proper (quote X): printed as the pair it is. */
	CHECK(eval_eq("(format \"%S\" (list 'quote 'x 'y))", "(quote x y)"));
	CHECK(eval_eq("(format \"%S\" (list 'quote))", "(quote)"));
	CHECK(eval_eq("(format \"%S\" (cons 'quote 'x))", "(quote . x)"));
	/* The reader's half: 'x is that list, element for element. */
	CHECK(eval_eq("(car ''x)", "quote"));
	CHECK(eval_eq("(car (cdr ''x))", "x"));
	CHECK(eval_eq("(length ''x)", "2"));

	kg_lisp_shutdown();
}

static void test_quasiquote(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("'`(a ,b ,@c)",
	    "(quasiquote (a (unquote b) (unquote-splicing c)))"));
	CHECK(eval_eq("(progn (setq b 42) `(a ,b c))", "(a 42 c)"));
	CHECK(eval_eq("(progn (setq l (list 7 8)) `(a ,@l c))", "(a 7 8 c)"));
	CHECK(eval_eq("`(a b)", "(a b)"));
	CHECK(eval_eq(
	    "(progn (setq b 42) `(nested (deep ,b)))", "(nested (deep 42))"));
	CHECK(eval_eq("(let ((x 2)) `(outer `(inner ,x)))",
	    "(outer (quasiquote (inner (unquote x))))"));
	CHECK(eval_eq("(let ((x 2)) `(outer `(inner ,,x)))",
	    "(outer (quasiquote (inner (unquote 2))))"));
	CHECK(eval_eq("(progn (setq b 42) `,b)", "42"));
	/* An empty backquote is a real nil, not a nil-shaped object. */
	CHECK(eval_eq("`()", "nil"));
	CHECK(eval_eq("(not `())", "t"));
	/* Backticks and commas inside strings stay inert. */
	CHECK(eval_eq("\"a, b `c` ,@d\"", "a, b `c` ,@d"));
	/* #'x reads as (function x), the Lisp-2 quote of a function name:
	 * it evaluates to the symbol designator, resolved when the call
	 * happens. */
	CHECK(eval_eq("(mapcar #'car (list (list 1 2) (list 3 4)))", "(1 3)"));
	CHECK(eval_eq("(function car)", "car"));
	/* #' over a lambda *form* is the other half of the reader
	 * abbreviation: (function (lambda ...)) evaluates the form to the
	 * closure, so the funcall below has a real function to call and not
	 * a quoted list. */
	CHECK(eval_eq("(funcall #'(lambda (n) (+ n 1)) 41)", "42"));
	CHECK(eval_eq("(functionp #'(lambda (n) n))", "t"));

	/* Dotted quasiquote.  Every row is GNU Emacs 31.0.90's answer
	 * (/opt-3/emacs-31-lucid, `emacs -Q --batch`).  The first two are
	 * the ones the reader makes hard: `,x' reads as the two-element
	 * list (unquote x), so `(1 . ,x)' is the PROPER list (1 unquote x)
	 * and the expander has to recognise an unquote form in cdr position
	 * rather than wait for an improper tail.  kg answered
	 * (1 unquote x) and (a b unquote x) for those, and `(a . b)' --
	 * a plain improper tail, which used to work -- raised
	 * void-variable list. */
	CHECK(eval_eq("(let ((x 2)) `(1 . ,x))", "(1 . 2)"));
	CHECK(eval_eq("(let ((x 2)) `(a b . ,x))", "(a b . 2)"));
	CHECK(eval_eq("`(a . b)", "(a . b)"));
	/* A splice in front of a dotted tail, and an unquoted nil tail,
	 * which Emacs answers as the proper list (a). */
	CHECK(eval_eq("(let ((x (list 1 2))) `(,@x . 9))", "(1 2 . 9)"));
	CHECK(eval_eq("`(a . ,nil)", "(a)"));
	/* The rule is structural, not syntactic: a literal `unquote' in cdr
	 * position means the same thing as the `,' that reads to it.  This
	 * row is kg's spelling, not Emacs': Emacs' reader produces the
	 * symbol named "," where fe's produces `unquote' (a recorded
	 * divergence in doc/fe-upstream.md), so Emacs answers
	 * (a unquote x) here while kg answers (a . 2). */
	CHECK(eval_eq("(let ((x 2)) `(a unquote x))", "(a . 2)"));

	/* A macro written with backquote. */
	CHECK(eval_ok("(defmacro unless2 (test . body)"
		      " `(if ,test nil (progn ,@body)))"));
	CHECK(eval_eq("(progn (setq n 0) (unless2 nil (setq n 5)) n)", "5"));
	CHECK(eval_eq("(progn (setq n 0) (unless2 t (setq n 5)) n)", "0"));

	kg_lisp_shutdown();
}

/* The improved diagnostic for calling something that is not a function. */
static void test_void_function(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_error_contains(
	    "(no-such-function 1)", "void-function no-such-function"));
	CHECK(eval_error_contains("(1 2)", "non-callable"));
	/* The Lisp-2 message reality: a name with only a value binding is
	 * void-function in call position -- call position sees the function
	 * cell and nothing else. */
	CHECK(eval_error_contains("(progn (setq v 7) (v))", "void-function v"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* A name that was never assigned is an error, not nil. */
static void test_void_variable(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(boundp 'no-such-variable)", "nil"));
	CHECK(eval_error_contains(
	    "no-such-variable", "void-variable no-such-variable"));
	CHECK(eval_error_contains(
	    "(+ 1 no-such-variable)", "void-variable no-such-variable"));

	/* Assignment creates the binding, and nil is a value like any other. */
	CHECK(eval_ok("(setq no-such-variable nil)"));
	CHECK(eval_eq("(boundp 'no-such-variable)", "t"));
	CHECK(eval_eq("no-such-variable", "nil"));
	CHECK(eval_eq("(makunbound 'no-such-variable)", "no-such-variable"));
	CHECK(eval_eq("(boundp 'no-such-variable)", "nil"));

	/* The mirror image: a name with only a function binding is
	 * void-variable in value position -- the namespaces are separate. */
	CHECK(eval_ok("(defalias 'fn-only (lambda () 1))"));
	CHECK(eval_error_contains("fn-only", "void-variable fn-only"));

	/* boundp sees lexical bindings too. */
	CHECK(eval_eq("((lambda (p) (boundp 'p)) 1)", "t"));
	CHECK(eval_eq("(boundp 'p)", "nil"));

	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* A structure that refers to itself used to make the writer loop forever,
 * with no C-g out of it: M-:, eval-buffer and C-j all render their result. */
static void test_cyclic_result(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq(
	    "(progn (setq c (cons 1 nil)) (setcdr c c) c)", "(1 . #<cycle>)"));
	CHECK(eval_eq("(progn (setq d (cons 1 (cons 2 nil)))"
		      " (setcdr (cdr d) d) d)",
	    "(1 2 1 . #<cycle>)"));
	/* Shared but acyclic structure is still printed in full. */
	CHECK(eval_eq("(progn (setq s '(1 2)) (list s s))", "((1 2) (1 2))"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* Lisp nesting is bounded by Fe's frame stack, not by how many GC stack
 * slots or how much C stack a call happens to consume (sub-plan 03F split
 * the old single evaluation_depth counter into two: this is the
 * `max_frames`/`frame_capacity` bound, "Lisp nesting"; native re-entry has
 * its own bound and error, exercised in fe/test_api.c, not here).
 * `(deep N)` now raises "evaluation frame limit exceeded" once N's frames
 * exceed the arena's frame capacity, instead of the accidental
 * "GC stack overflow" the pre-frame-machine evaluator could hit.
 *
 * Measured on this build via kg_lisp_arena_stats(): frame_capacity is
 * 10916 at Phase B's 10 MiB default, and `(deep 200)` alone reaches
 * peak_frame_depth 604 -- about 3.02 frames per recursion level for this
 * chain's shape (`if`, `+`, and the recursive call each open a frame).
 * `(deep 1000000)` therefore asks for roughly 3 million frames against a
 * 10916-frame arena, more than 270x over capacity -- demonstrably above
 * it without depending on the private Fe frame-size struct or
 * reverse-engineering the arena layout, only on the public
 * frame_capacity/peak_frame_depth counters this file already asserts
 * through. (frame_capacity itself is arena-derived and not
 * asserted to a specific number here, since a KG_LISP_ARENA_SIZE override
 * or a Fe-side frame-size change would move it without changing this
 * test's property.)
 *
 * The probe below also confirms 03C's requirement empirically: after the
 * error, allocation_failures is still 0 and peak_frame_depth sits exactly
 * at frame_capacity -- the frame bound fired before the arena was ever
 * asked for an object it didn't have, so deep recursion raises a clean
 * evaluator error rather than an allocation failure.
 *
 * The final eval_ok calls are the property that actually matters: a
 * bounded, host-recoverable error leaves state.context reusable, so a
 * legal deep-but-smaller call right after the overflow must not still see
 * the limit, or any other, exhausted. */
static void test_recursion_depth(void)
{
	struct kg_lisp_arena_stats before, after;

	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(defun deep (n) (if (<= n 0) 0 (+ 1 (deep (- n 1)))))"));
	CHECK(eval_eq("(deep 200)", "200"));
	CHECK(kg_lisp_arena_stats(&before) == 0);
	/* 200 real recursion levels cost 604 frames of a default-sized
	 * arena's 10916; confirms the 3-frames-per-level shape this
	 * comment's derivation relies on stays in that ballpark rather than
	 * silently becoming O(1) or O(N^2). */
	CHECK(before.peak_frame_depth > 200 * 2);
	CHECK(before.peak_frame_depth < before.frame_capacity);

	CHECK(eval_error_contains(
	    "(deep 1000000)", "evaluation frame limit exceeded"));
	CHECK(kg_lisp_arena_stats(&after) == 0);
	CHECK(after.allocation_failures == 0);
	CHECK(after.peak_frame_depth == after.frame_capacity);

	CHECK(eval_eq("(deep 200)", "200"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* kg_lisp_perf_dump_fe_json() (src/lisp_core.c, Phase 21's elisp-data-
 * model follow-up) has two implementations selected by FE_PERF_COUNTERS
 * at compile time: the real one, reached only from test/perfobj/kg and
 * test/test_perf.c (test_fe_perf_counters_reach_kg_json), which are the
 * only binaries whose fe objects are compiled with FE_PERF_COUNTERS=1;
 * and this facade, linked into every ordinary binary including this one,
 * which writes the JSON literal `null` because fe's own FePerfWriteJson()
 * is not even declared in that build (fe_perf.h's `#if FE_PERF_COUNTERS`
 * guard). Exercised here so the facade is not a function `make coverage`
 * reports as dead code -- kg_perf_dump() (src/perf.c) is its only other
 * caller, and that call site does not exist at all in a build without
 * KG_PERF_COUNTERS=1, which this ordinary test binary also is not. */
static void test_perf_dump_fe_json_facade(void)
{
	FILE *fp;
	char buf[16];
	size_t n;

	if (!kg_lisp_active()) {
		return;
	}
	fp = tmpfile();
	CHECK(fp != nullptr);
	kg_lisp_perf_dump_fe_json(fp);
	rewind(fp);
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	CHECK(strcmp(buf, "null\n") == 0);
}

/* Exhaustion of the fixed arena, from kg's own evaluator entry point.
 *
 * Sub-plan 09D Part 3, and the kg half of 09A's Table X.  Fe sub-plan 09B
 * pre-builds `(arena-exhaustion)` and `(evaluation-stack-exhaustion)` at
 * FeOpenContext and keeps them rooted, so signalling one allocates
 * nothing -- which is what makes an out-of-memory catchable by a handler
 * that has to *name* a condition.  Before that pin only `(t ...)` caught
 * it: the raise degraded the condition object to nil and ConditionMatches,
 * which walks a pair to reach the hierarchy, answered false for every
 * named handler.  This asserts the new contract through
 * kg_lisp_eval_string(), the same entry point M-: and an init file use,
 * rather than through fe's own test harness.
 *
 * There is no Emacs oracle for any of this and there deliberately is not
 * one: Emacs has no arena, so `comparison: kg-policy` is what
 * test/lisp-compat/features.json's `arena-exhaustion-catchable-by-name`
 * row records.
 *
 * Three things it pins that must NOT change with it: a Budget wall (the
 * step limit) is still caught by nothing, not even `(t ...)`, per 09A
 * Decision 2; a chain built into a `let` local is collectable again the
 * moment the handler runs, so the session recovers fully; and a chain
 * built into a *global* keeps the arena pinned afterwards -- 09A
 * Decision 3's "recorded, not rescued", which doc/TODO.md carries as the
 * reset-command debt.  The final kg_lisp_shutdown() is the exhausted
 * session's teardown; the suite runs under the sanitizer lanes' leak
 * checkers, so "it shut down without leaking" needs no separate gate. */
static void test_arena_exhaustion_conditions(void)
{
	/* Built into a let-local, so the chain is unreachable the instant
	 * the handler frame is entered and the handler is free to allocate. */
	static const char fill_local[]
	    = "(let ((l nil)) (while t (setq l (cons 1 l))))";
	struct kg_lisp_arena_stats fresh, caught, pinned;
	char form[256];

	/* The arena this instrument needs is one a single evaluation can
	 * actually run out of, and that is a property of the arena and the
	 * STEP BUDGET together: `fill_local' costs about 10.7 steps per
	 * cons, so KG_LISP_STEP_LIMIT's 2^20 steps buy roughly 97000 of
	 * them, and every arena whose slot count is above that raises
	 * Budget -- case 5's uncatchable wall -- before it raises
	 * arena-exhaustion.  Measured at the compiled 10 MiB default: 440489
	 * slots, "evaluation step limit exceeded", peak-live 97449.  So this
	 * function pins the 1 MiB arena (56145 slots) it was written
	 * against, the way utils/check_lisp_gc_stress.py pins its own; Phase
	 * B of doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md is
	 * what makes that possible.  What it does NOT do is weaken the
	 * contract: exhaustion is still catchable by name at any arena size
	 * a single evaluation can exhaust. */
	setenv(arena_env, "1M", 1);

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&fresh) == 0);
	CHECK(fresh.allocation_failures == 0);
	CHECK(fresh.free_slots * 2 > fresh.total_slots);

	/* 1. `(t ...)` -- the one handler shape that worked before the pin. */
	snprintf(form, sizeof(form), "(condition-case e %s (t 'caught))",
	    fill_local);
	CHECK(eval_eq(form, "caught"));
	CHECK(kg_lisp_arena_stats(&caught) == 0);
	CHECK(caught.allocation_failures == fresh.allocation_failures + 1);
	CHECK(caught.total_slots == fresh.total_slots);
	CHECK(caught.free_slots > 0);
	/* Ordinary evaluation between rounds: the session is not damaged. */
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 2. `(error ...)` -- the generic named handler, which is what an
	 * init file or a package actually writes.  This is the assertion
	 * that would have failed at every pin before this one. */
	snprintf(form, sizeof(form), "(condition-case e %s (error 'caught))",
	    fill_local);
	CHECK(eval_eq(form, "caught"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 3. The specific name, and what the handler's variable is bound to:
	 * the pre-built condition object, not nil. */
	snprintf(form, sizeof(form),
	    "(condition-case e %s (arena-exhaustion 'caught))", fill_local);
	CHECK(eval_eq(form, "caught"));
	snprintf(
	    form, sizeof(form), "(condition-case e %s (error e))", fill_local);
	CHECK(eval_eq(form, "(arena-exhaustion)"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 4. The hierarchy is a real answer, not a catch-all: an unrelated
	 * named handler still lets the exhaustion through to the host, and
	 * the host still renders the same message text it always did. */
	snprintf(form, sizeof(form),
	    "(condition-case e %s (arith-error 'caught))", fill_local);
	CHECK(eval_error_contains(form, "out of memory"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 5. Budget stays uncatchable (09A Decision 2): the step wall is not
	 * a condition, and `(t ...)` does not see it. */
	CHECK(eval_error_contains(
	    "(condition-case e (while t 1) (t 'caught))", "step limit"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 6. Rooted exhaustion is recorded, not rescued (09A Decision 3),
	 * and the measured answer is sharper than "it is not rescued": the
	 * handler does not even run.  The same chain built into a *global*
	 * is still reachable when the raise happens, so the collector has
	 * nothing to give back, and entering a `condition-case` handler that
	 * binds a variable is itself an allocation -- fe's handler-re-entry
	 * rule (09B) says a handler that cannot allocate re-raises, and with
	 * no enclosing handler the re-raise reaches the host.  So this is the
	 * one exhaustion shape in this test that is *reported* rather than
	 * caught, and the report is the same text as always.
	 *
	 * Measured here: rc 1, "eval:1: out of memory", free_slots 0,
	 * allocation_failures 7. */
	CHECK(eval_ok("(setq exhaustion-global nil)"));
	CHECK(eval_error_contains("(condition-case e"
				  " (while t (setq exhaustion-global"
				  " (cons 1 exhaustion-global)))"
				  " (error 'caught))",
	    "out of memory"));
	CHECK(kg_lisp_arena_stats(&pinned) == 0);
	CHECK(pinned.total_slots == fresh.total_slots);
	CHECK(pinned.free_slots * 100 < pinned.total_slots);
	CHECK(pinned.allocation_failures > caught.allocation_failures);

	/* 7. ... and the session is alive, correctly reporting, and useless
	 * -- 09A's three words for this state, each asserted.  Ordinary
	 * evaluation still answers, and a handler still catches an ordinary
	 * error, because the collector can still recycle what the *new* form
	 * allocates; what it cannot do is return the pinned chain.  This is
	 * the state M-x lisp-arena-stats exists to make visible, and the
	 * reset command doc/TODO.md records is what would end it. */
	CHECK(eval_eq("(+ 1 2)", "3"));
	CHECK(eval_eq("(condition-case nil (car 1) (error 'ok))", "ok"));
	CHECK(kg_lisp_arena_stats(&pinned) == 0);
	CHECK(pinned.free_slots * 100 < pinned.total_slots);

	kg_lisp_shutdown();

	/* And a fresh context after an exhausted one is a fresh context. */
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&fresh) == 0);
	CHECK(fresh.allocation_failures == 0);
	CHECK(fresh.free_slots * 2 > fresh.total_slots);
	CHECK(eval_eq("(+ 1 2)", "3"));
	kg_lisp_shutdown();

	/* 8. The arena is one of the two exhaustions Phase 9 named; this is
	 * the other.  README.md and doc/lisp-api.md both tell a user that a
	 * full GC root stack is catchable as `evaluation-stack-exhaustion`,
	 * and nothing on kg's side of the pin asserted it -- only fe's own
	 * test_api.c did, and that one provokes the overflow from a native
	 * written for the purpose.  This reaches it through kg's ordinary
	 * entry point instead.
	 *
	 * The route is the *reader*, not the evaluator: an evaluation deep
	 * enough to fill the 4096-slot root stack hits the 1090-frame wall
	 * first and raises Budget, which is uncatchable by design (case 5
	 * above).  Reading a datum nested `gc_stack_deep` levels pushes a
	 * root per level with no frame at all, so `(load FILE)` -- whose
	 * read happens at run time, inside the enclosing handler's extent --
	 * is what puts the overflow where a handler can see it.  Measured on
	 * this build: the same file loaded without a handler reports
	 * `<path>:1: GC stack overflow`, and the depth is chosen well past
	 * 4096 rather than at it, since the reader is not the only thing on
	 * that stack. */
	{
		char path[] = "/tmp/kg-lisp-deep-XXXXXX";
		static constexpr int gc_stack_deep = 8192;
		int fd = mkstemp(path);
		FILE *file = fd >= 0 ? fdopen(fd, "w") : nullptr;
		int i;

		CHECK(file != nullptr);
		if (file == nullptr) {
			if (fd >= 0) {
				(void)close(fd);
				(void)unlink(path);
			}
			unsetenv(arena_env);
			return;
		}
		(void)fputc('\'', file);
		for (i = 0; i < gc_stack_deep; i++) {
			(void)fputc('(', file);
		}
		(void)fputc('1', file);
		for (i = 0; i < gc_stack_deep; i++) {
			(void)fputc(')', file);
		}
		CHECK(fclose(file) == 0);

		CHECK(kg_lisp_init() == 0);
		snprintf(form, sizeof(form),
		    "(condition-case nil (load \"%s\")"
		    " (evaluation-stack-exhaustion 'caught))",
		    path);
		CHECK(eval_eq(form, "caught"));
		snprintf(form, sizeof(form),
		    "(condition-case e (load \"%s\") (error e))", path);
		CHECK(eval_eq(form, "(evaluation-stack-exhaustion)"));
		snprintf(form, sizeof(form), "(load \"%s\")", path);
		CHECK(eval_error_contains(form, "GC stack overflow"));
		CHECK(eval_eq("(+ 1 2)", "3"));
		kg_lisp_shutdown();
		CHECK(unlink(path) == 0);
	}
	unsetenv(arena_env);
}

/* Sub-plan 01A's second test path: lisp/prelude.el is the canonical source
 * and src/lisp_prelude_generated.inc is a generated copy of it that kg
 * evaluates at startup.  `make lisp-prelude-check` proves those two files
 * agree byte for byte; this proves the *file* and the *running editor*
 * agree definition for definition, and that the definitions are in the
 * order the prelude's first rule requires.
 *
 * It deliberately does not re-load the file into a booted context.  A
 * second evaluation is not idempotent, for exactly the reason rule 1
 * exists: `(defalias 'internal--let (symbol-function 'let))` captures Fe's
 * raw `let` primitive before the Emacs `let` macro shadows that name, so
 * re-running it where the macro already exists would bind internal--let to
 * the macro and break every list-library function that uses it.  That
 * hazard is what the type assertions below detect -- internal--let must
 * answer `primitive`, not `macro`, so a generator that ever reordered or
 * dropped forms fails here rather than silently shipping a broken prelude.
 *
 * Arity is not asserted separately: kg's Lisp surface has no func-arity
 * and a lambda's printed form carries no parameter list, so each
 * definition's type plus the byte-identity check above is what is
 * actually observable.
 *
 * Sub-plan 02D's dialect cutover deleted the kg-owned `setq` macro (built
 * on assignment `=`) and rewrote the remaining 53 definitions from `=' to
 * core `setq'; the scan below looked for column-zero "(setq NAME " forms.
 * Sub-plan 04E's Lisp-2 cut retargeted the function cell: the forms are
 * column-zero "(defalias 'NAME ...)" now, the deleted identity-lambda
 * `function` alias dropped the count to 52, and the type assertion reads
 * the function cell -- `(type-of (symbol-function 'NAME))` -- because the
 * names live there, not in the value namespace. */
static void test_condition_case_native_error(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char 5)"));
	/* goto-char with invalid string type raises wrong-type-argument;
	 * condition-case catches it, point stays at 5, buffer survives. */
	CHECK(eval_eq(
	    "(condition-case e (goto-char \"x\") (error 'caught))", "caught"));
	CHECK(eval_eq("(point)", "5"));
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Finding 2 of doc/reviews/2026-08-19-embedded-prelude-phase21-adversarial-
 * review.md.  Two Phase 2 natives reach an fe PRIMITIVE by building a form
 * and re-entering the evaluator (lisp_call_primitive(), src/lisp_cmd.c):
 * `internal--variable-doc-put' calls `put', `nconc' calls `setcdr'.  A
 * plain nested evaluation transfers an abnormal completion to kg's
 * outermost barrier, past every `condition-case' between the native and
 * the raise -- so the two probes below killed the whole evaluation
 * (kgbatch exit 1) instead of running their handlers.  What is pinned is
 * the containment AND what survives it: the condition symbol, its data,
 * and an evaluation that carries on afterwards. */
static void test_native_primitive_call_is_contained(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* `put' rejects a non-symbol, from inside the nested run. */
	CHECK(eval_eq("(condition-case e (internal--variable-doc-put 1 \"x\")"
		      " (error (list 'caught (car e))))",
	    "(caught wrong-type-argument)"));
	/* Structured, not flattened to a message: the data rides through
	 * the resignal, so a handler naming the condition matches too. */
	CHECK(eval_eq("(condition-case e (internal--variable-doc-put 1 \"x\")"
		      " (error e))",
	    "(wrong-type-argument symbolp 1)"));
	CHECK(eval_eq("(condition-case e (internal--variable-doc-put 1 \"x\")"
		      " (wrong-type-argument 'typed))",
	    "typed"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* The path an ordinary `defvar' with a docstring takes.  Going
	 * through the evaluator means the call resolves `put's function
	 * cell, so a replacement really is reached -- and its error is
	 * caught rather than fatal. */
	CHECK(eval_ok("(defalias 'put (lambda (&rest args)"
		      " (error \"replacement put failed\")))"));
	CHECK(eval_eq("(condition-case e (defvar phase2-native-probe 2 \"doc\")"
		      " (error (list 'caught (car e))))",
	    "(caught error)"));

	/* `nconc's own primitive, through the same helper: the splice is
	 * reached only once a second non-empty argument needs one. */
	CHECK(eval_ok("(defalias 'setcdr (lambda (&rest args)"
		      " (error \"replacement setcdr failed\")))"));
	CHECK(eval_eq("(condition-case e (nconc (list 1) (list 2))"
		      " (error (list 'caught (car e))))",
	    "(caught error)"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_catch_throw_unwind(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* throw unwinds through unwind-protect cleanups in innermost-first
	 * order */
	CHECK(eval_ok("(setq trail nil)"));
	CHECK(eval_eq("(catch 'exit "
		      "  (unwind-protect "
		      "    (unwind-protect "
		      "      (throw 'exit 'done) "
		      "      (setq trail (cons 'inner trail))) "
		      "    (setq trail (cons 'outer trail))))",
	    "done"));
	CHECK(eval_eq("trail", "(outer inner)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 06E.  save-excursion and with-current-buffer run a body between
 * a save and a restore, and both used FeCall to do it -- which starts a
 * *nested* evaluator run whose completions transfer to kg's own outermost
 * barrier, past every condition-case that lexically encloses the form.  The
 * protected call plus FeResignal put the completion back in flight in the
 * enclosing run instead, which is what these pin.  (The seam that did it
 * for these two forms was lisp_call_body(); 11D Part 4 turned both forms
 * into prelude macros and 1478302 deleted it, having no callers left.
 * lisp_eval_file() still uses the shape, which is the point of pinning it
 * rather than the function.)
 *
 * `throw` used to be the deliberate exception, pinned here as what it was
 * rather than as what Emacs answers: fe walls the throw search at a native
 * re-entry boundary by design, so a throw out of either body found no catch
 * and became `(no-catch TAG VALUE)`.  Sub-plan 11D Part 4 closed it by
 * removing the native frame -- both forms are prelude macros over Lisp
 * `unwind-protect` now -- so the two throw blocks below assert the reverse:
 * the catch receives the thrown value, and the restore still ran on that
 * path.  This test remains the condition-case guard the 11D contract names:
 * the transparency 06E bought must survive the change of shape.  What is
 * still a wall is every native re-entry that is NOT one of these two forms
 * -- hooks (test_hook_throw_containment), process callbacks, a nested
 * command-execute -- because those are callbacks kg invokes from its own C
 * and no prelude expansion removes the frame. */
static void test_wrapping_native_transparency(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* 1. An error inside save-excursion reaches the enclosing
	 * condition-case, and the restore has already run when it does. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_eq("(condition-case e"
		      "   (save-excursion (goto-char 8) (error \"boom\"))"
		      "   (error 'caught))",
	    "caught"));
	CHECK(eval_eq("(point)", "3"));

	/* 2. The handler sees the original condition, not a re-wrapped one. */
	CHECK(eval_eq("(condition-case e"
		      "   (save-excursion (car 1))"
		      "   (wrong-type-argument 'by-symbol))",
	    "by-symbol"));

	/* 3. A throw out of the body reaches the catch outside the form --
	 * with the value it threw, not a no-catch error -- and the restore
	 * ran on that path too.  The condition-case is left wrapped around
	 * it so this reads as the inversion of the assertion it replaces:
	 * the handler is simply never entered now. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_eq("(condition-case e"
		      "   (catch 'tag"
		      "     (save-excursion (goto-char 8) (throw 'tag 'gone)))"
		      "   (error (car e)))",
	    "gone"));
	CHECK(eval_eq("(point)", "3"));

	/* 4. with-current-buffer answers the same three ways, and restores
	 * the selected buffer on each of them. */
	CHECK(eval_ok("(setq b2 (get-buffer-create \"wrap2\"))"));
	CHECK(eval_eq("(condition-case e"
		      "   (with-current-buffer b2 (error \"boom\"))"
		      "   (error 'caught))",
	    "caught"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));
	CHECK(eval_eq("(condition-case e"
		      "   (catch 'tag (with-current-buffer b2"
		      "     (throw 'tag 'gone)))"
		      "   (error (car e)))",
	    "gone"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 5. A body that completes normally is unaffected by any of this. */
	CHECK(eval_eq("(with-current-buffer b2 (buffer-name))", "wrap2"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 6. The quit path, which no compat case can reach: C-g out of the
	 * body still restores.  An `unwind-protect' cleanup runs on every
	 * completion kind fe has, and this is the one that is neither a
	 * return, an error nor a throw. */
	{
		char result[128] = "";
		static const char quitting[]
		    = "(save-excursion (goto-char 8) (while t 1))";

		CHECK(eval_ok("(goto-char 3)"));
		interrupt_polls = 0;
		kg_lisp_set_interrupt_check(cancel_evaluation);
		CHECK(kg_lisp_eval_string(quitting, sizeof(quitting) - 1,
			  result, sizeof(result))
		    != 0);
		kg_lisp_set_interrupt_check(nullptr);
		CHECK(strcmp(result, "Quit") == 0);
		CHECK(eval_eq("(point)", "3"));

		CHECK(eval_ok("(set-buffer (get-buffer \"bridge.txt\"))"));
		interrupt_polls = 0;
		kg_lisp_set_interrupt_check(cancel_evaluation);
		CHECK(!eval_ok("(with-current-buffer b2 (while t 1))"));
		kg_lisp_set_interrupt_check(nullptr);
		CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));
	}

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 06E, closed by Phase 13.2 and 13.3: a kg native raises Emacs'
 * own condition, with Emacs' own data, and a handler naming the specific
 * symbol catches it.  Both halves had to move.  13.2: the raise used to
 * go through a nested FeEvaluate, whose completion transfers to the
 * OUTERMOST barrier -- past every lexically enclosing handler -- so the
 * wrong-type-argument arm below did not merely fail to match, it was
 * never reached at all and the error left kg uncatchable.  13.3: the
 * argument seams raised prose through FeHandleError, so even the generic
 * arm caught `(error "wrong-type-argument")` rather than the real thing.
 *
 * What this does NOT assert, deliberately: the *rendering* of an uncaught
 * one.  fe's `signal` uses the bare condition name as the completion's
 * message, so an uncaught raise still reports `wrong-type-argument` and
 * not Emacs' "Wrong type argument: integer-or-marker-p, \"x\"" -- that
 * needs the error-message property, which is Phase 19. */
/* Phase 19's interactive reflection: `interactive-form`, and the half of
 * `commandp` that stopped being about a name.
 *
 * doc/TODO.md recorded this as blocked on a METADATA decision, not on an
 * implementation: 07D stored a command's interactive specification as a
 * thunk -- a closure over the descriptor, which the dispatcher can call
 * and a reader can learn nothing from -- so there was nothing honest for
 * `interactive-form` to return.  The decision is to store the raw
 * specification beside the thunk (`define-command`'s fifth argument,
 * which `defun`'s expansion passes), which costs one root per command
 * and makes both questions answerable.
 *
 * `(interactive nil)` for a specification-less command is Emacs' own
 * normalization, measured on 31.0.90, not a convenience. */
static void test_interactive_form_reflection(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(defun p19s (n) \"Doc.\" (interactive \"p\") n)"));
	CHECK(eval_ok("(defun p19f (s) (interactive (list \"x\")) s)"));
	CHECK(eval_ok("(defun p19n () (interactive) 1)"));
	CHECK(eval_ok("(defun p19plain (x) x)"));

	/* A string spec comes back as it was written. */
	CHECK(eval_eq("(interactive-form 'p19s)", "(interactive \"p\")"));
	/* A FORM spec comes back UNEVALUATED -- the descriptor, not the
	 * closure the dispatcher calls.  This is the case 07D could not
	 * answer, and the reason the raw form is stored at all. */
	CHECK(
	    eval_eq("(interactive-form 'p19f)", "(interactive (list \"x\"))"));
	CHECK(eval_eq("(interactive-form 'p19n)", "(interactive nil)"));
	/* Not a command, and not anything at all. */
	CHECK(eval_eq("(interactive-form 'p19plain)", "nil"));
	CHECK(eval_eq("(interactive-form 'p19nosuch)", "nil"));
	CHECK(eval_eq("(interactive-form 5)", "nil"));
	/* A BUILT-IN answers `(interactive nil)` too -- kg policy, and true,
	 * since its handler reads the terminal itself rather than declaring
	 * arguments.  Asserted in test/lisp-compat's
	 * `phase19-interactive-form-builtin` rather than here: this binary
	 * links the editor stubs, whose command table is not cmd.c's. */

	/* The function object answers the same questions the name does,
	 * because it is the same object. */
	CHECK(eval_eq("(interactive-form (symbol-function 'p19f))",
	    "(interactive (list \"x\"))"));
	CHECK(eval_eq("(commandp (symbol-function 'p19s))", "t"));
	CHECK(eval_eq("(commandp (symbol-function 'p19plain))", "nil"));
	CHECK(eval_eq("(commandp (lambda () 1))", "nil"));
	CHECK(eval_eq("(commandp 'p19s)", "t"));

	/* Redefinition replaces the recorded specification rather than
	 * leaving the old one beside a new body. */
	CHECK(
	    eval_ok("(defun p19s (n) (interactive \"n Give a number: \") n)"));
	CHECK(eval_eq(
	    "(interactive-form 'p19s)", "(interactive \"n Give a number: \")"));
	/* ... and removing the command takes the form with it. */
	CHECK(eval_ok("(remove-command 'p19s)"));
	CHECK(eval_eq("(interactive-form 'p19s)", "nil"));
	CHECK(eval_eq("(commandp 'p19s)", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 19's discovery surface, the three things lisp/help-fns.el is
 * built on: what a name's documentation is, which names are enumerable,
 * and where a BUILT-IN command's documentation comes from.
 *
 * The registry is the prelude's `internal--docs' alist, and Phase 19
 * widened what goes in it: every definition form records its name, with
 * or without a docstring, because `apropos' asks for the names and a
 * function you can call and cannot find is not much better than one that
 * does not exist. */
static void test_help_fns_surface(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* The prelude documents its own public surface. */
	CHECK(eval_eq("(documentation 'mapcar)",
	    "Return the list of FUNCTION's values over the elements of LIST."));
	/* ... and deliberately not its machinery. */
	CHECK(eval_eq("(documentation 'internal--let)", "nil"));
	CHECK(eval_eq("(documentation 'no-such-name-at-all)", "nil"));

	/* A definition registers its name whether or not it documents it. */
	CHECK(eval_ok("(defun p19doc (x) \"Return X.\" x)"));
	CHECK(eval_ok("(defun p19undoc (x) x)"));
	CHECK(eval_ok("(defvar p19var 1)"));
	CHECK(eval_eq("(documentation 'p19doc)", "Return X."));
	CHECK(eval_eq("(documentation 'p19undoc)", "nil"));
	CHECK(eval_eq(
	    "(if (memq 'p19undoc (internal--defined-names)) 'yes 'no)", "yes"));
	CHECK(eval_eq(
	    "(if (memq 'p19var (internal--defined-names)) 'yes 'no)", "yes"));

	/* A redefinition takes the registry entry with the definition:
	 * a documented defun redefined through `defalias' with a docless
	 * lambda answers nil, as Emacs does -- the old docs died with the
	 * closure they were written on, where kg used to keep answering
	 * them from the alist.  "Present but undocumented" rather than
	 * deleted, so `apropos''s walk still finds the name. */
	CHECK(eval_ok("(defun p19redef (x) \"First docs.\" x)"));
	CHECK(eval_eq("(documentation 'p19redef)", "First docs."));
	CHECK(eval_ok("(defalias 'p19redef (lambda (x) x))"));
	CHECK(eval_eq("(documentation 'p19redef)", "nil"));
	CHECK(eval_eq(
	    "(if (memq 'p19redef (internal--defined-names)) 'yes 'no)", "yes"));
	/* Re-documenting lands after the invalidation, so it works; and a
	 * fresh documented defun beside it is untouched by another name's
	 * invalidation. */
	CHECK(eval_ok("(defalias 'p19redef (lambda (x) x) \"Second docs.\")"));
	CHECK(eval_eq("(documentation 'p19redef)", "Second docs."));
	CHECK(eval_eq("(documentation 'p19doc)", "Return X."));

	/* Every symbol fe has interned is enumerable without a new
	 * primitive: `env' is the obarray walk `apropos' needs, and it
	 * reaches the natives and the prelude alike. */
	CHECK(eval_eq("(if (memq 'buffer-substring (env)) 'yes 'no)", "yes"));
	CHECK(eval_eq("(if (memq 'mapcar (env)) 'yes 'no)", "yes"));

	/* The command table, which `env' cannot see: a built-in's name is
	 * not a symbol until something writes it.  This binary links the
	 * test stubs, so the table is theirs -- three rows -- and what is
	 * pinned here is the shape, with the real table exercised through
	 * kgbatch (test/lisp-compat's phase19-help-fns-surface) and on a
	 * terminal (test/pty/lisp-help-fns-apropos.yaml). */
	CHECK(eval_eq(
	    "(if (memq 'version (internal--command-names)) 'yes 'no)", "yes"));
	CHECK(
	    eval_ok("(define-command 'p19cmd (lambda () 1) nil \"Doc it.\")"));
	/* The Lisp half of the list is deliberately absent HERE: the stub
	 * `cmd_name_at' walks the stub table only, because this file is
	 * linked into binaries that carry no Lisp objects and may not name
	 * the adapter's symbols.  The real walk answers both halves, which
	 * is what the kgbatch case and the PTY case above measure. */
	/* A built-in's documentation is its one-line summary; a command
	 * defined with a docstring answers that; and the two ways of not
	 * being a command answer nil. */
	CHECK(eval_eq("(internal--command-documentation 'version)", "stub"));
	CHECK(eval_eq("(internal--command-documentation 'p19cmd)", "Doc it."));
	CHECK(eval_eq("(internal--command-documentation 'p19undoc)", "nil"));
	CHECK(eval_eq("(internal--command-documentation 5)", "nil"));
	/* ... and a command defined with no docstring at all: the root is
	 * there and holds nil. */
	CHECK(eval_ok("(define-command 'p19bare (lambda () 1))"));
	CHECK(eval_eq("(internal--command-documentation 'p19bare)", "nil"));
	CHECK(eval_eq("(documentation 'p19bare)", "nil"));
	CHECK(eval_ok("(remove-command 'p19cmd)"));
	CHECK(eval_ok("(remove-command 'p19bare)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 19: what a condition READS AS -- from Lisp through
 * `error-message-string`, and from the host through the diagnostic kg
 * puts in the echo area.
 *
 * The second half is `render_condition()` in src/lisp_core.c, and what it
 * is worth is the difference between `wrong-type-argument` and
 * `Wrong type argument: integer-or-marker-p, "x"`: fe's `signal` uses the
 * condition symbol as the completion's message, so every one of the
 * natives Phase 13.3 gave structured conditions to reported nothing but
 * its condition's name until this phase.
 *
 * The negative case matters as much: fe's own descriptive messages are
 * NOT replaced, because the splice only fires when the message ENDS in
 * the bare condition name.  `(car 1)` says "expected pair, got integer"
 * on both sides of this phase. */
static void test_error_message_rendering(void)
{
	char result[256] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* The rendering, from Lisp. */
	CHECK(eval_eq("(error-message-string '(wrong-type-argument listp 6))",
	    "Wrong type argument: listp, 6"));
	CHECK(eval_eq(
	    "(error-message-string '(error \"custom msg\"))", "custom msg"));
	CHECK(eval_eq("(condition-case e (goto-char \"x\") "
		      "(error (error-message-string e)))",
	    "Wrong type argument: integer-or-marker-p, \"x\""));
	/* The property the rendering reads, and a program's right to
	 * replace it. */
	CHECK(eval_eq(
	    "(get 'args-out-of-range 'error-message)", "Args out of range"));
	CHECK(eval_eq("(progn (put 'args-out-of-range 'error-message \"Nope\") "
		      "(error-message-string '(args-out-of-range 1)))",
	    "Nope: 1"));

	/* The diagnostic a host sees for an UNCAUGHT one. */
	CHECK(
	    kg_lisp_eval_string("(goto-char \"x\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "Wrong type argument: integer-or-marker-p, \"x\"")
	    != nullptr);
	CHECK(kg_lisp_eval_string("(signal 'error (list \"custom msg\"))", 35,
		  result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "custom msg") != nullptr);
	/* ... and fe's own descriptive message, which does not end in a
	 * condition name and is therefore left alone. */
	CHECK(kg_lisp_eval_string("(car 1)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, "expected pair, got integer") != nullptr);
	/* The source label and position fe latched survive the splice: only
	 * the trailing condition name is replaced. */
	CHECK(strstr(result, "eval:1:") != nullptr);

	/* Phase 19 took doc/TODO.md's writer row with the same pin: `prin1`
	 * escapes the backslash as well as the quote, so a printed string
	 * reads back.  `regexp-quote` is the kg surface that produces them. */
	CHECK(eval_eq("(format \"%S\" \"x\\\\y\")", "\"x\\\\y\""));
	CHECK(eval_eq("(format \"%S\" (regexp-quote \"a.b\"))", "\"a\\\\.b\""));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_condition_case_kg_native_conditions(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* A generic (error ...) handler still catches a kg native's raise:
	 * wrong-type-argument is a subtype of error. */
	CHECK(eval_eq("(condition-case e (goto-char \"x\") (error 'generic))",
	    "generic"));
	/* And the handler naming the symbol now matches, which is the whole
	 * of 13.2 plus 13.3 in one form. */
	CHECK(eval_eq("(condition-case e (goto-char \"x\") "
		      "(wrong-type-argument 'sym))",
	    "sym"));
	CHECK(eval_eq("(condition-case e (goto-char \"x\") (error (car e)))",
	    "wrong-type-argument"));
	/* Emacs' data shape: the predicate the argument failed first, the
	 * offending value second.  (goto-char "x") is
	 * (wrong-type-argument integer-or-marker-p "x") on 31.0.90. */
	CHECK(eval_eq("(condition-case e (goto-char \"x\") "
		      "(wrong-type-argument (car (cdr e))))",
	    "integer-or-marker-p"));
	CHECK(eval_eq("(condition-case e (goto-char \"x\") "
		      "(wrong-type-argument (car (cdr (cdr e)))))",
	    "x"));
	/* 13.2's own regression, on the caller the plan named: a handler
	 * lexically enclosing the native is reached at all.  This form
	 * used to escape its own condition-case. */
	CHECK(eval_eq("(condition-case e (prefix-numeric-value \"x\") "
		      "(wrong-type-argument 'caught))",
	    "caught"));
	/* An inner handler that does not name the condition still lets it
	 * through to an outer one that does -- the resignal keeps the
	 * condition object, not just the fact of a failure. */
	CHECK(eval_eq("(condition-case e "
		      "(condition-case inner (goto-char \"x\") (quit 'wrong)) "
		      "(wrong-type-argument (car e)))",
	    "wrong-type-argument"));
	/* 13.3's other condition: a range failure that is not a type
	 * failure.  (match-beginning -1) is (args-out-of-range -1 0) on
	 * Emacs, measured; a group past the last one is nil on both. */
	CHECK(eval_eq("(condition-case e (match-beginning -1) "
		      "(args-out-of-range (car e)))",
	    "args-out-of-range"));
	CHECK(eval_eq("(match-beginning 9)", "nil"));
	/* Fe's own natives were already classified, and still are. */
	CHECK(eval_eq(
	    "(condition-case e (car 1) (wrong-type-argument 'sym))", "sym"));
	/* 13.1, through kg: `signal`, `error` and `keywordp` are reachable
	 * as values at all.  All three answered `invalid-function` before
	 * the pin move -- and the one that mattered here is `mapcar`, since
	 * kg's prelude funcalls its function argument, so a predicate with
	 * no row in fe's primitive_is_function[] was unusable however
	 * ordinary it looked in call position. */
	CHECK(
	    eval_eq("(condition-case e "
		    "(funcall 'signal 'wrong-type-argument (list 'symbolp 1)) "
		    "(wrong-type-argument (car (cdr e))))",
		"symbolp"));
	CHECK(eval_eq(
	    "(condition-case e (apply 'error (list \"boom\")) (error (car e)))",
	    "error"));
	CHECK(eval_eq("(mapcar 'keywordp (list :a 1))", "(t nil)"));
	CHECK(eval_eq("(list (functionp 'signal) (functionp 'error) "
		      "(functionp 'keywordp))",
	    "(t t t)"));
	/* Every seam the sweep touched, by the predicate Emacs names. */
	CHECK(eval_eq("(condition-case e (string= 1 \"a\") "
		      "(wrong-type-argument (car (cdr e))))",
	    "stringp"));
	CHECK(eval_eq("(condition-case e (marker-position 1) "
		      "(wrong-type-argument (car (cdr e))))",
	    "markerp"));
	CHECK(eval_eq("(condition-case e (provide 1) "
		      "(wrong-type-argument (car (cdr e))))",
	    "symbolp"));
	CHECK(eval_eq("(condition-case e (forward-word \"x\") "
		      "(wrong-type-argument (car (cdr e))))",
	    "fixnump"));
	CHECK(eval_eq("(condition-case e (char-to-string \"a\") "
		      "(wrong-type-argument (car (cdr e))))",
	    "characterp"));
	/* A failure Emacs itself reports unstructured stays a plain error
	 * here: kg does not invent a condition Emacs does not have. */
	CHECK(eval_eq("(condition-case e (char-to-string 55296) "
		      "(wrong-type-argument 'wta) (error 'plain))",
	    "plain"));
	/* The interpreter is usable after every one of these. */
	CHECK(eval_eq("(+ 1 2)", "3"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 13.4's trap battery, kg's half.  Every one of these is a
 * semantic that cost weeks somewhere else and that kg already had right;
 * they are checked in as tests rather than written down as prose
 * precisely because the richest such batteries elsewhere were only ever
 * prose.  The compat corpus carries the same forms against the Emacs
 * oracle (phase13-trap-*); this is the native side that names them, so
 * a kg-owned manifest row has a kg_test to point at. */
static void test_phase13_trap_battery(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Variadic comparison is pairwise-ALL, not first-pair: an
	 * implementation that compares only arguments 0 and 1 answers t for
	 * the first of these, which is a silently wrong range check rather
	 * than an error. */
	CHECK(eval_eq("(< 1 5 3)", "nil"));
	CHECK(eval_eq("(> 3 1 2)", "nil"));
	CHECK(eval_eq("(<= 1 1 2)", "t"));
	CHECK(eval_eq("(>= 3 3 1)", "t"));
	CHECK(eval_eq("(> 3 2 1)", "t"));
	CHECK(eval_eq("(< 1 2 3 4)", "t"));

	/* Catch tags are compared with eq and type-check nothing, so `t` is
	 * legal; `nil` as a tag does not match its own catch on Emacs
	 * either, so the two constants are pinned separately.  A throw
	 * matching an outer catch passes through a mismatched inner one. */
	CHECK(eval_eq("(catch t (throw t 1))", "1"));
	CHECK(eval_eq("(condition-case e (catch nil (throw nil 2)) "
		      "(no-catch 'no-catch))",
	    "no-catch"));
	CHECK(eval_eq("(catch 'a (catch 'b (throw 'a 3)))", "3"));

	/* `declare` is metadata, stripped before the body is code -- alone,
	 * and in the full docstring/declare/interactive/body cross-product. */
	CHECK(eval_ok("(defun kg13-declared (x) (declare (side-effect-free t))"
		      " x)"));
	CHECK(eval_eq("(kg13-declared 7)", "7"));
	CHECK(eval_ok("(defun kg13-full (x) \"doc\" (declare (pure t)) "
		      "(interactive \"p\") (* x 2))"));
	CHECK(eval_eq("(kg13-full 4)", "8"));

	/* The known format directives consume their arguments in order and
	 * do not desync the cursor; extras are ignored.  The unknown-
	 * directive side is already strict (phase8-format-strictness), and
	 * a raise cannot desync a cursor. */
	CHECK(eval_eq("(format \"%d%d\" 1 2)", "12"));
	CHECK(eval_eq("(format \"%s\" 1 2)", "1"));
	CHECK(eval_eq("(format \"%s-%d-%s\" 'a 2 \"c\")", "a-2-c"));

	/* t, nil and keywords are constants. */
	CHECK(
	    eval_eq("(condition-case e (setq t 1) (setting-constant (car e)))",
		"setting-constant"));
	CHECK(eval_eq(
	    "(condition-case e (setq nil 1) (setting-constant (car e)))",
	    "setting-constant"));
	CHECK(
	    eval_eq("(condition-case e (setq :k 1) (setting-constant (car e)))",
		"setting-constant"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 14.  The symbol surface through kg's own evaluator, so that the
 * prelude's `equal`, `mapcar` and friends are in the picture rather than
 * only fe's core.  fe/test_api.c:TestSymbolPrimitives is the fe-side
 * twin; what is here is what only kg can answer. */
static void test_phase14_symbols(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* THE intern-soft CONTRACT, double-probed: nil on a miss, twice,
	 * and no interning as a side effect.  An intern-on-miss
	 * implementation answers the symbol on the second probe, and the
	 * `(while (setq x (intern-soft ...)) ...)` idiom then allocates
	 * until the arena is gone instead of terminating. */
	CHECK(eval_eq("(intern-soft \"kg14-fresh\")", "nil"));
	CHECK(eval_eq("(intern-soft \"kg14-fresh\")", "nil"));
	CHECK(eval_eq("(progn (intern \"kg14-fresh\") "
		      "(symbol-name (intern-soft \"kg14-fresh\")))",
	    "kg14-fresh"));
	/* A name that has only ever been a string is still not a symbol. */
	CHECK(eval_eq(
	    "(let ((s \"kg14-string-only\")) "
	    "(list (intern-soft s) (intern-soft \"kg14-string-only\")))",
	    "(nil nil)"));

	/* Uninterned symbols: identity, not name.  kg's prelude `equal`
	 * bottoms out in `eq` for atoms, so it agrees with Emacs that an
	 * uninterned symbol is equal to nothing but itself -- which is the
	 * comparison half of the policy this phase recorded. */
	CHECK(eval_eq("(eq (make-symbol \"kg14-u\") 'kg14-u)", "nil"));
	CHECK(eval_eq("(equal (make-symbol \"kg14-u\") 'kg14-u)", "nil"));
	CHECK(eval_eq("(let ((s (make-symbol \"kg14-u\"))) (equal s s))", "t"));
	CHECK(eval_eq("(symbol-name (make-symbol \"kg14-u\"))", "kg14-u"));
	CHECK(eval_eq("(intern-soft (make-symbol \"kg14-u\"))", "nil"));

	/* gensym is the hygiene mechanism: unique, and unreachable by name. */
	CHECK(eval_eq("(eq (gensym) (gensym))", "nil"));
	CHECK(eval_eq("(intern-soft (symbol-name (gensym)))", "nil"));

	/* Property lists, including the append order and the eq-compared
	 * property Emacs was measured to have. */
	CHECK(eval_eq("(progn (put 'kg14-p 'a 1) (put 'kg14-p 'b 2) "
		      "(symbol-plist 'kg14-p))",
	    "(a 1 b 2)"));
	CHECK(eval_eq(
	    "(progn (put 'kg14-p 'a 9) (symbol-plist 'kg14-p))", "(a 9 b 2)"));
	CHECK(eval_eq("(list (get 'kg14-p 'a) (get 'kg14-p 'nope) "
		      "(get 'kg14-never 'a) (get nil 'a))",
	    "(9 nil nil nil)"));

	/* All eight are ordinary functions, so the prelude's higher-order
	 * functions -- which funcall their argument -- can reach them. */
	CHECK(eval_eq("(mapcar 'symbol-name '(a b))", "(\"a\" \"b\")"));
	CHECK(eval_eq("(mapcar (lambda (n) (symbol-name (intern n))) "
		      "'(\"x\" \"y\"))",
	    "(\"x\" \"y\")"));

	/* Reader escapes and the printer that is their inverse, through
	 * kg's reader.  `format`'s %S is kg's route to the printed form. */
	CHECK(eval_eq("(symbol-name 'a\\ b)", "a b"));
	CHECK(eval_eq("(symbol-name '\\1)", "1"));
	CHECK(eval_eq("(length '(a \\. b))", "3"));
	CHECK(eval_eq("(format \"%S\" (intern \"a b\"))", "a\\ b"));
	CHECK(eval_eq("(format \"%S\" (intern \"1\"))", "\\1"));
	CHECK(eval_eq("(format \"%S\" (intern \"\"))", "##"));
	CHECK(eval_eq("(eq '## (intern \"\"))", "t"));
	CHECK(eval_eq("(eq 'a\\ b (intern \"a b\"))", "t"));

	/* The type errors are conditions a handler can name -- Phase 13's
	 * repair applied to a surface that landed after it. */
	CHECK(eval_eq("(condition-case e (intern 5) (wrong-type-argument e))",
	    "(wrong-type-argument stringp 5)"));
	CHECK(eval_eq(
	    "(condition-case e (symbol-name 5) (wrong-type-argument e))",
	    "(wrong-type-argument symbolp 5)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 14's gensym-hygiene demonstration, from the consumer's side.
 * `save-excursion` and `with-current-buffer` bind their captured state to
 * a gensym, so a body that assigns the old name -- `internal--excursion`
 * -- can no longer make the cleanup raise.  A raising cleanup REPLACES
 * the completion it is unwinding (fe 06A Decision 4, which is Emacs' rule
 * too), so before this the user's own error was lost. */
static void test_phase14_excursion_hygiene(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(condition-case e (save-excursion "
		      "(setq internal--excursion nil) (error \"MY-ERROR\")) "
		      "(error (car (cdr e))))",
	    "MY-ERROR"));
	CHECK(eval_eq("(condition-case e (with-current-buffer (current-buffer) "
		      "(setq internal--excursion nil) (error \"MY-ERROR\")) "
		      "(error (car (cdr e))))",
	    "MY-ERROR"));
	/* And the forms still do their job. */
	CHECK(eval_ok("(save-excursion (goto-char (point-min)))"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The rest of that sweep, over the prelude's FUNCTION temporaries.
 *
 * Since Phase 11 a `let` over a `defvar`'d name binds dynamically, so any
 * prelude temporary still bound while USER code runs is a two-way
 * capture: the callback's assignment lands on the prelude's accumulator
 * and the prelude's accumulator is what the user's variable reads back.
 * The measured `before` for each line is in the comment beside it; the
 * fix is that these temporaries are lambda PARAMETERS now, which fe binds
 * lexically unconditionally.  Seven of these are also compat cases
 * (prelude-hygiene-*), pinned against the oracle; what is here is the
 * whole battery plus the halves a compat case cannot ask -- the
 * LOADER's, which needs a file on disk.
 *
 * Every `defvar` below is the trap: the case is only meaningful because
 * the name it marks special is the one the prelude used to bind. */
static void test_prelude_temporary_hygiene(void)
{
	char loaded[] = "/tmp/kg-hygiene-load-XXXXXX";
	char form[512];

	CHECK(write_temp_lisp(
	    loaded, "(setq h 'SET-BY-LOADED-FILE)\n(setq cell 'ALSO-SET)\n"));

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Higher-order functions.  Before: (wrong-type-argument listp 3) --
	 * the callback had replaced the half-built list with an integer. */
	CHECK(eval_eq("(progn (defvar res 0) (list "
		      "(mapcar (lambda (x) (setq res x) x) (list 1 2 3)) res))",
	    "((1 2 3) 3)"));
	/* Before: (3 0) -- `mapc` answered its own temporary, not the list. */
	CHECK(eval_eq("(progn (defvar original 0) (list "
		      "(mapc (lambda (x) (setq original x)) (list 1 2 3)) "
		      "original))",
	    "((1 2 3) 3)"));
	/* Before: ("ab" 0) -- the first-iteration flag stayed truthy, so the
	 * separator was never emitted. */
	CHECK(eval_eq("(progn (defvar first 0) (list "
		      "(mapconcat (lambda (x) (setq first x) x) "
		      "(list \"a\" \"b\") \"-\") first))",
	    "(\"a-b\" \"b\")"));
	/* mapconcat holds two; the accumulator is the other.  Before:
	 * ("a-b" 0) -- the string was right and the user's variable lost. */
	CHECK(eval_eq("(progn (defvar result 0) (list "
		      "(mapconcat (lambda (x) (setq result x) x) "
		      "(list \"a\" \"b\") \"-\") result))",
	    "(\"a-b\" \"b\")"));
	/* A function REP holds five.  `limit` and `mb` raised
	 * (wrong-type-argument number-or-marker-p "b"); `start` and `out`
	 * answered ("aXc" 0), losing only the user's variable. */
	CHECK(eval_eq("(progn (defvar limit 0) (list "
		      "(replace-regexp-in-string \"b\" "
		      "(lambda (m) (setq limit m) \"X\") \"abc\") limit))",
	    "(\"aXc\" \"b\")"));
	CHECK(eval_eq("(progn (defvar mb 0) (list "
		      "(replace-regexp-in-string \"b\" "
		      "(lambda (m) (setq mb m) \"X\") \"abc\") mb))",
	    "(\"aXc\" \"b\")"));
	/* kg's `sort` is a Lisp merge sort, so the predicate runs inside
	 * `internal--merge`'s accumulator.  Before: (wrong-type-argument
	 * listp 3) for `out`, and ((1 2 3) 0) for `sort`'s own `runs`. */
	CHECK(eval_eq("(progn (defvar out 0) (list "
		      "(sort (list 3 1 2) (lambda (a b) (setq out a) (< a b))) "
		      "out))",
	    "((1 2 3) 2)"));
	CHECK(eval_eq("(progn (defvar runs 0) (list "
		      "(sort (list 3 1 2) (lambda (a b) (setq runs a) (< a b)))"
		      " runs))",
	    "((1 2 3) 2)"));
	/* The seq- shim is ordinary prelude Lisp, so all three of these ran
	 * their predicate inside a capturable accumulator. */
	CHECK(eval_eq("(progn (defvar out2 0) (list "
		      "(seq-filter (lambda (x) (setq out2 x) t) (list 1 2 3)) "
		      "out2))",
	    "((1 2 3) 3)"));
	CHECK(eval_eq("(progn (defvar found 0) (list "
		      "(seq-find (lambda (x) (setq found x) (= x 2)) "
		      "(list 1 2 3)) found))",
	    "(2 2)"));
	CHECK(eval_eq("(progn (defvar hit 0) (list "
		      "(seq-some (lambda (x) (setq hit x) nil) (list 1 2 3)) "
		      "hit))",
	    "(nil 3)"));

	/* The macro half: `dotimes` expands into `internal--dotimes`, whose
	 * counter was held across the `funcall` of the body.  Before, the
	 * body and the loop shared one binding, each iteration incremented
	 * it twice, and the user's counter answered 0. */
	CHECK(eval_eq(
	    "(progn (defvar i 0) (dotimes (k 3) (setq i (+ i 1))) i)", "3"));

	/* Not a callback at all: `add-to-list` takes the NAME of a variable,
	 * and its temporary had the shape of one.  Before: (9) -- the `set`
	 * wrote the shadow the function's own binding had just made, and
	 * nothing said so. */
	CHECK(eval_eq("(progn (defvar current (list 9)) "
		      "(add-to-list 'current 1) current)",
	    "(1 9)"));

	/* THE LOADER, which no compat case can ask: `internal--load-loop`
	 * holds its stream handle and its form cell across the `eval` of
	 * every form in the file it is reading.  Before, with `h` and `cell`
	 * marked special, the loaded file's own `setq h` replaced the stream
	 * handle and the next `internal--read-form` raised
	 * wrong-type-argument -- so a file could not even define a variable
	 * of that name.  Both names reach the user's binding now, and the
	 * load completes. */
	(void)snprintf(form, sizeof(form),
	    "(progn (defvar h 'INIT) (defvar cell 'INIT) (load \"%s\") "
	    "(list h cell))",
	    loaded);
	CHECK(eval_eq(form, "(SET-BY-LOADED-FILE ALSO-SET)"));

	kg_lisp_shutdown();
	teardown_editor();
	(void)unlink(loaded);
}

/* ---- Phase 15: the package-writer's string and list library ---------
 *
 * Every expectation below was measured against the pinned oracle Emacs
 * 31.0.90 BEFORE it was implemented -- that is what the phase's forecast
 * audit and its probe scripts were for -- and the same expectations are
 * pinned a second time as oracle cases in test/lisp-compat.  What these
 * add is the paths a compat case cannot reach: the ones that need a
 * buffer, and the ones whose answer is a condition rather than a value. */

static void test_phase15_string_natives(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* ASCII case conversion over both a string and a character. */
	CHECK(eval_eq("(upcase \"aBc\")", "ABC"));
	CHECK(eval_eq("(downcase \"aBc\")", "abc"));
	CHECK(eval_eq("(capitalize \"hELLO wORLD\")", "Hello World"));
	/* Emacs' word rule: a word begins after any non-alphanumeric, so
	 * `_' starts one and `1' does not. */
	CHECK(eval_eq("(capitalize \"a-b_c d1e\")", "A-B_C D1e"));
	CHECK(eval_eq(
	    "(list (upcase 97) (downcase 65) (capitalize 97))", "(65 97 65)"));
	/* A non-ASCII character comes back unchanged, and one outside
	 * Unicode is the same predicate the whole argument is judged by. */
	CHECK(eval_eq("(list (upcase 233) (downcase 233))", "(233 233)"));
	CHECK(eval_eq("(condition-case e (upcase 1114112) (error e))",
	    "(wrong-type-argument char-or-string-p 1114112)"));
	/* ASCII-only, and the recorded divergence: a byte >= 0x80 passes
	 * through, and it counts as a word constituent so the letters after
	 * it are not each read as the start of a word. */
	CHECK(eval_eq("(upcase \"caf\xc3\xa9\")", "CAF\xc3\xa9"));
	CHECK(
	    eval_eq("(capitalize \"\xc3\xa9lan vital\")", "\xc3\xa9lan Vital"));
	CHECK(eval_eq("(condition-case e (upcase '(1)) (error e))",
	    "(wrong-type-argument char-or-string-p (1))"));

	/* string-to-number, including the three shapes Emacs does NOT read
	 * as numbers and the "1." that is integer syntax to the reader. */
	CHECK(eval_eq("(list (string-to-number \"42\") "
		      "(string-to-number \"3.5\") (string-to-number \"x\") "
		      "(string-to-number \"  12 \") "
		      "(string-to-number \"12abc\"))",
	    "(42 3.5 0 12 12)"));
	CHECK(eval_eq("(list (string-to-number \"1.\") "
		      "(string-to-number \".5\") (string-to-number \"5e\") "
		      "(string-to-number \"0x10\") (string-to-number \"inf\"))",
	    "(1 0.5 5 0 0)"));
	CHECK(eval_eq("(string-to-number \"ff\" 16)", "255"));
	CHECK(eval_eq("(list (string-to-number \"-42\") "
		      "(string-to-number \"+7\") (string-to-number \"-.5\"))",
	    "(-42 7 -0.5)"));
	CHECK(eval_error_contains(
	    "(string-to-number \"1\" 1)", "Args out of range"));
	CHECK(eval_error_contains(
	    "(string-to-number \"1\" 35)", "Args out of range"));
	CHECK(eval_eq("(condition-case e (string-to-number 5) (error e))",
	    "(wrong-type-argument stringp 5)"));

	CHECK(eval_eq("(make-string 3 120)", "xxx"));
	CHECK(eval_eq("(string-length (make-string 2 233))", "2"));
	/* N is read as an integer, not through a double: a float N is
	 * (wrong-type-argument wholenump 2.0) in Emacs too. */
	CHECK(eval_eq("(condition-case e (make-string 2.0 120) (error e))",
	    "(wrong-type-argument wholenump 2.0)"));
	CHECK(eval_eq("(condition-case e (make-string 2 -1) (error e))",
	    "(wrong-type-argument characterp -1)"));
	CHECK(eval_eq("(condition-case e (make-string -1 120) (error e))",
	    "(wrong-type-argument wholenump -1)"));
	CHECK(eval_eq("(condition-case e (make-string 2 \"x\") (error e))",
	    "(wrong-type-argument characterp \"x\")"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase15_regex_seam(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "alpha beta", 10);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(string-match \"b+\" \"abbbc\")", "1"));
	CHECK(eval_eq("(list (match-beginning 0) (match-end 0))", "(1 4)"));
	CHECK(eval_eq("(string-match \"z\" \"abc\")", "nil"));
	CHECK(eval_eq("(string-match \"a\" \"aaa\" 1)", "1"));
	/* A negative START counts back from the end, as substring's does,
	 * and one past the front clamps to it rather than erring. */
	CHECK(eval_eq("(string-match \"a\" \"xa\" -1)", "1"));
	CHECK(eval_eq("(string-match \"x\" \"xa\" -9)", "0"));
	CHECK(eval_error_contains(
	    "(string-match \"a\" \"ab\" 5)", "Args out of range"));
	CHECK(eval_eq("(condition-case e (string-match 1 \"a\") (error e))",
	    "(wrong-type-argument stringp 1)"));
	CHECK(eval_eq("(condition-case e (string-match \"a\" 1) (error e))",
	    "(wrong-type-argument stringp 1)"));
	CHECK(eval_error_contains(
	    "(string-match \"[\" \"a\")", "invalid regexp"));

	/* Character indices, not bytes: the group starts after two
	 * multi-byte characters, so it begins at 2 and not at 6. */
	CHECK(eval_eq("(progn (string-match \"\\\\([a-z]+\\\\)\" "
		      "\"\xe6\xbc\xa2\xe5\xad\x97"
		      "ab\") "
		      "(list (match-beginning 1) (match-end 1) "
		      "(match-string 1 \"\xe6\xbc\xa2\xe5\xad\x97"
		      "ab\")))",
	    "(2 4 \"ab\")"));
	CHECK(eval_eq("(progn (string-match \"\\\\([a-z]+\\\\)-"
		      "\\\\([0-9]+\\\\)\" \"xx foo-42 yy\") "
		      "(list (match-string 1 \"xx foo-42 yy\") "
		      "(match-string 2 \"xx foo-42 yy\") "
		      "(match-string 3 \"xx foo-42 yy\")))",
	    "(\"foo\" \"42\" nil)"));

	/* The same match data register serves both subjects, as in Emacs:
	 * a buffer search after a string match reports positions again. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq("(list (re-search-forward \"beta\") "
		      "(match-beginning 0) (match-end 0))",
	    "(11 7 11)"));
	CHECK(eval_eq("(match-string 0)", "beta"));

	CHECK(eval_eq("(string-length (regexp-quote \"a.b\"))", "4"));
	CHECK(eval_eq("(string-match (regexp-quote \"a.b\") \"xa.by\")", "1"));
	CHECK(
	    eval_eq("(string-match (regexp-quote \"a.b\") \"xaXby\")", "nil"));

	CHECK(eval_eq(
	    "(replace-regexp-in-string \"b+\" \"X\" \"abbbcb\")", "aXcX"));
	CHECK(eval_eq(
	    "(replace-regexp-in-string \"x*\" \"-\" \"abc\")", "-a-b-c"));
	CHECK(eval_eq("(replace-regexp-in-string \"b\" "
		      "(lambda (m) (upcase m)) \"abc\")",
	    "aBc"));

	/* The engine's too-complex status reaches string-match as the
	 * distinct error it is, never folded into "no match" -- the same
	 * property test_regex_too_complex_and_bad_pattern pins for the
	 * buffer search, over the same catastrophic-backtracking shape. */
	{
		char source[256];

		fe_regex_source(source, sizeof(source),
		    "(string-match \"%s\" \"aaaaaaaaaaaaaaaaaaaaaaaa\")",
		    "\\(a*\\)*b");
		CHECK(eval_error_contains(source, "too complex"));
	}

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase15_string_library(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(split-string \"  a b\tc  \")", "(\"a\" \"b\" \"c\")"));
	CHECK(eval_eq(
	    "(split-string \"a,b,,c\" \",\")", "(\"a\" \"b\" \"\" \"c\")"));
	CHECK(eval_eq(
	    "(split-string \"a,b,,c\" \",\" t)", "(\"a\" \"b\" \"c\")"));
	CHECK(eval_eq("(list (split-string \"\") (split-string \"\" \",\"))",
	    "(nil (\"\"))"));
	/* The empty-match guard: without it "x*" never advances. */
	CHECK(eval_eq(
	    "(split-string \"abc\" \"x*\")", "(\"\" \"a\" \"b\" \"c\" \"\")"));

	CHECK(eval_eq("(string-join (list \"a\" \"b\") \"-\")", "a-b"));
	CHECK(eval_eq("(string-join (list \"a\" \"b\"))", "ab"));

	CHECK(eval_eq("(string-trim \"  a b  \")", "a b"));
	CHECK(eval_eq("(string-trim \"\t\n a \r\n\")", "a"));
	CHECK(eval_eq("(string-trim-left \"  a \")", "a "));
	CHECK(eval_eq("(string-trim-right \" a  \")", " a"));
	/* Emacs' REGEXP argument is refused by name, not ignored. */
	CHECK(eval_error_contains(
	    "(string-trim \"xax\" \"x+\")", "REGEXP argument is unsupported"));

	CHECK(eval_eq("(list (string-prefix-p \"ab\" \"abc\") "
		      "(string-prefix-p \"abc\" \"ab\") "
		      "(string-prefix-p \"AB\" \"abc\" t))",
	    "(t nil t)"));
	CHECK(eval_eq("(list (string-suffix-p \"bc\" \"abc\") "
		      "(string-suffix-p \"abc\" \"bc\") "
		      "(string-suffix-p \"BC\" \"abc\" t))",
	    "(t nil t)"));

	/* `string<' and `string>' are fe primitives since Phase 20 and were
	 * prelude Lisp before it; the answers are unchanged and this is
	 * where kg asks for them through its own stack. */
	CHECK(eval_eq("(list (string< \"a\" \"b\") (string< \"a\" \"a\") "
		      "(string< \"ab\" \"a\") (string< \"A\" \"a\") "
		      "(string< 'abc \"abd\"))",
	    "(t nil nil t t)"));
	CHECK(eval_eq("(list (string> \"b\" \"a\") (string> \"a\" \"b\") "
		      "(string> 'b 'a))",
	    "(t nil t)"));
	/* Codepoints, not bytes: a multi-byte character sorts above every
	 * ASCII one because its first byte does. */
	CHECK(eval_eq("(string< \"z\" \"\xc3\xa9\")", "t"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase15_list_library(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(list (cdar '((1 2) 3)) (caddr '(1 2 3 4)) "
		      "(cdddr '(1 2 3 4)) (cadddr '(1 2 3 4)))",
	    "((2) 3 (4) 4)"));

	CHECK(eval_eq("(list (elt '(1 2 3) 1) (elt \"abc\" 1) "
		      "(elt '(1 2) 5))",
	    "(2 98 nil)"));
	CHECK(eval_error_contains("(elt \"ab\" 5)", "Args out of range"));

	CHECK(eval_eq("(list (butlast '(1 2 3)) (butlast '(1 2 3) 2) "
		      "(butlast '(1)) (butlast nil) (butlast '(1 2 3) -1))",
	    "((1 2) (1) nil nil (1 2 3))"));
	CHECK(eval_eq("(let ((l (list 1 2))) (list (eq l (copy-sequence l)) "
		      "(equal l (copy-sequence l)) (copy-sequence \"ab\")))",
	    "(nil t \"ab\")"));
	CHECK(eval_eq("(list (number-sequence 1 5 2) (number-sequence 5 1) "
		      "(number-sequence 3))",
	    "((1 3 5) nil (3))"));
	CHECK(eval_error_contains(
	    "(number-sequence 1 5 0)", "increment can not be zero"));

	/* nconc and mapcan are destructive, which is the whole reason
	 * nconc exists here: mapcan over it agrees with Emacs even when a
	 * function returns a list the caller still holds. */
	CHECK(eval_eq("(let ((a (list 1)) (b (list 2))) "
		      "(list (nconc a b) a))",
	    "((1 2) (1 2))"));
	CHECK(eval_eq("(mapcan (lambda (x) (list x x)) '(1 2))", "(1 1 2 2)"));
	CHECK(eval_eq("(condition-case e (nconc 1 (list 2)) (error e))",
	    "(wrong-type-argument listp 1)"));

	CHECK(eval_eq("(let ((l (list (cons 'a 1) (cons 'b 2) (cons 'a 3)))) "
		      "(list (assq-delete-all 'a l) l))",
	    "(((b . 2)) ((a . 1) (b . 2)))"));
	CHECK(eval_eq("(list (alist-get 'a '((a . 1))) "
		      "(alist-get 'z '((a . 1))) (alist-get 'z '((a . 1)) 9))",
	    "(1 nil 9)"));
	CHECK(eval_eq("(list (plist-get '(:a 1 :b 2) :b) "
		      "(plist-get '(:a 1) :z) (plist-get nil :z) "
		      "(plist-get '(:a) :a))",
	    "(2 nil nil nil)"));
	CHECK(eval_eq("(let ((l (list :a 1))) (list (plist-put l :b 2) l))",
	    "((:a 1 :b 2) (:a 1 :b 2))"));

	/* Emacs' predicate for a non-sequence is sequencep; a dotted tail
	 * is still listp, about the tail, on both sides. */
	CHECK(eval_eq("(list (length \"abc\") (length '(1 2)))", "(3 2)"));
	CHECK(eval_eq("(condition-case e (length 5) (error e))",
	    "(wrong-type-argument sequencep 5)"));
	CHECK(eval_eq("(condition-case e (length '(1 . 2)) (error e))",
	    "(wrong-type-argument listp 2)"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase15_sort(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* The property that decides the implementation: Emacs 31.0.90 sorts
	 * a list by moving VALUES between the cells it already has, so the
	 * head cons keeps its identity and a cell the caller held
	 * separately sees a different value.  A relinking merge sort would
	 * leave `x' pointing at a suffix, and a copying one would leave it
	 * unsorted; both were measured against this shape before the
	 * setcar-writeback was written. */
	CHECK(eval_eq("(let* ((x (list 3 1 2)) (y (sort x '<))) "
		      "(list y x (eq y x)))",
	    "((1 2 3) (1 2 3) t)"));
	CHECK(eval_eq("(let* ((c (list 2)) (x (cons 3 c)) (y (sort x '<))) "
		      "(list y x c (eq y x)))",
	    "((2 3) (2 3) (3) t)"));

	CHECK(eval_eq(
	    "(sort (list \"b\" \"a\" \"C\") 'string<)", "(\"C\" \"a\" \"b\")"));
	CHECK(eval_eq("(list (sort nil '<) (sort (list 1) '<))", "(nil (1))"));
	/* Stable: equal keys keep their input order. */
	CHECK(eval_eq("(sort (list (cons 1 'a) (cons 1 'b) (cons 0 'c)) "
		      "(lambda (u v) (< (car u) (car v))))",
	    "((0 . c) (1 . a) (1 . b))"));
	/* A long enough list to run several bottom-up passes. */
	CHECK(eval_eq(
	    "(sort (list 9 3 7 1 8 2 6 4 5 0) '<)", "(0 1 2 3 4 5 6 7 8 9)"));

	CHECK(
	    eval_eq("(internal--merge (list 1 3) (list 2 4) '<)", "(1 2 3 4)"));
	CHECK(eval_eq("(internal--merge-pairs "
		      "(list (list 1) (list 0) (list 2)) '<)",
	    "((0 1) (2))"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 29's U.1a: the three library surfaces the demand census named.
 * Every value asserted here is Emacs 31.0.91's own, measured, and
 * recorded in test/lisp-compat/cases/u0-subr-x-*.json and
 * u1a-subr-x-*.json. */
static void test_phase29_subr_x(void)
{
	CHECK(kg_lisp_init() == 0);

	/* M0.1 retraction: `subr-x' is no longer advertised as a feature.
	 * The string/threading names stay callable (defined directly in the
	 * prelude), but `(require 'subr-x)' must now fail loudly instead of
	 * promising a contract kg does not implement, and `featurep' agrees.
	 */
	CHECK(eval_eq("(featurep 'subr-x)", "nil"));
	CHECK(eval_eq(
	    "(condition-case e (require 'subr-x) (file-missing (car e)))",
	    "file-missing"));

	/* string-blank-p answers a MATCH POSITION, not t. */
	CHECK(eval_eq("(list (string-blank-p \"  \") (string-blank-p \"\") "
		      "(string-blank-p \"\\n\\t\") (string-blank-p \" a \") "
		      "(string-blank-p \"a\"))",
	    "(0 0 0 nil nil)"));
	/* A prefix or suffix that does not match answers the STRING. */
	CHECK(eval_eq("(list (string-remove-prefix \"ab\" \"abc\") "
		      "(string-remove-prefix \"z\" \"abc\") "
		      "(string-remove-prefix \"\" \"abc\") "
		      "(string-remove-suffix \"bc\" \"abc\") "
		      "(string-remove-suffix \"z\" \"abc\"))",
	    "(\"c\" \"abc\" \"abc\" \"a\" \"abc\")"));
	/* string-pad pads right, or left under START, and never truncates. */
	CHECK(eval_eq("(list (string-pad \"a\" 3) (string-pad \"abcd\" 3) "
		      "(string-pad \"a\" 3 ?x) (string-pad \"a\" 3 nil t) "
		      "(string-pad \"\" 2))",
	    "(\"a  \" \"abcd\" \"axx\" \"  a\" \"  \")"));
	/* Any run of whitespace collapses, newlines included. */
	CHECK(eval_eq("(list (string-clean-whitespace \"  a   b  \") "
		      "(string-clean-whitespace \"a\\n\\nb\") "
		      "(string-clean-whitespace \"\"))",
	    "(\"a b\" \"a b\" \"\")"));
	/* The threading pair differs in WHERE the value goes, which is why
	 * the steps below are not commutative; a bare symbol step is a
	 * one-argument call for both. */
	CHECK(eval_eq("(list (thread-first 5 (- 1) (* 2)) "
		      "(thread-last 5 (- 1) (* 2)) (thread-first 5 - (* 2)) "
		      "(thread-last 5 - (* 2)) (thread-first 5))",
	    "(8 -8 -10 -10 5)"));
	CHECK(eval_eq("(list (thread-first (list 1 2) (append (list 3))) "
		      "(thread-last (list 1 2) (append (list 3))))",
	    "((1 2 3) (3 1 2))"));

	kg_lisp_shutdown();
}

/* The names a package file reaches before any of its own code runs.
 * Recorded in test/lisp-compat/cases/u1a-static-if-*.json,
 * u1a-eval-when-compile-*.json, u1a-make-obsolete-variable-storage.json,
 * u1a-emacs-major-version-is-an-integer.json and the
 * u1a-lexical-binding-* pair. */
static void test_phase29_package_preamble_names(void)
{
	CHECK(kg_lisp_init() == 0);

	/* kg's evaluator IS lexical, so lexical-binding is t -- the
	 * truthful value, measured by the closure probe in
	 * test_phase11_dynamic_binding -- and it is special.  nil was a
	 * lie the other way: it told every probe that closures close over
	 * nothing while they went on closing. */
	CHECK(eval_eq("(list lexical-binding (boundp 'lexical-binding) "
		      "(special-variable-p 'lexical-binding) "
		      "(default-value 'lexical-binding))",
	    "(t t t t)"));
	/* The closure probe beside the claim: the advertised dialect and
	 * the observed one cannot drift apart again. */
	CHECK(eval_eq("(let ((n 7)) "
		      "(funcall (funcall (lambda () (lambda () n)))))",
	    "7"));
	CHECK(eval_error_contains(
	    "(eval (list '+ 1 2) lexical-binding)", "eval lexical argument"));

	/* emacs-major-version WAS BOUND TO 31 AND IS GONE: a version claim
	 * is a capability promise, and packages read 31 as "modern code
	 * paths are safe" for capabilities kg does not have.  A package
	 * that needs the name gets void-variable, which names the gap. */
	CHECK(eval_eq("(condition-case e emacs-major-version "
		      "(error (car e)))",
	    "void-variable"));

	/* make-obsolete-variable is storage: (CURRENT-NAME ACCESS-TYPE WHEN),
	 * which is not the argument order, and it answers OBSOLETE-NAME. */
	CHECK(eval_eq("(list (make-obsolete-variable 'ob-v 'new-v \"1.0\") "
		      "(get 'ob-v 'byte-obsolete-variable))",
	    "(ob-v (new-v nil \"1.0\"))"));
	CHECK(eval_eq("(progn (make-obsolete-variable 'ob-w 'new-w \"1.0\" "
		      "'set) (get 'ob-w 'byte-obsolete-variable))",
	    "(new-w set \"1.0\")"));

	/* static-if evaluates its condition at EXPANSION time and does not
	 * expand the branch it did not take -- an (error ...) in either arm
	 * is harmless. */
	CHECK(eval_eq("(list (static-if (fboundp 'car) 'yes 'no) "
		      "(static-if nil (error \"boom\") 1) "
		      "(static-if t 1 (error \"boom\")) (static-if nil 1))",
	    "(yes 1 1 nil)"));
	CHECK(eval_eq("(list (macroexpand '(static-if t 1 2)) "
		      "(macroexpand '(static-if nil 1 2 3)))",
	    "(1 (progn 2 3))"));

	/* eval-when-compile and eval-and-compile EVALUATE their bodies and
	 * answer the value quoted -- identically, in an interpreter. */
	CHECK(eval_eq("(list (eval-when-compile 1 2 3) "
		      "(eval-and-compile 4 5 6))",
	    "(3 6)"));
	CHECK(eval_eq("(progn (eval-when-compile (setq ewc-ran 'yes)) "
		      "(list (boundp 'ewc-ran) ewc-ran))",
	    "(t yes)"));
	CHECK(eval_eq(
	    "(progn (eval-and-compile (setq eac-ran 'yes)) eac-ran)", "yes"));

	kg_lisp_shutdown();
}

/* The cl-lib slice, and the policy behind it: the FEATURE claim is
 * RETRACTED -- `(require 'cl-lib)' raises `file-missing' again, because a
 * provided feature is a capability promise and inheritenv proved it
 * false by loading past the require and dying at `cl-letf*'.  The three
 * implemented names stay, unadvertised.  Recorded in
 * test/lisp-compat/cases/u1a-cl-*.json. */
static void test_phase29_cl_lib(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(featurep 'cl-lib)", "nil"));
	CHECK(eval_eq("(condition-case e (require 'cl-lib) "
		      "(file-missing (car e)))",
	    "file-missing"));

	/* cl-incf answers the NEW value; the default increment is 1; and a
	 * THIRD argument is wrong-number-of-arguments -- measured on Emacs
	 * 31.0.91 -- which the expansion checks rather than silently
	 * ignoring as it first did. */
	CHECK(eval_eq("(list (let ((x 1)) (list (cl-incf x) x)) "
		      "(let ((x 1)) (list (cl-incf x 5) x)) "
		      "(let ((x 1.0)) (cl-incf x)))",
	    "((2 2) (6 6) 2.0)"));
	CHECK(eval_error_contains(
	    "(let ((x 1)) (cl-incf x 1 2))", "Wrong number of arguments"));
	/* A generalised place is refused at expansion, by name. */
	CHECK(eval_error_contains(
	    "(macroexpand '(cl-incf (nth 0 lst)))", "must be a variable name"));

	/* Every clause shape, including the two that pin the comparison as
	 * `eql': a string key does not match an equal string, a float does. */
	CHECK(eval_eq("(list (cl-case 2 (1 'one) (2 'two) (t 'other)) "
		      "(cl-case 9 (1 'one) (t 'other)) (cl-case 9 (1 'one)) "
		      "(cl-case 'b ((a b) 'ab) (t 'no)) "
		      "(cl-case 3 (otherwise 'ow)) (cl-case 1 (1)) "
		      "(cl-case \"a\" (\"a\" 'str) (t 'no)) "
		      "(cl-case 2.0 (2.0 'flt) (t 'no)))",
	    "(two other nil ab ow nil no flt)"));
	/* The key form is evaluated ONCE however many clauses are tested. */
	CHECK(eval_eq("(progn (setq cl-side 0) "
		      "(list (cl-case (progn (setq cl-side (+ cl-side 1)) 2) "
		      "(1 'one) (2 'two)) cl-side))",
	    "(two 1)"));
	/* The body sees the surrounding bindings: the temporary the
	 * expansion binds is a gensym, so it shadows nothing. */
	CHECK(eval_eq("(let ((v 5)) (cl-case 2 (2 v)))", "5"));

	CHECK(eval_eq("(list (cl-find-if (lambda (x) (< 2 x)) '(1 2 3 4)) "
		      "(cl-find-if (lambda (x) (< 9 x)) '(1 2 3 4)) "
		      "(cl-find-if 'stringp (list 1 \"a\" 2)))",
	    "(3 nil \"a\")"));

	/* THE POLICY: a cl- name outside the subset is void-function at the
	 * CALL -- but the require above no longer succeeds, so the
	 * diagnostic a package meets first is `file-missing' at the
	 * require, which is the honest one. */
	CHECK(eval_eq("(condition-case e (cl-loop for x in '(1 2) collect x) "
		      "(error (car e)))",
	    "void-function"));

	kg_lisp_shutdown();
}

static void test_phase15_seq_shim(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(seq-map '1+ '(1 2))", "(2 3)"));
	CHECK(eval_eq("(seq-filter (lambda (x) (< 1 x)) '(1 2 3))", "(2 3)"));
	CHECK(eval_eq("(seq-remove (lambda (x) (< 1 x)) '(1 2 3))", "(1)"));
	CHECK(eval_eq("(list (seq-find (lambda (x) (< 1 x)) '(1 2 3)) "
		      "(seq-find (lambda (x) nil) '(1 2)) "
		      "(seq-find (lambda (x) nil) '(1 2) 'none))",
	    "(2 nil none)"));
	/* seq-some answers the first non-nil RESULT, not the element. */
	CHECK(eval_eq("(seq-some (lambda (x) (if (< 1 x) (* x 10) nil)) "
		      "'(1 2 3))",
	    "20"));
	CHECK(eval_eq("(list (seq-take '(1 2 3) 2) (seq-take '(1 2) 5) "
		      "(seq-take '(1 2) 0))",
	    "((1 2) (1 2) nil)"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase15_arithmetic(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(list (abs -3) (abs 3) (abs -3.5))", "(3 3 3.5)"));
	/* -0.0 is not less than zero, so the non-negative arm has to
	 * normalise the sign; (+ n 0) is what does it. */
	CHECK(eval_eq("(list (abs -0.0) (abs 0.0))", "(0.0 0.0)"));
	CHECK(eval_eq("(condition-case e (abs \"x\") (error e))",
	    "(wrong-type-argument numberp \"x\")"));

	CHECK(eval_eq(
	    "(list (% 7 2) (% -7 2) (% 7 -2) (% -7 -2))", "(1 -1 1 -1)"));
	CHECK(eval_eq("(list (mod 7 2) (mod -7 2) (mod 7 -2) (mod -7 -2))",
	    "(1 1 -1 -1)"));
	CHECK(eval_eq("(mod 5.5 2)", "1.5"));
	CHECK(eval_eq("(condition-case e (% 7.5 2) (error e))",
	    "(wrong-type-argument integer-or-marker-p 7.5)"));
	CHECK(eval_error_contains("(% 7 0)", "Arithmetic error"));

	CHECK(eval_eq("(list (ash 1 4) (ash 16 -2) (ash -16 -2) (ash 1 0) "
		      "(ash 0 5))",
	    "(16 4 -4 1 0)"));
	CHECK(eval_eq("(ash 1 62)", "4611686018427387904"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 06E.  Hook containment swallows a hook's *error* and carries on
 * with the next hook; it must not swallow a quit, or C-g would be eaten by
 * whichever hook happened to be running when it arrived.  The quit is put
 * back in flight with FeResignal and cancels the whole evaluation. */
static void test_hook_quit_is_not_contained(void)
{
	char result[128] = "";
	static const char *src = "(progn (run-hooks 'spin-hook) 'survived)";

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq after-ran nil)"));
	CHECK(eval_ok("(add-hook 'spin-hook (lambda () (while t 1)))"));
	CHECK(eval_ok("(add-hook 'spin-hook (lambda () (setq after-ran t)))"));

	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(
	    kg_lisp_eval_string(src, strlen(src), result, sizeof(result)) != 0);
	kg_lisp_set_interrupt_check(nullptr);
	/* Reported as the cancellation it is, not as a hook error, and the
	 * hooks after it never ran. */
	CHECK(strcmp(result, "Quit") == 0);
	CHECK(strstr(test_status_message, "Hook error") == nullptr);
	CHECK(eval_eq("after-ran", "nil"));
	/* The interpreter is still usable afterwards. */
	CHECK(eval_eq("(+ 1 1)", "2"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_hook_throw_containment(void)
{
	char result[128] = "";
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(
	    eval_ok("(add-hook 'my-test-hook (lambda () (throw 'tag 'val)))"));
	const char *src = "(catch 'tag (run-hooks 'my-test-hook))";
	int rc = kg_lisp_eval_string(src, strlen(src), result, sizeof(result));

	/* The hook's throw cannot reach the catch outside (run-hooks): the
	 * protected call's barrier is a catch wall, so the throw finds no
	 * catch inside the hook's own run and is contained as no-catch.  The
	 * enclosing evaluation survives -- it is the whole point of hook
	 * containment -- and the status line says which hook failed and
	 * why. */
	CHECK(rc == 0);
	CHECK(strcmp(result, "nil") == 0);
	CHECK(strstr(test_status_message, "Hook error (my-test-hook)")
	    != nullptr);
	CHECK(strstr(test_status_message, "no-catch") != nullptr);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_signal(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* signal arith-error caught by arith-error handler */
	CHECK(eval_eq("(condition-case e (signal 'arith-error '(1)) "
		      "(arith-error (car e)))",
	    "arith-error"));

	/* signal arith-error caught by parent error handler */
	CHECK(eval_eq(
	    "(condition-case e (signal 'arith-error '(1)) (error (car e)))",
	    "arith-error"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_ignore_errors(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* ignore-errors returns value on success, nil on error */
	CHECK(eval_eq("(ignore-errors (+ 1 2))", "3"));
	CHECK(eval_eq("(ignore-errors (/ 1 0))", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_condition_case_reentry(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* condition-case inside hook inside condition-case */
	CHECK(eval_ok("(add-hook 'before-save-hook "
		      "  (lambda () (condition-case nil (/ 1 0) (error "
		      "'hook-caught))))"));
	CHECK(eval_eq("(condition-case nil (progn (run-hooks "
		      "'before-save-hook) 'ok) (error 'outer-caught))",
	    "ok"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The in-flight condition survives a containment inside a cleanup drain.
 * kg's `run-hooks' contains each hook function's completion (a failing
 * hook is reported, not propagated), and that containment publishes the
 * contained condition in the very context field the completion being
 * unwound is using.  When the drain is an `unwind-protect' cleanup that
 * kg's own error is passing through, an enclosing `condition-case' used to
 * bind the HOOK's condition object instead of the body's -- an error whose
 * message said one thing and whose object said another.  Fixed in fe at
 * this pin ("Hold the completion in flight across a cleanup drain"); this
 * is kg's half of the evidence, since kg is where a containment inside a
 * cleanup is reachable from ordinary Lisp. */
static void test_cleanup_containment_keeps_condition(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(add-hook 'before-save-hook (lambda () (car 6)))"));
	/* Body raises arith-error; the cleanup contains a
	 * wrong-type-argument.  The handler must see the body's. */
	CHECK(eval_eq("(condition-case e (unwind-protect (/ 1 0) "
		      "(run-hooks 'before-save-hook)) (error (car e)))",
	    "arith-error"));
	/* The cleanup still runs, and the contained hook error is still
	 * contained rather than escaping. */
	CHECK(eval_eq("(condition-case e (unwind-protect 'body "
		      "(run-hooks 'before-save-hook)) (error (car e)))",
	    "body"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 12D Part 1: what the fe pin's cleanup-handler fix changes,
 * asserted in-process.  A `condition-case' or `ignore-errors' established
 * INSIDE an `unwind-protect' cleanup now handles what that cleanup raises;
 * before the pin every raise inside a running cleanup behaved as unhandled
 * because RaiseCompletionCore tested ctx->cleanup_catch before it ever ran
 * the handler search.  The last three assertions are 06A Decision 4's
 * guard, which 12A Decision 2 requires the fix to leave byte-identical: an
 * UNHANDLED cleanup raise still replaces the completion being unwound. */
static void test_phase12_cleanup_handler_visibility(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Audit probe B9: no unwind in flight at all. */
	CHECK(
	    eval_eq("(unwind-protect 'body (ignore-errors (car 6)))", "body"));
	/* A1: the cleanup handles its own error while an arith-error is
	 * being unwound; the in-flight condition survives. */
	CHECK(eval_eq("(condition-case e (unwind-protect (/ 1 0)"
		      " (ignore-errors (car 6))) (error (car e)))",
	    "arith-error"));
	/* A7b: the cleanup's own handler binds the cleanup's error. */
	CHECK(eval_eq("(let ((m 'unset)) (list (condition-case e"
		      " (unwind-protect (/ 1 0) (condition-case c (car 6)"
		      " (error (setq m c)))) (error (car e))) m))",
	    "(arith-error (wrong-type-argument listp 6))"));
	/* A3c: the same, while a throw rather than an error unwinds. */
	CHECK(eval_eq("(let ((m 'unset)) (list (catch 'tg (unwind-protect"
		      " (throw 'tg 99) (setq m (ignore-errors (car 6))))) m))",
	    "(99 nil)"));

	/* 06A Decision 4, preserved: unhandled still replaces. */
	CHECK(eval_eq("(condition-case e (unwind-protect (/ 1 0) (car 6))"
		      " (error (car e)))",
	    "wrong-type-argument"));
	/* And a cleanup that throws still abandons the error (A16). */
	CHECK(eval_eq("(catch 'ct (condition-case e (unwind-protect (/ 1 0)"
		      " (throw 'ct 'from-cleanup)) (error 'handled)))",
	    "from-cleanup"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_quit_uncaught(void)
{
	char result[128] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* signal quit is caught by quit handler, but NOT by error handler */
	CHECK(
	    eval_eq("(condition-case e (signal 'quit nil) (quit 'caught-quit))",
		"caught-quit"));

	const char *src
	    = "(condition-case e (signal 'quit nil) (error 'caught-error))";
	CHECK(
	    kg_lisp_eval_string(src, strlen(src), result, sizeof(result)) != 0);
	CHECK(strcmp(result, "Quit") == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* Top-level (defalias 'NAME ...) forms in lisp/prelude.el: 77 through
 * Phase 12's close, +3 at its fix cycle's loader rebuild (load, require,
 * internal--load-loop), +2 for the line-motion pair
 * (move-beginning-of-line, move-end-of-line), +40 for Phase 15's string
 * and list library, +3 for Phase 17's with-temp-buffer and the
 * beginning-of-buffer/end-of-buffer pair, +1 for Phase 19's
 * internal--defined-names, and net +1 at Phase 20 (-1 for `string<',
 * which became an fe primitive, +2 for `documentation-property' and its
 * `internal--variable-doc-put' helper). */
/* Phase 2 of doc/plans/2026-08-14-embedded-prelude.md moved seven names
 * (listp, reverse, internal--bind-name, internal--bind-value,
 * internal--doc-put, internal--variable-doc-put, nconc) from
 * lisp/prelude.el defalias forms to natives (src/lisp_cmd.c), so the
 * source file's own top-level defalias count fell 129 -> 122; Phase A of
 * doc/plans/2026-08-19-fe-simplification-and-cheap-compat.md removed
 * internal--make-deferred-stub with the rest of the deferral machinery,
 * taking it to 121, and that plan's Phase C1 added the no-op `autoload'
 * macro, taking it to 122.  Phase 24.2 added four: the two captured fe
 * primitives kg wraps (internal--fe-length, internal--fe-elt) and the two
 * helpers the sequence generalisation is built from
 * (internal--seq-to-list, internal--equal-vectors), taking it to 126.
 * Phase 26.2 added `count-matches' and `replace-match', the two missing
 * NAMES Phase 25's honest frontier measured unmodified s.el stopping at,
 * both prelude Lisp because both are ordinary Lisp over what kg already
 * had.  The frontier demand phase added `assoc-string' and the one
 * internal name under it, `internal--string-designator', and then
 * `multibyte-string-p' and `regexp-opt', taking it to 139.  Phase 29's
 * U.1a added the docstring-taking `defalias', a prelude SHADOW of fe's
 * two-argument primitive placed below every definition in the file so
 * the prelude's own forms stay on the primitive, taking it to 140, and
 * then the three library surfaces the demand census named: subr-x's
 * eight (string-blank-p, string-remove-prefix, string-remove-suffix,
 * string-pad, string-clean-whitespace, internal--thread, thread-first,
 * thread-last), the four package-preamble names that are functions or
 * macros (make-obsolete-variable, static-if, eval-when-compile,
 * eval-and-compile -- `lexical-binding' is a variable and is not counted
 * here; `emacs-major-version' was one too and is gone), and cl-lib's
 * four (cl-incf, internal--cl-case-test, cl-case, cl-find-if), taking it
 * to 156.  The external-review correctness tranche added
 * `internal--doc-invalidate', the registry half of defalias's redefined
 * definition taking its stale documentation with it, taking it to 157. */
#define PRELUDE_DEFS 157

/* What a value CAPTURED out of a symbol's function cell is: that symbol's
 * definition at the moment of capture, and it stays that definition
 * however often the name is redefined afterwards; the name, meanwhile,
 * means whatever was defined last.  Emacs decides both halves, and both
 * are recorded against it (test/lisp-compat/cases/prelude-capture-
 * redefine.json and prelude-capture-force-redefine.json); this is the
 * same pair in-process, plus the cell-level assertions a printed value
 * cannot make.
 *
 * kg's prelude defines every name eagerly, so all of this is what an
 * ordinary `defalias' does and none of it needs a mechanism to be true.
 * It is asserted anyway, because it was NOT true here: finding 1 of
 * doc/reviews/2026-08-19-embedded-prelude-phase21-adversarial-review.md
 * caught a lazily-installed forwarding stub in these cells getting both
 * halves wrong.  Phase A of doc/plans/2026-08-19-fe-simplification-and-
 * cheap-compat.md deleted the deferral outright; this unit is what stops
 * a function cell holding a forwarder again without anyone noticing.
 *
 * Two names rather than one process each: `mapcar' is captured before it
 * has ever been called and `1-' after, which are the two orders the
 * defect had different wrong answers for. */
static void test_captured_function_value(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Captured, then redefined, then called: the capture answers the
	 * prelude's `mapcar' and the redefinition survives being called
	 * through.  kg answered ((2 3) (2 3)) here while the cell held a
	 * stub, which wrote the cell whatever it already held and so
	 * silently destroyed the line above it. */
	CHECK(eval_eq("(let ((saved (symbol-function 'mapcar)))"
		      "  (defalias 'mapcar"
		      "    (lambda (function sequence) 'replacement))"
		      "  (list (funcall saved '1+ '(1 2))"
		      "        (mapcar '1+ '(1 2))))",
	    "((2 3) replacement)"));
	/* ... and the redefinition is still in the cell afterwards, which
	 * is the half the printed value above cannot show on its own. */
	CHECK(eval_eq("(mapcar '1+ '(1 2))", "replacement"));

	/* Captured, CALLED, then redefined, then called again: a captured
	 * value reads no cell, so the second call answers the same
	 * definition as the first. */
	CHECK(eval_eq("(let ((saved (symbol-function '1-)))"
		      "  (list (funcall saved 5)"
		      "        (progn (defalias '1- (lambda (n) 'replacement))"
		      "               (funcall saved 5))"
		      "        (1- 5)))",
	    "(4 4 replacement)"));

	/* And the cell itself is STABLE across a call: calling a prelude
	 * name by name leaves its function cell holding the same object it
	 * held before, so a value captured either side of the call is the
	 * same value.  This is the assertion that inverted when the cell
	 * held a self-replacing stub -- `eq' answered nil there, because
	 * the first call swapped the stub out for the real closure -- and
	 * it is the cheapest direct statement that nothing lazy lives in
	 * these cells any more. */
	CHECK(eval_ok("(setq captured-before (symbol-function 'string-join))"));
	CHECK(eval_eq("(string-join (list \"a\" \"b\") \"-\")", "a-b"));
	CHECK(eval_ok("(setq captured-after (symbol-function 'string-join))"));
	CHECK(eval_eq("(eq captured-before captured-after)", "t"));
	CHECK(eval_eq(
	    "(funcall captured-before (list \"a\" \"b\") \"-\")", "a-b"));
	CHECK(eval_eq(
	    "(funcall captured-after (list \"a\" \"b\") \"-\")", "a-b"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Phase 2 of doc/plans/2026-08-14-embedded-prelude.md moved `reverse' from
 * a Lisp `while' loop to a native.  A native's own C locals are not part
 * of any `FeEvalFrame' `FeMarkEvaluatorRoots' walks, so they stay live
 * only through the small, fixed-size root stack `FePushGC'/`FeRestoreGC'
 * manage (`GcStackSize' 4096, `GcStackReserve' 64 held back for the
 * ordinary completion ceiling -- fe/fe_internal.h) -- and `MakeObject()'
 * pushes every single allocation onto it.  A loop that conses once per
 * input element and never restores the checkpoint therefore roots one
 * slot per element, forever, and dies around 4032 -- a limit the Lisp
 * `while' loop this replaced never had, because a *Lisp* loop's
 * accumulator lives in a `let'-bound frame slot, marked directly by
 * `FeMarkEvaluatorRoots' rather than by the root stack, so it costs the
 * root stack nothing per iteration.  5000 is chosen well past 4032 for
 * the same reason test_arena_exhaustion_conditions' own reader-depth case
 * picks 8192 over 4096: past the ceiling, not merely at it, so a future
 * off-by-one in whatever fix this pins does not slip through by landing
 * exactly on the edge.
 * `mapcar' is asserted too, unprompted by anything but its own body
 * ending in `(reverse res)' -- the same fix and the same test therefore
 * cover the most heavily used name in the language, not just the one
 * this phase edited directly. */
static void test_native_reverse_gc_stack(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq p2-big nil)"));
	CHECK(eval_ok("(setq p2-i 0)"));
	CHECK(eval_ok("(while (< p2-i 5000)"
		      " (setq p2-big (cons p2-i p2-big))"
		      " (setq p2-i (+ p2-i 1)))"));
	CHECK(eval_eq("(length p2-big)", "5000"));
	CHECK(eval_eq("(length (reverse p2-big))", "5000"));
	/* The reversed list's own first and last elements, not just its
	 * length: a native that rooted nothing at all would still answer
	 * a 5000-long list of garbage without raising. */
	CHECK(eval_eq("(car (reverse p2-big))", "0"));
	CHECK(eval_eq("(car (last (reverse p2-big)))", "4999"));
	CHECK(eval_eq("(length (mapcar '1+ p2-big))", "5000"));
	CHECK(eval_eq("(car (mapcar '1+ p2-big))", "5000"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_prelude_source_file(void)
{
	static char names[PRELUDE_DEFS][64];
	static char types[PRELUDE_DEFS][16];
	/* Two things bound this: the file has to fit whole (the CHECK
	 * below), and it is `static' rather than automatic because a
	 * 128 KiB frame is not something to put on the C stack.  The
	 * prelude passed 64 KiB at Phase 20; it is 74 KiB there. */
	static char text[131072];
	const char *paths[] = { "lisp/prelude.el", "../lisp/prelude.el" };
	FILE *fp = nullptr;
	size_t len = 0, count = 0, i, let_index = PRELUDE_DEFS;
	char *line;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fp = fopen(paths[i], "r");
		if (fp != nullptr) {
			break;
		}
	}
	CHECK(fp != nullptr);
	if (fp == nullptr) {
		return;
	}
	len = fread(text, 1, sizeof(text) - 1, fp);
	fclose(fp);
	text[len] = '\0';
	/* The whole file has to have fit, or the scan below reports a short
	 * count for a reason that has nothing to do with the prelude. */
	CHECK(len > 0 && len < sizeof(text) - 1);

	/* A definition is "(defalias 'NAME " in column 0; every continuation
	 * line in the file is indented, so nothing nested is ever found. */
	for (line = text; line != nullptr; line = strchr(line, '\n')) {
		const char *name_start, *value_start;
		size_t name_len;

		if (line[0] == '\n') {
			line++;
		}
		if (strncmp(line, "(defalias '", 11) != 0) {
			continue;
		}
		CHECK(count < PRELUDE_DEFS);
		if (count >= PRELUDE_DEFS) {
			break;
		}
		name_start = line + 11;
		name_len = strcspn(name_start, " )\n");
		CHECK(name_len > 0 && name_len < sizeof(names[0]));
		memcpy(names[count], name_start, name_len);
		names[count][name_len] = '\0';

		/* The declared shape decides the type the editor must report:
		 * a lambda form is a lambda, a macro form is a macro, and the
		 * four (symbol-function 'PRIM) forms are aliases of Fe
		 * primitives. */
		value_start = name_start + name_len;
		value_start += strspn(value_start, " ");
		if (strncmp(value_start, "(lambda", 7) == 0) {
			strcpy(types[count], "lambda");
		} else if (strncmp(value_start, "(macro", 6) == 0) {
			strcpy(types[count], "macro");
		} else if (strncmp(value_start, "(symbol-function '", 18)
		    == 0) {
			/* An ALIAS captures whatever the name it copies was
			 * bound to, and kg's function surface has two
			 * implementations -- fe primitives and kg natives --
			 * so this shape does not decide which.  The check
			 * below accepts either and nothing else; what stays
			 * pinned is that the value IS one of them, so a
			 * `defalias' that started capturing a lambda still
			 * fails here. */
			strcpy(types[count], "alias");
		} else {
			/* Not a fallback: those three are the whole
			 * vocabulary this file defines things with, and a
			 * fourth shape has to be classified deliberately
			 * rather than silently called a primitive. */
			fprintf(stderr,
			    "prelude definition '%s' has an unrecognised value "
			    "shape: %.40s\n",
			    names[count], value_start);
			CHECK(0);
			break;
		}

		if (strcmp(names[count], "let") == 0) {
			let_index = count;
		}
		count++;
	}

	/* The count is pinned: a definition added or lost without an
	 * accompanying manifest entry is 00C's checker's business, but one
	 * added or lost without anyone noticing at all is this test's. */
	CHECK(count == PRELUDE_DEFS);

	/* Rule 1, structurally: the primitive alias comes first, and the
	 * macro that shadows the name it aliased comes after it. */
	CHECK(strcmp(names[0], "internal--let") == 0);
	CHECK(let_index > 0 && let_index < PRELUDE_DEFS);
	CHECK(strcmp(types[let_index], "macro") == 0);

	CHECK(kg_lisp_init() == 0);
	for (i = 0; i < count; i++) {
		char source[128];

		/* The definitions live in function cells, so the type probe
		 * reads the cell: a bare name in value position is void now. */
		(void)snprintf(source, sizeof(source),
		    "(type-of (symbol-function '%s))", names[i]);
		if (strcmp(types[i], "alias") == 0) {
			if (!eval_eq(source, "primitive")
			    && !eval_eq(source, "native-fn")) {
				fprintf(stderr,
				    "prelude definition %zu (%s): an alias "
				    "captured neither a primitive nor a "
				    "native\n",
				    i, names[i]);
				CHECK(0);
			}
			continue;
		}
		if (!eval_eq(source, types[i])) {
			fprintf(stderr,
			    "prelude definition %zu (%s): expected type %s\n",
			    i, names[i], types[i]);
			CHECK(0);
		}
	}
	/* Rule 1 again, behaviourally this time: internal--let is still Fe's
	 * one-binding primitive, so the list library it carries still works. */
	CHECK(eval_eq("(reverse '(1 2 3))", "(3 2 1)"));

	kg_lisp_shutdown();
}

/* 08A Table C, as GNU Emacs 31.0.90 answers it (/opt-3/emacs-31-lucid,
 * emacs -Q --batch).  fe core owns the refusals; kg's prelude owns the
 * `let' half, because a `let' compiles into a lambda application and fe
 * lets a lambda parameter named `t' shadow -- which Emacs also does for a
 * lambda, and does NOT do for a `let'. */
static void test_phase8_constants_and_keywords(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(condition-case e (setq t nil)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant t)"));
	CHECK(eval_eq("(condition-case e (setq nil 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant nil)"));
	CHECK(eval_eq("(condition-case e (setq :kw 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant :kw)"));
	CHECK(eval_eq("(condition-case e (let ((t 1)) t)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant t)"));
	CHECK(eval_eq("(condition-case e (let ((nil 1)) 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant nil)"));
	CHECK(eval_eq("(condition-case e (let ((:kw 1)) 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant :kw)"));
	/* let* goes through the same binding-name helper. */
	CHECK(eval_eq("(condition-case e (let* ((t 1)) t)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant t)"));
	/* An ordinary binding list still binds. */
	CHECK(eval_eq("(let ((a 1) (b 2)) (+ a b))", "3"));
	CHECK(eval_eq("(let* ((a 1) (b (+ a 1))) b)", "2"));
	/* A lambda parameter named t may shadow -- Emacs answers 1 too. */
	CHECK(eval_eq("((lambda (t) t) 1)", "1"));
	/* Keywords self-evaluate and answer keywordp. */
	CHECK(eval_eq("(list :foo (keywordp :foo) (keywordp 'foo) (eq :a ':a))",
	    "(:foo t nil t)"));
	/* `:' alone is a keyword too, which 08B first guessed it was not. */
	CHECK(eval_eq("(keywordp ':)", "t"));

	kg_lisp_shutdown();
}

/* 08A Table R.  Every accepted row is byte-identical to the pinned
 * oracle's answer; every rejected row is a named read error, which is
 * 08C's "reject rather than misread" rule made checkable. */
static void test_phase8_reader_literals(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(list ?a ?\\n ?\\t ?\\e ?\\\\ ?\\s ?\\d)",
	    "(97 10 9 27 92 32 127)"));
	CHECK(eval_eq("(list ?\\C-a ?\\M-a)", "(1 134217825)"));
	CHECK(eval_eq("?\xc3\xa9", "233"));
	/* The control modifier is not `& 0x1f': ? is DEL, @.._ and a..z
	 * fold, and everything else keeps its value with the 2^26 bit. */
	CHECK(eval_eq(
	    "(list ?\\C-? ?\\C-% ?\\C-\xc3\xa9)", "(127 67108901 67109097)"));
	CHECK(eval_eq("(list #x10 #o17 #b101 #xff)", "(16 15 5 255)"));
	CHECK(eval_eq("(list \"\\x41\" \"\\101\" \"\\e\" \"\\d\" \"\\s\")",
	    "(\"A\" \"A\" \"\x1b\" \"\x7f\" \" \")"));

	/* Rejections, by name.  Each of these used to be read as something
	 * else -- ?ab as one character plus a leftover token, "\x41f" as
	 * "Af". */
	/* `[1 2 3]' left this list in Phase 24, which gave fe a vector to
	 * read into; test_phase24_vectors is where it is asserted now.
	 * `#:' stays, because an uninterned symbol still has no
	 * representation. */
	CHECK(eval_error_contains("(list '#:sym)", "unsupported read syntax"));
	/* Phase 14 implements symbol escapes, so what is left to reject
	 * here is a backslash with nothing after it; the positive rows are
	 * in test_phase14_symbols below. */
	CHECK(eval_error_contains("(list 'a\\", "unterminated symbol escape"));
	CHECK(eval_error_contains("(list \"\\q\")", "unknown escape"));
	CHECK(eval_error_contains("(list ?ab)", "? literal without delimiter"));
	CHECK(eval_error_contains("(list ?\\s-a)", "\\s character modifier"));
	CHECK(eval_error_contains("(list ?\\^a)", "\\^ character modifier"));
	CHECK(eval_error_contains(
	    "(list \"\\x41f\")", "character above 255 in string"));
	/* `"\\0"' left this list in Phase 25, which gave fe strings a
	 * length instead of a terminator; test_phase25_strings is where
	 * the escape is asserted to READ now. */
	CHECK(eval_error_contains(
	    "(list ?\\x110000)", "\\x character out of range"));

	kg_lisp_shutdown();
}

/* Phase C2, the fe pin that made the form feed (0x0C) reader whitespace.
 * Emacs' `read1' retries on five bytes and fe had four of them, so the
 * page separator every Elisp file uses between sections was an ordinary
 * symbol constituent: the first form below answered `void-variable \'
 * before the pin -- the unbound symbol being the page break itself --
 * and the rows after it walk the other sites the reader asks the same
 * question, including the two the fix deliberately leaves alone. */
static void test_reader_form_feed_page_break(void)
{
	CHECK(kg_lisp_init() == 0);

	/* A page break alone on a line between two forms, which is the
	 * reader-form-feed-page-break oracle case's own expression. */
	CHECK(eval_eq("(progn\n  nil\n\f\n  nil)", "nil"));
	/* The skip loop and the atom delimiter: it separates tokens and
	 * ends the one beside it. */
	CHECK(eval_eq("(+ 1\f2)", "3"));
	/* The `?' literal's own delimiter, and the radix digits'. */
	CHECK(eval_eq("(list ?a\f?b)", "(97 98)"));
	CHECK(eval_eq("(list #x1f\f2)", "(31 2)"));
	/* Unmoved: an ESCAPED form feed is a symbol constituent, as an
	 * escaped space is -- the escape decides, not the byte. */
	CHECK(eval_eq("(length (symbol-name 'a\\\fb))", "3"));
	/* Unmoved: inside a string body the byte was never reader syntax. */
	CHECK(eval_eq("(length \"a\fb\")", "3"));
	/* Unmoved: a comment still ends at a newline and nothing else, so a
	 * page break inside one is comment text. */
	CHECK(eval_eq("(progn ; page \f break\n  42)", "42"));

	kg_lisp_shutdown();
}

/* Phase C1: `autoload' is a documented no-op macro, and this is the
 * "documented" half made checkable.  Emacs' arms lazy loading; kg's
 * records nothing, so the declaration is accepted and the function it
 * names is still void at its first CALL -- the correct kg answer until
 * kg has package loading at all.  s.el:34's `void-function autoload' is
 * the measured blocker the name exists for; the divergence itself is the
 * `prelude-autoload' manifest row and its three oracle cases. */
static void test_autoload_no_op(void)
{
	CHECK(kg_lisp_init() == 0);

	/* Accepted at top level, and evaluation continues past it. */
	CHECK(eval_eq("(progn (autoload 'kg-al-probe \"kg-no-such-library\")"
		      " 'accepted)",
	    "accepted"));
	/* Nothing was recorded: the function cell is still empty ... */
	CHECK(eval_eq("(fboundp 'kg-al-probe)", "nil"));
	/* ... so the first call is `void-function', not a load attempt. */
	CHECK(eval_error_contains("(kg-al-probe)", "void-function"));
	/* A real definition afterwards is what defines the name, the macro
	 * having written nothing for it to fight with. */
	CHECK(eval_eq(
	    "(progn (defun kg-al-probe () 'mine) (kg-al-probe))", "mine"));
	/* And it says what it is rather than pretending. */
	CHECK(eval_eq("(documentation 'autoload)",
	    "Accept an autoload declaration; kg loads nothing and records "
	    "nothing."));

	kg_lisp_shutdown();
}

/* Phase C3's frontier, MOVED.  The fixture is synthetic --
 * test/lisp-compat/fixtures/sel-frontier.el says why -- and reproduces
 * s.el's blocker sequence in the order a load meets it: C1's autoload
 * declaration, C2's page separator, and a `declare' debug spec carrying a
 * vector literal.  All three are landed now, the third by Phase 24, so
 * this test asserts the flip the row was written to demand: the file
 * loads clean and every probe in it answers.  It stays because it is the
 * cheapest regression guard for all three blockers at once. */
static void test_sel_frontier_vector_literal(void)
{
	static const char *const paths[] = {
		"test/lisp-compat/fixtures/sel-frontier.el",
		"../test/lisp-compat/fixtures/sel-frontier.el",
	};
	const char *path = nullptr;
	char form[256];
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *fp = fopen(paths[i], "r");

		if (fp != nullptr) {
			fclose(fp);
			path = paths[i];
			break;
		}
	}
	CHECK(path != nullptr);
	if (path == nullptr) {
		return;
	}

	CHECK(kg_lisp_init() == 0);

	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (error (error-message-string e)))",
	    path);
	CHECK(eval_eq(form, "t"));
	/* Nothing is a frontier any more: the autoload declaration, the
	 * page break AND the vector literal are all past, so the macro is
	 * defined and the form after it ran.  (t t nil nil) is what this
	 * answered while the third blocker stood. */
	CHECK(eval_eq("(list (boundp 'sel-frontier-past-autoload)"
		      " (boundp 'sel-frontier-past-page-break)"
		      " (boundp 'sel-frontier-loaded)"
		      " (fboundp 'sel-frontier-when-let))",
	    "(t t t t)"));
	/* And the macro the debug spec is attached to WORKS, which is the
	 * claim a load that merely stopped signalling would not make. */
	CHECK(eval_eq("(sel-frontier-when-let (ignored 7) 'ran)", "ran"));

	kg_lisp_shutdown();
}

/* Phase 24.2: the vector surface, seen from kg rather than from fe.
 * test/lisp-compat/cases/vector-*.json is the contract -- thirty-four
 * cases frozen against Emacs 31.0.91 before any code existed -- and
 * `make lisp-oracle-check' is what compares kg to it.  This asserts the
 * three things that runner structurally cannot:
 *
 *   1. condition DATA.  The runner compares a condition's SYMBOL, so
 *      `args-out-of-range' would pass with the index alone, or with the
 *      pair the wrong way round.  The frozen data is (SEQUENCE INDEX).
 *   2. the STRING arm kg keeps for itself.  fe's `length' and `elt' are
 *      generic, and fe counts a string's BYTES; every oracle case is
 *      ASCII, where bytes and characters agree, so only a non-ASCII
 *      string can tell kg's wrapper from fe's primitive.
 *   3. the PAYLOAD region really being used, which is the substrate
 *      claim under all of it: no vector allocates one byte of it in a
 *      session that has only booted, and building one does. */
static void test_phase24_vectors(void)
{
	struct kg_lisp_arena_stats before, after;

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&before) == 0);

	/* Reader, writer and the type's two names. */
	CHECK(eval_eq("[1 2 3]", "[1 2 3]"));
	CHECK(eval_eq("[]", "[]"));
	CHECK(eval_eq("'(a [1 2] b)", "(a [1 2] b)"));
	CHECK(eval_eq("(type-of [1 2 3])", "vector"));
	CHECK(eval_eq("(list (vectorp []) (vectorp \"ab\") (vectorp '(1)))",
	    "(t nil nil)"));
	/* A missing bracket is Emacs' GENERIC incomplete-input condition,
	 * not a vector-specific invention -- the name 24.0 measured rather
	 * than assumed. */
	CHECK(eval_error_contains("[1 2", "end-of-file"));
	CHECK(eval_error_contains("[1 2", "unclosed vector"));

	/* 1. CONDITION DATA, which the oracle runner compares only by
	 * symbol.  The offending sequence comes FIRST on both sides and for
	 * all three names, and `aref' names `arrayp' -- strings are arrays
	 * -- rather than `vectorp'. */
	CHECK(eval_eq("(condition-case e (aref [10 20] 5)"
		      " (args-out-of-range (cdr e)))",
	    "([10 20] 5)"));
	CHECK(eval_eq("(condition-case e (aref [10 20] -1)"
		      " (args-out-of-range (cdr e)))",
	    "([10 20] -1)"));
	CHECK(eval_eq("(condition-case e (aset (make-vector 2 0) 2 1)"
		      " (args-out-of-range (cdr e)))",
	    "([0 0] 2)"));
	CHECK(eval_eq("(condition-case e (elt [1 2] 9)"
		      " (args-out-of-range (cdr e)))",
	    "([1 2] 9)"));
	CHECK(eval_eq("(condition-case e (aref 5 0)"
		      " (wrong-type-argument (cdr e)))",
	    "(arrayp 5)"));

	/* Identity, mutation through two references, and structural
	 * `equal' -- including the cross-type rule in both argument
	 * orders, which is the half an implementation loses first. */
	CHECK(eval_eq("(let ((a [1]) (b [1]))"
		      " (list (eq a b) (eq a a) (eql a a) (eql a b)))",
	    "(nil t t nil)"));
	CHECK(eval_eq("(let* ((a (make-vector 3 0)) (b a))"
		      " (list (aset a 1 'x) (aref b 1) (eq a b) b))",
	    "(x x t [0 x 0])"));
	CHECK(eval_eq("(list (equal [1 2] [1 2]) (equal [] [])"
		      " (equal [1 2] [1 2 3]) (equal [1 2] '(1 2))"
		      " (equal '(1 2) [1 2]) (equal [[1]] [[1]]))",
	    "(t t nil nil nil t)"));

	/* 2. THE STRING ARM.  Every oracle case for `length' and `elt' is
	 * ASCII, where fe's byte answer and Emacs' character answer are the
	 * same number.  These are not: "é" is two bytes and one character,
	 * so a kg that had simply taken fe's generic primitives would
	 * answer 2 and 195 here. */
	CHECK(eval_eq("(list (length '(1 2 3)) (length \"abc\")"
		      " (length [1 2 3]) (length []))",
	    "(3 3 3 0)"));
	CHECK(eval_eq("(length \"\xc3\xa9\")", "1"));
	CHECK(eval_eq("(elt \"\xc3\xa9\" 0)", "233"));
	CHECK(eval_eq("(list (elt '(a b c) 1) (elt \"abc\" 1) (elt [a b c] 1))",
	    "(b 98 b)"));
	/* And the asymmetry both sides of `elt' inherit from where they
	 * route: past the end of a LIST is nil, past the end of a VECTOR
	 * raises. */
	CHECK(eval_eq("(elt '(1 2) 9)", "nil"));
	CHECK(eval_error_contains("(elt [1 2] 9)", "out of range"));
	/* The dotted and non-sequence conditions the hand-written `length'
	 * used to raise are fe's now, and unchanged. */
	CHECK(eval_eq("(condition-case e (length 5)"
		      " (wrong-type-argument (cdr e)))",
	    "(sequencep 5)"));
	CHECK(eval_eq("(condition-case e (length '(1 . 2))"
		      " (wrong-type-argument (cdr e)))",
	    "(listp 2)"));

	/* The combinators, and the result types Emacs gives them: a list
	 * from `mapcar' over a vector, a flattened list from `append', a
	 * fresh VECTOR from `copy-sequence', the sequence's own type from
	 * `seq-take'. */
	CHECK(eval_eq("(mapcar '1+ [1 2 3])", "(2 3 4)"));
	CHECK(eval_eq("(let ((acc nil))"
		      " (mapc (lambda (x) (setq acc (cons x acc))) [1 2 3])"
		      " acc)",
	    "(3 2 1)"));
	CHECK(eval_eq("(mapconcat 'number-to-string [1 2 3] \"-\")", "1-2-3"));
	CHECK(eval_eq("(list (append [1 2] nil) (append [1] '(2))"
		      " (append \"ab\" nil))",
	    "((1 2) (1 2) (97 98))"));
	CHECK(eval_eq("(let* ((a [1 2]) (b (copy-sequence a)))"
		      " (aset b 0 9) (list a b))",
	    "([1 2] [9 2])"));
	CHECK(eval_eq("(list (seq-map '1+ [1 2]) (seq-filter 'zerop [0 1 0])"
		      " (seq-take [1 2 3] 2) (seq-take \"abc\" 2)"
		      " (seq-take '(1 2 3) 2))",
	    "((2 3) (0 0) [1 2] \"ab\" (1 2))"));

	/* 3. THE PAYLOAD REGION, read at the end against the reading taken
	 * before any of the above ran.  A vector's elements live in it,
	 * and since Phase 25 so do every string's bytes -- so the region
	 * is already in use when this test starts, which is what the
	 * `before' reading is here to say: the numbers below are a DELTA a
	 * vector made, not the region's whole story.
	 * .ci/prelude-startup-census.json is the ratchet on the startup
	 * figure itself.  A thousand-element vector costs 8032 bytes (32 +
	 * 8n, fe's asserted equality and not kg's to re-derive), so the
	 * high-water mark has to clear the live bytes that were there
	 * before it by at least that. */
	CHECK(eval_eq("(length (make-vector 1000 0))", "1000"));
	CHECK(kg_lisp_arena_stats(&after) == 0);
	CHECK(before.payload_capacity_bytes > 0);
	CHECK(before.payload_live_bytes > 0);
	CHECK(before.payload_live_bytes < before.payload_capacity_bytes);
	CHECK(after.payload_peak_bytes >= before.payload_live_bytes + 8032);
	CHECK(after.payload_peak_bytes >= before.payload_peak_bytes);
	CHECK(after.payload_allocation_failures == 0);

	kg_lisp_shutdown();
}

/* Phase 25's fe cut, from kg's side: a string is a header carrying a
 * byte LENGTH over payload bytes, where it was a chain of seven-byte
 * cells terminated by a NUL.  Everything asserted here is behaviour the
 * pin moved and nothing kg wrote -- it is the kg-side citation
 * test/lisp-compat's `string25-*' rows carried as `kg_test: null' while
 * kg could not build such a string at all.
 *
 * What the NUL half pins is that a NUL is now DATA: it reads, it counts
 * as one unit, it is indexable, it compares, and `%S' escapes it back
 * into the octal form the reader accepts, which is fe's decision and not
 * Emacs' (Emacs emits the raw byte).  The recorded divergence is that
 * one printed character, not the round trip. */
static void test_phase25_strings(void)
{
	CHECK(kg_lisp_init() == 0);

	/* The reader takes the escape that used to be a named refusal, and
	 * the NUL is one unit among three. */
	CHECK(eval_eq("(length \"a\\0b\")", "3"));
	CHECK(eval_eq("(list (aref \"a\\0b\" 0) (aref \"a\\0b\" 1)"
		      " (aref \"a\\0b\" 2))",
	    "(97 0 98)"));
	CHECK(eval_eq("(format \"%S\" \"a\\0b\")", "\"a\\000b\""));
	/* Equality is length-then-bytes, so a NUL neither ends a string nor
	 * hides what follows it. */
	CHECK(eval_eq("(equal \"a\\0b\" \"a\\0b\")", "t"));
	CHECK(eval_eq("(list (string= \"a\\0b\" \"a\\0b\")"
		      " (string= \"a\\0b\" \"a\\0c\"))",
	    "(t nil)"));

	/* `aset' on a string, which was `unsupported: aset on a string'
	 * until this pin.  A fe string is a byte string, so this is Emacs'
	 * UNIBYTE answer exactly: the stored value reads back, the length
	 * does not move, and a value above 255 is refused in Emacs' own
	 * words. */
	CHECK(eval_eq("(let ((s (copy-sequence \"abc\")))"
		      " (list (aset s 0 122) s))",
	    "(122 \"zbc\")"));
	CHECK(eval_eq("(let ((s (copy-sequence \"abc\")))"
		      " (aset s 1 233) (list (length s) (aref s 1)))",
	    "(3 233)"));
	CHECK(eval_error_contains("(aset (copy-sequence \"abc\") 0 26085)",
	    "Attempt to store non-byte value into unibyte string"));
	/* The bounds half was already Emacs': the offending STRING first. */
	CHECK(eval_eq("(condition-case e (aset (copy-sequence \"abc\") 5 97)"
		      " (args-out-of-range (cdr e)))",
	    "(\"abc\" 5)"));

	/* The lengths the master plan's Phase 25 names, built and read back
	 * through kg's own surface: 0, 7 and 8 bracket the seven-byte cell
	 * a string used to be a chain of, and 256 and 8192 are past
	 * anything such a chain kept contiguous.  That they survive a
	 * COMPACTION is fe's own gate (fe/test/payload_tests.c), which can
	 * force a collection where nothing in kg's Lisp surface can.  */
	CHECK(eval_eq("(mapcar 'length (list \"\" (make-string 7 97)"
		      " (make-string 8 98) (make-string 256 99)"
		      " (make-string 8192 100)))",
	    "(0 7 8 256 8192)"));
	/* A symbol's name is one of those strings now, so a name built at
	 * run time still interning to the symbol the reader made is the
	 * same question asked of the length-then-bytes comparison. */
	CHECK(eval_eq("(let ((s (intern (concat \"kg-phase25-\" \"name\"))))"
		      " (list (eq s 'kg-phase25-name) (symbol-name s)))",
	    "(t \"kg-phase25-name\")"));

	/* THE FOUR ROUTES TO A NUL that do not go through the reader, all
	 * of which kg refused before this phase in three different ways
	 * from three different places.  Emacs' answer to the first three
	 * is (1 1 3), which is the corpus case string25-nul-constructed. */
	CHECK(eval_eq("(list (length (char-to-string 0))"
		      " (length (make-string 1 0))"
		      " (length (concat \"a\" (char-to-string 0) \"b\")))",
	    "(1 1 3)"));
	CHECK(eval_eq("(length (format \"%c\" 0))", "1"));
	CHECK(eval_eq("(length (format \"x%cy\" 0))", "3"));
	/* The range around it is Emacs' and nothing else: -1 is not a
	 * character, and the surrogates and U+110000 are still refused. */
	CHECK(eval_eq("(condition-case e (char-to-string -1) (error e))",
	    "(wrong-type-argument characterp -1)"));
	CHECK(eval_error_contains("(format \"%c\" -1)", "out of range"));

	/* And the kg-side sites that COPY bytes out and back: each of them
	 * carries the length now, where a `strlen' cut the value at the
	 * NUL.  doc/lisp-string-nul-policy.md is the per-site record, and
	 * these are its "carries the length" rows asked in Lisp. */
	CHECK(eval_eq("(list (length (substring \"a\\0b\" 0 2))"
		      " (length (upcase \"a\\0b\"))"
		      " (length (regexp-quote \"a\\0b\"))"
		      " (length (format \"%s\" \"a\\0b\")))",
	    "(2 3 3 3)"));
	/* `elt' answered nil for the middle of \"a\\0b\" before this, because
	 * its prelude wrapper reaches `substring'. */
	CHECK(eval_eq("(list (elt \"a\\0b\" 1) (string-to-list \"a\\0b\"))",
	    "(0 (97 0 98))"));

	kg_lisp_shutdown();
}

/* Phase 25.2's capability track: the three names Phase 24's frontier
 * probe found unmodified s.el stopping at, the two accessors they are
 * built out of, and the two inconsistencies 25.0 measured on the way
 * past.  Every expectation here is Emacs 31.0.91's, frozen by a
 * `string25-*' case in test/lisp-compat before any of it existed.
 */
static void test_phase25_match_data(void)
{
	CHECK(kg_lisp_init() == 0);

	/* The long spellings, and that they are the SAME objects. */
	CHECK(eval_eq("(list (fboundp 'string-equal) (fboundp 'string-lessp)"
		      " (fboundp 'string-greaterp))",
	    "(t t t)"));
	CHECK(eval_eq(
	    "(list (eq (symbol-function 'string-equal)"
	    " (symbol-function 'string=))"
	    " (string-equal \"abc\" \"abc\")"
	    " (string-lessp \"a\" \"b\") (string-greaterp \"b\" \"a\"))",
	    "(t t t t)"));
	/* `string=' takes a symbol on either side now, as `string<' always
	 * did, and still refuses anything that is neither. */
	CHECK(eval_eq("(list (string= 'foo \"foo\") (string= \"foo\" 'foo)"
		      " (string= 'foo 'foo) (string= nil \"nil\"))",
	    "(t t t t)"));
	CHECK(eval_eq("(condition-case e (string= \"abc\" 3) (error e))",
	    "(wrong-type-argument stringp 3)"));

	/* `match-data' shape: a flat list of BEGIN END pairs, group 0
	 * first, and (nil nil) for a group that did not participate. */
	CHECK(eval_eq("(progn (string-match \"\\\\(b\\\\)\" \"abc\")"
		      " (match-data))",
	    "(1 2 1 2)"));
	CHECK(eval_eq(
	    "(progn (string-match \"b+\" \"abbbc\") (match-data))", "(1 4)"));
	CHECK(eval_eq("(progn (string-match \"\\\\(z\\\\)?b\" \"ab\")"
		      " (match-data))",
	    "(1 2 nil nil)"));
	/* And the round trip `save-match-data' is built on. */
	CHECK(eval_eq("(progn (string-match \"b\" \"abc\")"
		      " (let ((d (match-data)))"
		      "  (string-match \"c\" \"xc\") (set-match-data d)"
		      "  (list (match-beginning 0) (match-end 0))))",
	    "(1 2)"));

	/* `save-match-data' told apart from its two wrong implementations
	 * by one value: ((3 5) 1 2) restores, ((1 2) 1 2) restores too
	 * early, ((3 5) 3 5) does not restore. */
	CHECK(eval_eq("(progn (string-match \"b\" \"abc\")"
		      " (list (save-match-data"
		      "         (string-match \"cc\" \"xxxcc\")"
		      "         (list (match-beginning 0) (match-end 0)))"
		      "       (match-beginning 0) (match-end 0)))",
	    "((3 5) 1 2)"));
	/* The half no corpus case can ask for, and the reason the macro is
	 * an `unwind-protect' rather than a `let' and a restore: a body
	 * that RAISES restores too. */
	CHECK(eval_eq("(progn (string-match \"b\" \"abc\")"
		      " (condition-case e"
		      "   (save-match-data (string-match \"cc\" \"xxxcc\")"
		      "                    (error \"boom\"))"
		      "  (error nil))"
		      " (list (match-beginning 0) (match-end 0)))",
	    "(1 2)"));

	/* `string-match-p' answers the index and leaves the data alone --
	 * the whole of what the `-p' spelling promises here. */
	CHECK(eval_eq("(progn (string-match \"b+\" \"abbbc\")"
		      " (list (string-match-p \"c\" \"xxc\")"
		      "       (string-match-p \"z\" \"abc\")"
		      "       (match-beginning 0) (match-end 0)))",
	    "(2 nil 1 4)"));
	/* The sensitivity control: a second `string-match' DOES move it,
	 * so the case above is testifying rather than unable to see. */
	CHECK(eval_eq("(progn (string-match \"b+\" \"abbbc\")"
		      " (string-match \"c\" \"xxc\")"
		      " (list (match-beginning 0) (match-end 0)))",
	    "(2 3)"));

	/* `aref' counts characters on a string now, like `elt' beside it
	 * and like Emacs; every other sequence is fe's primitive. */
	CHECK(eval_eq("(list (aref \"h\xc3\xa9llo\" 1) (elt \"h\xc3\xa9llo\" 1)"
		      " (aref \"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\" 0)"
		      " (aref [1 2 3] 1) (aref \"abc\" 0))",
	    "(233 233 26085 2 97)"));
	/* Bounds are unmoved by the wrapper: the offending STRING first,
	 * which is Emacs' shape and was already kg's. */
	CHECK(eval_eq("(condition-case e (aref \"abc\" 5)"
		      " (args-out-of-range (cdr e)))",
	    "(\"abc\" 5)"));
	CHECK(eval_eq("(condition-case e (aref \"abc\" -1)"
		      " (args-out-of-range (cdr e)))",
	    "(\"abc\" -1)"));

	kg_lisp_shutdown();
}

/* Phase 26.2's `count-matches', against the five properties 26.0 froze
 * from the pinned Emacs (test/lisp-compat/cases/phase26-count-matches-*).
 * It is prelude Lisp over `re-search-forward', and the two lines a naive
 * `while' gets wrong are the zero-width advance and the restored point. */
static void test_phase26_count_matches(void)
{
	CHECK(kg_lisp_init() == 0);

	/* The plain call counts from POINT to the end of the accessible
	 * buffer, resumes at the END of the previous match -- so "ana" over
	 * "banana" is 1 and not 2 -- and LEAVES POINT WHERE IT WAS. */
	CHECK(eval_eq("(with-temp-buffer (insert \"banana\")"
		      " (goto-char (point-min))"
		      " (list (count-matches \"an\") (count-matches \"ana\")"
		      "  (count-matches \"z\") (point)))",
	    "(2 1 0 1)"));
	/* The region form s.el takes, including an empty region and the
	 * bounds THE WRONG WAY ROUND, which Emacs orders rather than
	 * refuses. */
	CHECK(eval_eq("(with-temp-buffer (insert \"banana\")"
		      " (goto-char (point-min))"
		      " (list (count-matches \"an\" 1 (point-max))"
		      "  (count-matches \"a\" 1 4)"
		      "  (count-matches \"a\" 4 (point-max))"
		      "  (count-matches \"a\" 1 1)"
		      "  (count-matches \"a\" 4 2)))",
	    "(2 1 2 0 1)"));
	/* A zero-width match counts and then advances one character: 6 over
	 * a six-character buffer.  NOT 3 (the real `a' matches do not
	 * absorb the empty ones) and NOT 7 (the position after the last
	 * character gets no empty match of its own). */
	CHECK(eval_eq("(with-temp-buffer (insert \"banana\")"
		      " (goto-char (point-min))"
		      " (list (count-matches \"a*\" 1 (point-max))"
		      "  (count-matches \"\" 1 (point-max))))",
	    "(6 6)"));
	/* A VALUE, not a message. */
	CHECK(eval_eq("(let ((r (with-temp-buffer (insert \"banana\")"
		      "  (goto-char (point-min))"
		      "  (count-matches \"an\" 1 (point-max)))))"
		      " (list r (integerp r) (stringp r)))",
	    "(2 t nil)"));
	/* And it CLOBBERS the match register on its LAST match, which is a
	 * requirement rather than an accident: s.el's own `save-match-data'
	 * around the call is what makes `s-count-matches' side-effect-free.
	 */
	CHECK(eval_eq("(with-temp-buffer (insert \"banana\")"
		      " (goto-char (point-min)) (string-match \"z*\" \"zzz\")"
		      " (list (count-matches \"\\\\(a\\\\)n\" 1 (point-max))"
		      "  (match-beginning 0) (match-end 0)))",
	    "(2 4 6)"));

	kg_lisp_shutdown();
}

/* Phase 26.2's `replace-match', STRING form, against the rows 26.0 froze
 * (test/lisp-compat/cases/phase26-replace-match-*).  Two of the frozen
 * five could not be asked of kg at all -- FIXEDCASE nil's case rule needs
 * `case-fold-search' to make its own patterns match, which kg does not
 * have -- so what is here is what is checkable: the basic replacement,
 * both readings of LITERAL, SUBEXP, the register left ALONE afterwards,
 * and the three error shapes. */
static void test_phase26_replace_match(void)
{
	CHECK(kg_lisp_init() == 0);

	/* A NEW string; nothing is mutated. */
	CHECK(eval_eq("(progn (string-match \"b+\" \"abbbc\")"
		      " (replace-match \"X\" t t \"abbbc\"))",
	    "aXc"));
	/* LITERAL nil expands \\N, \\& and \\\\ -- the same three
	 * `replace-regexp-in-string' has, through the same helper. */
	CHECK(eval_eq(
	    "(list (progn (string-match \"\\\\(a\\\\)\\\\(b\\\\)\" \"ab\")"
	    "        (replace-match \"\\\\2\\\\1\" t nil \"ab\"))"
	    " (progn (string-match \"b+\" \"abbbc\")"
	    "  (replace-match \"[\\\\&]\" t nil \"abbbc\")))",
	    "(\"ba\" \"a[bbb]c\")"));
	/* LITERAL t inserts a replacement that WOULD have been an escape
	 * verbatim, which is the half s.el uses. */
	CHECK(eval_eq("(progn (string-match \"b\" \"ab\")"
		      " (replace-match \"\\\\1x\" t t \"ab\"))",
	    "a\\1x"));
	/* SUBEXP replaces one group's span -- and after a STRING
	 * replacement THE MATCH DATA IS NOT ADJUSTED: 1..4 still stands
	 * over a string of another length. */
	CHECK(eval_eq(
	    "(list (progn (string-match \"\\\\(a\\\\)\\\\(b\\\\)\" \"ab\")"
	    "        (replace-match \"X\" t t \"ab\" 2))"
	    " (progn (string-match \"b+\" \"abbbc\")"
	    "  (list (replace-match \"XY\" t t \"abbbc\")"
	    "   (match-beginning 0) (match-end 0))))",
	    "(\"aX\" (\"aXYc\" 1 4))"));
	/* The three error shapes, as VALUES.  The first two share a
	 * condition AND a message and are told apart only by the SUBEXP as
	 * written -- nil against 2 -- because group 0 is a subexpression
	 * like any other and an empty register has none. */
	CHECK(eval_eq(
	    "(list (progn (set-match-data nil)"
	    "        (condition-case e (replace-match \"x\" t t \"abc\")"
	    "          (error (list (car e) (cdr e)))))"
	    " (progn (string-match \"\\\\(a\\\\)\\\\|\\\\(b\\\\)\" \"a\")"
	    "  (condition-case e (replace-match \"X\" t t \"a\" 2)"
	    "    (error (list (car e) (cdr e)))))"
	    " (progn (string-match \"b+\" \"abbbc\")"
	    "  (condition-case e (replace-match \"X\" t t \"ab\")"
	    "    (error (list (car e) (cdr e))))))",
	    "((error (\"replace-match subexpression does not exist\" nil))"
	    " (error (\"replace-match subexpression does not exist\" 2))"
	    " (args-out-of-range (1 4)))"));
	/* The buffer form is a different operation on a different object,
	 * and kg says so rather than doing something else. */
	CHECK(eval_error_contains("(progn (string-match \"b\" \"ab\")"
				  " (replace-match \"X\"))",
	    "STRING form"));

	kg_lisp_shutdown();
}

 /* SMOKE probe for vendored s.el (master plan 2026-08-21, M1.5: this is
  * reported as SMOKE, never as package support).  Phase 24.3's last
  * required case: the REAL package the synthetic sel-frontier fixture
  * stood in for.  external/elpa/s.el is 793 lines of GPLv3+ Emacs Lisp
  * vendored for testing only -- provenance in
  * external/provenance-ledger.toml, quarantine enforced by
  * utils/check_external_quarantine.py -- and kg could not READ it before
  * this phase, because one `declare' debug spec at s.el:455 carries a
  * vector literal and every top-level form has to be read before `load'
  * can finish.
  *
  * What this pins is BOTH halves of the honest answer: the file loads
  * unmodified, and the frontier moved rather than vanished.  The corpus
  * cases s-el-vendored-load and s-el-vendored-call-frontier are the same
  * two claims against a checked-in Emacs snapshot; this is the kg-side
  * assertion the manifest row cites, and it names the missing functions
  * exactly, which a comparison against Emacs cannot (Emacs has them).
  * SMOKE, not support: loading and a handful of entry points completing
  * without a raise is the smoke-green label, not a scenario-green or
  * supported claim -- those require exact-value scenarios (see
  * test/elisp-packages/). */
static void test_s_el_vendored_load(void)
{
	static const char *const paths[] = {
		"external/elpa/s.el",
		"../external/elpa/s.el",
	};
	const char *path = nullptr;
	char form[256];
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *fp = fopen(paths[i], "r");

		if (fp != nullptr) {
			fclose(fp);
			path = paths[i];
			break;
		}
	}
	CHECK(path != nullptr);
	if (path == nullptr) {
		return;
	}

	CHECK(kg_lisp_init() == 0);

	/* It loads, unmodified, and answers t rather than a message. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (error (error-message-string e)))",
	    path);
	CHECK(eval_eq(form, "t"));
	CHECK(eval_eq("(list (fboundp 's-join) (fboundp 's-trim)"
		      " (fboundp 's-word-wrap))",
	    "(t t t)"));

	/* And what it defines runs, where kg has what it reaches for. */
	CHECK(eval_eq("(s-join \"-\" '(\"a\" \"b\" \"c\"))", "a-b-c"));
	CHECK(eval_eq("(s-replace \"a\" \"X\" \"banana\")", "bXnXnX"));
	CHECK(eval_eq("(s-repeat 2 \"ab\")", "abab"));

	/* WHAT PHASE 25.2 BOUGHT, in the package's own words: `s-contains?'
	 * and `s-equals?' were two of the three names this line used to
	 * record as MISSING, and `s-match' is the third and the deepest --
	 * its body is `save-match-data' over `string-match' and
	 * `(match-data)', so it needs all three of the phase's names at
	 * once. */
	CHECK(eval_eq("(list (s-contains? \"an\" \"banana\")"
		      " (s-equals? \"a\" \"a\")"
		      " (s-match \"\\\\(b+\\\\)\" \"abbc\"))",
	    "(t t (\"bb\" \"bb\"))"));

	/* THE FRONTIER PHASE 25 LEFT IS CLOSED.  Both of its gaps landed in
	 * 26.2 -- the two anchor spellings in the engine (fe/tiny-regex-c)
	 * and then `count-matches' and `replace-match' in the prelude -- so
	 * `s-trim' trims, `s-count-matches' counts, and the two
	 * `string-match' elements are the spelling's own before and after in
	 * one call each: "\\` " matches at offset 0 where "\\`h" cannot, and
	 * `^ ' beside them is the control that has always worked. */
	CHECK(eval_eq("(list (s-count-matches \"a\" \"banana\")"
		      " (s-trim \"  hi  \") (s-trim-left \"  hi  \")"
		      " (s-trim-right \"  hi  \")"
		      " (string-match \"\\\\`h\" \"  hi\")"
		      " (string-match \"\\\\` \" \"  hi\")"
		      " (string-match \"^ \" \"  hi\")"
		      " (fboundp 'replace-match))",
	    "(3 \"hi\" \"hi  \" \"  hi\" nil 0 0 t)"));

	/* AND THE NEXT ONE, measured the same way rather than predicted:
	 * 51 of the package's own entry points were called and the ones
	 * that raised are below.  Five are missing NAMES and the sixth is
	 * not a name at all -- kg's `re-search-forward' takes REGEXP and
	 * BOUND and s.el passes Emacs' third argument, NOERROR, so the gap
	 * is an ARITY.  Phase 27+'s demand signal, and a phase that lands
	 * one of them fails this line, which is the point of writing it
	 * down.
	 *
	 *   compare-strings      s-shared-start, s-shared-end, s-ends-with?
	 *   fill-region          s-word-wrap
	 *   regexp-opt           s-replace-all
	 *   multibyte-string-p   s-reverse
	 *   assoc-string         s-format, s--aget
	 *   re-search-forward/3  s-split-up-to
	 *
	 * THE FRONTIER DEMAND PHASE IS ANSWERING THEM, one surface per
	 * commit, and this line moves with each one: a name that lands
	 * turns its element from the missing NAME into the package's own
	 * VALUE, which is the only assertion that says the want was
	 * answered rather than merely bound.  `re-search-forward/3' is the
	 * first, and it took ONE NAME MORE than the arity -- s-split-up-to
	 * is `(setq op (goto-char (point-min)))' and kg's `goto-char'
	 * answered nil where Emacs answers POSITION, so `buffer-substring'
	 * raised one line further along.  Both are bound now.
	 */
	CHECK(eval_eq(
	    "(list (condition-case e (s-shared-start \"abc\" \"abd\")"
	    "        (void-function (car (cdr e))))"
	    " (condition-case e (s-word-wrap 3 \"aa bb\")"
	    "   (void-function (car (cdr e))))"
	    " (condition-case e (s-replace-all '((\"a\" . \"X\")) \"abc\")"
	    "   (void-function (car (cdr e))))"
	    " (condition-case e (s-reverse \"abc\")"
	    "   (void-function (car (cdr e))))"
	    " (condition-case e (s-format \"${a}\" 'aget '((\"a\" . \"1\")))"
	    "   (void-function (car (cdr e))))"
	    " (condition-case e (s-split-up-to \",\" \"a,b,c\" 1)"
	    "   (wrong-number-of-arguments (cdr e))))",
	    "(\"ab\" \"aa\nbb\" \"Xbc\" \"cba\""
	    " \"1\" (\"a\" \"b,c\"))"));

	/* THE REST OF WHAT THE FIVE SURFACES BOUGHT, one call per s.el
	 * entry point the probe above does not reach: the other two
	 * `compare-strings' callers, and `s--aget', which is the wrapper
	 * `s-format''s replacer goes through.  Values, not `fboundp': a
	 * name that is bound and answers the wrong thing is the failure
	 * this whole phase exists to make visible. */
	CHECK(eval_eq("(list (s-shared-end \"bar\" \"far\")"
		      " (s-ends-with? \"bar\" \"foobar\")"
		      " (s-ends-with? \"BAR\" \"foobar\" t)"
		      " (s--aget (list (cons \"k\" \"v\")) \"k\"))",
	    "(\"ar\" t t \"v\")"));

	/* AND WHAT REMAINS, asserted as a value rather than described:
	 * NOTHING.  This calls all six of the entry points the frontier
	 * probe measured and collects the name of every one that still
	 * answers `void-function\'; the list is nil, so the six wants are
	 * six answers and the phase\'s demand track is closed.  A name that
	 * regresses reappears here as itself rather than as a diff in a
	 * comment. */
	CHECK(eval_eq(
	    "(let ((missing nil))"
	    " (condition-case e (s-shared-start \"abc\" \"abd\")"
	    "   (void-function (setq missing (cons (car (cdr e)) missing))))"
	    " (condition-case e (s-word-wrap 3 \"aa bb\")"
	    "   (void-function (setq missing (cons (car (cdr e)) missing))))"
	    " (condition-case e (s-replace-all '((\"a\" . \"X\")) \"abc\")"
	    "   (void-function (setq missing (cons (car (cdr e)) missing))))"
	    " (condition-case e (s-reverse \"abc\")"
	    "   (void-function (setq missing (cons (car (cdr e)) missing))))"
	    " (condition-case e (s-format \"${a}\" 'aget '((\"a\" . \"1\")))"
	    "   (void-function (setq missing (cons (car (cdr e)) missing))))"
	    " (condition-case e (s-split-up-to \",\" \"a,b,c\" 1)"
	    "   (void-function (setq missing (cons (car (cdr e)) missing))))"
	    " missing)",
	    "nil"));

	kg_lisp_shutdown();
}

static void test_phase8_library(void)
{
	struct kg_lisp_arena_stats before, after;

	CHECK(kg_lisp_init() == 0);
	CHECK(eval_eq("(assq 'a '((a . 1) (b . 2)))", "(a . 1)"));
	CHECK(eval_eq("(mapc (lambda (x) x) '(1 2 3))", "(1 2 3)"));
	CHECK(eval_eq(
	    "(mapconcat (lambda (x) x) '(\"1\" \"2\" \"3\") \",\")", "1,2,3"));
	/* SEPARATOR is optional since Emacs 29 and defaults to "". */
	CHECK(eval_eq("(mapconcat (lambda (x) x) '(\"a\" \"b\"))", "ab"));
	CHECK(eval_eq(
	    "(mapconcat 'number-to-string '(1 2 3) \", \")", "1, 2, 3"));
	/* number-to-string: Emacs' integer and float syntaxes, which are
	 * fe's writer's, and wrong-type-argument for a non-number. */
	CHECK(eval_eq("(number-to-string 3)", "3"));
	CHECK(eval_eq("(number-to-string 1.0)", "1.0"));
	CHECK(eval_eq("(number-to-string 1.5)", "1.5"));
	CHECK(eval_eq("(number-to-string -0.0)", "-0.0"));
	CHECK(eval_error_contains(
	    "(number-to-string \"x\")", "Wrong type argument"));
	/* string-to-list: codepoints, not bytes, and nil for "". */
	CHECK(eval_eq("(string-to-list \"ab\xc3\xa9\")", "(97 98 233)"));
	CHECK(eval_eq("(string-to-list \"\")", "nil"));
	/* mapc is distinguishable from mapcar only through a side effect:
	 * it answers its LIST argument, not the collected results. */
	CHECK(eval_eq("(let ((acc nil))"
		      " (list (mapc (lambda (x) (setq acc (cons x acc)))"
		      " '(1 2 3)) acc (mapcar (lambda (x) (* x 2)) '(1 2 3))))",
	    "((1 2 3) (3 2 1) (2 4 6))"));
	CHECK(eval_eq("(progn (setq x (list 1 2 3)) (nreverse x))", "(3 2 1)"));
	/* nreverse mutates: the name still points at what was the head, so
	 * it is now the one-element tail.  Emacs answers ((2 1) (1)). */
	CHECK(eval_eq(
	    "(let ((x (list 1 2))) (list (nreverse x) x))", "((2 1) (1))"));
	CHECK(eval_eq("(delq 2 (list 1 2 1))", "(1 1)"));
	CHECK(eval_eq("(delete '(a) (list '(a) '(b)))", "((b))"));
	/* delq compares with eq and delete with equal, which is the whole
	 * difference: the same freshly consed needle is found by one and
	 * not the other.  Emacs answers (((1) (2)) ((2))). */
	CHECK(eval_eq("(let ((needle (list 1)) (items (list (list 1)"
		      " (list 2)))) (list (delq needle items)"
		      " (delete needle (list (list 1) (list 2)))))",
	    "(((1) (2)) ((2)))"));
	/* The head-removal contract: the result must be assigned back,
	 * because removing the first element cannot be done in place. */
	CHECK(eval_eq(
	    "(progn (setq x (list 1 2)) (setq x (delq 1 x)) x)", "(2)"));
	/* A cond clause with no body answers its own test's value. */
	CHECK(eval_eq("(list (cond (5)) (cond (nil 1) (2)) (cond (nil 1))"
		      " (cond))",
	    "(5 2 nil nil)"));
	CHECK(
	    eval_eq("(progn (setq x '(2 1)) (add-to-list 'x 3) x)", "(3 2 1)"));
	/* APPEND puts the new element last, and an element already there
	 * (by `equal') leaves the list alone. */
	CHECK(eval_eq(
	    "(progn (setq x (list 2 1)) (add-to-list 'x 3 t))", "(2 1 3)"));
	CHECK(eval_eq(
	    "(progn (setq x (list 1 2)) (add-to-list 'x 1) x)", "(1 2)"));
	/* Emacs' add-to-list is a function taking a symbol, so the symbol
	 * argument is evaluated: this reaches my-list, not `s'.  kg's was a
	 * macro pattern-matching a literal (quote NAME) and answered
	 * (1) here, having assigned to `s'. */
	CHECK(eval_eq("(let ((s 'my-list)) (setq my-list (list 1))"
		      " (add-to-list s 2) my-list)",
	    "(2 1)"));
	CHECK(eval_eq("(identity 7)", "7"));
	CHECK(eval_eq("(prog2 1 2 3)", "2"));
	CHECK(eval_eq("(max 1 4 2)", "4"));
	CHECK(eval_eq("(min 1 4 2)", "1"));
	/* Emacs answers (wrong-number-of-arguments max 0) for a bare call
	 * and (wrong-type-argument ...) for a non-number operand; kg used
	 * to answer a prose error for the first. */
	CHECK(eval_error_contains("(max)", "Wrong number of arguments"));
	CHECK(eval_error_contains("(min)", "Wrong number of arguments"));
	CHECK(eval_error_contains("(max \"a\" 1)", "Wrong type argument"));
	CHECK(eval_error_contains("(min \"a\" 1)", "Wrong type argument"));
	CHECK(eval_eq("(progn (makunbound 'no-value) (defvar no-value) "
		      "(boundp 'no-value))",
	    "nil"));
	CHECK(eval_eq("(progn (defun documented () \"A doc.\" nil) "
		      "(documentation 'documented))",
	    "A doc."));
	CHECK(eval_eq("(progn (defvar documented-var 1 \"A variable.\") "
		      "(documentation 'documented-var))",
	    "A variable."));
	CHECK(eval_eq("(progn (defconst documented-const 1 \"A constant.\") "
		      "(documentation 'documented-const))",
	    "A constant."));
	/* (defvar SYMBOL &optional VALUE DOCSTRING): a lone string is the
	 * VALUE, not a docstring.  Emacs 31.0.90 answers "hello" for the
	 * first row; kg left dv1 unbound because a single string argument
	 * was classified as documentation and no setq was emitted.  Only
	 * the second element is ever documentation. */
	CHECK(eval_eq(
	    "(progn (makunbound 'dv1) (defvar dv1 \"hello\") dv1)", "hello"));
	CHECK(eval_eq("(progn (makunbound 'dv2) (defvar dv2 \"v\" \"doc\")"
		      " (list dv2 (documentation 'dv2)))",
	    "(\"v\" \"doc\")"));
	CHECK(eval_eq(
	    "(progn (makunbound 'dv3) (defvar dv3) (boundp 'dv3))", "nil"));
	CHECK(eval_eq("(progn (makunbound 'dv4) (defvar dv4 1) dv4)", "1"));
	/* And the rule defvar exists for: an already-bound variable keeps
	 * its value, whatever the declaration says. */
	CHECK(eval_eq("(progn (setq dv5 9) (defvar dv5 1) dv5)", "9"));
	/* defmacro takes defun's lone-string rule -- a string is
	 * documentation only when a further form follows it -- and routes
	 * the docstring to internal--doc-put.  Emacs answers
	 * "just a string" and "doc" for these two. */
	CHECK(eval_eq("(progn (defmacro dm1 (x) \"just a string\") (dm1 3))",
	    "just a string"));
	CHECK(eval_eq("(progn (defmacro dm2 (x) \"doc\" (list '+ x 1))"
		      " (list (dm2 3) (documentation 'dm2)))",
	    "(4 \"doc\")"));
	CHECK(eval_eq("(progn (defmacro dm3 (x) (list '+ x 1)) (dm3 3))", "4"));
	CHECK(eval_eq("(progn (defmacro dm4 (x) \"d\" (declare (indent 0))"
		      " (list '+ x 1)) (list (dm4 3) (documentation 'dm4)))",
	    "(4 \"d\")"));
	CHECK(eval_eq("(progn (setq-default answer 8) answer)", "8"));
	CHECK(eval_eq("(progn (setq-local answer 9) answer)", "9"));
	CHECK(eval_eq("(kbd \"C-c k\")", "C-c k"));
	CHECK(eval_error_contains("(kbd \"M-x\")", "cannot bind key sequence"));
	CHECK(eval_eq("(progn (makunbound 'phase8-custom)"
		      " (defcustom phase8-custom (+ 2 3) \"custom doc\""
		      " :type 'integer :group 'editing))",
	    "phase8-custom"));
	CHECK(eval_eq("phase8-custom", "5"));
	CHECK(eval_eq("(documentation 'phase8-custom)", "custom doc"));
	CHECK(eval_ok(
	    "(progn (setq phase8-custom 9)"
	    " (defcustom phase8-custom (error \"evaluated\") \"doc\"))"));
	CHECK(eval_eq("phase8-custom", "9"));
	CHECK(eval_ok("(custom-set-variables '(phase8-custom 11))"));
	CHECK(eval_eq("phase8-custom", "11"));
	CHECK(eval_error_contains(
	    "(custom-set-variables '(phase8-custom 12 :type integer))",
	    "entry must be"));
	CHECK(eval_error_contains(
	    "(defcustom phase8-bad 1 \"doc\" :initialize t)",
	    "semantic keyword"));
	CHECK(eval_error_contains(
	    "(defcustom phase8-bad 1 \"doc\" :unknown t)", "unknown keyword"));
	CHECK(eval_error_contains(
	    "(defcustom phase8-bad 1 \"doc\" :type)", "keyword tail"));
	CHECK(!eval_ok("(custom-set-variables '(\"x\" 12))"));
	CHECK(eval_eq("(progn (defun phase8-declared () \"doc\""
		      " (declare (something ignored)) 7) (phase8-declared))",
	    "7"));
	CHECK(eval_eq("(progn (defmacro phase8-declared-macro (x)"
		      " \"doc\" (declare (something ignored)) x)"
		      " (phase8-declared-macro 8))",
	    "8"));
	/* What the Phase 8 prelude batch costs the fixed arena.  The
	 * assertion this replaces -- after.peak_live >= before.peak_live --
	 * was a tautology: peak_live_objects is a high-water mark, so it can
	 * never fall.  The claim worth making is margin, the same one
	 * test_perf.c's prelude case makes: after the whole prelude and
	 * every form above it, more than half the arena is still free and
	 * the high-water mark is a small fraction of it.  The arena the slot
	 * counts in this comment were measured in is the 1 MiB one that was
	 * the compiled default until Phase B made it 10 MiB and 440489
	 * slots; what the series tracks is fe's partitioning of a fixed size,
	 * so it reads at the size it was taken at.  Re-measured on this
	 * build via kg_lisp_arena_stats() at the let-binding-buffer-tag pin:
	 * 56147 object slots (56263 at the Phase 20 pin, 56259 at the Phase
	 * 19 pin, 56239 at the Phase 14
	 * pin, 56225 at the Phase 12 pin, 56226 at the Phase 11 fix cycle's
	 * pin, 56224 at the Phase 10 pin, 56222 before that -- the frame record
	 * fe's dynamic-binding work grew costs one frame slot, frame_capacity
	 * 1096 -> 1095, and returns two object slots; Phase 12's fe additions
	 * take one object slot back and no frame slot; Phase 14's grow
	 * FeMinimumArenaSize by 2264 bytes, which moves two frame slots' worth
	 * of bytes to the object side, 1095
	 * -> 1093 frames and +14 objects; Phase 19's seeded `error-message'
	 * properties grow it by 3248 more, moving three more frame slots'
	 * worth across, 1093 -> 1090 frames and +20 objects; Phase 20's two
	 * string primitives and two condition rows grow it by 608, one more
	 * frame slot's worth, 1090 -> 1089 frames and +4 objects; and the
	 * let-binding-buffer-tag pin's per-cleanup-entry host tag grows it by
	 * 2064, which the FRAME side pays for this time, 1089 -> 1087 frames
	 * and -116 objects),
	 * peak_live 7363 after the prelude alone -- the first FALL in this
	 * series, and not a growth measurement at all: prelude-embedding
	 * Phase 1 (doc/plans/2026-08-14-embedded-prelude.md) defers 93 of the
	 * prelude's names to a second embedded array, so evaluate_prelude()
	 * no longer builds them.  Re-measured by instrumenting the
	 * kg_lisp_init() at the top of this function on THIS tree, per the
	 * rule below, not derived from the 11339 it replaces
	 * (11339 at the let-binding-buffer-tag pin,
	 * 11281 at the merge that opened Phase 20, 6205 at the Phase 14
	 * pin, 5301 at the Phase 12 pin, 5295 at the Phase 11 fix cycle's
	 * pin, 5210 at the Phase 10 pin, 5188 at Phase 9's -- the 9887 this
	 * comment carried for the Phase 19 pin measures 11281 on the merged
	 * tree, so most of that gap is the LSP/occur/xref wave's own natives
	 * and prelude and not this phase's 54), and 14776 at this point in
	 * this function, after every
	 * Phase 8 form above -- 26.3% of the arena for everything kg ships
	 * plus this test's own corpus.  That second figure and the one the
	 * `peak_live * 3` comment below carries are the SAME quantity read
	 * from the same `after`; they had drifted apart (12984 here against
	 * 14579 there at the Phase 19 pin) because they were measured at
	 * different times, and Phase 20 measured them once.
	 *
	 * Both figures were re-measured here
	 * at the Phase 19 pin by instrumenting these two lines, and both had
	 * gone stale: Phases 15-18 grew the prelude by the string and list
	 * library, the seq shim and the buffer-local forms without
	 * re-measuring them, which is most of the 6205 -> 9887.  Phase 19's
	 * own share is +170, the `error-message' properties fe now seeds.
	 * Phase 14 is the largest single move in the series and the reason is
	 * structural rather than additive: every symbol object is one cons
	 * bigger, so the +904 on the prelude figure is mostly the prelude's
	 * own several hundred interned names paying that cons each.  The Phase
	 * 12 PIN cost +6 and +17: fe's `eval' primitive and its interned name,
	 * the `file-missing' hierarchy row, and the input-unit field the
	 * defvar scope carrier stamps onto each let-dynamic-only mark.
	 *
	 * Where the movement went, since this comment got it wrong once and
	 * the rule below is what it exists to enforce.  The Phase 11 PIN cost
	 * +17 on both figures: fe's two new primitives
	 * (internal--mark-special, special-variable-p) and their interned
	 * names.  The prelude's switch onto fe's core `let' came a commit
	 * LATER and cost +68 here (5227 -> 5295) and +128 at the census line
	 * (8159 -> 8287).  The version of this comment written at the pin
	 * attributed both deltas to the +17 and named a cause that had not
	 * happened yet; the acceptance review caught it, and the numbers
	 * above are measured by instrumenting this exact line and a
	 * post-kg_lisp_init() probe on THIS tree, not derived.  That is the
	 * rule -- the "8230" this comment used to carry never reproduced
	 * either, and every number here is measured at the pin that records
	 * it.  The Phase 10 comment also carried "5751 with
	 * lisp/auto-fill.el on top of it"; that figure is dropped rather
	 * than carried forward, because the method behind it was not
	 * recorded and evaluating that file's text in a fresh post-prelude
	 * context measures 5927 here, which is not the same measurement.
	 *
	 * Prelude-embedding Phase 1 (doc/plans/2026-08-14-embedded-prelude.md)
	 * moved this figure for a new reason: not a growth, a deferral.  93
	 * of the prelude's 128 names are no longer evaluated by
	 * evaluate_prelude() at all -- their forms move to a second embedded
	 * array, read only by whichever of them a caller actually reaches --
	 * so `before` at this pin (still after every Phase 8 form above,
	 * including several that call one of the 93 and so have already
	 * forced its real definition back in) is 12503, not a number in the
	 * 14xxx/11xxx family above; re-measured by instrumenting this exact
	 * line, on this tree, once. */
	CHECK(kg_lisp_arena_stats(&before) == 0);
	CHECK(eval_ok("(mapconcat (lambda (x) x) "
		      "'(\"1\" \"2\" \"3\" \"4\" \"5\") \":\")"));
	CHECK(kg_lisp_arena_stats(&after) == 0);
	CHECK(after.free_slots * 2 > after.total_slots);
	/* Under a quarter of the arena when the arena was 1 MiB, where
	 * Phase 19 through the let-binding-buffer-tag pin measured a third:
	 * the figure this line bounds -- the high-water mark after the
	 * prelude AND every form this function evaluated above -- measures
	 * 12617 of 56147 (22.47%) at the prelude-embedding Phase 1 pin
	 * (doc/plans/2026-08-14-embedded-prelude.md), against 14817 of 56147
	 * (26.4%) at the let-binding-buffer-tag pin, 14776 of 56263 (26.3%)
	 * at Phase 20's, 14579 of 56259 (25.9%) at Phase 19's and 13238 of
	 * 56239 (23.5%) before that -- the first fall in this comment's
	 * history, and the point of the phase that produced it: fewer of the
	 * prelude's own names are permanently live, not more docstrings or
	 * more library added on top.  Every denominator in that series is
	 * the 1 MiB arena's; Phase B made the default 10 MiB and 440489
	 * slots, so the same marks are now about a fortieth of it and the
	 * numerator is the half of the fraction this file can move.  The
	 * claim being made is still margin; what would not be is a bound
	 * nobody re-measured. */
	CHECK(after.peak_live_objects * 3 < after.total_slots);
	CHECK(after.free_slots <= before.free_slots);
	CHECK(after.allocation_failures == 0);
	kg_lisp_shutdown();
}

static void test_save_excursion(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* 1. Body moves point, after return point is restored. */
	CHECK(eval_eq(
	    "(progn (goto-char 1) (save-excursion (goto-char 7)) (point))",
	    "1"));

	/* 2. Body errors, point is restored. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_error_contains(
	    "(save-excursion (goto-char 8) (car 1))", "expected pair"));
	CHECK(eval_eq("(point)", "3"));

	/* 3. Body inserts before saved point, restoration follows the marker.
	 */
	CHECK(eval_ok("(goto-char 7)"));
	CHECK(eval_ok("(save-excursion (goto-char 1) (insert \"prefix \"))"));
	CHECK(eval_eq("(point)", "14"));

	/* 4. Body attempts to kill buffer (fails because modified). */
	CHECK(eval_error_contains(
	    "(save-excursion (kill-buffer (get-buffer \"bridge.txt\")))",
	    "modified"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The adapter object pool bounds how many excursions are open AT ONCE,
 * not how many a run performs.  The Phase 11 acceptance review's BLOCKER
 * B1: 11D Part 4's capture mints a marker record out of the pool
 * (LISP_MAX_OBJECTS) and the restore only detached it, so the record
 * stayed taken until fe's collector swept the wrapper -- which a loop
 * allocating almost no arena never provokes, the pool having no
 * back-pressure of its own.  The 65th `save-excursion' between two
 * collections raised "too many marker objects", where the pre-Phase-11
 * malloc'd implementation ran 5000 of them.
 *
 * Every number below is the review's, so a re-introduction fails here
 * rather than in a user's init.el.  There is no test above this one that
 * calls `save-excursion' more than twice, which is why every gate was
 * green on the defect.
 *
 * SUB-PLAN 12D PART 3 raised the pool 64 -> 256, so the numbers that were
 * about the pool's SIZE have moved and are re-measured on this tree
 * rather than carried; the numbers that were about the review's defect --
 * that a closed excursion gives its record back at all -- are unchanged,
 * which is the point of keeping both here. */
static void test_save_excursion_pool_bound(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* 1. The review's exact threshold, kept at 65 rather than moved with
	 * the pool: 64 records was the size then, so 65 sequential
	 * excursions was the first failure, and it is the number that
	 * regresses if a closed excursion ever stops giving its record
	 * back. */
	CHECK(eval_eq("(let ((i 0)) (while (< i 65) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "65"));

	/* 2. The old answer, restored: 5000 sequential excursions. */
	CHECK(eval_eq("(let ((i 0)) (while (< i 5000) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "5000"));

	/* 3. Records do not survive the evaluation that made them: this is a
	 * second, separate eval_eq(), and 160 + 160 > 256. */
	CHECK(eval_eq("(let ((i 0)) (while (< i 160) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "160"));
	CHECK(eval_eq("(let ((i 0)) (while (< i 160) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "160"));

	/* 4. The budget is shared with buffer objects, so the review's
	 * three-extra-buffers variant is its own case. */
	CHECK(eval_ok("(get-buffer-create \"pool1\")"));
	CHECK(eval_ok("(get-buffer-create \"pool2\")"));
	CHECK(eval_ok("(get-buffer-create \"pool3\")"));
	CHECK(eval_eq("(let ((i 0)) (while (< i 200) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "200"));

	/* 5. Nesting is bounded by whichever of the pool and fe's frame
	 * capacity is smaller, and WHICH ONE moves with the arena.  At the
	 * 1 MiB arena that was the default through Phase B, frames were the
	 * smaller: 217 nested excursions answered `deep' and the 218th
	 * raised "evaluation frame limit exceeded" (218/219 until the frame
	 * partition fell 1089 -> 1087 at the let-binding-buffer-tag pin),
	 * and `with-current-buffer' met the same ceiling at 155 deep, 156
	 * raising.  Phase B's 10 MiB default takes frame_capacity to 10916
	 * and hands the bound back to the pool, re-measured on this tree:
	 * 256 nested excursions answer `deep' and the 257th raises "too many
	 * marker objects" -- LISP_MAX_OBJECTS, the constraint sub-plan 12D
	 * Part 3 raised 64 -> 256 to get out of the way, now the binding one
	 * again at four times the depth it used to bind at.  Nested
	 * `with-current-buffer' meets its own 256th-form wall, "cleanup stack
	 * overflow".  Five deep in the assertion, because a 257-form C string
	 * literal tests nothing this does not; the ceilings themselves are
	 * the measurement, and they are recorded in the commit and in
	 * src/lisp_obj.h. */
	CHECK(eval_eq("(save-excursion (save-excursion (save-excursion "
		      "(save-excursion (save-excursion 'deep)))))",
	    "deep"));

	/* 6. A released record is reusable, and the collector must not take
	 * the reuser's record with the dead wrapper it sweeps.  The marker
	 * below lands on a record some earlier excursion released; the cons
	 * loop then forces collections that sweep those dead excursion
	 * wrappers.  Without the wrapper-identity check in lisp_object_gc()
	 * this answers "marker-position: expected a marker". */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_ok("(setq pm (point-marker))"));
	CHECK(eval_ok("(let ((j 0)) (while (< j 40000) (setq j (+ j 1)) "
		      "(cons j j)))"));
	CHECK(eval_eq("(marker-position pm)", "3"));

	/* 7. The tolerances the release must not cost: a second manual
	 * restore of a spent state is still harmless, and a value that is not
	 * an adapter object at all is still a type error. */
	CHECK(eval_eq("(let ((s (internal--excursion-capture t))) (list "
		      "(internal--excursion-restore s) "
		      "(internal--excursion-restore s)))",
	    "(nil nil)"));
	CHECK(eval_error_contains("(internal--excursion-restore 5)",
	    "expected a marker or a buffer"));
	CHECK(eval_error_contains("(internal--excursion-restore nil)",
	    "expected a marker or a buffer"));

	/* 8. And the point restoration itself still works after all of it. */
	CHECK(eval_eq(
	    "(progn (goto-char 2) (save-excursion (goto-char 7)) (point))",
	    "2"));

	/* 9. The pool is still A bound, one order of magnitude further out
	 * than the excursion nesting it used to cap.  200 markers held live
	 * at once fit where 65 did not; 400 do not, and the diagnostic says
	 * which mint ran out.  Not 256 exactly, because this function is
	 * holding buffer records of its own by now: the claim is the order
	 * of magnitude, not an arithmetic identity.  Last in the function
	 * because a failed 400 leaves those records taken until something
	 * provokes a collection. */
	CHECK(eval_eq("(let ((held '()) (i 0)) (while (< i 200)"
		      " (setq held (cons (point-marker) held))"
		      " (setq i (+ i 1))) (length held))",
	    "200"));
	CHECK(eval_error_contains("(let ((held '()) (i 0)) (while (< i 400)"
				  " (setq held (cons (point-marker) held))"
				  " (setq i (+ i 1))) (length held))",
	    "too many marker objects"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Phase 17: reaching the editor ------------------------------------
 *
 * These six functions are the kg side of the manifest rows the oracle
 * cases pin from the other side.  What is here rather than there is what
 * an Emacs snapshot cannot see: kg's undo steps, its read-only refusal,
 * its own bounds, and the shapes that raise.
 */

static void test_phase17_line_motion(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "aa", 2);
	editor_insert_row(bcur(), 1, "bbbb", 4);
	editor_insert_row(bcur(), 2, "cc", 2);
	CHECK(kg_lisp_init() == 0);

	/* The shortfall, and where point lands, for a move that fits and one
	 * that does not.  A buffer with no final newline costs one extra. */
	CHECK(eval_eq(
	    "(progn (goto-char 1) (list (forward-line 1) (point)))", "(0 4)"));
	CHECK(eval_eq(
	    "(progn (goto-char 1) (list (forward-line 9) (point)))", "(6 11)"));
	CHECK(eval_eq("(progn (goto-char 9) (list (forward-line -9) (point)))",
	    "(-7 1)"));

	CHECK(
	    eval_eq("(progn (goto-char 6) (beginning-of-line) (point))", "4"));
	CHECK(eval_eq("(progn (goto-char 6) (end-of-line) (point))", "8"));
	CHECK(eval_eq(
	    "(progn (goto-char 1) (beginning-of-line 3) (point))", "9"));
	CHECK(eval_eq("(progn (goto-char 1) (end-of-line 99) (point))", "11"));

	CHECK(eval_eq(
	    "(progn (goto-char 5) (beginning-of-buffer) (point))", "1"));
	CHECK(eval_eq("(progn (goto-char 5) (end-of-buffer) (point))", "11"));
	/* The recorded half: neither form pushes the mark. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (beginning-of-buffer) (mark))", "nil"));

	/* A count that is not a number is Emacs' condition, not fe's prose. */
	CHECK(
	    eval_error_contains("(forward-line \"x\")", "Wrong type argument"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase17_char_motion(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "de", 2);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (list (forward-char 2) (point)))",
	    "(nil 3)"));
	/* A line break is one character. */
	CHECK(eval_eq("(progn (goto-char 3) (forward-char 2) (point))", "5"));
	CHECK(eval_eq("(progn (goto-char 5) (backward-char 2) (point))", "3"));
	/* A negative count reverses each of them. */
	CHECK(eval_eq("(progn (goto-char 1) (backward-char -3) (point))", "4"));

	/* Emacs' answer since Phase 20, where kg clamped silently before:
	 * point moves as far as it can and the count that could not be
	 * spent signals the end it ran into, with no data.  Landing exactly
	 * on an end is not a signal, which the (forward-char 2) case above
	 * and the two here bracket. */
	CHECK(eval_eq("(progn (goto-char 1) (list (condition-case e "
		      "(forward-char 900) (error e)) (point)))",
	    "((end-of-buffer) 7)"));
	CHECK(eval_eq("(progn (goto-char 7) (list (condition-case e "
		      "(backward-char 900) (error e)) (point)))",
	    "((beginning-of-buffer) 1)"));
	CHECK(eval_eq("(progn (goto-char 1) (list (condition-case e "
		      "(forward-char 6) (error e)) (point)))",
	    "(nil 7)"));
	/* An `error' handler catches either, both being children of
	 * `error' in fe's hierarchy as they are in Emacs', and neither
	 * catches the other. */
	CHECK(eval_eq("(progn (goto-char 1) (condition-case nil "
		      "(forward-char 900) (beginning-of-buffer 'bob) "
		      "(end-of-buffer 'eob)))",
	    "eob"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase17_looking_at(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "foo-42 bar", 10);
	editor_insert_row(bcur(), 1, "second", 6);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (looking-at \"foo\"))", "t"));
	/* Anchored: the pattern is present, but not AT point. */
	CHECK(eval_eq("(progn (goto-char 1) (looking-at \"bar\"))", "nil"));
	CHECK(eval_eq("(progn (goto-char 8) (looking-at \"bar\"))", "t"));
	/* It sets the match data and moves point nowhere. */
	CHECK(eval_eq("(progn (goto-char 1)"
		      " (looking-at \"\\\\([a-z]+\\\\)-\\\\([0-9]+\\\\)\")"
		      " (list (match-beginning 0) (match-end 0)"
		      " (match-beginning 1) (match-end 1) (point)))",
	    "(1 7 1 4 1)"));
	/* The row is the subject, so the anchors are line anchors. */
	CHECK(eval_eq("(progn (goto-char 1) (looking-at \"^foo\"))", "t"));
	CHECK(eval_eq("(progn (goto-char 2) (looking-at \"^oo\"))", "nil"));
	CHECK(eval_eq("(progn (goto-char 8) (looking-at \"bar$\"))", "t"));
	/* And the recorded consequence: no match crosses a line break. */
	CHECK(eval_eq(
	    "(progn (goto-char 8) (looking-at \"bar\\nsecond\"))", "nil"));

	CHECK(eval_error_contains("(looking-at 5)", "Wrong type argument"));
	CHECK(eval_error_contains("(looking-at \"[\")", "invalid regexp"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase17_skip_chars(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "  abc12", 7);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (list (skip-chars-forward \" \")"
		      " (point)))",
	    "(2 3)"));
	CHECK(eval_eq("(progn (goto-char 3) (list (skip-chars-forward \"a-z\")"
		      " (point)))",
	    "(3 6)"));
	CHECK(eval_eq("(progn (goto-char 8) (list (skip-chars-backward \"0-9\")"
		      " (point)))",
	    "(-2 6)"));
	/* A leading `^' negates, so the empty set skips nothing and its
	 * negation skips everything. */
	CHECK(eval_eq("(progn (goto-char 1) (skip-chars-forward \"\"))", "0"));
	CHECK(eval_eq("(progn (goto-char 1) (skip-chars-forward \"^\"))", "7"));
	/* LIM bounds it, and a LIM already behind point moves nothing. */
	CHECK(eval_eq(
	    "(progn (goto-char 3) (skip-chars-forward \"a-z\" 5))", "2"));
	CHECK(eval_eq(
	    "(progn (goto-char 3) (skip-chars-forward \"a-z\" 2))", "0"));

	CHECK(eval_error_contains(
	    "(skip-chars-forward 5)", "Wrong type argument"));
	/* The bound is a raise, not a silent truncation: 64 ranges fit. */
	CHECK(eval_error_contains(
	    "(skip-chars-forward (make-string 200 97))", "set is too large"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase17_buffer_edits(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	editor_insert_row(bcur(), 1, "ghi", 3);
	CHECK(kg_lisp_init() == 0);

	/* One gateway call, so one undo step: the whole delete comes back. */
	CHECK(eval_eq("(progn (goto-char 2) (delete-char 3)"
		      " (buffer-substring (point-min) (point-max)))",
	    "aef\nghi"));
	CHECK(eval_eq("(progn (goto-char 3) (delete-char -2)"
		      " (buffer-substring (point-min) (point-max)))",
	    "f\nghi"));
	/* A count the buffer is too short for signals the end it ran into
	 * and deletes NOTHING, which is Emacs' answer and the one this
	 * family clamped away until Phase 20 -- the only member of it whose
	 * clamp cost a user text. */
	CHECK(eval_eq("(progn (goto-char 1) (list (condition-case e "
		      "(delete-char 99) (error e)) "
		      "(buffer-substring (point-min) (point-max)) (point)))",
	    "((end-of-buffer) \"f\nghi\" 1)"));
	CHECK(eval_eq("(progn (goto-char 3) (list (condition-case e "
		      "(delete-char -99) (error e)) "
		      "(buffer-substring (point-min) (point-max)) (point)))",
	    "((beginning-of-buffer) \"f\nghi\" 3)"));

	/* erase-buffer empties it and leaves point at the only position. */
	CHECK(eval_eq(
	    "(progn (erase-buffer) (list (point) (point-max)))", "(1 1)"));
	CHECK(eval_eq("(erase-buffer)", "nil"));

	/* Both refuse a read-only buffer by name rather than failing
	 * silently in the gateway. */
	bcur()->readonly = 1;
	CHECK(eval_error_contains("(delete-char 1)", "read-only"));
	CHECK(eval_error_contains("(erase-buffer)", "read-only"));
	bcur()->readonly = 0;

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase17_buffer_status(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* setup_editor() gives the buffer a filename, which is what makes
	 * this the interesting half: a buffer that visits a file reports it,
	 * a buffer that only has a NAME reports nil even though kg keeps
	 * both in the same field. */
	CHECK(eval_eq("(buffer-file-name)", "/tmp/bridge.txt"));
	CHECK(eval_eq("(format \"%S\" (buffer-file-name"
		      " (get-buffer-create \"named\")))",
	    "nil"));

	CHECK(eval_eq("(buffer-modified-p)", "nil"));
	CHECK(eval_eq("(progn (insert \"x\") (buffer-modified-p))", "t"));
	CHECK(eval_eq("(set-buffer-modified-p nil)", "nil"));
	CHECK(eval_eq("(buffer-modified-p)", "nil"));
	CHECK(eval_eq(
	    "(progn (set-buffer-modified-p t) (buffer-modified-p))", "t"));
	/* buffer-modified-p takes the optional buffer; set- does not. */
	CHECK(eval_eq(
	    "(buffer-modified-p (get-buffer-create \"named\"))", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* with-temp-buffer's own bound, which no oracle case can see: kg refuses
 * a kill when the editor's lifecycle event queue is full, and the queue
 * drains once per keystroke.  The form tolerates that -- a cleanup that
 * raised would REPLACE the body's completion -- so what this pins is that
 * the body's value still arrives, and that ordinary use kills its buffer
 * rather than leaking one. */
/* Phase 18: the buffer-local storage, from the C side where the table's
 * own bounds and the kill path are reachable.  The oracle-compared
 * semantics live in test/lisp-compat (the phase18-* rows); what is here is
 * everything a snapshot cannot see -- that the table refuses rather than
 * dropping, that a killed buffer's binding is reclaimed rather than
 * leaked, and that the swap survives being asked for the same buffer
 * repeatedly. */
/* Phase 20: a variable's docstring on the symbol's plist, where Emacs
 * keeps one and where `documentation-property' reads it back. */
static void test_phase20_variable_documentation(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* `defvar', `defconst' and `defcustom' (which expands to `defvar')
	 * all put; an undocumented `defvar' puts nothing, so the property
	 * stays unset rather than becoming nil. */
	CHECK(eval_eq("(progn (defvar p20a 1 \"A docstring.\") "
		      "(get 'p20a 'variable-documentation))",
	    "A docstring."));
	CHECK(eval_eq("(progn (defvar p20b 2) "
		      "(get 'p20b 'variable-documentation))",
	    "nil"));
	CHECK(eval_eq("(progn (defconst p20c 3 \"A const docstring.\") "
		      "(get 'p20c 'variable-documentation))",
	    "A const docstring."));
	CHECK(eval_eq("(progn (defcustom p20d 4 \"A custom docstring.\" "
		      ":type 'integer) (get 'p20d 'variable-documentation))",
	    "A custom docstring."));
	/* Emacs' re-`defvar' rule, both halves: the VALUE is left alone and
	 * the DOCSTRING is replaced. */
	CHECK(eval_eq("(progn (defvar p20e 1 \"first\") "
		      "(defvar p20e 2 \"second\") "
		      "(list p20e (get 'p20e 'variable-documentation)))",
	    "(1 \"second\")"));

	/* `documentation-property' answers a string property and nothing
	 * else: nil for a property that is not a string (Emacs' own answer
	 * for an integer, which indexes a DOC file kg does not have), nil
	 * for one nobody set, and nil for nil, which owns no plist. */
	CHECK(eval_eq("(documentation-property 'p20a 'variable-documentation)",
	    "A docstring."));
	CHECK(eval_eq("(progn (put 'p20f 'kg-probe 5) "
		      "(documentation-property 'p20f 'kg-probe))",
	    "nil"));
	CHECK(eval_eq("(documentation-property 'p20f 'kg-never-set)", "nil"));
	CHECK(eval_eq(
	    "(documentation-property nil 'variable-documentation)", "nil"));
	/* RAW is accepted and ignored. */
	CHECK(
	    eval_eq("(documentation-property 'p20a 'variable-documentation t)",
		"A docstring."));
	/* A non-symbol is `get''s own wrong-type-argument, not a nil --
	   Emacs' answer too, `(wrong-type-argument symbolp "str")'. */
	CHECK(eval_eq("(condition-case e (documentation-property \"str\" "
		      "'variable-documentation) (error e))",
	    "(wrong-type-argument symbolp \"str\")"));
	/* A program can put its own over one, which is the reason the
	 * docstring lives on the plist rather than only in kg's registry. */
	CHECK(eval_eq("(progn (put 'p20a 'variable-documentation \"Mine.\") "
		      "(documentation-property 'p20a "
		      "'variable-documentation))",
	    "Mine."));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase18_buffer_locals(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Two buffers, one name, two values -- and neither is the default. */
	CHECK(eval_eq("(progn (defvar p18v 70)"
		      " (with-current-buffer (get-buffer-create \"one\")"
		      "  (setq-local p18v 12))"
		      " (with-current-buffer (get-buffer-create \"two\")"
		      "  (setq-local p18v 40))"
		      " (list (with-current-buffer (get-buffer \"one\") p18v)"
		      "  (with-current-buffer (get-buffer \"two\") p18v)"
		      "  p18v (default-value 'p18v)))",
	    "(12 40 70 70)"));

	/* Selecting the same buffer twice is not a second swap: the second
	 * one must not stash the first one's value on top of the default. */
	CHECK(eval_eq("(with-current-buffer (get-buffer \"one\")"
		      " (with-current-buffer (get-buffer \"one\")"
		      "  (list p18v (default-value 'p18v))))",
	    "(12 70)"));

	/* The binding dies with its buffer, and the table reclaims the slot
	 * rather than holding a stale handle: after both kills the name is an
	 * ordinary global again, which `set-default' reaching the value cell
	 * is what proves. */
	CHECK(eval_eq("(progn (kill-buffer (get-buffer \"one\"))"
		      " (kill-buffer (get-buffer \"two\"))"
		      " (setq-default p18v 71)"
		      " (list p18v (default-value 'p18v)))",
	    "(71 71)"));

	/* Killing the buffer whose bindings are in force puts the default
	 * back for the rest of the frame, rather than leaving a dead
	 * buffer's value readable -- and `default-value' answers it even
	 * though the execution context is now a stale handle, which is the
	 * one place a reader has to tolerate what a writer refuses. */
	CHECK(eval_eq("(let ((b (get-buffer-create \"three\")))"
		      " (set-buffer b) (setq-local p18v 5)"
		      " (kill-buffer b)"
		      " (default-value 'p18v))",
	    "71"));
	/* ... and a writer does refuse it. */
	CHECK(eval_error_contains("(let ((b (get-buffer-create \"four\")))"
				  " (set-buffer b) (kill-buffer b)"
				  " (setq-local p18v 5))",
	    "current buffer is dead"));

	/* The ceilings refuse by name.  17 distinct names is one past
	 * LISP_MAX_LOCAL_VARS; the message is the table's, not a generic
	 * arena failure. */
	CHECK(eval_error_contains(
	    "(with-current-buffer (get-buffer-create \"many\")"
	    " (let ((i 0))"
	    "  (while (< i 20)"
	    "   (internal--set-buffer-local (intern (format \"p18n%d\" i)) i)"
	    "   (setq i (1+ i)))))",
	    "too many buffer-local variables"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The buffer tag a `let' over a buffer-local name carries (Phase 18's
 * follow-up, fe FE_API_VERSION 11).  The five *values* this produces are
 * pinned against the oracle in test/lisp-compat -- the
 * lettag-let-binding-buffer-tag row -- so what is here is the half a
 * compat case cannot reach: the tag's own lifetime rules, and the two
 * shapes that must NOT be tagged at all. */
static void test_let_binding_buffer_tag(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* A `let' over a name with no buffer-local binding anywhere is
	 * tagged with nothing and restores into the value cell, which is
	 * the state every session that never says `setq-local' stays in.
	 * Asserted first because it is the one this seam must not have
	 * changed. */
	CHECK(eval_eq("(progn (defvar plt1 'D) (let ((plt1 'L)) plt1))", "L"));
	CHECK(eval_eq("plt1", "D"));

	/* A lambda parameter named after a special is bound LEXICALLY and
	 * pushes no dynamic binding, so it reaches no tag either: the
	 * buffer-local binding underneath is untouched by the call. */
	CHECK(eval_eq("(with-current-buffer (get-buffer-create \"tag-one\")"
		      " (setq-local plt1 'A)"
		      " (list ((lambda (plt1) plt1) 'ARG) plt1))",
	    "(ARG A)"));

	/* A stamp is not reused: the binding this `let' displaced is killed
	 * inside the form and a DIFFERENT variable takes the table slot, so
	 * a tag matched by index rather than by identity would restore the
	 * old value into the new binding.  It must be dropped instead. */
	CHECK(eval_eq("(with-current-buffer (get-buffer \"tag-one\")"
		      " (let ((plt1 'L))"
		      "  (kill-local-variable 'plt1)"
		      "  (setq-local plt2 'FRESH))"
		      " (list plt1 (local-variable-p 'plt1) plt2))",
	    "(D nil FRESH)"));

	/* The buffer a live binding named is killed while another buffer is
	 * current: the restore has nowhere to land and drops the value
	 * rather than writing it into whatever is in force.  Emacs' own
	 * answer -- its unbind consults `local-variable-p SYM WHERE', and a
	 * killed buffer has no local variables. */
	CHECK(eval_eq("(progn (defvar plt3 'D)"
		      " (with-current-buffer (get-buffer-create \"tag-two\")"
		      "  (setq-local plt3 'A))"
		      " (get-buffer-create \"tag-three\")"
		      " (with-current-buffer (get-buffer \"tag-two\")"
		      "  (let ((plt3 'L))"
		      "   (set-buffer (get-buffer \"tag-three\"))"
		      "   (kill-buffer (get-buffer \"tag-two\"))))"
		      " (list (default-value 'plt3)"
		      "  (with-current-buffer (get-buffer \"tag-three\")"
		      "   plt3)))",
	    "(D D)"));

	/* And an error out of such a form restores the same way a normal
	 * return does: the drain that runs the restore is the same one on
	 * every completion kind. */
	CHECK(eval_eq("(progn (defvar plt4 'D)"
		      " (with-current-buffer (get-buffer-create \"tag-four\")"
		      "  (setq-local plt4 'A))"
		      " (with-current-buffer (get-buffer-create \"tag-five\")"
		      "  (setq-local plt4 'B))"
		      " (condition-case nil"
		      "  (with-current-buffer (get-buffer \"tag-four\")"
		      "   (let ((plt4 'L))"
		      "    (set-buffer (get-buffer \"tag-five\"))"
		      "    (car 5)))"
		      "  (error nil))"
		      " (list (buffer-local-value 'plt4"
		      "   (get-buffer \"tag-four\"))"
		      "  (buffer-local-value 'plt4 (get-buffer \"tag-five\"))"
		      "  (default-value 'plt4)))",
	    "(A B D)"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_phase17_with_temp_buffer(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(with-temp-buffer (insert \"x\") 42)", "42"));
	/* The caller's buffer is current again, and untouched. */
	CHECK(eval_eq("(progn (with-temp-buffer (insert \"x\"))"
		      " (buffer-name (current-buffer)))",
	    "bridge.txt"));
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", ""));
	/* A nested use gets its own buffer. */
	CHECK(eval_eq("(with-temp-buffer (insert \"outer\")"
		      " (with-temp-buffer (insert \"inner\"))"
		      " (buffer-substring (point-min) (point-max)))",
	    "outer"));
	/* And the buffer really is gone: two uses in a row leave the buffer
	 * list where they found it. */
	CHECK(eval_eq("(let ((n (length (buffer-list))))"
		      " (with-temp-buffer (insert \"a\"))"
		      " (with-temp-buffer (insert \"b\"))"
		      " (- (length (buffer-list)) n))",
	    "0"));
	/* An error out of the body still reaches the caller. */
	CHECK(eval_error_contains(
	    "(with-temp-buffer (insert \"x\") (car 1))", "expected pair"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* switch-to-buffer is the function form kg had only as a command, and
 * the one that makes a package able to put its own buffer on screen.
 * What is asserted here is the half a PTY case cannot see cheaply: it
 * creates on a new name, resolves an existing one, and leaves the
 * created buffer CURRENT for the rest of the form, which is what lets a
 * package switch and then write. */
static void test_phase17_switch_to_buffer(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq(
	    "(buffer-name (switch-to-buffer \"*report*\"))", "*report*"));
	CHECK(eval_eq("(progn (switch-to-buffer \"*report*\")"
		      " (insert \"x\") (buffer-name (current-buffer)))",
	    "*report*"));
	/* A second switch to the same name finds it rather than making a
	 * second buffer of that name. */
	CHECK(eval_eq("(let ((n (length (buffer-list))))"
		      " (switch-to-buffer \"*report*\")"
		      " (- (length (buffer-list)) n))",
	    "0"));
	/* It takes a buffer OBJECT as well as a name. */
	CHECK(eval_eq("(buffer-name (switch-to-buffer"
		      " (get-buffer \"*report*\")))",
	    "*report*"));
	CHECK(
	    eval_error_contains("(switch-to-buffer 5)", "Wrong type argument"));

	/* Leave the harness as it was found.  The stub bufmgr frees a killed
	 * buffer's name and nothing else does, so a test that creates a
	 * buffer and walks away leaks it -- which is what .ci/ci-04's
	 * LeakSanitizer reported for this function's first version. */
	CHECK(eval_ok("(progn (with-current-buffer (get-buffer \"*report*\")"
		      " (set-buffer-modified-p nil))"
		      " (kill-buffer (get-buffer \"*report*\")))"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_with_current_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "main buffer", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq b2 (get-buffer-create \"buf2\"))"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 1. Body operates on non-displayed buffer, displayed window unchanged.
	 */
	CHECK(eval_ok("(with-current-buffer b2 (insert \"hidden content\"))"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));
	CHECK(eval_eq("(with-current-buffer b2 (buffer-substring 1 15))",
	    "hidden content"));

	/* 2. Body errors, context is restored. */
	CHECK(eval_error_contains(
	    "(with-current-buffer b2 (car 1))", "expected pair"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 3. Buffer argument is dead/stale: the body never runs, and the
	 * current buffer is unchanged afterwards.  Since 11D Part 4 the
	 * resolution is `(set-buffer BUF)' as the body's own first form
	 * inside the unwind-protect, not an argument the native resolved
	 * before entering -- so the error is raised a step later than it was,
	 * with the same observable outcome and with the cleanup running.
	 * Note: buf2 is dirty after insert, so kill-buffer with optional
	 * buffer obj works or we clean it first */
	CHECK(eval_ok("(setq b3 (get-buffer-create \"buf3\"))"));
	CHECK(eval_ok("(kill-buffer b3)"));
	CHECK(eval_error_contains(
	    "(with-current-buffer b3 (insert \"hi\"))", "buffer is dead"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A hook member that signals keeps its place; Emacs' would have been
 * removed (sub-plan 10D Part 1).
 *
 * Measured under /opt-3/emacs-31-lucid/bin/emacs 31.0.90 before this was
 * written, not read out of a manual: a member of `after-change-functions'
 * that signals is called exactly ONCE across three insertions -- the
 * error reaches the caller and Emacs empties the hook, so the two later
 * insertions are clean and the hook's value is nil afterwards.  kg
 * contains the error, reports `Hook error (...)', and leaves the member
 * armed, so the same shape calls it three times.
 *
 * Recorded, not fixed: which policy is right is a host design question
 * (Emacs' disarming is a modification-hook rule, not a general hook
 * rule, and kg's containment is what makes C-g and budget completions
 * survive a hook), and a package can take Emacs' policy for itself --
 * lisp/auto-fill.el does, with a condition-case that removes itself, and
 * test/pty/lisp-auto-fill-mode-error-disarms.yaml is the proof that
 * works.  This test is the divergence's own witness, cited by
 * test/lisp-compat/features.json's hook-error-does-not-disarm row.
 *
 * before-save-hook rather than after-change-functions: the counting is
 * the property, run-hooks is the direct way to run one three times, and
 * it needs no editing to do it. */
static void test_hook_error_does_not_disarm(void)
{
	int i;

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq disarm-calls 0)"));
	CHECK(eval_ok("(defun disarm-hook-fn ()"
		      " (setq disarm-calls (+ disarm-calls 1))"
		      " (car 1))"));
	CHECK(eval_ok("(add-hook 'before-save-hook 'disarm-hook-fn)"));
	for (i = 0; i < 3; i++) {
		test_status_message[0] = '\0';
		CHECK(eval_ok("(run-hooks 'before-save-hook)"));
		/* Every run reports, which is the other half of "still
		 * armed": Emacs reports once. */
		CHECK(strstr(test_status_message, "Hook error") != nullptr);
	}
	/* Three calls here; Emacs' equivalent shape answers 1. */
	CHECK(eval_eq("disarm-calls", "3"));
	/* And the entry is genuinely still there, not merely re-added: a
	 * repaired definition runs from the same registration. */
	CHECK(eval_ok("(defun disarm-hook-fn () (setq disarm-calls 'healed))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("disarm-calls", "healed"));
	CHECK(eval_ok("(remove-hook 'before-save-hook 'disarm-hook-fn)"));

	kg_lisp_shutdown();
}

static void test_hooks(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* The value form: a closure handed over directly.  (A defun'd name
	 * would be void as a value -- its function lives in the function
	 * cell, so a bare my-hook read would be void-variable.) */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(setq my-hook (lambda () (setq hook-ran t)))"));
	CHECK(eval_ok("(add-hook 'before-save-hook my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "t"));

	/* remove-hook */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(remove-hook 'before-save-hook my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "nil"));

	/* The quoted-designator form is the Emacs idiom: the symbol is
	 * stored and resolved through its function cell when the hook runs.
	 * It used to register without error and then do nothing, because
	 * the symbol reached FeCallWithOptions unresolved. */
	CHECK(eval_ok("(defun my-hook () (setq hook-ran t))"));
	CHECK(eval_ok("(add-hook 'before-save-hook 'my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "t"));

	/* Resolution happens when the hook runs, not when it is added, so
	 * redefining the function afterwards takes effect -- as in Emacs. */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(defun my-hook () (setq hook-ran 'redefined))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "redefined"));

	/* ... and removing by the same designator really removes it: the
	 * proof is that the next run leaves the flag alone. */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(remove-hook 'before-save-hook 'my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "nil"));

	/* A symbol whose function cell is empty is a contained hook error
	 * that names the symbol -- what README.md and doc/lisp-api.md
	 * promise.  Resolving the designator before FeCallWithOptions sees
	 * it used to turn this into Fe's anonymous "tried to call
	 * non-callable value". */
	CHECK(eval_ok("(add-hook 'before-save-hook 'never-defined-hook-fn)"));
	test_status_message[0] = '\0';
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(strstr(test_status_message, "Hook error") != nullptr);
	CHECK(strstr(test_status_message, "void-function") != nullptr);
	CHECK(strstr(test_status_message, "never-defined-hook-fn") != nullptr);
	/* Contained: the interpreter is still usable, and defining the name
	 * afterwards makes the very same hook entry work, since resolution
	 * happens when the hook runs. */
	CHECK(eval_ok("(+ 1 2)"));
	CHECK(
	    eval_ok("(defun never-defined-hook-fn () (setq hook-ran 'late))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "late"));
	CHECK(
	    eval_ok("(remove-hook 'before-save-hook 'never-defined-hook-fn)"));

	/* A cell holding something Fe will not call from the host side -- a
	 * macro, or a primitive -- is named too, and is equally contained.
	 * Both diagnostics used to reach FeCall's own guard, which raises
	 * from a point the guarded frame above does not catch when the hook
	 * runs from a (run-hooks) inside a live evaluator run: the editor
	 * died there rather than printing anything. */
	CHECK(eval_ok("(add-hook 'before-save-hook 'cond)"));
	test_status_message[0] = '\0';
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(strstr(test_status_message, "invalid-function cond") != nullptr);
	CHECK(eval_ok("(remove-hook 'before-save-hook 'cond)"));
	CHECK(eval_ok("(+ 1 2)"));

	/* A cyclic alias chain was the last uncontained one: resolving the
	 * designator raised `cyclic-function-indirection' from a C frame
	 * that had already returned by the time the handler longjmped, so
	 * this case segfaulted rather than reporting anything.  fe's
	 * host-facing resolver answers nil for a cycle now, which lands the
	 * hook in the same reported-not-raised path an empty cell takes.
	 * `void-function' is the condition it lands under, deliberately: a
	 * cycle and a dead multi-link chain are both nil from the resolver
	 * and nothing available here separates them (see
	 * lisp_callable_designator). */
	CHECK(eval_ok("(fset 'cyc-hook 'cyc-hook)"));
	CHECK(eval_ok("(add-hook 'before-save-hook 'cyc-hook)"));
	test_status_message[0] = '\0';
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(strstr(test_status_message, "Hook error") != nullptr);
	CHECK(strstr(test_status_message, "void-function cyc-hook") != nullptr);
	CHECK(eval_ok("(remove-hook 'before-save-hook 'cyc-hook)"));
	CHECK(eval_ok("(+ 1 2)"));
	/* Calling the same name still raises, and is caught as an error
	 * rather than taking the editor down: only the *host*-side resolver
	 * stopped raising. */
	CHECK(eval_error_contains("(funcall 'cyc-hook)",
	    "Symbol's chain of function indirections contains a loop"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A process filter named by a quoted symbol must resolve through its
 * function cell when the callback runs.  set-process-filter always
 * accepted symbols, but nothing ever resolved them, so the callback
 * silently never ran; the function-cell-only rule is the same one hooks
 * and functionp follow.  The value namespace is not consulted. */
static void test_process_callback_designator(void)
{
	int i;

	setup_editor();
	kg_process_table_init();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq filter-ran nil)"));
	CHECK(
	    eval_ok("(defalias 'p-filter (lambda (p s) (setq filter-ran t)))"));
	CHECK(eval_ok(
	    "(setq proc (start-shell-command \"filt\" nil \"echo hi\"))"));
	CHECK(eval_ok("(set-process-filter proc 'p-filter)"));

	/* Drive the child to completion the way the editor's main loop
	 * would: poll the process table and drain the published events. */
	for (i = 0; i < 2000 && !eval_eq("filter-ran", "t"); i++) {
		kg_process_table_poll();
		kg_event_drain_safe();
		usleep(1000);
	}
	CHECK(eval_eq("filter-ran", "t"));

	/* A name bound only as a value has an empty function cell, so the
	 * callback never runs -- and says so by name, as a contained
	 * "Process filter error: void-function only-value" on the status
	 * line rather than Fe's anonymous non-callable complaint. */
	CHECK(eval_ok("(setq filter-ran nil)"));
	test_status_message[0] = '\0';
	CHECK(
	    eval_ok("(setq only-value (lambda (p s) (setq filter-ran 'ran)))"));
	CHECK(eval_ok(
	    "(setq proc2 (start-shell-command \"filt2\" nil \"echo yo\"))"));
	CHECK(eval_ok("(set-process-filter proc2 'only-value)"));

	for (i = 0; i < 2000; i++) {
		kg_process_table_poll();
		kg_event_drain_safe();
		usleep(1000);
		if (eval_eq("filter-ran", "t")) {
			break;
		}
	}
	CHECK(eval_eq("filter-ran", "nil"));
	CHECK(strstr(test_status_message, "Process filter error") != nullptr);
	CHECK(strstr(test_status_message, "void-function") != nullptr);
	CHECK(strstr(test_status_message, "only-value") != nullptr);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_keymap_apis(void)
{
	struct keymap *global, *mode;

	setup_editor();
	/* The editor builds these in kbd.c, which this harness does not
	 * link; without them every map name would be one define-key
	 * invented on the spot, and the Emacs spellings below would have
	 * nothing real to resolve to. */
	keymap_reset();
	global = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	mode = keymap_create("dired", KEYMAP_LAYER_MAJOR);
	CHECK(global != nullptr && mode != nullptr);
	keymap_set_active(mode, 0);
	CHECK(kg_lisp_init() == 0);

	/* The Emacs spelling "global-map" resolves to kg's "global". */
	CHECK(eval_ok("(define-key 'global-map \"C-c k\" 'goto-line)"));
	CHECK(eval_eq("(lookup-key 'global-map \"C-c k\")", "goto-line"));
	CHECK(eval_eq("(lookup-key 'global \"C-c k\")", "goto-line"));
	CHECK(eval_eq("(lookup-key 'global-map \"C-c q\")", "nil"));

	/* lookup-key reports what one map says, not what the editor would
	 * do now: an inactive mode map still answers for its own keys, and
	 * does not answer for the global map's. */
	CHECK(eval_ok("(define-key 'dired-mode-map \"C-c d\" 'goto-line)"));
	CHECK(eval_eq("(lookup-key 'dired-mode-map \"C-c d\")", "goto-line"));
	CHECK(eval_eq("(lookup-key 'dired-mode-map \"C-c k\")", "nil"));
	CHECK(eval_eq("(lookup-key 'global-map \"C-c d\")", "nil"));

	/* An unknown map name is nil rather than a map invented for it. */
	CHECK(eval_eq("(lookup-key 'no-such-map \"C-c k\")", "nil"));

	/* current-local-map names the active major-mode map, and nothing
	 * when none is active. */
	CHECK(eval_eq("(current-local-map)", "nil"));
	keymap_set_active(mode, 1);
	CHECK(eval_eq("(current-local-map)", "dired"));

	/* define-key on a name no map answers to creates a major-mode map
	 * rather than failing, and nil as the command unbinds. */
	CHECK(eval_ok("(define-key 'fresh-mode-map \"C-c f\" 'goto-line)"));
	CHECK(eval_eq("(lookup-key 'fresh-mode-map \"C-c f\")", "goto-line"));
	CHECK(eval_ok("(define-key 'fresh-mode-map \"C-c f\" nil)"));
	CHECK(eval_eq("(lookup-key 'fresh-mode-map \"C-c f\")", "nil"));

	/* A sequence keymap_parse_sequence() cannot make sense of is
	 * refused outright rather than half-registered. */
	CHECK(eval_error_contains(
	    "(define-key 'global-map \"not-a-key\" 'goto-line)",
	    "cannot bind key sequence"));

	/* An empty name is not a map, a key or a command. */
	CHECK(eval_error_contains(
	    "(define-key \"\" \"C-c f\" 'goto-line)", "invalid command name"));

	kg_lisp_shutdown();
	teardown_editor();
}

int main(void)
{
	/* Pure and Fe-free, so it is the same function and the same answers
	 * in a WITH_LISP=0 build: run it before the early return. */
	RUN(test_number_token_classifier);
	if (!kg_lisp_active()) {
		RUN(test_disabled);
		return test_summary();
	}
	test_lisp_command_exists = kg_lisp_command_exists;
	test_lisp_command_run = kg_lisp_run_command;

	RUN(test_lifecycle);
	RUN(test_arena_env_parsing);
	RUN(test_arena_env_floor_refused);
	RUN(test_arena_floor_matches_census);
	RUN(test_eval_and_recovery);
	RUN(test_sized_input);
	RUN(test_variable_non_nil);
	RUN(test_fill_column_variable);
	RUN(test_fill_region);
	RUN(test_load_file);
	RUN(test_phase12_one_arg_defvar_file_scope);
	RUN(test_load_file_error);
	RUN(test_load_error_condition_reachability);
	RUN(test_load_incremental_loop);
	RUN(test_interrupt);
	RUN(test_load_quit);
	RUN(test_cleanup_raise_residuals);
	RUN(test_message_arity);
	RUN(test_insert_and_undo);
	RUN(test_insert_read_only_recovery);
	RUN(test_goto_line_and_buffer_name);
	RUN(test_command_allow_list);
	RUN(test_init_file);
	RUN(test_load_package);
	RUN(test_require_provide);
	RUN(test_require_el_suffix);
	RUN(test_load_path_order);
	RUN(test_load_path_missing_dirs);
	RUN(test_define_and_run_command);
	RUN(test_defun_redefinition_is_atomic);
	RUN(test_interactive_prompt_context_refusal);
	RUN(test_phase16_read_forms);
	RUN(test_run_command_interrupt);
	RUN(test_run_command_reentrancy);
	RUN(test_command_prefix_delivery);
	RUN(test_key_bindings);
	RUN(test_math_natives);
	RUN(test_point_offsets);
	RUN(test_current_column_tab);
	RUN(test_char_after);
	RUN(test_buffer_substring);
	RUN(test_mark_and_region);
	RUN(test_word_motion);
	RUN(test_word_motion_crosses_rows);
	RUN(test_editor_bridge);
	RUN(test_buffer_objects);
	RUN(test_kill_buffer);
	RUN(test_buffer_stale_slot);
	RUN(test_point_table_reaches_the_last_slot);
	RUN(test_buffer_point_sync);
	RUN(test_numeric_argument_seam);
	RUN(test_point_survives_set_buffer);
	RUN(test_hidden_buffer_point_persists_across_frames);
	RUN(test_frame_entry_overwrites_from_window_cursor);
	RUN(test_buffer_object_capacity);
	RUN(test_delete_region);
	RUN(test_delete_region_utf8);
	RUN(test_delete_region_malformed_byte);
	RUN(test_delete_region_read_only_recovery);
	RUN(test_replace_region);
	RUN(test_replace_region_read_only_recovery);
	RUN(test_replace_region_alloc_failure_is_atomic);
	RUN(test_search_forward_backward);
	RUN(test_search_noerror_three_ways);
	RUN(test_search_forward_bound);
	RUN(test_re_search_and_match_data);
	RUN(test_re_search_backward);
	RUN(test_regex_too_complex_and_bad_pattern);
	RUN(test_search_cancellation);
	RUN(test_markers);
	RUN(test_marker_survives_buffer_kill);
	RUN(test_save_excursion);
	RUN(test_save_excursion_pool_bound);
	RUN(test_phase17_line_motion);
	RUN(test_phase17_char_motion);
	RUN(test_phase17_looking_at);
	RUN(test_phase17_skip_chars);
	RUN(test_phase17_buffer_edits);
	RUN(test_phase17_buffer_status);
	RUN(test_phase20_variable_documentation);
	RUN(test_phase18_buffer_locals);
	RUN(test_let_binding_buffer_tag);
	RUN(test_phase17_with_temp_buffer);
	RUN(test_phase17_switch_to_buffer);
	RUN(test_with_current_buffer);
	RUN(test_hooks);
	RUN(test_hook_error_does_not_disarm);
	RUN(test_process_callback_designator);
	RUN(test_keymap_apis);
	RUN(test_compare_strings);
	RUN(test_regexp_opt);
	RUN(test_assoc_string_and_designator);
	RUN(test_string_length_and_substring);
	RUN(test_string_concat_and_equal);
	RUN(test_format_natives);
	RUN(test_char_string_round_trip);
	RUN(test_thing_at_point);
	RUN(test_line_motion);
	RUN(test_prelude_forms);
	RUN(test_elisp_if);
	RUN(test_list_library);
	RUN(test_type_predicates);
	RUN(test_numeric_core_error_rules);
	RUN(test_binding_forms);
	RUN(test_phase11_dynamic_binding);
	RUN(test_definition_forms);
	RUN(test_strict_arity);
	RUN(test_writer_quote_abbreviation);
	RUN(test_quasiquote);
	RUN(test_void_function);
	RUN(test_void_variable);
	RUN(test_cyclic_result);
	RUN(test_recursion_depth);
	RUN(test_perf_dump_fe_json_facade);
	RUN(test_arena_exhaustion_conditions);
	RUN(test_condition_case_native_error);
	RUN(test_native_primitive_call_is_contained);
	RUN(test_catch_throw_unwind);
	RUN(test_wrapping_native_transparency);
	RUN(test_condition_case_kg_native_conditions);
	RUN(test_error_message_rendering);
	RUN(test_interactive_form_reflection);
	RUN(test_help_fns_surface);
	RUN(test_phase13_trap_battery);
	RUN(test_phase14_symbols);
	RUN(test_phase14_excursion_hygiene);
	RUN(test_prelude_temporary_hygiene);
	RUN(test_phase15_string_natives);
	RUN(test_phase15_regex_seam);
	RUN(test_phase15_string_library);
	RUN(test_phase15_list_library);
	RUN(test_phase15_sort);
	RUN(test_phase15_seq_shim);
	RUN(test_phase29_subr_x);
	RUN(test_phase29_package_preamble_names);
	RUN(test_phase29_cl_lib);
	RUN(test_phase15_arithmetic);
	RUN(test_hook_quit_is_not_contained);
	RUN(test_hook_throw_containment);
	RUN(test_signal);
	RUN(test_ignore_errors);
	RUN(test_condition_case_reentry);
	RUN(test_cleanup_containment_keeps_condition);
	RUN(test_phase12_cleanup_handler_visibility);
	RUN(test_quit_uncaught);
	RUN(test_phase8_constants_and_keywords);
	RUN(test_phase8_reader_literals);
	RUN(test_reader_form_feed_page_break);
	RUN(test_autoload_no_op);
	RUN(test_sel_frontier_vector_literal);
	RUN(test_phase24_vectors);
	RUN(test_phase25_strings);
	RUN(test_phase25_match_data);
	RUN(test_phase26_count_matches);
	RUN(test_phase26_replace_match);
	RUN(test_s_el_vendored_load);
	RUN(test_phase8_library);
	RUN(test_captured_function_value);
	RUN(test_native_reverse_gc_stack);
	RUN(test_prelude_source_file);
	return test_summary();
}
