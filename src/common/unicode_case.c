/*-------------------------------------------------------------------------
 * unicode_case.c
 *		Unicode case mapping and case conversion.
 *
 * Portions Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode_case.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/unicode_case.h"
#include "common/unicode_case_table.h"
#include "mb/pg_wchar.h"

static const pg_case_map *find_case_map(pg_wchar ucs);
static size_t convert_case(char *dst, size_t dstsize, const char *src, ssize_t srclen,
						   CaseKind str_casekind, WordBoundaryNext wbnext,
						   void *wbstate);

pg_wchar
unicode_lowercase_simple(pg_wchar code)
{
	const pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[CaseLower] : code;
}

pg_wchar
unicode_titlecase_simple(pg_wchar code)
{
	const pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[CaseTitle] : code;
}

pg_wchar
unicode_uppercase_simple(pg_wchar code)
{
	const pg_case_map *map = find_case_map(code);

	return map ? map->simplemap[CaseUpper] : code;
}

/*
 * unicode_strlower()
 *
 * Convert src to lowercase, and return the result length (not including
 * terminating NUL).
 *
 * String src must be encoded in UTF-8. If srclen < 0, src must be
 * NUL-terminated.
 *
 * Result string is stored in dst, truncating if larger than dstsize. If
 * dstsize is greater than the result length, dst will be NUL-terminated;
 * otherwise not.
 *
 * If dstsize is zero, dst may be NULL. This is useful for calculating the
 * required buffer size before allocating.
 */
size_t
unicode_strlower(char *dst, size_t dstsize, const char *src, ssize_t srclen)
{
	return convert_case(dst, dstsize, src, srclen, CaseLower, NULL, NULL);
}

/*
 * unicode_strtitle()
 *
 * Convert src to titlecase, and return the result length (not including
 * terminating NUL).
 *
 * String src must be encoded in UTF-8. If srclen < 0, src must be
 * NUL-terminated.
 *
 * Result string is stored in dst, truncating if larger than dstsize. If
 * dstsize is greater than the result length, dst will be NUL-terminated;
 * otherwise not.
 *
 * If dstsize is zero, dst may be NULL. This is useful for calculating the
 * required buffer size before allocating.
 *
 * Titlecasing requires knowledge about word boundaries, which is provided by
 * the callback wbnext. A word boundary is the offset of the start of a word
 * or the offset of the character immediately following a word.
 *
 * The caller is expected to initialize and free the callback state
 * wbstate. The callback should first return offset 0 for the first boundary;
 * then the offset of each subsequent word boundary; then the total length of
 * the string to indicate the final boundary.
 */
size_t
unicode_strtitle(char *dst, size_t dstsize, const char *src, ssize_t srclen,
				 WordBoundaryNext wbnext, void *wbstate)
{
	return convert_case(dst, dstsize, src, srclen, CaseTitle, wbnext,
						wbstate);
}

/*
 * unicode_strupper()
 *
 * Convert src to uppercase, and return the result length (not including
 * terminating NUL).
 *
 * String src must be encoded in UTF-8. If srclen < 0, src must be
 * NUL-terminated.
 *
 * Result string is stored in dst, truncating if larger than dstsize. If
 * dstsize is greater than the result length, dst will be NUL-terminated;
 * otherwise not.
 *
 * If dstsize is zero, dst may be NULL. This is useful for calculating the
 * required buffer size before allocating.
 */
size_t
unicode_strupper(char *dst, size_t dstsize, const char *src, ssize_t srclen)
{
	return convert_case(dst, dstsize, src, srclen, CaseUpper, NULL, NULL);
}

/*
 * If str_casekind is CaseLower or CaseUpper, map each character in the string
 * for which a mapping is available.
 *
 * If str_casekind is CaseTitle, maps characters found on a word boundary to
 * uppercase and other characters to lowercase.
 */
static size_t
convert_case(char *dst, size_t dstsize, const char *src, ssize_t srclen,
			 CaseKind str_casekind, WordBoundaryNext wbnext, void *wbstate)
{
	/* character CaseKind varies while titlecasing */
	CaseKind	chr_casekind = str_casekind;
	size_t		srcoff = 0;
	size_t		result_len = 0;
	size_t		boundary = 0;

	Assert((str_casekind == CaseTitle && wbnext && wbstate) ||
		   (str_casekind != CaseTitle && !wbnext && !wbstate));

	if (str_casekind == CaseTitle)
	{
		boundary = wbnext(wbstate);
		Assert(boundary == 0);	/* start of text is always a boundary */
	}

	while ((srclen < 0 || srcoff < srclen) && src[srcoff] != '\0')
	{
		pg_wchar	u1 = utf8_to_unicode((unsigned char *) src + srcoff);
		int			u1len = unicode_utf8len(u1);
		const pg_case_map *casemap = find_case_map(u1);

		if (str_casekind == CaseTitle)
		{
			if (srcoff == boundary)
			{
				chr_casekind = CaseUpper;
				boundary = wbnext(wbstate);
			}
			else
				chr_casekind = CaseLower;
		}

		/* perform mapping, update result_len, and write to dst */
		if (casemap)
		{
			pg_wchar	u2 = casemap->simplemap[chr_casekind];
			pg_wchar	u2len = unicode_utf8len(u2);

			if (result_len + u2len <= dstsize)
				unicode_to_utf8(u2, (unsigned char *) dst + result_len);

			result_len += u2len;
		}
		else
		{
			/* no mapping; copy bytes from src */
			if (result_len + u1len <= dstsize)
				memcpy(dst + result_len, src + srcoff, u1len);

			result_len += u1len;
		}

		srcoff += u1len;
	}

	if (result_len < dstsize)
		dst[result_len] = '\0';

	return result_len;
}

/* find entry in simple case map, if any */
static const pg_case_map *
find_case_map(pg_wchar ucs)
{
	int			min;
	int			mid;
	int			max;

	/* all chars <= 0x80 are stored in array for fast lookup */
	Assert(lengthof(case_map) >= 0x80);
	if (ucs < 0x80)
	{
		const pg_case_map *map = &case_map[ucs];

		Assert(map->codepoint == ucs);
		return map;
	}

	/* otherwise, binary search */
	min = 0x80;
	max = lengthof(case_map) - 1;
	while (max >= min)
	{
		mid = (min + max) / 2;
		if (ucs > case_map[mid].codepoint)
			min = mid + 1;
		else if (ucs < case_map[mid].codepoint)
			max = mid - 1;
		else
			return &case_map[mid];
	}

	return NULL;
}
