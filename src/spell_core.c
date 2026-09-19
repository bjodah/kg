/* Lifecycle, language and dictionary behind src/spell.h's facade, plus
 * the session word list.  Compiled in every configuration, like
 * src/lisp_core.c: the WITH_ENCHANT=0 build links the same entry points
 * answering "not available", so the editor calls them unconditionally.
 *
 * Dictionaries are opened lazily by the first check rather than by
 * spell_init(): the init file sets `spell-language' after the editor is
 * up, so opening one at init would read the default and never the user's
 * choice.  One dictionary at a time is cached; a language change drops
 * it and the next check opens the new one.
 */

#include "lisp.h"
#include "spell.h"

#ifdef KG_USE_ENCHANT
#include <enchant.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tag read when `spell-language' is unbound or unusable, and the most
 * `spell-language' may hold.  A BCP 47 tag is a dozen bytes; anything
 * longer is a sentence, not a tag, and is refused rather than passed to
 * the broker. */
#define SPELL_DEFAULT_LANGUAGE "en_US"
#define SPELL_LANGUAGE_MAX 32

/* Session words are an in-memory list only (jinx--session-words):
 * forgotten at shutdown, never written to a personal dictionary.  Saving
 * to personal, file-local and directory-local word lists is follow-up
 * work. */
#define SPELL_SESSION_MAX 256
#define SPELL_SESSION_WORD_MAX 64

static char spell_configured[SPELL_LANGUAGE_MAX] = { 0 };
static char spell_opened[SPELL_LANGUAGE_MAX] = { 0 };

#ifdef KG_USE_ENCHANT
static EnchantBroker *spell_broker = NULL;
static EnchantDict *spell_dict = NULL;
#endif

static char *spell_session[SPELL_SESSION_MAX];
static size_t spell_session_count;

void spell_init(void)
{
#ifdef KG_USE_ENCHANT
	if (!spell_broker) {
		spell_broker = enchant_broker_init();
	}
#endif
	spell_sync_highlight_style();
}

/* Forget the cached dictionary without dropping the broker: the next
 * check re-opens it, which is what makes a language change take effect
 * without a restart. */
static void spell_close_dict(void)
{
#ifdef KG_USE_ENCHANT
	if (spell_dict) {
		enchant_broker_free_dict(spell_broker, spell_dict);
		spell_dict = NULL;
	}
#endif
	spell_opened[0] = '\0';
}

void spell_shutdown(void)
{
	size_t i;

	spell_close_dict();
#ifdef KG_USE_ENCHANT
	if (spell_broker) {
		enchant_broker_free(spell_broker);
		spell_broker = NULL;
	}
#endif
	for (i = 0; i < spell_session_count; i++) {
		free(spell_session[i]);
		spell_session[i] = NULL;
	}
	spell_session_count = 0;
}

int spell_supported(void)
{
#ifdef KG_USE_ENCHANT
	return 1;
#else
	return 0;
#endif
}

/* The tag in force: `spell-language' when it names a string,
 * empty (the caller's cue for the default) otherwise.  Read on every
 * use rather than cached, so an init file or a later setq takes effect
 * on the next check. */
static void spell_effective_language(char *out, size_t size)
{
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	(void)kg_lisp_variable_string("spell-language", out, size);
}

static enum spell_highlight_style cached_highlight_style
    = SPELL_HIGHLIGHT_UNDERLINE;

void spell_sync_highlight_style(void)
{
	char style[16];

	style[0] = '\0';
	(void)kg_lisp_variable_string(
	    "spell-highlight-style", style, sizeof(style));
	if (strcmp(style, "color") == 0) {
		cached_highlight_style = SPELL_HIGHLIGHT_COLOR;
	} else {
		cached_highlight_style = SPELL_HIGHLIGHT_UNDERLINE;
	}
}

enum spell_highlight_style spell_effective_highlight_style(void)
{
	return cached_highlight_style;
}

/* The dictionary for the configured language, opening (and caching) it
 * on first use.  NULL when this build has no backend, the broker is
 * down, the tag changed out from under the cache, or no provider owns
 * the language. */
static void *spell_dict_for(const char *lang)
{
#ifdef KG_USE_ENCHANT
	if (!spell_broker || !lang || !lang[0]) {
		return NULL;
	}
	if (!spell_dict || strcmp(spell_opened, lang) != 0) {
		spell_close_dict();
		spell_dict = enchant_broker_request_dict(spell_broker, lang);
		if (!spell_dict) {
			return NULL;
		}
		snprintf(spell_opened, sizeof(spell_opened), "%s", lang);
	}
	return spell_dict;
#else
	(void)lang;
	return NULL;
#endif
}

const char *spell_language(void)
{
	spell_effective_language(spell_configured, sizeof(spell_configured));
	if (!spell_configured[0]) {
		return SPELL_DEFAULT_LANGUAGE;
	}
	return spell_configured;
}

int spell_available(void)
{
	char lang[SPELL_LANGUAGE_MAX];

	/* First: a build without the backend never touches the
	 * interpreter for this -- every frame calls here through
	 * spell_update(), and interning a name into an exhausted arena
	 * is a side effect a disabled feature must not have. */
	if (!spell_supported()) {
		return 0;
	}
	spell_effective_language(lang, sizeof(lang));
	if (!lang[0]) {
		snprintf(lang, sizeof(lang), "%s", SPELL_DEFAULT_LANGUAGE);
	}
	return spell_dict_for(lang) != NULL;
}

/* The tag the cached dictionary was opened for ("" when none is), for
 * spell_update()'s stamp: answering from the cache costs no
 * interpreter read, where spell_language() consults Lisp. */
const char *spell_open_language(void) { return spell_opened; }

int spell_session_has(const char *word, size_t len)
{
	size_t i;

	if (!word || len == 0 || len > SPELL_SESSION_WORD_MAX) {
		return 0;
	}
	for (i = 0; i < spell_session_count; i++) {
		if (strlen(spell_session[i]) == len
		    && memcmp(spell_session[i], word, len) == 0) {
			return 1;
		}
	}
	return 0;
}

int spell_session_accept(const char *word, size_t len)
{
	char *copy;

	if (!word || len == 0 || len > SPELL_SESSION_WORD_MAX) {
		return 1;
	}
	if (spell_session_has(word, len)) {
		return 0;
	}
	if (spell_session_count >= SPELL_SESSION_MAX) {
		return 1;
	}
	copy = malloc(len + 1);
	if (!copy) {
		return 1;
	}
	memcpy(copy, word, len);
	copy[len] = '\0';
	spell_session[spell_session_count++] = copy;
	return 0;
}

int spell_check_word(const char *word, size_t len)
{
#ifdef KG_USE_ENCHANT
	char tmp[SPELL_SESSION_WORD_MAX + 1];
	char lang[SPELL_LANGUAGE_MAX];
	void *dict;

	if (!word || len == 0 || len > SPELL_SESSION_WORD_MAX) {
		return 1;
	}
	if (spell_session_has(word, len)) {
		return 1;
	}
	spell_effective_language(lang, sizeof(lang));
	if (!lang[0]) {
		snprintf(lang, sizeof(lang), "%s", SPELL_DEFAULT_LANGUAGE);
	}
	dict = spell_dict_for(lang);
	if (!dict) {
		return -1;
	}
	memcpy(tmp, word, len);
	tmp[len] = '\0';
	return enchant_dict_check(dict, tmp, (ssize_t)len) == 0 ? 1 : 0;
#else
	(void)word;
	(void)len;
	return -1;
#endif
}

#ifdef KG_USE_ENCHANT
/* Copy at most SPELL_SUGGEST_MAX of Enchant's answers into `answer',
 * answering how many survived.  A NULL entry is skipped; a failed
 * strdup ends the list rather than leaving a hole. */
static size_t spell_keep_suggestions(char **raw, size_t count, char **answer)
{
	size_t kept = 0, i;

	for (i = 0; i < count && kept < SPELL_SUGGEST_MAX; i++) {
		if (!raw[i]) {
			continue;
		}
		answer[kept] = strdup(raw[i]);
		if (!answer[kept]) {
			break;
		}
		kept++;
	}
	return kept;
}
#endif

size_t spell_suggest(const char *word, size_t len, char ***out)
{
#ifdef KG_USE_ENCHANT
	char tmp[SPELL_SESSION_WORD_MAX + 1];
	char lang[SPELL_LANGUAGE_MAX];
	void *dict;
	char **raw = NULL;
	size_t count = 0, kept;
	char **answer = NULL;

	if (out) {
		*out = NULL;
	}
	if (!word || len == 0 || len > SPELL_SESSION_WORD_MAX || !out) {
		return 0;
	}
	spell_effective_language(lang, sizeof(lang));
	if (!lang[0]) {
		snprintf(lang, sizeof(lang), "%s", SPELL_DEFAULT_LANGUAGE);
	}
	dict = spell_dict_for(lang);
	if (!dict) {
		return 0;
	}
	memcpy(tmp, word, len);
	tmp[len] = '\0';
	raw = enchant_dict_suggest(dict, tmp, (ssize_t)len, &count);
	if (!raw) {
		return 0;
	}
	answer = calloc(count > SPELL_SUGGEST_MAX ? SPELL_SUGGEST_MAX : count,
	    sizeof(*answer));
	if (!answer) {
		enchant_dict_free_string_list(dict, raw);
		return 0;
	}
	kept = spell_keep_suggestions(raw, count, answer);
	enchant_dict_free_string_list(dict, raw);
	if (kept == 0) {
		free(answer);
		return 0;
	}
	*out = answer;
	return kept;
#else
	(void)word;
	(void)len;
	if (out) {
		*out = NULL;
	}
	return 0;
#endif
}

void spell_free_suggestions(char **list, size_t n)
{
	size_t i;

	if (!list) {
		return;
	}
	for (i = 0; i < n; i++) {
		free(list[i]);
	}
	free(list);
}
