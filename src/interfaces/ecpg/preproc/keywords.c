/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for reserved words in PostgreSQL
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/ecpg/preproc/keywords.c,v 1.38 2001/02/21 18:53:47 tgl Exp $
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
	/* name, value */
	{"abort", ABORT_TRANS},
	{"absolute", ABSOLUTE},
	{"access", ACCESS},
	{"action", ACTION},
	{"add", ADD},
	{"after", AFTER},
	{"aggregate", AGGREGATE},
	{"all", ALL},
	{"alter", ALTER},
	{"analyse", ANALYSE}, /* British spelling */
	{"analyze", ANALYZE},
	{"and", AND},
	{"any", ANY},
	{"as", AS},
	{"asc", ASC},
	{"at", AT},
	{"backward", BACKWARD},
	{"before", BEFORE},
	{"begin", BEGIN_TRANS},
	{"between", BETWEEN},
	{"binary", BINARY},
	{"bit", BIT},
	{"both", BOTH},
	{"by", BY},
	{"cache", CACHE},
	{"cascade", CASCADE},
	{"case", CASE},
	{"cast", CAST},
	{"chain", CHAIN},
	{"char", CHAR},
	{"character", CHARACTER},
	{"characteristics", CHARACTERISTICS},
	{"check", CHECK},
	{"checkpoint", CHECKPOINT},
	{"close", CLOSE},
	{"cluster", CLUSTER},
	{"coalesce", COALESCE},
	{"collate", COLLATE},
	{"column", COLUMN},
	{"comment", COMMENT},
	{"commit", COMMIT},
	{"committed", COMMITTED},
	{"constraint", CONSTRAINT},
	{"constraints", CONSTRAINTS},
	{"copy", COPY},
	{"create", CREATE},
	{"createdb", CREATEDB},
	{"createuser", CREATEUSER},
	{"cross", CROSS},
	{"current_date", CURRENT_DATE},
	{"current_time", CURRENT_TIME},
	{"current_timestamp", CURRENT_TIMESTAMP},
	{"current_user", CURRENT_USER},
	{"cursor", CURSOR},
	{"cycle", CYCLE},
	{"database", DATABASE},
	{"day", DAY_P},
	{"dec", DEC},
	{"decimal", DECIMAL},
	{"declare", DECLARE},
	{"default", DEFAULT},
	{"deferrable", DEFERRABLE},
	{"deferred", DEFERRED},
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
	{"escape", ESCAPE},
	{"except", EXCEPT},
	{"exclusive", EXCLUSIVE},
	{"execute", EXECUTE},
	{"exists", EXISTS},
	{"explain", EXPLAIN},
	{"extend", EXTEND},
	{"extract", EXTRACT},
	{"false", FALSE_P},
	{"fetch", FETCH},
	{"float", FLOAT},
	{"for", FOR},
	{"force", FORCE},
	{"foreign", FOREIGN},
	{"forward", FORWARD},
	{"from", FROM},
	{"full", FULL},
	{"function", FUNCTION},
	{"global", GLOBAL},
	{"grant", GRANT},
	{"group", GROUP},
	{"handler", HANDLER},
	{"having", HAVING},
	{"hour", HOUR_P},
	{"ilike", ILIKE},
	{"immediate", IMMEDIATE},
	{"in", IN},
	{"increment", INCREMENT},
	{"index", INDEX},
	{"inherits", INHERITS},
	{"initially", INITIALLY},
	{"inner", INNER_P},
	{"inout", INOUT},
	{"insensitive", INSENSITIVE},
	{"insert", INSERT},
	{"instead", INSTEAD},
	{"intersect", INTERSECT},
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
	{"limit", LIMIT},
	{"listen", LISTEN},
	{"load", LOAD},
	{"local", LOCAL},
	{"location", LOCATION},
	{"lock", LOCK_P},
	{"match", MATCH},
	{"maxvalue", MAXVALUE},
	{"minute", MINUTE_P},
	{"minvalue", MINVALUE},
	{"mode", MODE},
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
	{"off", OFF},
	{"offset", OFFSET},
	{"oids", OIDS},
	{"old", OLD},
	{"on", ON},
	{"only", ONLY},
	{"operator", OPERATOR},
	{"option", OPTION},
	{"or", OR},
	{"order", ORDER},
	{"out", OUT},
	{"outer", OUTER_P},
	{"overlaps", OVERLAPS},
	{"owner", OWNER},
	{"partial", PARTIAL},
	{"password", PASSWORD},
	{"path", PATH_P},
	{"pendant", PENDANT},
	{"position", POSITION},
	{"precision", PRECISION},
	{"primary", PRIMARY},
	{"prior", PRIOR},
	{"privileges", PRIVILEGES},
	{"procedural", PROCEDURAL},
	{"procedure", PROCEDURE},
	{"public", PUBLIC},
	{"read", READ},
	{"references", REFERENCES},
	{"reindex", REINDEX},
	{"relative", RELATIVE},
	{"rename", RENAME},
	{"reset", RESET},
	{"restrict", RESTRICT},
	{"returns", RETURNS},
	{"revoke", REVOKE},
	{"right", RIGHT},
	{"rollback", ROLLBACK},
	{"row", ROW},
	{"rule", RULE},
	{"schema", SCHEMA},
	{"scroll", SCROLL},
	{"second", SECOND_P},
	{"select", SELECT},
	{"sequence", SEQUENCE},
	{"serial", SERIAL},
	{"serializable", SERIALIZABLE},
	{"session", SESSION},
	{"session_user", SESSION_USER},
	{"set", SET},
	{"setof", SETOF},
	{"share", SHARE},
	{"show", SHOW},
	{"some", SOME},
	{"start", START},
	{"statement", STATEMENT},
	{"stdin", STDIN},
	{"stdout", STDOUT},
	{"substring", SUBSTRING},
	{"sysid", SYSID},
	{"table", TABLE},
	{"temp", TEMP},
	{"template", TEMPLATE},
	{"temporary", TEMPORARY},
	{"then", THEN},
	{"time", TIME},
	{"timestamp", TIMESTAMP},
	{"timezone_hour", TIMEZONE_HOUR},
	{"timezone_minute", TIMEZONE_MINUTE},
	{"to", TO},
	{"toast", TOAST},
	{"trailing", TRAILING},
	{"transaction", TRANSACTION},
	{"trigger", TRIGGER},
	{"trim", TRIM},
	{"true", TRUE_P},
	{"truncate", TRUNCATE},
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
	{"without", WITHOUT},
	{"work", WORK},
	{"year", YEAR_P},
	{"zone", ZONE},
};

/*
 * ScanKeywordLookup - see if a given word is a keyword
 *
 * Returns a pointer to the ScanKeyword table entry, or NULL if no match.
 *
 * The match is done case-insensitively.  Note that we deliberately use a
 * dumbed-down case conversion that will only translate 'A'-'Z' into 'a'-'z',
 * even if we are in a locale where tolower() would produce more or different
 * translations.  This is to conform to the SQL99 spec, which says that
 * keywords are to be matched in this way even though non-keyword identifiers
 * receive a different case-normalization mapping.
 */
ScanKeyword *
ScanKeywordLookup(char *text)
{
	int			len,
				i;
	char		word[NAMEDATALEN];
	ScanKeyword *low;
	ScanKeyword *high;

	len = strlen(text);
	/* We assume all keywords are shorter than NAMEDATALEN. */
	if (len >= NAMEDATALEN)
		return NULL;

	/*
	 * Apply an ASCII-only downcasing.  We must not use tolower() since
	 * it may produce the wrong translation in some locales (eg, Turkish),
	 * and we don't trust isupper() very much either.  In an ASCII-based
	 * encoding the tests against A and Z are sufficient, but we also check
	 * isupper() so that we will work correctly under EBCDIC.  The actual
	 * case conversion step should work for either ASCII or EBCDIC.
	 */
	for (i = 0; i < len; i++)
	{
		char	ch = text[i];

		if (ch >= 'A' && ch <= 'Z' && isupper((unsigned char) ch))
			ch += 'a' - 'A';
		word[i] = ch;
	}
	word[len] = '\0';

	/*
	 * Now do a binary search using plain strcmp() comparison.
	 */
	low = &ScanKeywords[0];
	high = endof(ScanKeywords) - 1;
	while (low <= high)
	{
		ScanKeyword *middle;
		int			difference;

		middle = low + (high - low) / 2;
		difference = strcmp(middle->name, word);
		if (difference == 0)
			return middle;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return NULL;
}
