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
	{"allocate", SQL_ALLOCATE},
	{"at", SQL_AT},
	{"autocommit", SQL_AUTOCOMMIT},
	{"bool", SQL_BOOL},
	{"break", SQL_BREAK},
	{"call", SQL_CALL},
	{"connect", SQL_CONNECT},
	{"connection", SQL_CONNECTION},
	{"continue", SQL_CONTINUE},
	{"count", SQL_COUNT},
	{"data", SQL_DATA},
	{"datetime_interval_code", SQL_DATETIME_INTERVAL_CODE},
	{"datetime_interval_precision", SQL_DATETIME_INTERVAL_PRECISION},
	{"deallocate", SQL_DEALLOCATE},
	{"descriptor", SQL_DESCRIPTOR},
	{"disconnect", SQL_DISCONNECT},
	{"enum", SQL_ENUM},
	{"found", SQL_FOUND},
	{"free", SQL_FREE},
	{"get", SQL_GET},
	{"go", SQL_GO},
	{"goto", SQL_GOTO},
	{"identified", SQL_IDENTIFIED},
	{"indicator", SQL_INDICATOR},
	{"int", SQL_INT},
	{"key_member", SQL_KEY_MEMBER},
	{"length", SQL_LENGTH},
	{"long", SQL_LONG},
	{"name", SQL_NAME},
	{"nullable", SQL_NULLABLE},
	{"octet_length", SQL_OCTET_LENGTH},
	{"off", SQL_OFF},
	{"open", SQL_OPEN},
	{"prepare", SQL_PREPARE},
	{"reference", SQL_REFERENCE},
	{"release", SQL_RELEASE},
	{"returned_length", SQL_RETURNED_LENGTH},
	{"returned_octet_length", SQL_RETURNED_OCTET_LENGTH},
	{"scale", SQL_SCALE},
	{"section", SQL_SECTION},
	{"short", SQL_SHORT},
	{"signed", SQL_SIGNED},
	{"sql",SQL_SQL}, // strange thing, used for into sql descriptor MYDESC;
	{"sqlerror", SQL_SQLERROR},
	{"sqlprint", SQL_SQLPRINT},
	{"sqlwarning", SQL_SQLWARNING},
	{"stop", SQL_STOP},
	{"struct", SQL_STRUCT},
	{"unsigned", SQL_UNSIGNED},
	{"value", SQL_VALUE},
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
