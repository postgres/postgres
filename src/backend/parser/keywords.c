/*-------------------------------------------------------------------------
 *
 * keywords.c--
 *	  lexical token lookup for reserved words in postgres SQL
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/keywords.c,v 1.50 1998/12/18 09:10:34 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parse.h"
#include "parser/keywords.h"
#include "utils/elog.h"

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static ScanKeyword ScanKeywords[] = {
	/* name, value */
	{"abort", ABORT_TRANS},
	{"absolute", ABSOLUTE},
	{"action", ACTION},
	{"add", ADD},
	{"after", AFTER},
	{"aggregate", AGGREGATE},
	{"all", ALL},
	{"alter", ALTER},
	{"analyze", ANALYZE},
	{"and", AND},
	{"any", ANY},
	{"as", AS},
	{"asc", ASC},
	{"backward", BACKWARD},
	{"before", BEFORE},
	{"begin", BEGIN_TRANS},
	{"between", BETWEEN},
	{"binary", BINARY},
	{"both", BOTH},
	{"by", BY},
	{"cache", CACHE},
	{"cascade", CASCADE},
	{"case", CASE},
	{"cast", CAST},
	{"char", CHAR},
	{"character", CHARACTER},
	{"check", CHECK},
	{"close", CLOSE},
	{"cluster", CLUSTER},
	{"coalesce", COALESCE},
	{"collate", COLLATE},
	{"column", COLUMN},
	{"commit", COMMIT},
	{"constraint", CONSTRAINT},
	{"copy", COPY},
	{"create", CREATE},
	{"createdb", CREATEDB},
	{"createuser", CREATEUSER},
	{"cross", CROSS},
	{"current", CURRENT},		/* 6.4 to 6.5 is migration time! CURRENT
								 * will be removed in 6.5! Use OLD keyword
								 * in rules. Jan */
	{"current_date", CURRENT_DATE},
	{"current_time", CURRENT_TIME},
	{"current_timestamp", CURRENT_TIMESTAMP},
	{"current_user", CURRENT_USER},
	{"cursor", CURSOR},
	{"cycle", CYCLE},
	{"database", DATABASE},
	{"day", DAY_P},
	{"decimal", DECIMAL},
	{"declare", DECLARE},
	{"default", DEFAULT},
	{"delete", DELETE},
	{"delimiters", DELIMITERS},
	{"desc", DESC},
	{"distinct", DISTINCT},
	{"do", DO},
	{"double", DOUBLE},
	{"drop", DROP},
	{"each", EACH},
	{"else", ELSE},
	{"encoding", ENCODING},
	{"end", END_TRANS},
	{"execute", EXECUTE},
	{"exists", EXISTS},
	{"explain", EXPLAIN},
	{"extend", EXTEND},
	{"extract", EXTRACT},
	{"false", FALSE_P},
	{"fetch", FETCH},
	{"float", FLOAT},
	{"for", FOR},
	{"foreign", FOREIGN},
	{"forward", FORWARD},
	{"from", FROM},
	{"full", FULL},
	{"function", FUNCTION},
	{"grant", GRANT},
	{"group", GROUP},
	{"handler", HANDLER},
	{"having", HAVING},
	{"hour", HOUR_P},
	{"in", IN},
	{"increment", INCREMENT},
	{"index", INDEX},
	{"inherits", INHERITS},
	{"inner", INNER_P},
	{"insensitive", INSENSITIVE},
	{"insert", INSERT},
	{"instead", INSTEAD},
	{"interval", INTERVAL},
	{"into", INTO},
	{"is", IS},
	{"isnull", ISNULL},
	{"isolation", ISOLATION},
	{"join", JOIN},
	{"key", KEY},
	{"lancompiler", LANCOMPILER},
	{"language", LANGUAGE},
	{"leading", LEADING},
	{"left", LEFT},
	{"level", LEVEL},
	{"like", LIKE},
	{"listen", LISTEN},
	{"load", LOAD},
	{"local", LOCAL},
	{"location", LOCATION},
	{"lock", LOCK_P},
	{"match", MATCH},
	{"maxvalue", MAXVALUE},
	{"minute", MINUTE_P},
	{"minvalue", MINVALUE},
	{"month", MONTH_P},
	{"move", MOVE},
	{"names", NAMES},
	{"national", NATIONAL},
	{"natural", NATURAL},
	{"nchar", NCHAR},
	{"new", NEW},
	{"next", NEXT},
	{"no", NO},
	{"nocreatedb", NOCREATEDB},
	{"nocreateuser", NOCREATEUSER},
	{"none", NONE},
	{"not", NOT},
	{"nothing", NOTHING},
	{"notify", NOTIFY},
	{"notnull", NOTNULL},
	{"null", NULL_P},
	{"nullif", NULLIF},
	{"numeric", NUMERIC},
	{"of", OF},
	{"oids", OIDS},
	{"old", CURRENT},
	{"on", ON},
	{"only", ONLY},
	{"operator", OPERATOR},
	{"option", OPTION},
	{"or", OR},
	{"order", ORDER},
	{"outer", OUTER_P},
	{"partial", PARTIAL},
	{"password", PASSWORD},
	{"position", POSITION},
	{"precision", PRECISION},
	{"primary", PRIMARY},
	{"prior", PRIOR},
	{"privileges", PRIVILEGES},
	{"procedural", PROCEDURAL},
	{"procedure", PROCEDURE},
	{"public", PUBLIC},
	{"read", READ},
	{"recipe", RECIPE},
	{"references", REFERENCES},
	{"relative", RELATIVE},
	{"rename", RENAME},
	{"reset", RESET},
	{"returns", RETURNS},
	{"revoke", REVOKE},
	{"right", RIGHT},
	{"rollback", ROLLBACK},
	{"row", ROW},
	{"rule", RULE},
	{"scroll", SCROLL},
	{"second", SECOND_P},
	{"select", SELECT},
	{"sequence", SEQUENCE},
	{"serial", SERIAL},
	{"set", SET},
	{"setof", SETOF},
	{"show", SHOW},
	{"start", START},
	{"statement", STATEMENT},
	{"stdin", STDIN},
	{"stdout", STDOUT},
	{"substring", SUBSTRING},
	{"table", TABLE},
	{"then", THEN},
	{"time", TIME},
	{"timestamp", TIMESTAMP},
	{"timezone_hour", TIMEZONE_HOUR},
	{"timezone_minute", TIMEZONE_MINUTE},
	{"to", TO},
	{"trailing", TRAILING},
	{"transaction", TRANSACTION},
	{"trigger", TRIGGER},
	{"trim", TRIM},
	{"true", TRUE_P},
	{"trusted", TRUSTED},
	{"type", TYPE_P},
	{"union", UNION},
	{"unique", UNIQUE},
	{"unlisten", UNLISTEN},
	{"until", UNTIL},
	{"update", UPDATE},
	{"user", USER},
	{"using", USING},
	{"vacuum", VACUUM},
	{"valid", VALID},
	{"values", VALUES},
	{"varchar", VARCHAR},
	{"varying", VARYING},
	{"verbose", VERBOSE},
	{"version", VERSION},
	{"view", VIEW},
	{"when", WHEN},
	{"where", WHERE},
	{"with", WITH},
	{"work", WORK},
	{"year", YEAR_P},
	{"zone", ZONE},
};

ScanKeyword *
ScanKeywordLookup(char *text)
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
