/*-------------------------------------------------------------------------
 *
 * c_keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 * src/interfaces/ecpg/preproc/c_keywords.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "preproc_extern.h"
#include "preproc.h"

/* ScanKeywordList lookup data for C keywords */
#include "c_kwlist_d.h"

/* Token codes for C keywords */
#define PG_KEYWORD(kwname, value) value,

static const uint16 ScanCKeywordTokens[] = {
#include "c_kwlist.h"
};

#undef PG_KEYWORD


/*
 * ScanCKeywordLookup - see if a given word is a keyword
 *
 * Returns the token value of the keyword, or -1 if no match.
 *
 * Do a binary search using plain strcmp() comparison.  This is much like
 * ScanKeywordLookup(), except we want case-sensitive matching.
 */
int
ScanCKeywordLookup(const char *text)
{
	const char *kw_string;
	const uint16 *kw_offsets;
	const uint16 *low;
	const uint16 *high;

	if (strlen(text) > ScanCKeywords.max_kw_len)
		return -1;				/* too long to be any keyword */

	kw_string = ScanCKeywords.kw_string;
	kw_offsets = ScanCKeywords.kw_offsets;
	low = kw_offsets;
	high = kw_offsets + (ScanCKeywords.num_keywords - 1);

	while (low <= high)
	{
		const uint16 *middle;
		int			difference;

		middle = low + (high - low) / 2;
		difference = strcmp(kw_string + *middle, text);
		if (difference == 0)
			return ScanCKeywordTokens[middle - kw_offsets];
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return -1;
}
