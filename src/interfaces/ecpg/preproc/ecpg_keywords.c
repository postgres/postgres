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
	{"at", SQL_AT},
	{"autocommit", SQL_AUTOCOMMIT},
	{"bool", SQL_BOOL},
	{"break", SQL_BREAK},
	{"call", SQL_CALL},
	{"connect", SQL_CONNECT},
	{"connection", SQL_CONNECTION},
	{"continue", SQL_CONTINUE},
	{"deallocate", SQL_DEALLOCATE},
	{"disconnect", SQL_DISCONNECT},
	{"enum", SQL_ENUM},
	{"found", SQL_FOUND},
	{"free", SQL_FREE},
	{"go", SQL_GO},
	{"goto", SQL_GOTO},
	{"identified", SQL_IDENTIFIED},
	{"immediate", SQL_IMMEDIATE},
	{"indicator", SQL_INDICATOR},
	{"int", SQL_INT},
	{"long", SQL_LONG},
	{"off", SQL_OFF},
	{"open", SQL_OPEN},
	{"prepare", SQL_PREPARE},
	{"reference", SQL_REFERENCE},
	{"release", SQL_RELEASE},
	{"section", SQL_SECTION},
	{"short", SQL_SHORT},
	{"signed", SQL_SIGNED},
	{"sqlerror", SQL_SQLERROR},
	{"sqlprint", SQL_SQLPRINT},
	{"sqlwarning", SQL_SQLWARNING},
	{"stop", SQL_STOP},
	{"struct", SQL_STRUCT},
	{"unsigned", SQL_UNSIGNED},
	{"var", SQL_VAR},
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
			return middle;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return NULL;
}
