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

enum CaseMapResult
{
	CASEMAP_SELF,
	CASEMAP_SIMPLE,
	CASEMAP_SPECIAL,
};

/*
 * Map for each case kind.
 */
static const pg_wchar *const casekind_map[NCaseKind] =
{
	[CaseLower] = case_map_lower,
	[CaseTitle] = case_map_title,
	[CaseUpper] = case_map_upper,
	[CaseFold] = case_map_fold,
};

static pg_wchar find_case_map(pg_wchar ucs, const pg_wchar *map);
static size_t convert_case(char *dst, size_t dstsize, const char *src, ssize_t srclen,
						   CaseKind str_casekind, bool full, WordBoundaryNext wbnext,
						   void *wbstate);
static enum CaseMapResult casemap(pg_wchar u1, CaseKind casekind, bool full,
								  const char *src, size_t srclen, size_t srcoff,
								  pg_wchar *simple, const pg_wchar **special);

pg_wchar
unicode_lowercase_simple(pg_wchar code)
{
	pg_wchar	cp = find_case_map(code, case_map_lower);

	return cp != 0 ? cp : code;
}

pg_wchar
unicode_titlecase_simple(pg_wchar code)
{
	pg_wchar	cp = find_case_map(code, case_map_title);

	return cp != 0 ? cp : code;
}

pg_wchar
unicode_uppercase_simple(pg_wchar code)
{
	pg_wchar	cp = find_case_map(code, case_map_upper);

	return cp != 0 ? cp : code;
}

pg_wchar
unicode_casefold_simple(pg_wchar code)
{
	pg_wchar	cp = find_case_map(code, case_map_fold);

	return cp != 0 ? cp : code;
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
 * unicode_strfold()
 *
 * Case fold src, and return the result length (not including terminating
 * NUL).
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
unicode_strfold(char *dst, size_t dstsize, const char *src, ssize_t srclen,
				bool full)
{
	return convert_case(dst, dstsize, src, srclen, CaseFold, full, NULL,
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
		pg_wchar	simple = 0;
		const pg_wchar *special = NULL;
		enum CaseMapResult casemap_result;

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

		casemap_result = casemap(u1, chr_casekind, full, src, srclen, srcoff,
								 &simple, &special);

		switch (casemap_result)
		{
			case CASEMAP_SELF:
				/* no mapping; copy bytes from src */
				Assert(simple == 0);
				Assert(special == NULL);
				if (result_len + u1len <= dstsize)
					memcpy(dst + result_len, src + srcoff, u1len);

				result_len += u1len;
				break;
			case CASEMAP_SIMPLE:
				{
					/* replace with single character */
					pg_wchar	u2 = simple;
					pg_wchar	u2len = unicode_utf8len(u2);

					Assert(special == NULL);
					if (result_len + u2len <= dstsize)
						unicode_to_utf8(u2, (unsigned char *) dst + result_len);

					result_len += u2len;
				}
				break;
			case CASEMAP_SPECIAL:
				/* replace with up to MAX_CASE_EXPANSION characters */
				Assert(simple == 0);
				for (int i = 0; i < MAX_CASE_EXPANSION && special[i]; i++)
				{
					pg_wchar	u2 = special[i];
					size_t		u2len = unicode_utf8len(u2);

					if (result_len + u2len <= dstsize)
						unicode_to_utf8(u2, (unsigned char *) dst + result_len);

					result_len += u2len;
				}
				break;
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

/*
 * Unicode allows for special casing to be applied only under certain
 * circumstances. The only currently-supported condition is Final_Sigma.
 */
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

/*
 * Map the given character to the requested case.
 *
 * If full is true, and a special case mapping is found and the conditions are
 * met, 'special' is set to the mapping result (which is an array of up to
 * MAX_CASE_EXPANSION characters) and CASEMAP_SPECIAL is returned.
 *
 * Otherwise, search for a simple mapping, and if found, set 'simple' to the
 * result and return CASEMAP_SIMPLE.
 *
 * If no mapping is found, return CASEMAP_SELF, and the caller should copy the
 * character without modification.
 */
static enum CaseMapResult
casemap(pg_wchar u1, CaseKind casekind, bool full,
		const char *src, size_t srclen, size_t srcoff,
		pg_wchar *simple, const pg_wchar **special)
{
	uint16		idx;

	/* Fast path for codepoints < 0x80 */
	if (u1 < 0x80)
	{
		/*
		 * The first elements in all tables are reserved as 0 (as NULL). The
		 * data starts at index 1, not 0.
		 */
		*simple = casekind_map[casekind][u1 + 1];

		return CASEMAP_SIMPLE;
	}

	idx = case_index(u1);

	if (idx == 0)
		return CASEMAP_SELF;

	if (full && case_map_special[idx] &&
		check_special_conditions(special_case[case_map_special[idx]].conditions,
								 src, srclen, srcoff))
	{
		*special = special_case[case_map_special[idx]].map[casekind];
		return CASEMAP_SPECIAL;
	}

	*simple = casekind_map[casekind][idx];

	return CASEMAP_SIMPLE;
}

/*
 * Find entry in simple case map.
 * If the entry does not exist, 0 will be returned.
 */
static pg_wchar
find_case_map(pg_wchar ucs, const pg_wchar *map)
{
	/* Fast path for codepoints < 0x80 */
	if (ucs < 0x80)
		/* The first elements in all tables are reserved as 0 (as NULL). */
		return map[ucs + 1];
	return map[case_index(ucs)];
}
