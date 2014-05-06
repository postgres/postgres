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

#include "extern.h"
#include "preproc.h"

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static const ScanKeyword ScanCKeywords[] = {
	/* name, value, category */

	/*
	 * category is not needed in ecpg, it is only here so we can share the
	 * data structure with the backend
	 */
	{"VARCHAR", VARCHAR, 0},
	{"auto", S_AUTO, 0},
	{"bool", SQL_BOOL, 0},
	{"char", CHAR_P, 0},
	{"const", S_CONST, 0},
	{"enum", ENUM_P, 0},
	{"extern", S_EXTERN, 0},
	{"float", FLOAT_P, 0},
	{"hour", HOUR_P, 0},
	{"int", INT_P, 0},
	{"long", SQL_LONG, 0},
	{"minute", MINUTE_P, 0},
	{"month", MONTH_P, 0},
	{"register", S_REGISTER, 0},
	{"second", SECOND_P, 0},
	{"short", SQL_SHORT, 0},
	{"signed", SQL_SIGNED, 0},
	{"static", S_STATIC, 0},
	{"struct", SQL_STRUCT, 0},
	{"to", TO, 0},
	{"typedef", S_TYPEDEF, 0},
	{"union", UNION, 0},
	{"unsigned", SQL_UNSIGNED, 0},
	{"varchar", VARCHAR, 0},
	{"volatile", S_VOLATILE, 0},
	{"year", YEAR_P, 0},
};


/*
 * Do a binary search using plain strcmp() comparison.  This is much like
 * ScanKeywordLookup(), except we want case-sensitive matching.
 */
const ScanKeyword *
ScanCKeywordLookup(const char *text)
{
	const ScanKeyword *low = &ScanCKeywords[0];
	const ScanKeyword *high = &ScanCKeywords[lengthof(ScanCKeywords) - 1];

	while (low <= high)
	{
		const ScanKeyword *middle;
		int			difference;

		middle = low + (high - low) / 2;
		difference = strcmp(middle->name, text);
		if (difference == 0)
			return middle;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return NULL;
}
