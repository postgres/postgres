/*-------------------------------------------------------------------------
 *
 * ecpg_keywords.c
 *	  lexical token lookup for reserved words in postgres embedded SQL
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/ecpg_keywords.c,v 1.37 2007/11/15 21:14:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * !!WARNING!!: This list must be sorted, because binary
 *		 search is used to locate entries.
 */
static const ScanKeyword ScanECPGKeywords[] = {
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
	{"data", SQL_DATA},
	{"datetime_interval_code", SQL_DATETIME_INTERVAL_CODE},
	{"datetime_interval_precision", SQL_DATETIME_INTERVAL_PRECISION},
	{"describe", SQL_DESCRIBE},
	{"descriptor", SQL_DESCRIPTOR},
	{"disconnect", SQL_DISCONNECT},
	{"found", SQL_FOUND},
	{"free", SQL_FREE},
	{"go", SQL_GO},
	{"goto", SQL_GOTO},
	{"identified", SQL_IDENTIFIED},
	{"indicator", SQL_INDICATOR},
	{"key_member", SQL_KEY_MEMBER},
	{"length", SQL_LENGTH},
	{"long", SQL_LONG},
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
	{"var", SQL_VAR},
	{"whenever", SQL_WHENEVER},
};
