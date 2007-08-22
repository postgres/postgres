/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 * $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/c_keywords.c,v 1.21 2007/08/22 08:20:58 meskes Exp $
 * §
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
	/* name					value			*/
	{"VARCHAR", VARCHAR},
	{"auto", S_AUTO},
	{"bool", SQL_BOOL},
	{"char", CHAR_P},
	{"const", S_CONST},
	{"enum", ENUM_P},
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

const ScanKeyword *
ScanCKeywordLookup(char *text)
{
	return DoLookup(text, &ScanCKeywords[0], endof(ScanCKeywords) - 1);
}
