/*-------------------------------------------------------------------------
 *
 * keywords.c--
 *	  lexical token lookup for reserved words in postgres SQL
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/keywords.c,v 1.17 1997/09/13 03:13:37 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "parse.h"
#include "utils/elog.h"
#include "parser/keywords.h"
#include "parser/dbcommands.h"	/* createdb, destroydb stop_vacuum */


/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static ScanKeyword ScanKeywords[] = {
	/* name					value			*/
	{"abort", ABORT_TRANS},
	{"acl", ACL},
	{"add", ADD},
	{"after", AFTER},
	{"aggregate", AGGREGATE},
	{"all", ALL},
	{"alter", ALTER},
	{"analyze", ANALYZE},
	{"and", AND},
	{"append", APPEND},
	{"archIve", ARCHIVE},		/* XXX crooked: I < _ */
	{"arch_store", ARCH_STORE},
	{"archive", ARCHIVE},		/* XXX crooked: i > _ */
	{"as", AS},
	{"asc", ASC},
	{"backward", BACKWARD},
	{"before", BEFORE},
	{"begin", BEGIN_TRANS},
	{"between", BETWEEN},
	{"binary", BINARY},
	{"both", BOTH},
	{"by", BY},
	{"cast", CAST},
	{"change", CHANGE},
	{"character", CHARACTER},
	{"check", CHECK},
	{"close", CLOSE},
	{"cluster", CLUSTER},
	{"column", COLUMN},
	{"commit", COMMIT},
	{"constraint", CONSTRAINT},
	{"copy", COPY},
	{"create", CREATE},
	{"cross", CROSS},
	{"current", CURRENT},
	{"cursor", CURSOR},
	{"database", DATABASE},
	{"day", DAYINTERVAL},
	{"declare", DECLARE},
	{"default", DEFAULT},
	{"delete", DELETE},
	{"delimiters", DELIMITERS},
	{"desc", DESC},
	{"distinct", DISTINCT},
	{"do", DO},
	{"double", DOUBLE},
	{"drop", DROP},
	{"end", END_TRANS},
	{"execute", EXECUTE},
	{"exists", EXISTS},
	{"explain", EXPLAIN},
	{"extend", EXTEND},
	{"extract", EXTRACT},
	{"fetch", FETCH},
	{"for", FOR},
	{"forward", FORWARD},
	{"from", FROM},
	{"full", FULL},
	{"function", FUNCTION},
	{"grant", GRANT},
	{"group", GROUP},
	{"having", HAVING},
	{"heavy", HEAVY},
	{"hour", HOURINTERVAL},
	{"in", IN},
	{"index", INDEX},
	{"inherits", INHERITS},
	{"inner", INNERJOIN},
	{"insert", INSERT},
	{"instead", INSTEAD},
	{"interval", INTERVAL},
	{"into", INTO},
	{"is", IS},
	{"isnull", ISNULL},
	{"join", JOIN},
	{"language", LANGUAGE},
	{"leading", LEADING},
	{"left", LEFT},
	{"light", LIGHT},
	{"like", LIKE},
	{"listen", LISTEN},
	{"load", LOAD},
	{"local", LOCAL},
	{"merge", MERGE},
	{"minute", MINUTEINTERVAL},
	{"month", MONTHINTERVAL},
	{"move", MOVE},
	{"natural", NATURAL},
	{"new", NEW},
	{"none", NONE},
	{"not", NOT},
	{"nothing", NOTHING},
	{"notify", NOTIFY},
	{"notnull", NOTNULL},
	{"null", PNULL},
	{"oids", OIDS},
	{"on", ON},
	{"operator", OPERATOR},
	{"option", OPTION},
	{"or", OR},
	{"order", ORDER},
	{"outer", OUTERJOIN},
	{"position", POSITION},
	{"precision", PRECISION},
	{"privileges", PRIVILEGES},
	{"procedure", PROCEDURE},
	{"public", PUBLIC},
	{"purge", PURGE},
	{"recipe", RECIPE},
	{"rename", RENAME},
	{"replace", REPLACE},
	{"reset", RESET},
	{"retrieve", RETRIEVE},
	{"returns", RETURNS},
	{"revoke", REVOKE},
	{"right", RIGHT},
	{"rollback", ROLLBACK},
	{"rule", RULE},
	{"second", SECONDINTERVAL},
	{"select", SELECT},
	{"sequence", SEQUENCE},
	{"set", SET},
	{"setof", SETOF},
	{"show", SHOW},
	{"stdin", STDIN},
	{"stdout", STDOUT},
	{"store", STORE},
	{"substring", SUBSTRING},
	{"table", TABLE},
	{"time", TIME},
	{"to", TO},
	{"trailing", TRAILING},
	{"transaction", TRANSACTION},
	{"trigger", TRIGGER},
	{"trim", TRIM},
	{"type", P_TYPE},
	{"union", UNION},
	{"unique", UNIQUE},
	{"update", UPDATE},
	{"using", USING},
	{"vacuum", VACUUM},
	{"values", VALUES},
	{"varying", VARYING},
	{"verbose", VERBOSE},
	{"version", VERSION},
	{"view", VIEW},
	{"where", WHERE},
	{"with", WITH},
	{"work", WORK},
	{"year", YEARINTERVAL},
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
			return (middle);
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return (NULL);
}

#ifdef NOT_USED
char	   *
AtomValueGetString(int atomval)
{
	ScanKeyword *low = &ScanKeywords[0];
	ScanKeyword *high = endof(ScanKeywords) - 1;
	int			keyword_list_length = (high - low);
	int			i;

	for (i = 0; i < keyword_list_length; i++)
		if (ScanKeywords[i].value == atomval)
			return (ScanKeywords[i].name);

	elog(WARN, "AtomGetString called with bogus atom # : %d", atomval);
	return (NULL);
}

#endif
