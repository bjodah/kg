/* lsp_rename.c — see lsp_rename.h.
 *
 * Compiled in both configurations, the way src/xref.c is: the command
 * table's row for `lsp-rename` is unconditional, so a WITH_LSP=0 kg says
 * the feature was not compiled in rather than "Unknown command".
 */

#include "lsp_rename.h"

#ifdef KG_USE_LSP

#include "bufmgr.h"
#include "dabbrev.h"
#include "def.h"
#include "event.h"
#include "localvars.h"
#include "lsp_edit.h"
#include "lsp_json.h"
#include "lsp_req.h"
#include "lsp_sync.h"

#include <stdio.h>
#include <string.h>

/* The name every message this prints spells, which is the name a user
 * would type at M-x and the name kg(1) documents. */
#define RENAME_WHO "lsp-rename"

/* ------------------------------ the prompt ---------------------------- */

/* The symbol point is in or immediately after, as `thing-at-point` would
 * read it, into `out`.  Zero when there is none, which is a rename with
 * no default rather than a refusal -- the server decides whether the
 * position can be renamed, not kg.
 *
 * A word here is src/dabbrev.h's: the run of ASCII alphanumerics and
 * underscores M-f and M-d already move over.  Reading the abbreviation
 * before point and then the whole word it starts is what makes `fo|o`
 * and `foo|` both answer "foo". */
static int lsp_rename_symbol_at_point(char *out, size_t size)
{
	struct dabbrev_pos at
	    = { editor_current_filerow_or_eof(), editor_current_filecol() };
	struct dabbrev_pos start;
	const erow *rows = bcur()->row;
	int numrows = bcur()->numrows;
	int len;

	out[0] = '\0';
	if (dabbrev_abbrev_before(rows, numrows, at, &start) <= 0) {
		return 0;
	}
	len = dabbrev_word_at(rows, numrows, start);
	if (len <= 0 || (size_t)len >= size) {
		return 0;
	}
	memcpy(out, rows[start.row].chars + start.col, (size_t)len);
	out[len] = '\0';
	return len;
}

/* Read the new name.  Emacs' eglot prompts with the old name as the
 * minibuffer's DEFAULT-VALUE -- an empty answer means "that one" -- and
 * this is that, spelled in the one prompt facility kg has: the prompt
 * names the old symbol and an empty answer stands for it.  (kg's
 * minibuffer can be pre-filled instead, which is what M-x compile does,
 * but a pre-filled name has to be erased before a new one can be typed,
 * and a rename is a command you reach for to type something else.)
 *
 * False when the read was cancelled, when it overflowed, and when there
 * is neither a typed name nor a symbol to default to. */
static bool lsp_rename_read_name(int fd, char *out, int size)
{
	char old[LSP_RENAME_NAME_MAX];
	char prompt[LSP_RENAME_NAME_MAX + 64];
	int have = lsp_rename_symbol_at_point(old, sizeof(old));

	if (have > 0) {
		snprintf(prompt, sizeof(prompt), "Rename `%s' to: ", old);
	} else {
		snprintf(prompt, sizeof(prompt), "Rename symbol to: ");
	}
	out[0] = '\0';
	if (editor_read_line(fd, prompt, out, size) != MINIBUF_ACCEPTED) {
		return false;
	}
	if (out[0]) {
		return true;
	}
	if (have <= 0) {
		editor_set_status_message(RENAME_WHO ": no new name");
		return false;
	}
	memcpy(out, old, (size_t)have + 1);
	return true;
}

/* ------------------------------ the answer ---------------------------- */

/* What was skipped, as a clause to hang off the report, or "" when
 * nothing was.  A resource operation is a file kg was asked to create,
 * rename or delete; it does not perform any of them, and a report that
 * did not say so would be a report of a rename that only half happened.
 */
static const char *lsp_rename_skipped(
    const struct lsp_workspace_edit *edit, char *out, size_t size)
{
	out[0] = '\0';
	if (edit->resource_ops > 0) {
		snprintf(out, size, "; skipped %zu file %s (%s)",
		    edit->resource_ops,
		    edit->resource_ops == 1 ? "operation" : "operations",
		    edit->resource_kind[0] ? edit->resource_kind : "unknown");
	}
	return out;
}

/* Whether the answer is one to apply at all.  A partial answer is not:
 * an edit kg could not read is an occurrence that would be left behind,
 * and a rename that renamed most of the uses of a symbol is worse than
 * one that renamed none of them and said so. */
static bool lsp_rename_usable(const struct lsp_workspace_edit *edit)
{
	if (edit->dropped > 0) {
		editor_set_status_message(RENAME_WHO
		    ": the answer names %zu edit%s kg cannot apply; nothing "
		    "was renamed",
		    edit->dropped, edit->dropped == 1 ? "" : "s");
		return false;
	}
	if (edit->item_count == 0) {
		editor_set_status_message(RENAME_WHO ": nothing to rename");
		return false;
	}
	return true;
}

/* What happened, and never more than what happened: a rename that failed
 * partway through is reported as one, naming the file it stopped in.  The
 * refusals never reach here -- those printed their own reason, with their
 * own file, before anything was written. */
static void lsp_rename_report(
    const struct lsp_workspace_edit *edit, const struct lsp_edit_report *report)
{
	char skipped[128];

	if (report->incomplete) {
		editor_set_status_message(RENAME_WHO
		    ": renamed %zu occurrence%s in %zu file%s, then "
		    "FAILED in %s",
		    report->edits, report->edits == 1 ? "" : "s", report->files,
		    report->files == 1 ? "" : "s", report->failed);
		return;
	}
	editor_set_status_message("Renamed %zu occurrence%s in %zu file%s%s",
	    report->edits, report->edits == 1 ? "" : "s", report->files,
	    report->files == 1 ? "" : "s",
	    lsp_rename_skipped(edit, skipped, sizeof(skipped)));
}

static void lsp_rename_answer(const struct lsp_req_answer *answer)
{
	/* Which question this is the answer to, so that a buffer that moved
	 * while the server was thinking is refused rather than rewritten
	 * against text the server never saw (src/lsp_edit.h). */
	const struct lsp_edit_origin origin = {
		.client = answer->client,
		.buffer = answer->point.buffer,
		.generation = answer->sent_generation,
		.version
		= lsp_sync_version(answer->client, answer->point.buffer),
		.valid = answer->sent_generation_valid,
	};
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	/* Not while a minibuffer prompt is up.  This runs from lsp_poll(),
	 * which the key reader re-enters on every idle timeout, so the
	 * answer can land while the user is halfway through a filename --
	 * and applying it selects buffers and opens files, which would
	 * leave that prompt editing a buffer nobody opened it over. */
	if (kg_event_prompt_active()) {
		editor_set_status_message(
		    RENAME_WHO ": a prompt is open; nothing was renamed");
		return;
	}
	/* `null` is how a server says the position cannot be renamed at
	 * all, which is an answer rather than a failure. */
	if (lsp_json_kind_of(answer->result) == LSP_JSON_NULL) {
		editor_set_status_message(
		    RENAME_WHO ": the server will not rename this");
		return;
	}
	edit = lsp_workspace_edit_read(answer->result);
	if (!edit) {
		editor_set_status_message(
		    RENAME_WHO ": the server sent no workspace edit");
		return;
	}
	if (lsp_rename_usable(edit)) {
		(void)lsp_workspace_edit_apply(
		    RENAME_WHO, edit, answer->encoding, &origin, &report);
		if (report.refused) {
			/* A refusal has already named its own file and its
			 * own reason, and applied nothing at all. */
		} else if (report.edits > 0 || report.incomplete) {
			lsp_rename_report(edit, &report);
		} else {
			editor_set_status_message(
			    RENAME_WHO ": nothing was renamed");
		}
	}
	lsp_workspace_edit_free(edit);
}

/* ------------------------------ the command --------------------------- */

void editor_lsp_rename(int fd)
{
	struct lsp_req_extra extra = { "newName", NULL };
	char name[LSP_RENAME_NAME_MAX];

	if (!lsp_rename_read_name(fd, name, (int)sizeof(name))) {
		return;
	}
	extra.value = name;
	if (lsp_req_send_position(RENAME_WHO, "textDocument/rename", &extra,
		lsp_rename_answer, NULL, NULL)) {
		editor_set_status_message("Renaming...");
	}
}

#else /* !KG_USE_LSP */

#include "def.h"

void editor_lsp_rename(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

#endif /* KG_USE_LSP */
