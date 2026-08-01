#ifndef KG_KEYEVENT_H
#define KG_KEYEVENT_H

#include <stddef.h>
#include <stdint.h>

/* A key, as one base and a set of modifiers.
 *
 * The terminal decoder still reports the integers in enum KEY_ACTION,
 * where "M-f" is the enumerator ALT_F and "C-f" is the byte 6: a key's
 * identity is spread over a number line with no structure, so nothing
 * can ask "is this Meta?" or print a key back the way the user typed it.
 * A key_event has both halves separately, which is what keymaps are
 * keyed by and what a binding string parses into.
 *
 * `base` is a Unicode scalar value, or one of the named terminal keys
 * below.  The transitional adapter maps a raw byte >= 0x80 -- the lead of
 * a multi-byte character the terminal sends one byte at a time -- to the
 * scalar of the same number.  That is lossless (nothing binds those keys;
 * they reach the self-insert fallback and are reassembled there), and it
 * goes away with the decoder flag day. */

enum key_mods {
	KEY_MOD_CTRL = 1 << 0,
	KEY_MOD_META = 1 << 1,
	KEY_MOD_SHIFT = 1 << 2,
	KEY_MOD_ALL = 7,
};

/* Named keys, past the last Unicode scalar so a base is unambiguous. */
enum key_base {
	KEY_BASE_LAST_SCALAR = 0x10FFFF,
	KEY_BASE_RET = 0x110000,
	KEY_BASE_TAB,
	KEY_BASE_ESC,
	KEY_BASE_DEL, /* Backspace, which the terminal sends as DEL */
	KEY_BASE_LEFT,
	KEY_BASE_RIGHT,
	KEY_BASE_UP,
	KEY_BASE_DOWN,
	KEY_BASE_HOME,
	KEY_BASE_END,
	KEY_BASE_PRIOR, /* PageUp */
	KEY_BASE_NEXT, /* PageDown */
	KEY_BASE_INSERT,
	KEY_BASE_DELETE, /* the forward-delete key */
	KEY_BASE_F3,
	KEY_BASE_F4,
};

struct key_event {
	int32_t base;
	uint8_t mods;
};

/* Longest canonical spelling of one key, with room for the terminator. */
#define KEY_FORMAT_MAX 16

[[nodiscard]] int key_event_equal(struct key_event a, struct key_event b);

/* `event` is exactly `base_` with `mods_` set -- the spot check every
 * dispatch site that used to compare a legacy int now writes instead of
 * building a temporary and calling key_event_equal() by hand. */
#define KEY_IS(event, base_, mods_)                                           \
	key_event_equal((event), (struct key_event) { (base_), (mods_) })

/* The event a decoder integer stands for.  Every enum KEY_ACTION value
 * has exactly one, and no two have the same. */
[[nodiscard]] struct key_event key_event_from_legacy(int key);

/* Parse one key.  Modifiers are "C-", "M-" and "S-" in any order but at
 * most once each; the base is a named key ("RET", "TAB", "SPC", "ESC",
 * "DEL", "<left>", "<f3>", ...) or a single UTF-8 character.  Returns 0
 * on success.  Trailing input, an unknown name, a repeated modifier and
 * "S-" on anything but a named key are all rejected. */
[[nodiscard]] int key_parse(const char *text, struct key_event *out);

/* The canonical spelling of `key`: modifiers in C-, M-, S- order, then
 * the base's name or its character.  Returns 0 on success, non-zero when
 * `size` is too small (KEY_FORMAT_MAX is always enough). */
[[nodiscard]] int key_format(struct key_event key, char *out, size_t size);

/* Whether `key` is one of the `count` decoder integers in `keys`.  The
 * key sets the editor still asks about as integers live in tables and
 * are answered here, rather than being spelled out as a run of
 * comparisons at each site. */
[[nodiscard]] int key_in_list(const int *keys, size_t count, int key);
#define KEY_IN_LIST(list, key)                                                 \
	key_in_list((list), sizeof(list) / sizeof(*(list)), (key))

#endif /* KG_KEYEVENT_H */
