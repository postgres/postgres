/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"
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
	{"VARCHAR", S_VARCHAR},
	{"auto", S_AUTO},
	{"bool", S_BOOL},
	{"char", S_CHAR},
	{"const", S_CONST},
	{"double", S_DOUBLE},
	{"enum", S_ENUM},
	{"extern", S_EXTERN},
	{"float", S_FLOAT},
	{"int", S_INT},
	{"long", S_LONG},
	{"register", S_REGISTER},
	{"short", S_SHORT},
	{"signed", S_SIGNED},
	{"static", S_STATIC},
	{"struct", S_STRUCT},
	{"union", S_UNION},
	{"unsigned", S_UNSIGNED},
	{"varchar", S_VARCHAR},
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
