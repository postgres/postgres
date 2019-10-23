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

/* ScanKeywordList lookup data for C keywords */
#include "c_kwlist_d.h"
#include "preproc_extern.h"
#include "preproc.h"

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
 * Do a hash search using plain strcmp() comparison.  This is much like
 * ScanKeywordLookup(), except we want case-sensitive matching.
 */
int
ScanCKeywordLookup(const char *str)
{
	size_t		len;
	int			h;
	const char *kw;

	/*
	 * Reject immediately if too long to be any keyword.  This saves useless
	 * hashing work on long strings.
	 */
	len = strlen(str);
	if (len > ScanCKeywords.max_kw_len)
		return -1;

	/*
	 * Compute the hash function.  Since it's a perfect hash, we need only
	 * match to the specific keyword it identifies.
	 */
	h = ScanCKeywords_hash_func(str, len);

	/* An out-of-range result implies no match */
	if (h < 0 || h >= ScanCKeywords.num_keywords)
		return -1;

	kw = GetScanKeyword(h, &ScanCKeywords);

	if (strcmp(kw, str) == 0)
		return ScanCKeywordTokens[h];

	return -1;
}
