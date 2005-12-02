/*-------------------------------------------------------------------------
 *
 * ecpg_keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/ecpg_keywords.c,v 1.32 2005/12/02 15:03:57 meskes Exp $
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
	{"allocate", SQL_ALLOCATE},
	{"autocommit", SQL_AUTOCOMMIT},
	{"bool", SQL_BOOL},
	{"break", SQL_BREAK},
	{"call", SQL_CALL},
	{"cardinality", SQL_CARDINALITY},
	{"connect", SQL_CONNECT},
	{"continue", SQL_CONTINUE},
	{"count", SQL_COUNT},
	{"current", SQL_CURRENT},
	{"data", SQL_DATA},
	{"datetime_interval_code", SQL_DATETIME_INTERVAL_CODE},
	{"datetime_interval_precision", SQL_DATETIME_INTERVAL_PRECISION},
	{"describe", SQL_DESCRIBE},
	{"descriptor", SQL_DESCRIPTOR},
	{"disconnect", SQL_DISCONNECT},
	{"enum", SQL_ENUM},
	{"found", SQL_FOUND},
	{"free", SQL_FREE},
	{"go", SQL_GO},
	{"goto", SQL_GOTO},
	{"identified", SQL_IDENTIFIED},
	{"indicator", SQL_INDICATOR},
	{"key_member", SQL_KEY_MEMBER},
	{"length", SQL_LENGTH},
	{"long", SQL_LONG},
	{"name", SQL_NAME},
	{"nullable", SQL_NULLABLE},
	{"octet_length", SQL_OCTET_LENGTH},
	{"open", SQL_OPEN},
	{"output", SQL_OUTPUT},
	{"reference", SQL_REFERENCE},
	{"returned_length", SQL_RETURNED_LENGTH},
	{"returned_octet_length", SQL_RETURNED_OCTET_LENGTH},
	{"scale", SQL_SCALE},
	{"section", SQL_SECTION},
	{"short", SQL_SHORT},
	{"signed", SQL_SIGNED},
	{"sql", SQL_SQL},			/* strange thing, used for into sql descriptor
								 * MYDESC; */
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

/*
 * ScanECPGKeywordLookup - see if a given word is a keyword
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
ScanECPGKeywordLookup(char *text)
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
	 * Apply an ASCII-only downcasing.	We must not use tolower() since it may
	 * produce the wrong translation in some locales (eg, Turkish), and we
	 * don't trust isupper() very much either.  In an ASCII-based encoding the
	 * tests against A and Z are sufficient, but we also check isupper() so
	 * that we will work correctly under EBCDIC.  The actual case conversion
	 * step should work for either ASCII or EBCDIC.
	 */
	for (i = 0; i < len; i++)
	{
		char		ch = text[i];

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
