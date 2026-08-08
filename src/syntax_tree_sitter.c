/* ================== Tree-sitter syntax highlighting backend ===============
 *
 * The second implementation of the private contract in
 * src/syntax_backend.h, selected by the Makefile's source list when
 * WITH_TREE_SITTER=1 -- never by a preprocessor branch, so a build that
 * installs this backend simply does not compile src/syntax_legacy.c, and a
 * default build does not compile this file.
 *
 * This is the SKELETON the build-axis slice lands
 * (doc/plans/kg-tree-sitter-plan.md, Phase 5).  It parses nothing and
 * colours nothing: every row comes out HL_NORMAL, which is exactly what the
 * plan says a language with no grammar must degrade to (Refinement decision
 * 4), so a WITH_TREE_SITTER=1 kg today is an ordinary plain-text editor
 * that happens to link libtree-sitter.  What arrives later: a row-backed
 * TSInput and the first grammar, then TSInputEdit-driven incremental
 * reparsing and ranged highlighting, then the dlopen'd grammar/query
 * registry.  `struct kg_syntax_state` becomes the per-buffer parser and
 * tree at that point; it stays undefined and NULL here.
 *
 * What this file does have to do now is prove the link.  ts_parser_new()
 * and ts_parser_delete() below are called for their effect on the build:
 * a missing header, a core library the linker cannot find, or a shared
 * object the loader cannot reach at run time all fail in the slice that
 * added the dependency rather than in the slice that starts using it. */

#include <tree_sitter/api.h>

#include "def.h"
#include "syntax_backend.h"

/* Opaque to this backend, which keeps none yet; the type is never defined
 * here (syntax.h forward-declares it for everyone else). */
struct kg_syntax_state;

/* The backend contract: colour one non-empty row.  The facade has already
 * reserved row->hl for row->rsize bytes and filled every one of them with
 * HL_NORMAL, so a backend that recognizes nothing returns. */
void syntax_backend_update_row(struct editor_buffer *b, struct erow *row)
{
	(void)b;
	(void)row;
}

/* The other half: derive what this backend keeps about a whole staged
 * document, and colour its rows, because a load pays for no second
 * highlighting pass and the display reads row->hl unconditionally.
 * Colouring is the facade's per-row call, which reserves and blanks hl and
 * then asks this backend for the row -- nothing above, so the pass is
 * "allocate and blank".  No cross-row state means no need for the legacy
 * backend's row-at-a-time copy of the record: the staged record is walked
 * in place and left exactly as it was found.
 *
 * Failure is the facade's per-row failure: syntax_update_row_only() clears
 * `running` when it cannot reserve a row's hl. */
struct kg_syntax_state *syntax_backend_prepare(
    struct editor_buffer *staged, int *ok)
{
	int saved_running = running;
	TSParser *parser;
	int i;

	parser = ts_parser_new();
	ts_parser_delete(parser);

	*ok = 1;
	for (i = 0; i < staged->numrows; i++) {
		syntax_update_row_only(staged, &staged->row[i]);
		if (running != saved_running) {
			running = saved_running;
			*ok = 0;
			break;
		}
	}
	return NULL;
}

void syntax_backend_state_free(struct kg_syntax_state *st) { (void)st; }
