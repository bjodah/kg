/* kgbatch - repeatable, terminal-free init-file and corpus driver.
 *
 * It links the editor's own objects and calls kg_lisp_eval_string(), so it
 * really is kg's evaluator; what it does not have is a terminal, a key
 * loop or a screen.  Two things use it:
 *
 *   * an audit, as a one-second experiment -- "what does kg's Lisp answer
 *     for this file?" -- which is why it is not in TESTBINS and asserts
 *     nothing of its own;
 *   * utils/check_lisp_oracle.py, which drives one case per process and
 *     compares the printed record against the checked-in Emacs snapshot.
 *
 * The flags exist for the second caller and for the GC-stress lane, and
 * are described where they are parsed.  Note what stayed out: there is
 * still no terminal, no key input and no screen refresh here.  `-b` gives
 * the process a *buffer*, which is editor state, not an editor.
 *
 * Output contract, which the runner parses: on success, one line
 * `PATH: RENDERING` on stdout and exit 0; on a Lisp error, one line
 * `PATH: MESSAGE` on stderr and a nonzero exit.  In `-r` mode RENDERING
 * starts `V:` (value), `E:` (condition symbol), or `Q:` (quit).  The caller
 * knows the path it passed, so the prefix is unambiguous. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "lisp.h"

/* Big enough that a corpus case's rendering is compared rather than
 * truncated; kg_lisp_eval_string() snprintf()s into it. */
#define KGBATCH_RESULT_MAX 4096

/* `-p`: print the file's value the way `prin1` does -- a top-level string
 * comes back quoted and escaped -- by evaluating
 *
 *     (format "%S" (progn FILE-CONTENTS))
 *
 * rather than the contents bare.  Without it a case whose value is the
 * string "abc" prints `abc`, which is not what any Emacs snapshot holds
 * and is indistinguishable from the symbol `abc`.  `progn` returns its
 * last form's value, so wrapping the whole file is exactly "evaluate the
 * setup forms, then the expression" -- the same shape
 * oracle/emacs-shim.el runs -- and `format`'s %S is kg's own writer, so
 * this adds no second rendering path to disagree with the first. */
static char *wrap_source(char *source, size_t length, const char *prefix,
    const char *suffix, size_t *out_length)
{
	size_t prefix_length = strlen(prefix), suffix_length = strlen(suffix);
	size_t total = prefix_length + length + suffix_length;
	char *wrapped = malloc(total + 1);

	if (!wrapped) {
		return nullptr;
	}
	memcpy(wrapped, prefix, prefix_length);
	memcpy(wrapped + prefix_length, source, length);
	memcpy(wrapped + prefix_length + length, suffix, suffix_length);
	wrapped[total] = '\0';
	*out_length = total;
	return wrapped;
}

/* `-r`: make the result self-describing without a new public adapter API.
 * The condition-case runs inside kg's evaluator, where the condition object
 * is available: ordinary errors therefore report their exact symbol rather
 * than making the oracle runner search diagnostic prose.  Reader errors occur
 * before this wrapper can run, and uncatchable budgets still use kgbatch's
 * ordinary nonzero/message contract. */
static char *wrap_in_record(char *source, size_t length, size_t *out_length)
{
	static const char prefix[] = "(condition-case kgbatch--condition\n"
				     "    (format \"V:%S\" (progn\n";
	static const char suffix[]
	    = "\n))\n"
	      "  (quit \"Q:quit\")\n"
	      "  (error (format \"E:%S\" (car kgbatch--condition))))\n";

	return wrap_source(source, length, prefix, suffix, out_length);
}

/* `-b`: a live scratch buffer, so a form that reads or moves point runs
 * instead of raising `current buffer is dead`.  lisp_exec_enter() resolves
 * the *window's* buffer and leaves the execution context empty when there
 * is none, which is what a bare kgbatch has: no editor ever started.
 * Everything below is buffer/window state a running editor would have set;
 * nothing here opens a terminal. */
static void open_scratch_buffer(void)
{
	struct editor_buffer *b = bcur();

	memset(&editor, 0, sizeof(editor));
	memset(b, 0, sizeof(*b));
	b->id = 1;
	b->active = 1;
	b->readonly_override = -1;
	b->filename = nullptr;
	/* buf_load_args() is what normally claims the first slot, and it sets
	 * this; without it bufmgr believes it holds zero buffers and refuses
	 * to kill any -- buf_kill_buffer() keeps the last one alive -- so the
	 * first `with-temp-buffer' of a run leaked its buffer.  Found by
	 * Phase 17's with-temp-buffer, which is the first thing here to kill
	 * a buffer at all. */
	buf_count = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	winlist[win_current].active = 1;
	wcur()->buf = buf_handle(buf_current);
	undo_init();
}

static char *read_file(const char *path, size_t *out_length)
{
	FILE *file = fopen(path, "rb");
	char *source;
	long length;
	size_t read;

	if (!file) {
		perror(path);
		return nullptr;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
	    || fseek(file, 0, SEEK_SET) != 0) {
		perror(path);
		fclose(file);
		return nullptr;
	}
	source = malloc((size_t)length + 1);
	if (!source) {
		fprintf(stderr, "%s: %s\n", path, strerror(ENOMEM));
		fclose(file);
		return nullptr;
	}
	read = fread(source, 1, (size_t)length, file);
	fclose(file);
	if (read != (size_t)length) {
		fprintf(stderr, "%s: cannot read file\n", path);
		free(source);
		return nullptr;
	}
	source[length] = '\0';
	*out_length = (size_t)length;
	return source;
}

static int evaluate_file(const char *path, bool prin1, bool record)
{
	char result[KGBATCH_RESULT_MAX];
	size_t length;
	char *source = read_file(path, &length);
	char *evaluated = source;
	int failed;

	if (!source) {
		return 1;
	}
	if (record) {
		evaluated = wrap_in_record(source, length, &length);
	} else if (prin1) {
		evaluated = wrap_source(source, length,
		    "(format \"%S\" (progn\n", "\n))\n", &length);
		if (!evaluated) {
			fprintf(stderr, "%s: %s\n", path, strerror(ENOMEM));
			free(source);
			return 1;
		}
	}
	failed = kg_lisp_eval_string(evaluated, length, result, sizeof(result));
	if (evaluated != source) {
		free(evaluated);
	}
	free(source);
	if (failed) {
		fprintf(stderr, "%s: %s\n", path, result);
		return 1;
	}
	printf("%s: %s\n", path, result);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	    "usage: %s [-p|-r] [-b] [-g] [-a] FILE...\n"
	    "  -p  print the value the way prin1 does (strings quoted)\n"
	    "  -r  print a tagged value, condition symbol, or quit record\n"
	    "  -b  open a live scratch buffer before evaluating\n"
	    "  -g  print one arena-stats line after the results\n"
	    "  -a  print one census line with every FeArenaStats field\n",
	    argv0);
}

/* `-g`: the arena-stats surface, printed rather than rendered into the
 * echo area the way M-x lisp-arena-stats does.  It exists for
 * utils/check_lisp_gc_stress.py, which runs the same script through an
 * ordinary kgbatch and through one whose fe was built with
 * -DFE_GC_STRESS=1 and compares the collection counts.  Off by default,
 * and never passed by the oracle runner, so kgbatch's one-line output
 * contract is unchanged for everything else. */
static void print_arena_stats(void)
{
	struct kg_lisp_arena_stats stats;

	if (kg_lisp_arena_stats(&stats) != 0) {
		fprintf(stderr, "arena stats unavailable\n");
		return;
	}
	printf("arena: collections=%zu peak-live=%zu failures=%zu\n",
	    stats.collection_count, stats.peak_live_objects,
	    stats.allocation_failures);
}

/* `-a': every FeArenaStats field on one line, prefixed `census:' rather
 * than `arena:' so utils/check_lisp_gc_stress.py's `^arena: ...$' regex
 * never has to change to tolerate it.  Built for
 * utils/prelude_slot_census.py (Phase 0.1's per-section slot census),
 * which needs total_slots alongside peak-live: total_slots is fixed by
 * the arena partition, not by how much of the prelude ran, so one `-a'
 * call against the real, whole prelude gives the denominator every `-g'
 * reading from a truncated prelude slice is read against. */
static void print_full_arena_stats(void)
{
	struct kg_lisp_arena_stats stats;

	if (kg_lisp_arena_stats(&stats) != 0) {
		fprintf(stderr, "arena stats unavailable\n");
		return;
	}
	printf("census: total=%zu free=%zu peak-live=%zu collections=%zu "
	       "failures=%zu\n",
	    stats.total_slots, stats.free_slots, stats.peak_live_objects,
	    stats.collection_count, stats.allocation_failures);
}

int main(int argc, char **argv)
{
	bool prin1 = false, record = false, scratch = false, stats = false;
	bool full_stats = false;
	int i, status = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			prin1 = true;
		} else if (strcmp(argv[i], "-r") == 0) {
			record = true;
		} else if (strcmp(argv[i], "-b") == 0) {
			scratch = true;
		} else if (strcmp(argv[i], "-g") == 0) {
			stats = true;
		} else if (strcmp(argv[i], "-a") == 0) {
			full_stats = true;
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else if (argv[i][0] == '-' && argv[i][1] != '\0') {
			usage(argv[0]);
			return 2;
		} else {
			break;
		}
	}
	if (i >= argc) {
		usage(argv[0]);
		return 2;
	}
	if (prin1 && record) {
		usage(argv[0]);
		return 2;
	}
	if (scratch) {
		open_scratch_buffer();
	}
	if (kg_lisp_init()) {
		fprintf(stderr, "cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	for (; i < argc; i++) {
		status |= evaluate_file(argv[i], prin1, record);
	}
	if (stats) {
		print_arena_stats();
	}
	if (full_stats) {
		print_full_arena_stats();
	}
	kg_lisp_shutdown();
	return status;
}
