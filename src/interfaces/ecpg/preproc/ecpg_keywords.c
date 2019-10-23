/*-------------------------------------------------------------------------
 *
 * ecpg_keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/ecpg_keywords.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>

/* ScanKeywordList lookup data for ECPG keywords */
#include "ecpg_kwlist_d.h"
#include "preproc_extern.h"
#include "preproc.h"

/* Token codes for ECPG keywords */
#define PG_KEYWORD(kwname, value) value,

static const uint16 ECPGScanKeywordTokens[] = {
#include "ecpg_kwlist.h"
};

#undef PG_KEYWORD


/*
 * ScanECPGKeywordLookup - see if a given word is a keyword
 *
 * Returns the token value of the keyword, or -1 if no match.
 *
 * Keywords are matched using the same case-folding rules as in the backend.
 */
int
ScanECPGKeywordLookup(const char *text)
{
	int			kwnum;

	/* First check SQL symbols defined by the backend. */
	kwnum = ScanKeywordLookup(text, &ScanKeywords);
	if (kwnum >= 0)
		return SQLScanKeywordTokens[kwnum];

	/* Try ECPG-specific keywords. */
	kwnum = ScanKeywordLookup(text, &ScanECPGKeywords);
	if (kwnum >= 0)
		return ECPGScanKeywordTokens[kwnum];

	return -1;
}
