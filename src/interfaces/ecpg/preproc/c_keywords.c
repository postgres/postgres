/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
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
static ScanKeyword ScanKeywords[] = {
	/* name					value			*/
	{"VARCHAR", VARCHAR},
	{"auto", S_AUTO},
	{"bool", SQL_BOOL},
	{"char", CHAR_P},
	{"const", S_CONST},
	{"enum", SQL_ENUM},
	{"extern", S_EXTERN},
	{"float", FLOAT_P},
	{"hour", HOUR_P},
	{"int", INT_P},
	{"long", SQL_LONG},
	{"minute", MINUTE_P},
	{"month", MONTH_P},
	{"register", S_REGISTER},
	{"second", SECOND_P},
	{"short", SQL_SHORT},
	{"signed", SQL_SIGNED},
	{"static", S_STATIC},
	{"struct", SQL_STRUCT},
	{"to", TO},
	{"typedef", S_TYPEDEF},
	{"union", UNION},
	{"unsigned", SQL_UNSIGNED},
	{"varchar", VARCHAR},
	{"volatile", S_VOLATILE},
	{"year", YEAR_P},
};

ScanKeyword *
ScanCKeywordLookup(char *text)
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
			return middle;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return NULL;
}
