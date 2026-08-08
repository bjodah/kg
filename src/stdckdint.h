#ifndef KG_STDCKDINT_H
#define KG_STDCKDINT_H

/* MSVC 14.44 has the C11 library but not <stdckdint.h>.  kg only needs the
 * three checked integer operations, so keep this fallback local and use the
 * standard header everywhere else. */
#ifdef _WIN32

#include <stdint.h>
#include <stddef.h>

static inline int kg_ckd_add_unsigned(
    uint64_t *out, uint64_t left, uint64_t right)
{
	*out = left + right;
	return *out < left;
}

static inline int kg_ckd_sub_unsigned(
    uint64_t *out, uint64_t left, uint64_t right)
{
	*out = left - right;
	return left < right;
}

static inline int kg_ckd_mul_unsigned(
    uint64_t *out, uint64_t left, uint64_t right)
{
	*out = left * right;
	return right != 0 && left > UINT64_MAX / right;
}

static inline int kg_ckd_add_signed(
    int64_t *out, int64_t left, int64_t right)
{
	if ((right > 0 && left > INT64_MAX - right)
	    || (right < 0 && left < INT64_MIN - right)) {
		*out = 0;
		return 1;
	}
	*out = left + right;
	return 0;
}

static inline int kg_ckd_sub_signed(
    int64_t *out, int64_t left, int64_t right)
{
	if ((right < 0 && left > INT64_MAX + right)
	    || (right > 0 && left < INT64_MIN + right)) {
		*out = 0;
		return 1;
	}
	*out = left - right;
	return 0;
}

static inline int kg_ckd_mul_signed(
    int64_t *out, int64_t left, int64_t right)
{
	int overflow = 0;

	if (left > 0) {
		if (right > 0)
			overflow = left > INT64_MAX / right;
		else if (right < 0)
			overflow = right < INT64_MIN / left;
	} else if (left < 0) {
		if (right > 0)
			overflow = left < INT64_MIN / right;
		else if (right < 0)
			overflow = left < INT64_MAX / right;
	}
	*out = overflow ? 0 : left * right;
	return overflow;
}

#define ckd_add(out, left, right) \
	_Generic(*(out), int64_t: kg_ckd_add_signed, \
		default: kg_ckd_add_unsigned)((void *)(out), (left), (right))
#define ckd_sub(out, left, right) \
	_Generic(*(out), int64_t: kg_ckd_sub_signed, \
		default: kg_ckd_sub_unsigned)((void *)(out), (left), (right))
#define ckd_mul(out, left, right) \
	_Generic(*(out), int64_t: kg_ckd_mul_signed, \
		default: kg_ckd_mul_unsigned)((void *)(out), (left), (right))

#else

/* #include_next is a GCC/Clang extension; -pedantic flags the directive
 * itself even though the standard header it reaches is what every other
 * translation unit gets.  system_header silences that for the rest of
 * this file without touching the _WIN32 branch above. */
#pragma GCC system_header
#include_next <stdckdint.h>

#endif

#endif
