#ifndef KG_SPELL_H
#define KG_SPELL_H

#include <stddef.h>

/* The one editor-facing facade of the optional Enchant spell checker,
 * kg's answer to Emacs' jinx module: just-in-time highlighting of
 * misspelled words plus a correction command with dictionary
 * suggestions, backed by libenchant-2 when this build has it.
 *
 * Everything below exists in both build configurations -- src/spell_core.c
 * and src/spell.c are compiled whatever WITH_ENCHANT says, the way
 * src/lsp_core.c is -- so no caller carries a KG_USE_ENCHANT conditional.
 * A WITH_ENCHANT=0 editor simply calls entry points that report the
 * feature as not compiled in.  This header names no editor type that
 * needs a definition (only pointers to forward-declared structs), so it
 * stays free-standing (`make header-check`).
 */

struct editor_buffer;
struct erow;
struct kg_display_options;

/* Called once from init_editor(), and once from editor_cleanup() beside
 * the other subsystem shutdowns.  Neither opens a dictionary: those are
 * requested lazily by the first check, which is after the init file has
 * had its say about `spell-language'. */
void spell_init(void);
void spell_shutdown(void);

/* Whether this build was compiled with the Enchant backend at all --
 * the `kg -V' answer as a predicate.  False in every WITH_ENCHANT=0
 * build, whatever dictionaries the box holds. */
int spell_supported(void);

/* Whether a check can run right now: supported, and a dictionary is
 * loadable for the configured language.  False until the first
 * successful check, which is what loads the dictionary. */
int spell_available(void);

/* The language tag in force (the `spell-language' variable, or "en_US"
 * when nothing set one).  Never NULL. */
const char *spell_language(void);

enum spell_highlight_style {
	SPELL_HIGHLIGHT_UNDERLINE = 0,
	SPELL_HIGHLIGHT_COLOR = 1,
};

/* The misspelling highlight style in force: read from `spell-highlight-style',
 * defaulting to SPELL_HIGHLIGHT_UNDERLINE. */
void spell_sync_highlight_style(void);
[[nodiscard]] enum spell_highlight_style spell_effective_highlight_style(void);

/* Word verdicts, in jinx--mod-check's vocabulary: 1 means the
 * dictionaries accept the word, 0 means they do not, and -1 means no
 * check ran -- unsupported build, no dictionary, or a session word.
 * Session words answer 1 without reaching Enchant. */
int spell_check_word(const char *word, size_t len);

/* Up to SPELL_SUGGEST_MAX dictionary suggestions for `word', newest
 * call's answer in `*out' (NULL and 0 when there are none, or no check
 * can run).  The caller frees with spell_free_suggestions(). */
#define SPELL_SUGGEST_MAX 16
size_t spell_suggest(const char *word, size_t len, char ***out);
void spell_free_suggestions(char **list, size_t n);

/* Words accepted for this session only (jinx--session-words): checked
 * before the dictionaries, forgotten at shutdown.  0 on success, nonzero
 * when the word cannot be kept. */
int spell_session_accept(const char *word, size_t len);
int spell_session_has(const char *word, size_t len);

/* One misspelled word, as byte offsets into row->chars (chars space,
 * half-open). */
struct spell_span {
	int start;
	int end;
};

/* Whether `word' is worth asking a dictionary about: 0 for the shapes
 * no dictionary can judge -- empty, overlong, holding a digit (hex
 * codes, versions), or all-uppercase past the first letter (acronyms) --
 * which is jinx-exclude-regexps' default set spelled as one predicate.
 * 1 otherwise. */
int spell_word_candidate(const char *word, size_t len);

/* A dictionary verdict for the scanner below: 1 when the word is fine,
 * 0 when it is misspelled.  The editor passes spell_check_word; tests
 * pass a stub. */
typedef int (*spell_is_ok_fn)(const char *word, size_t len, void *ctx);

/* The misspelled words of one row, in chars-space spans.  Words are runs
 * of letters, digits, `_', ASCII apostrophes and non-ASCII bytes, with
 * edge apostrophes trimmed (jinx' syntax table, minus the camelCase
 * splitting, which is follow-up work).  When `check_all' is 0, only
 * words touching a comment or string face count -- the code-buffer rule;
 * prose buffers pass 1.  At most `max' spans are reported; the return
 * value is the number stored. */
int spell_scan_row(struct erow *row, const struct kg_display_options *display,
    int check_all, spell_is_ok_fn is_ok, void *ctx, struct spell_span *out,
    int max);

/* Every displayed buffer's misspellings, brought up to date before the
 * frame that shows them -- show_paren_update()'s and
 * update_git_diagnostics()'s seam, for the same reason: decorations are
 * buffer state, published outside the render bracket.  Incremental and
 * bounded per frame, so a large buffer with spell-mode on costs a fixed
 * slice of each repaint rather than one long pause. */
void spell_update(void);

/* The per-buffer mode flag editor_buffer carries, and the four commands
 * cmdtable rows reach.  The next/previous/correct commands enable the
 * mode when it is off (jinx--correct-guard's rule) and say so only when
 * no check can run. */
int spell_mode_on(const struct editor_buffer *b);
void spell_cmd_mode(int fd);
void spell_cmd_next(int fd);
void spell_cmd_previous(int fd);
void spell_cmd_correct(int fd);

#endif /* KG_SPELL_H */
