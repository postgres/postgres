/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for reserved words in PostgreSQL
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/keywords.c,v 1.166 2005/10/15 02:49:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "nodes/parsenodes.h"
#include "parser/keywords.h"
#include "parser/parse.h"

/* NB: This file is also used by pg_dump. */

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static const ScanKeyword ScanKeywords[] = {
	/* name, value */
	{"abort", ABORT_P},
	{"absolute", ABSOLUTE_P},
	{"access", ACCESS},
	{"action", ACTION},
	{"add", ADD},
	{"admin", ADMIN},
	{"after", AFTER},
	{"aggregate", AGGREGATE},
	{"all", ALL},
	{"also", ALSO},
	{"alter", ALTER},
	{"analyse", ANALYSE},		/* British spelling */
	{"analyze", ANALYZE},
	{"and", AND},
	{"any", ANY},
	{"array", ARRAY},
	{"as", AS},
	{"asc", ASC},
	{"assertion", ASSERTION},
	{"assignment", ASSIGNMENT},
	{"asymmetric", ASYMMETRIC},
	{"at", AT},
	{"authorization", AUTHORIZATION},
	{"backward", BACKWARD},
	{"before", BEFORE},
	{"begin", BEGIN_P},
	{"between", BETWEEN},
	{"bigint", BIGINT},
	{"binary", BINARY},
	{"bit", BIT},
	{"boolean", BOOLEAN_P},
	{"both", BOTH},
	{"by", BY},
	{"cache", CACHE},
	{"called", CALLED},
	{"cascade", CASCADE},
	{"case", CASE},
	{"cast", CAST},
	{"chain", CHAIN},
	{"char", CHAR_P},
	{"character", CHARACTER},
	{"characteristics", CHARACTERISTICS},
	{"check", CHECK},
	{"checkpoint", CHECKPOINT},
	{"class", CLASS},
	{"close", CLOSE},
	{"cluster", CLUSTER},
	{"coalesce", COALESCE},
	{"collate", COLLATE},
	{"column", COLUMN},
	{"comment", COMMENT},
	{"commit", COMMIT},
	{"committed", COMMITTED},
	{"connection", CONNECTION},
	{"constraint", CONSTRAINT},
	{"constraints", CONSTRAINTS},
	{"conversion", CONVERSION_P},
	{"convert", CONVERT},
	{"copy", COPY},
	{"create", CREATE},
	{"createdb", CREATEDB},
	{"createrole", CREATEROLE},
	{"createuser", CREATEUSER},
	{"cross", CROSS},
	{"csv", CSV},
	{"current_date", CURRENT_DATE},
	{"current_role", CURRENT_ROLE},
	{"current_time", CURRENT_TIME},
	{"current_timestamp", CURRENT_TIMESTAMP},
	{"current_user", CURRENT_USER},
	{"cursor", CURSOR},
	{"cycle", CYCLE},
	{"database", DATABASE},
	{"day", DAY_P},
	{"deallocate", DEALLOCATE},
	{"dec", DEC},
	{"decimal", DECIMAL_P},
	{"declare", DECLARE},
	{"default", DEFAULT},
	{"defaults", DEFAULTS},
	{"deferrable", DEFERRABLE},
	{"deferred", DEFERRED},
	{"definer", DEFINER},
	{"delete", DELETE_P},
	{"delimiter", DELIMITER},
	{"delimiters", DELIMITERS},
	{"desc", DESC},
	{"disable", DISABLE_P},
	{"distinct", DISTINCT},
	{"do", DO},
	{"domain", DOMAIN_P},
	{"double", DOUBLE_P},
	{"drop", DROP},
	{"each", EACH},
	{"else", ELSE},
	{"enable", ENABLE_P},
	{"encoding", ENCODING},
	{"encrypted", ENCRYPTED},
	{"end", END_P},
	{"escape", ESCAPE},
	{"except", EXCEPT},
	{"excluding", EXCLUDING},
	{"exclusive", EXCLUSIVE},
	{"execute", EXECUTE},
	{"exists", EXISTS},
	{"explain", EXPLAIN},
	{"external", EXTERNAL},
	{"extract", EXTRACT},
	{"false", FALSE_P},
	{"fetch", FETCH},
	{"first", FIRST_P},
	{"float", FLOAT_P},
	{"for", FOR},
	{"force", FORCE},
	{"foreign", FOREIGN},
	{"forward", FORWARD},
	{"freeze", FREEZE},
	{"from", FROM},
	{"full", FULL},
	{"function", FUNCTION},
	{"global", GLOBAL},
	{"grant", GRANT},
	{"granted", GRANTED},
	{"greatest", GREATEST},
	{"group", GROUP_P},
	{"handler", HANDLER},
	{"having", HAVING},
	{"header", HEADER},
	{"hold", HOLD},
	{"hour", HOUR_P},
	{"ilike", ILIKE},
	{"immediate", IMMEDIATE},
	{"immutable", IMMUTABLE},
	{"implicit", IMPLICIT_P},
	{"in", IN_P},
	{"including", INCLUDING},
	{"increment", INCREMENT},
	{"index", INDEX},
	{"inherit", INHERIT},
	{"inherits", INHERITS},
	{"initially", INITIALLY},
	{"inner", INNER_P},
	{"inout", INOUT},
	{"input", INPUT_P},
	{"insensitive", INSENSITIVE},
	{"insert", INSERT},
	{"instead", INSTEAD},
	{"int", INT_P},
	{"integer", INTEGER},
	{"intersect", INTERSECT},
	{"interval", INTERVAL},
	{"into", INTO},
	{"invoker", INVOKER},
	{"is", IS},
	{"isnull", ISNULL},
	{"isolation", ISOLATION},
	{"join", JOIN},
	{"key", KEY},
	{"lancompiler", LANCOMPILER},
	{"language", LANGUAGE},
	{"large", LARGE_P},
	{"last", LAST_P},
	{"leading", LEADING},
	{"least", LEAST},
	{"left", LEFT},
	{"level", LEVEL},
	{"like", LIKE},
	{"limit", LIMIT},
	{"listen", LISTEN},
	{"load", LOAD},
	{"local", LOCAL},
	{"localtime", LOCALTIME},
	{"localtimestamp", LOCALTIMESTAMP},
	{"location", LOCATION},
	{"lock", LOCK_P},
	{"login", LOGIN_P},
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
	{"nocreaterole", NOCREATEROLE},
	{"nocreateuser", NOCREATEUSER},
	{"noinherit", NOINHERIT},
	{"nologin", NOLOGIN_P},
	{"none", NONE},
	{"nosuperuser", NOSUPERUSER},
	{"not", NOT},
	{"nothing", NOTHING},
	{"notify", NOTIFY},
	{"notnull", NOTNULL},
	{"nowait", NOWAIT},
	{"null", NULL_P},
	{"nullif", NULLIF},
	{"numeric", NUMERIC},
	{"object", OBJECT_P},
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
	{"out", OUT_P},
	{"outer", OUTER_P},
	{"overlaps", OVERLAPS},
	{"overlay", OVERLAY},
	{"owner", OWNER},
	{"partial", PARTIAL},
	{"password", PASSWORD},
	{"placing", PLACING},
	{"position", POSITION},
	{"precision", PRECISION},
	{"prepare", PREPARE},
	{"prepared", PREPARED},
	{"preserve", PRESERVE},
	{"primary", PRIMARY},
	{"prior", PRIOR},
	{"privileges", PRIVILEGES},
	{"procedural", PROCEDURAL},
	{"procedure", PROCEDURE},
	{"quote", QUOTE},
	{"read", READ},
	{"real", REAL},
	{"recheck", RECHECK},
	{"references", REFERENCES},
	{"reindex", REINDEX},
	{"relative", RELATIVE_P},
	{"release", RELEASE},
	{"rename", RENAME},
	{"repeatable", REPEATABLE},
	{"replace", REPLACE},
	{"reset", RESET},
	{"restart", RESTART},
	{"restrict", RESTRICT},
	{"returns", RETURNS},
	{"revoke", REVOKE},
	{"right", RIGHT},
	{"role", ROLE},
	{"rollback", ROLLBACK},
	{"row", ROW},
	{"rows", ROWS},
	{"rule", RULE},
	{"savepoint", SAVEPOINT},
	{"schema", SCHEMA},
	{"scroll", SCROLL},
	{"second", SECOND_P},
	{"security", SECURITY},
	{"select", SELECT},
	{"sequence", SEQUENCE},
	{"serializable", SERIALIZABLE},
	{"session", SESSION},
	{"session_user", SESSION_USER},
	{"set", SET},
	{"setof", SETOF},
	{"share", SHARE},
	{"show", SHOW},
	{"similar", SIMILAR},
	{"simple", SIMPLE},
	{"smallint", SMALLINT},
	{"some", SOME},
	{"stable", STABLE},
	{"start", START},
	{"statement", STATEMENT},
	{"statistics", STATISTICS},
	{"stdin", STDIN},
	{"stdout", STDOUT},
	{"storage", STORAGE},
	{"strict", STRICT_P},
	{"substring", SUBSTRING},
	{"superuser", SUPERUSER_P},
	{"symmetric", SYMMETRIC},
	{"sysid", SYSID},
	{"system", SYSTEM_P},
	{"table", TABLE},
	{"tablespace", TABLESPACE},
	{"temp", TEMP},
	{"template", TEMPLATE},
	{"temporary", TEMPORARY},
	{"then", THEN},
	{"time", TIME},
	{"timestamp", TIMESTAMP},
	{"to", TO},
	{"toast", TOAST},
	{"trailing", TRAILING},
	{"transaction", TRANSACTION},
	{"treat", TREAT},
	{"trigger", TRIGGER},
	{"trim", TRIM},
	{"true", TRUE_P},
	{"truncate", TRUNCATE},
	{"trusted", TRUSTED},
	{"type", TYPE_P},
	{"uncommitted", UNCOMMITTED},
	{"unencrypted", UNENCRYPTED},
	{"union", UNION},
	{"unique", UNIQUE},
	{"unknown", UNKNOWN},
	{"unlisten", UNLISTEN},
	{"until", UNTIL},
	{"update", UPDATE},
	{"user", USER},
	{"using", USING},
	{"vacuum", VACUUM},
	{"valid", VALID},
	{"validator", VALIDATOR},
	{"values", VALUES},
	{"varchar", VARCHAR},
	{"varying", VARYING},
	{"verbose", VERBOSE},
	{"view", VIEW},
	{"volatile", VOLATILE},
	{"when", WHEN},
	{"where", WHERE},
	{"with", WITH},
	{"without", WITHOUT},
	{"work", WORK},
	{"write", WRITE},
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
const ScanKeyword *
ScanKeywordLookup(const char *text)
{
	int			len,
				i;
	char		word[NAMEDATALEN];
	const ScanKeyword *low;
	const ScanKeyword *high;

	len = strlen(text);
	/* We assume all keywords are shorter than NAMEDATALEN. */
	if (len >= NAMEDATALEN)
		return NULL;

	/*
	 * Apply an ASCII-only downcasing.	We must not use tolower() since it may
	 * produce the wrong translation in some locales (eg, Turkish).
	 */
	for (i = 0; i < len; i++)
	{
		char		ch = text[i];

		if (ch >= 'A' && ch <= 'Z')
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
		const ScanKeyword *middle;
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
