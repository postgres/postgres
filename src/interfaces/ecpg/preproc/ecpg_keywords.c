/*-------------------------------------------------------------------------
 *
 * keywords.c--
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "type.h"
#include "y.tab.h"
#include "extern.h"

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static ScanKeyword ScanKeywords[] = {
	/* name					value			*/
	{"call", SQL_CALL},
	{"connect", SQL_CONNECT},
	{"continue", SQL_CONTINUE},
	{"found", SQL_FOUND},
	{"go", SQL_GO},
	{"goto", SQL_GOTO},
	{"immediate", SQL_IMMEDIATE},
	{"indicator", SQL_INDICATOR},
	{"open", SQL_OPEN},
	{"section", SQL_SECTION},
	{"sqlerror", SQL_SQLERROR},
	{"sqlprint", SQL_SQLPRINT},
	{"stop", SQL_STOP},
	{"whenever", SQL_WHENEVER},
};

ScanKeyword *
ScanECPGKeywordLookup(char *text)
{
	ScanKeyword *low = &ScanKeywords[0];
	ScanKeyword *high = endof(ScanKeywords) - 1;
	ScanKeyword *middle;
	int			difference;

	while (low <= high)
	{
		middle = low + (high - low) / 2;
		difference = strcmp(middle->name, text);
		if (difference == 0)
			return (middle);
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return (NULL);
}
