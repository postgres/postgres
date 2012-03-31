/*-------------------------------------------------------------------------
 *
 * ecpg_keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/ecpg_keywords.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>

#include "extern.h"
#include "preproc.h"

/* Globals from keywords.c */
extern const ScanKeyword SQLScanKeywords[];
extern const int NumSQLScanKeywords;

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static const ScanKeyword ECPGScanKeywords[] = {
	/* name, value, category */

	/*
	 * category is not needed in ecpg, it is only here so we can share the
	 * data structure with the backend
	 */
	{"allocate", SQL_ALLOCATE, 0},
	{"autocommit", SQL_AUTOCOMMIT, 0},
	{"bool", SQL_BOOL, 0},
	{"break", SQL_BREAK, 0},
	{"call", SQL_CALL, 0},
	{"cardinality", SQL_CARDINALITY, 0},
	{"connect", SQL_CONNECT, 0},
	{"count", SQL_COUNT, 0},
	{"datetime_interval_code", SQL_DATETIME_INTERVAL_CODE, 0},
	{"datetime_interval_precision", SQL_DATETIME_INTERVAL_PRECISION, 0},
	{"describe", SQL_DESCRIBE, 0},
	{"descriptor", SQL_DESCRIPTOR, 0},
	{"disconnect", SQL_DISCONNECT, 0},
	{"found", SQL_FOUND, 0},
	{"free", SQL_FREE, 0},
	{"get", SQL_GET, 0},
	{"go", SQL_GO, 0},
	{"goto", SQL_GOTO, 0},
	{"identified", SQL_IDENTIFIED, 0},
	{"indicator", SQL_INDICATOR, 0},
	{"key_member", SQL_KEY_MEMBER, 0},
	{"length", SQL_LENGTH, 0},
	{"long", SQL_LONG, 0},
	{"nullable", SQL_NULLABLE, 0},
	{"octet_length", SQL_OCTET_LENGTH, 0},
	{"open", SQL_OPEN, 0},
	{"output", SQL_OUTPUT, 0},
	{"reference", SQL_REFERENCE, 0},
	{"returned_length", SQL_RETURNED_LENGTH, 0},
	{"returned_octet_length", SQL_RETURNED_OCTET_LENGTH, 0},
	{"scale", SQL_SCALE, 0},
	{"section", SQL_SECTION, 0},
	{"short", SQL_SHORT, 0},
	{"signed", SQL_SIGNED, 0},
	{"sql", SQL_SQL, 0},		/* strange thing, used for into sql descriptor
								 * MYDESC; */
	{"sqlerror", SQL_SQLERROR, 0},
	{"sqlprint", SQL_SQLPRINT, 0},
	{"sqlwarning", SQL_SQLWARNING, 0},
	{"stop", SQL_STOP, 0},
	{"struct", SQL_STRUCT, 0},
	{"unsigned", SQL_UNSIGNED, 0},
	{"var", SQL_VAR, 0},
	{"whenever", SQL_WHENEVER, 0},
};

/*
 * ScanECPGKeywordLookup - see if a given word is a keyword
 *
 * Returns a pointer to the ScanKeyword table entry, or NULL if no match.
 * Keywords are matched using the same case-folding rules as in the backend.
 */
const ScanKeyword *
ScanECPGKeywordLookup(const char *text)
{
	const ScanKeyword *res;

	/* First check SQL symbols defined by the backend. */
	res = ScanKeywordLookup(text, SQLScanKeywords, NumSQLScanKeywords);
	if (res)
		return res;

	/* Try ECPG-specific keywords. */
	res = ScanKeywordLookup(text, ECPGScanKeywords, lengthof(ECPGScanKeywords));
	if (res)
		return res;

	return NULL;
}
