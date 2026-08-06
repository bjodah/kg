/* ============================== Path helpers ===============================
 *
 * Self-contained filename utilities used by the minibuffer's Tab completion.
 * Kept here, separate from bufmgr.c, so unit tests can exercise them without
 * pulling in the rest of the editor's globals. */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"

/* Score a picker entry against the typed needle: 0 for prefix match,
 * 1 for substring match elsewhere in the name, -1 for no match.  Empty
 * needle ranks every candidate as a prefix match.  Callers use the
 * rank both to filter and to sort prefix matches above mid-name ones.
 * Lives here so path.c (already linked by test_complete) owns it,
 * rather than pulling cmd.o or bufmgr.o into the test target. */
int editor_picker_match_rank(const char *haystack, const char *needle)
{
	size_t nlen;

	if (!needle || !*needle) {
		return 0;
	}
	nlen = strlen(needle);
	if (strncmp(haystack, needle, nlen) == 0) {
		return 0;
	}
	if (strstr(haystack, needle)) {
		return 1;
	}
	return -1;
}

/* The two-pass rank filter every ido-style picker in kg runs: prefix
 * matches first, mid-name matches after, each pass keeping the
 * enumerator's own order so a table that is already sorted stays sorted
 * within its rank.  The second pass is skipped for an empty query, where
 * every candidate already ranked as a prefix match.
 *
 * `names` receives at most `max` matches, but the return value counts
 * them all, so a picker can say "(+N more)".  `order`, when not NULL,
 * receives each stored name's enumerator index -- what the caller needs
 * to map a selection back to the thing it named.  `*prefix_matches`
 * receives how many matches ranked as prefix matches: Tab completion
 * extends the longest common prefix over that group only.
 *
 * Lives here beside editor_picker_match_rank() for the same reason: this
 * is the whole of the ranking policy, and path.c pulls in none of the
 * editor's globals to state it. */
int editor_picker_filter(picker_name_fn name_at, void *data, const char *query,
    const char **names, int *order, int max, int *prefix_matches)
{
	const char *entry;
	int rank, i, total = 0;

	for (rank = 0; rank <= 1; rank++) {
		for (i = 0; (entry = name_at(i, data)); i++) {
			if (editor_picker_match_rank(entry, query) != rank) {
				continue;
			}
			if (total < max) {
				names[total] = entry;
				if (order) {
					order[total] = i;
				}
			}
			total++;
		}
		if (rank == 0) {
			*prefix_matches = total;
			if (!query[0]) {
				break;
			}
		}
	}
	return total;
}

void editor_picker_cycle(int *selection, int matches, int direction)
{
	if (matches > 0) {
		*selection = (*selection + direction + matches) % matches;
	}
}

/* qsort comparator state: the typed needle that ranked these entries.
 * Set just before qsort, cleared after, so the comparator can keep
 * prefix matches above mid-name matches without an extra rank field
 * in the struct exposed by def.h. */
static const char *path_cmp_needle;

static int path_entry_cmp(const void *a, const void *b)
{
	const struct path_entry *pa = a, *pb = b;

	if (path_cmp_needle && *path_cmp_needle) {
		int ra = editor_picker_match_rank(pa->name, path_cmp_needle);
		int rb = editor_picker_match_rank(pb->name, path_cmp_needle);
		if (ra != rb) {
			return ra - rb;
		}
	}
	return strcmp(pa->name, pb->name);
}

/* Expand a leading "~" or "~/" in `buf` to $HOME, in place.  No-op for
 * "~user/" forms or when $HOME isn't set or the result wouldn't fit. */
int editor_path_expand_tilde(char *buf, int bufsize)
{
	const char *home;
	int home_len, rest_len;

	if (buf[0] != '~') {
		return 0;
	}
	if (buf[1] != '/' && buf[1] != '\0') {
		return 0;
	}
	home = getenv("HOME");
	if (!home || !home[0]) {
		return 0;
	}
	home_len = (int)strlen(home);
	rest_len = (int)strlen(buf + 1);
	if (home_len + rest_len + 1 > bufsize) {
		return 1;
	}
	memmove(buf + home_len, buf + 1, rest_len + 1);
	memcpy(buf, home, home_len);
	return 0;
}

/* Split `path` into directory part (up to and including the last '/') and
 * file part (the rest).  If no '/' is present the directory is "./" and the
 * file is the whole path.  The directory is tilde-expanded so opendir/stat
 * can use it directly.  Both outputs are NUL-terminated. */
void editor_path_split(
    const char *path, char *dir, int dsize, char *file, int fsize)
{
	const char *slash = strrchr(path, '/');
	int dlen;

	if (!slash) {
		snprintf(dir, dsize, "./");
		snprintf(file, fsize, "%s", buf_basename(path));
		return;
	}
	dlen = (int)(slash - path) + 1;
	if (dlen >= dsize) {
		dlen = dsize - 1;
	}
	memcpy(dir, path, dlen);
	dir[dlen] = '\0';
	editor_path_expand_tilde(dir, dsize);
	snprintf(file, fsize, "%s", buf_basename(path));
}

/* Scan `dir` for entries that match `prefix` as either a leading or a
 * mid-name substring.  Returns the total number of matches (or -1 on
 * opendir failure); writes the first `max` of them into `entries[]`,
 * sorted with prefix matches above mid-name matches and alphabetical
 * within each group.  Computes the longest common prefix into `lcp`
 * when non-NULL, considering only the prefix-matched group (mid-name
 * matches share no leading text worth Tab-extending to).  Dotfiles
 * are hidden unless `prefix` itself starts with '.'. */
int editor_path_complete_entries(const char *dir, const char *prefix,
    struct path_entry *entries, int max, char *lcp, int lcp_size)
{
	struct dirent *de;
	DIR *dp;
	int matches = 0;
	int prefix_matches = 0;
	int filled = 0;
	int lcp_len = 0;
	int i;

	if (lcp) {
		lcp[0] = '\0';
	}

	dp = opendir(dir[0] ? dir : ".");
	if (!dp) {
		return -1;
	}

	while ((de = readdir(dp)) != NULL) {
		const char *name = de->d_name;
		int rank;

		if (name[0] == '.' && (name[1] == '\0' || prefix[0] != '.')) {
			continue;
		}
		rank = editor_picker_match_rank(name, prefix);
		if (rank < 0) {
			continue;
		}

		matches++;

		if (rank == 0 && lcp) {
			prefix_matches++;
			if (prefix_matches == 1) {
				snprintf(lcp, lcp_size, "%s", name);
				lcp_len = (int)strlen(lcp);
			} else {
				for (i = 0;
				    i < lcp_len && name[i] && lcp[i] == name[i];
				    i++)
					;
				lcp_len = i;
				lcp[lcp_len] = '\0';
			}
		}

		if (entries && filled < max) {
			struct path_entry *e = &entries[filled++];
			size_t nlen = strlen(name);
			if (nlen >= sizeof(e->name)) {
				nlen = sizeof(e->name) - 1;
			}
			memcpy(e->name, name, nlen);
			e->name[nlen] = '\0';
			e->is_dir = (de->d_type == DT_DIR);
			/* DT_UNKNOWN: fall back to stat(). */
			if (de->d_type == DT_UNKNOWN) {
				char full[PATH_MAX];
				int n = snprintf(full, sizeof(full), "%s%s",
				    dir[0] ? dir : "./", name);
				if (n < (int)sizeof(full)
				    && path_is_dir(full)) {
					e->is_dir = 1;
				}
			}
		}
	}
	closedir(dp);

	if (entries && filled > 0) {
		path_cmp_needle = prefix;
		qsort(entries, filled, sizeof(*entries), path_entry_cmp);
		path_cmp_needle = NULL;
	}

	return matches;
}
