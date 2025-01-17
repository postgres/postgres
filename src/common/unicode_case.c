/*-------------------------------------------------------------------------
 * unicode_case.c
 *		Unicode case mapping and case conversion.
 *
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
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
#include "common/unicode_category.h"
#include "mb/pg_wchar.h"

static const pg_case_map *find_case_map(pg_wchar ucs);
static size_t convert_case(char *dst, size_t dstsize, const char *src, ssize_t srclen,
						   CaseKind str_casekind, bool full, WordBoundaryNext wbnext,
						   void *wbstate);
static bool check_special_conditions(int conditions, const char *str,
									 size_t len, size_t offset);

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
 *
 * If full is true, use special case mappings if available and if the
 * conditions are satisfied.
 */
size_t
unicode_strlower(char *dst, size_t dstsize, const char *src, ssize_t srclen,
				 bool full)
{
	return convert_case(dst, dstsize, src, srclen, CaseLower, full, NULL,
						NULL);
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
 * If full is true, use special case mappings if available and if the
 * conditions are satisfied. Otherwise, use only simple mappings and use
 * uppercase instead of titlecase.
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
				 bool full, WordBoundaryNext wbnext, void *wbstate)
{
	return convert_case(dst, dstsize, src, srclen, CaseTitle, full, wbnext,
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
 *
 * If full is true, use special case mappings if available and if the
 * conditions are satisfied.
 */
size_t
unicode_strupper(char *dst, size_t dstsize, const char *src, ssize_t srclen,
				 bool full)
{
	return convert_case(dst, dstsize, src, srclen, CaseUpper, full, NULL,
						NULL);
}

/*
 * Implement Unicode Default Case Conversion algorithm.
 *
 * If str_casekind is CaseLower or CaseUpper, map each character in the string
 * for which a mapping is available.
 *
 * If str_casekind is CaseTitle, maps characters found on a word boundary to
 * titlecase (or uppercase if full is false) and other characters to
 * lowercase. NB: does not currently implement the Unicode behavior in which
 * the word boundary is adjusted to the next Cased character. That behavior
 * could be implemented as an option, but it doesn't match the default
 * behavior of ICU, nor does it match the documented behavior of INITCAP().
 *
 * If full is true, use special mappings for relevant characters, which can
 * map a single codepoint to multiple codepoints, or depend on conditions.
 */
static size_t
convert_case(char *dst, size_t dstsize, const char *src, ssize_t srclen,
			 CaseKind str_casekind, bool full, WordBoundaryNext wbnext,
			 void *wbstate)
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
		const pg_special_case *special = NULL;

		if (str_casekind == CaseTitle)
		{
			if (srcoff == boundary)
			{
				chr_casekind = full ? CaseTitle : CaseUpper;
				boundary = wbnext(wbstate);
			}
			else
				chr_casekind = CaseLower;
		}

		/*
		 * Find special case that matches the conditions, if any.
		 *
		 * Note: only a single special mapping per codepoint is currently
		 * supported, though Unicode allows for multiple special mappings for
		 * a single codepoint.
		 */
		if (full && casemap && casemap->special_case)
		{
			int16		conditions = casemap->special_case->conditions;

			Assert(casemap->special_case->codepoint == u1);
			if (check_special_conditions(conditions, src, srclen, srcoff))
				special = casemap->special_case;
		}

		/* perform mapping, update result_len, and write to dst */
		if (special)
		{
			for (int i = 0; i < MAX_CASE_EXPANSION; i++)
			{
				pg_wchar	u2 = special->map[chr_casekind][i];
				size_t		u2len = unicode_utf8len(u2);

				if (u2 == '\0')
					break;

				if (result_len + u2len <= dstsize)
					unicode_to_utf8(u2, (unsigned char *) dst + result_len);

				result_len += u2len;
			}
		}
		else if (casemap)
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

/*
 * Check that the condition matches Final_Sigma, described in Unicode Table
 * 3-17. The character at the given offset must be directly preceded by a
 * Cased character, and must not be directly followed by a Cased character.
 *
 * Case_Ignorable characters are ignored. NB: some characters may be both
 * Cased and Case_Ignorable, in which case they are ignored.
 */
static bool
check_final_sigma(const unsigned char *str, size_t len, size_t offset)
{
	/* the start of the string is not preceded by a Cased character */
	if (offset == 0)
		return false;

	/* iterate backwards, looking for Cased character */
	for (int i = offset - 1; i >= 0; i--)
	{
		if ((str[i] & 0x80) == 0 || (str[i] & 0xC0) == 0xC0)
		{
			pg_wchar	curr = utf8_to_unicode(str + i);

			if (pg_u_prop_case_ignorable(curr))
				continue;
			else if (pg_u_prop_cased(curr))
				break;
			else
				return false;
		}
		else if ((str[i] & 0xC0) == 0x80)
			continue;

		Assert(false);			/* invalid UTF-8 */
	}

	/* end of string is not followed by a Cased character */
	if (offset == len)
		return true;

	/* iterate forwards, looking for Cased character */
	for (int i = offset + 1; i < len && str[i] != '\0'; i++)
	{
		if ((str[i] & 0x80) == 0 || (str[i] & 0xC0) == 0xC0)
		{
			pg_wchar	curr = utf8_to_unicode(str + i);

			if (pg_u_prop_case_ignorable(curr))
				continue;
			else if (pg_u_prop_cased(curr))
				return false;
			else
				break;
		}
		else if ((str[i] & 0xC0) == 0x80)
			continue;

		Assert(false);			/* invalid UTF-8 */
	}

	return true;
}

static bool
check_special_conditions(int conditions, const char *str, size_t len,
						 size_t offset)
{
	if (conditions == 0)
		return true;
	else if (conditions == PG_U_FINAL_SIGMA)
		return check_final_sigma((unsigned char *) str, len, offset);

	/* no other conditions supported */
	Assert(false);
	return false;
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
