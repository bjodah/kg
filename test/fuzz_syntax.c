/* fuzz_syntax.c -- a byte stream as a syntax-enabled buffer, then as edits.
 *
 * The first byte chooses a syntax mode.  C, P, S, J, R, M, L, Y, K, T and H
 * name the common C, Python, Shell, JavaScript, Rust, Markdown, Lisp, YAML,
 * Makefile, TypeScript and HTML modes; every other byte selects across the
 * whole HLDB registry.  The bytes after it, up to the first 0xff -- never a
 * valid UTF-8 byte, so plain source seeds are unaffected -- are file
 * contents, split at '\n' into rows and inserted through the editor's real
 * row builder.
 *
 * Bytes after the first 0xff are an edit script: each operation is a
 * two-byte position, a delete length, an insert length, and that many
 * bytes of replacement text, applied through kg_buffer_replace() -- the
 * transaction every command uses, and the one route into
 * syntax_after_edit().  Inserted '\n' bytes split rows and deleted ones
 * join them, so the incremental rescan sees every topology change.
 *
 * Two oracles run at the end.  Every row's hl[] must be allocated, large
 * enough, and hold only defined highlight values.  Then the incrementally
 * maintained hl bytes are snapshotted, the buffer is rebuilt from scratch
 * with syntax_rebuild(), and the two colourings must agree byte for byte:
 * an edit whose rescan stopped too early -- a stale hl_oc chain -- is
 * exactly a disagreement here.  In particular, the mode is installed
 * before the first row is built: the bug fixed by 32d37b4 was invisible to
 * fuzz_keypress because its buffer deliberately has no syntax mode.
 */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/syntax.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_SYNTAX_MAX_ROWS 128
#define FUZZ_SYNTAX_MAX_EDITS 32
#define FUZZ_SYNTAX_SCRIPT_SEP 0xff

static enum kg_mode_id fuzz_syntax_mode(uint8_t tag)
{
	switch (tag) {
	case 'C':
		return KG_MODE_C;
	case 'P':
		return KG_MODE_PYTHON;
	case 'S':
		return KG_MODE_SHELL;
	case 'J':
		return KG_MODE_JAVASCRIPT;
	case 'R':
		return KG_MODE_RUST;
	case 'M':
		return KG_MODE_MARKDOWN;
	case 'L':
		return KG_MODE_LISP;
	case 'Y':
		return KG_MODE_YAML;
	case 'K':
		return KG_MODE_MAKEFILE;
	case 'T':
		return KG_MODE_TYPESCRIPT;
	case 'H':
		return KG_MODE_HTML;
	default:
		return KG_MODE_C + tag % (KG_MODE_YAML - KG_MODE_C + 1);
	}
}

/* TypeScript's Tree-sitter registry chooses TSX from the filename.  The
 * legacy scanners do not care, but naming every buffer like a plausible
 * file keeps this target on the same selection inputs as a real load. */
static const char *fuzz_syntax_filename(enum kg_mode_id mode)
{
	switch (mode) {
	case KG_MODE_C:
		return "fuzz.c";
	case KG_MODE_PYTHON:
		return "fuzz.py";
	case KG_MODE_SHELL:
		return "fuzz.sh";
	case KG_MODE_JAVASCRIPT:
		return "fuzz.js";
	case KG_MODE_RUST:
		return "fuzz.rs";
	case KG_MODE_JAVA:
		return "Fuzz.java";
	case KG_MODE_TYPESCRIPT:
		return "fuzz.ts";
	case KG_MODE_CSHARP:
		return "Fuzz.cs";
	case KG_MODE_PHP:
		return "fuzz.php";
	case KG_MODE_RUBY:
		return "fuzz.rb";
	case KG_MODE_SWIFT:
		return "fuzz.swift";
	case KG_MODE_SQL:
		return "fuzz.sql";
	case KG_MODE_DART:
		return "fuzz.dart";
	case KG_MODE_HTML:
		return "fuzz.html";
	case KG_MODE_REACT:
		return "fuzz.tsx";
	case KG_MODE_VUE:
		return "fuzz.vue";
	case KG_MODE_ANGULAR:
		return "fuzz.component.ts";
	case KG_MODE_SVELTE:
		return "fuzz.svelte";
	case KG_MODE_MAKEFILE:
		return "Makefile";
	case KG_MODE_MARKDOWN:
		return "fuzz.md";
	case KG_MODE_LISP:
		return "fuzz.el";
	case KG_MODE_GIT_COMMIT:
		return "COMMIT_EDITMSG";
	case KG_MODE_GIT_REBASE:
		return "git-rebase-todo";
	case KG_MODE_YAML:
		return "fuzz.yaml";
	default:
		return "fuzz.txt";
	}
}

static void fuzz_syntax_reset(enum kg_mode_id mode)
{
	memset(&editor, 0, sizeof(editor));
	memset(buflist, 0, sizeof(buflist));
	memset(winlist, 0, sizeof(winlist));
	running = 1;
	global_auto_revert = 0;
	buf_current = 0;
	buf_count = 1;
	win_current = 0;
	win_count = 1;
	win_total_rows = 24;
	win_total_cols = 80;
	bcur()->active = 1;
	bcur()->id = 1;
	bcur()->filename = strdup(fuzz_syntax_filename(mode));
	bcur()->syntax = syntax_find_by_mode(mode);
	wcur()->h = 22;
	wcur()->w = 80;
	wcur()->desired_visual_col = -1;
	winlist[0].active = 1;
	winlist[0].buf.slot = 0;
	winlist[0].buf.id = 1;
}

static void fuzz_syntax_load(const uint8_t *text, size_t text_size)
{
	size_t pos = 0;
	int rows = 0;

	while (rows < FUZZ_SYNTAX_MAX_ROWS) {
		size_t start = pos;

		while (pos < text_size && text[pos] != '\n') {
			pos++;
		}
		editor_insert_row(bcur(), bcur()->numrows,
		    (const char *)text + start, pos - start);
		rows++;
		if (!running || pos == text_size) {
			return;
		}
		pos++;
	}
}

/* Each operation: pos_hi pos_lo del_len ins_len, then ins_len bytes of
 * replacement.  Positions and lengths are taken modulo what the buffer
 * has, so every script byte is meaningful and none is rejected. */
static void fuzz_syntax_edit(const uint8_t *script, size_t script_size)
{
	size_t pos = 0;
	int edits = 0;

	while (edits < FUZZ_SYNTAX_MAX_EDITS && running &&
	    pos + 4 <= script_size) {
		size_t blen = buffer_byte_length(bcur());
		size_t begin = ((size_t)script[pos] << 8 | script[pos + 1]) %
		    (blen + 1);
		size_t end = begin + script[pos + 2] % (blen - begin + 1);
		size_t ins = script[pos + 3];
		struct kg_edit e;

		pos += 4;
		if (ins > script_size - pos) {
			ins = script_size - pos;
		}
		/* KG_EDIT_INTERNAL takes the same syntax_after_edit()
		 * path as a user edit without needing the undo machinery
		 * this target never initializes. */
		e = kg_edit_internal(
		    bcur(), begin, end, (const char *)script + pos, ins);
		(void)kg_buffer_replace(&e, NULL);
		pos += ins;
		edits++;
	}
}

static void fuzz_syntax_check_rows(void)
{
	int i, j;

	for (i = 0; i < bcur()->numrows; i++) {
		erow *row = &bcur()->row[i];

		if (row->rsize == 0) {
			continue;
		}
		if (!row->render || !row->hl || row->hl_capacity < row->rsize) {
			abort();
		}
		for (j = 0; j < row->rsize; j++) {
			if (row->hl[j] > HL_PAREN_MISMATCH) {
				abort();
			}
		}
	}
}

/* The differential oracle: the hl bytes the incremental path maintained
 * must equal what a from-scratch rebuild of the same text produces.  A
 * disagreement is a rescan that stopped too early or leaked state. */
static void fuzz_syntax_check_rebuild_match(void)
{
	int numrows = bcur()->numrows;
	unsigned char **saved;
	int *sizes, *ocs;
	int i;

	saved = calloc(numrows ? numrows : 1, sizeof(*saved));
	sizes = calloc(numrows ? numrows : 1, sizeof(*sizes));
	ocs = calloc(numrows ? numrows : 1, sizeof(*ocs));
	if (!saved || !sizes || !ocs) {
		goto out;
	}
	for (i = 0; i < numrows; i++) {
		erow *row = &bcur()->row[i];

		sizes[i] = row->rsize;
		ocs[i] = row->hl_oc;
		if (row->rsize > 0) {
			saved[i] = malloc(row->rsize);
			if (!saved[i]) {
				goto out;
			}
			memcpy(saved[i], row->hl, row->rsize);
		}
	}
	syntax_rebuild(bcur());
	if (!running || bcur()->numrows != numrows) {
		goto out;
	}
	for (i = 0; i < numrows; i++) {
		erow *row = &bcur()->row[i];

		if (row->rsize != sizes[i]) {
			goto out;
		}
		if (row->hl_oc != ocs[i]) {
			fprintf(stderr,
			    "fuzz_syntax: row %d hl_oc %d after edits, "
			    "%d after rebuild\n",
			    i, ocs[i], row->hl_oc);
			abort();
		}
		if (row->rsize > 0 &&
		    memcmp(saved[i], row->hl, row->rsize) != 0) {
			int j;

			for (j = 0; j < row->rsize; j++) {
				if (saved[i][j] != row->hl[j]) {
					break;
				}
			}
			fprintf(stderr,
			    "fuzz_syntax: row %d render byte %d hl %d "
			    "after edits, %d after rebuild\n",
			    i, j, saved[i][j], row->hl[j]);
			abort();
		}
	}
out:
	for (i = 0; i < numrows; i++) {
		free(saved ? saved[i] : NULL);
	}
	free(saved);
	free(sizes);
	free(ocs);
}

static void fuzz_syntax_teardown(void)
{
	syntax_state_release(bcur());
	editor_free_all_rows(bcur());
	free(bcur()->filename);
	bcur()->filename = NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	enum kg_mode_id mode;
	const uint8_t *sep;
	size_t text_size;

	if (size == 0) {
		return 0;
	}
	mode = fuzz_syntax_mode(data[0]);
	fuzz_syntax_reset(mode);
	if (!bcur()->filename || !bcur()->syntax) {
		fuzz_syntax_teardown();
		return 0;
	}
	sep = memchr(data + 1, FUZZ_SYNTAX_SCRIPT_SEP, size - 1);
	text_size = sep ? (size_t)(sep - (data + 1)) : size - 1;
	fuzz_syntax_load(data + 1, text_size);
	if (running && sep) {
		fuzz_syntax_edit(sep + 1, size - 1 - text_size - 1);
	}
	if (running) {
		fuzz_syntax_check_rows();
		/* Runs the backend's full-document path -- also the
		 * production reload path a Tree-sitter backend uses --
		 * and cross-checks it against the incremental colouring. */
		fuzz_syntax_check_rebuild_match();
		fuzz_syntax_check_rows();
	}
	fuzz_syntax_teardown();
	return 0;
}
