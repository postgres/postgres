/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for reserved words in PostgreSQL
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/keywords.c,v 1.85 2008/01/01 19:45:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "extern.h"
#include "preproc.h"

/* compile both keyword lists in one file because they are always scanned together */
#include "ecpg_keywords.c"

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static const ScanKeyword ScanPGSQLKeywords[] = {
	/* name, value */
	{"abort", ABORT_P},
	{"absolute", ABSOLUTE_P},
	{"access", ACCESS},
	{"action", ACTION},
	{"add", ADD_P},
	{"admin", ADMIN},
	{"after", AFTER},
	{"aggregate", AGGREGATE},
	{"all", ALL},
	{"also", ALSO},
	{"alter", ALTER},
	{"always", ALWAYS},
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
	{"cascaded", CASCADED},
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
	{"concurrently", CONCURRENTLY},
	{"configuration", CONFIGURATION},
	{"connection", CONNECTION},
	{"constraint", CONSTRAINT},
	{"constraints", CONSTRAINTS},
	{"content", CONTENT_P},
	{"conversion", CONVERSION_P},
	{"copy", COPY},
	{"cost", COST},
	{"create", CREATE},
	{"createdb", CREATEDB},
	{"createrole", CREATEROLE},
	{"createuser", CREATEUSER},
	{"cross", CROSS},
	{"csv", CSV},
	{"current", CURRENT_P},
	{"current_date", CURRENT_DATE},
	{"current_role", CURRENT_ROLE},
	{"current_time", CURRENT_TIME},
	{"current_timestamp", CURRENT_TIMESTAMP},
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
	{"dictionary", DICTIONARY},
	{"disable", DISABLE_P},
	{"discard", DISCARD},
	{"distinct", DISTINCT},
	{"do", DO},
	{"document", DOCUMENT_P},
	{"domain", DOMAIN_P},
	{"double", DOUBLE_P},
	{"drop", DROP},
	{"each", EACH},
	{"else", ELSE},
	{"enable", ENABLE_P},
	{"encoding", ENCODING},
	{"encrypted", ENCRYPTED},
	{"end", END_P},
	{"enum", ENUM_P},
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
	{"family", FAMILY},
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
	{"get", GET},
	{"global", GLOBAL},
	{"grant", GRANT},
	{"granted", GRANTED},
	{"greatest", GREATEST},
	{"group", GROUP_P},
	{"handler", HANDLER},
	{"having", HAVING},
	{"header", HEADER_P},
	{"hold", HOLD},
	{"hour", HOUR_P},
	{"if", IF_P},
	{"ilike", ILIKE},
	{"immediate", IMMEDIATE},
	{"immutable", IMMUTABLE},
	{"implicit", IMPLICIT_P},
	{"in", IN_P},
	{"including", INCLUDING},
	{"increment", INCREMENT},
	{"index", INDEX},
	{"indexes", INDEXES},
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
	{"location", LOCATION},
	{"lock", LOCK_P},
	{"login", LOGIN_P},
	{"mapping", MAPPING},
	{"match", MATCH},
	{"maxvalue", MAXVALUE},
	{"minute", MINUTE_P},
	{"minvalue", MINVALUE},
	{"mode", MODE},
	{"month", MONTH_P},
	{"move", MOVE},
	{"name", NAME_P},
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
	{"nulls", NULLS_P},
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
	{"owned", OWNED},
	{"owner", OWNER},
	{"parser", PARSER},
	{"partial", PARTIAL},
	{"password", PASSWORD},
	{"placing", PLACING},
	{"plans", PLANS},
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
	{"reassign", REASSIGN},
	{"recheck", RECHECK},
	{"references", REFERENCES},
	{"reindex", REINDEX},
	{"relative", RELATIVE_P},
	{"release", RELEASE},
	{"rename", RENAME},
	{"repeatable", REPEATABLE},
	{"replace", REPLACE},
	{"replica", REPLICA},
	{"reset", RESET},
	{"restart", RESTART},
	{"restrict", RESTRICT},
	{"returning", RETURNING},
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
	{"search", SEARCH},
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
	{"standalone", STANDALONE_P},
	{"start", START},
	{"statement", STATEMENT},
	{"statistics", STATISTICS},
	{"stdin", STDIN},
	{"stdout", STDOUT},
	{"storage", STORAGE},
	{"strict", STRICT_P},
	{"strip", STRIP_P},
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
	{"text", TEXT_P},
	{"then", THEN},
	{"time", TIME},
	{"timestamp", TIMESTAMP},
	{"to", TO},
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
	{"value", VALUE_P},
	{"values", VALUES},
	{"varchar", VARCHAR},
	{"varying", VARYING},
	{"verbose", VERBOSE},
	{"version", VERSION_P},
	{"view", VIEW},
	{"volatile", VOLATILE},
	{"when", WHEN},
	{"where", WHERE},
	{"whitespace", WHITESPACE_P},
	{"with", WITH},
	{"without", WITHOUT},
	{"work", WORK},
	{"write", WRITE},
	{"xml", XML_P},
	{"xmlattributes", XMLATTRIBUTES},
	{"xmlconcat", XMLCONCAT},
	{"xmlelement", XMLELEMENT},
	{"xmlforest", XMLFOREST},
	{"xmlparse", XMLPARSE},
	{"xmlpi", XMLPI},
	{"xmlroot", XMLROOT},
	{"xmlserialize", XMLSERIALIZE},
	{"year", YEAR_P},
	{"yes", YES_P},
	{"zone", ZONE},
};


/*
 * Now do a binary search using plain strcmp() comparison.
 */
const ScanKeyword *
DoLookup(char *word, const ScanKeyword *low, const ScanKeyword *high)
{
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
ScanKeywordLookup(char *text)
{
	int			len,
				i;
	char		word[NAMEDATALEN];
	const ScanKeyword *res;

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
	res = DoLookup(word, &ScanPGSQLKeywords[0], endof(ScanPGSQLKeywords) - 1);
	if (res)
		return res;

	return DoLookup(word, &ScanECPGKeywords[0], endof(ScanECPGKeywords) - 1);
}
