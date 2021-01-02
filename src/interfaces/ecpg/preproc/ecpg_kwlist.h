/*-------------------------------------------------------------------------
 *
 * ecpg_kwlist.h
 *
 * The keyword lists are kept in their own source files for use by
 * automatic tools.  The exact representation of a keyword is determined
 * by the PG_KEYWORD macro, which is not defined in this file; it can
 * be defined by the caller for special purposes.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/ecpg/preproc/ecpg_kwlist.h
 *
 *-------------------------------------------------------------------------
 */

/* There is deliberately not an #ifndef ECPG_KWLIST_H here. */

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * Note: gen_keywordlist.pl requires the entries to appear in ASCII order.
 */

/* name, value */
PG_KEYWORD("allocate", SQL_ALLOCATE)
PG_KEYWORD("autocommit", SQL_AUTOCOMMIT)
PG_KEYWORD("bool", SQL_BOOL)
PG_KEYWORD("break", SQL_BREAK)
PG_KEYWORD("cardinality", SQL_CARDINALITY)
PG_KEYWORD("connect", SQL_CONNECT)
PG_KEYWORD("count", SQL_COUNT)
PG_KEYWORD("datetime_interval_code", SQL_DATETIME_INTERVAL_CODE)
PG_KEYWORD("datetime_interval_precision", SQL_DATETIME_INTERVAL_PRECISION)
PG_KEYWORD("describe", SQL_DESCRIBE)
PG_KEYWORD("descriptor", SQL_DESCRIPTOR)
PG_KEYWORD("disconnect", SQL_DISCONNECT)
PG_KEYWORD("found", SQL_FOUND)
PG_KEYWORD("free", SQL_FREE)
PG_KEYWORD("get", SQL_GET)
PG_KEYWORD("go", SQL_GO)
PG_KEYWORD("goto", SQL_GOTO)
PG_KEYWORD("identified", SQL_IDENTIFIED)
PG_KEYWORD("indicator", SQL_INDICATOR)
PG_KEYWORD("key_member", SQL_KEY_MEMBER)
PG_KEYWORD("length", SQL_LENGTH)
PG_KEYWORD("long", SQL_LONG)
PG_KEYWORD("nullable", SQL_NULLABLE)
PG_KEYWORD("octet_length", SQL_OCTET_LENGTH)
PG_KEYWORD("open", SQL_OPEN)
PG_KEYWORD("output", SQL_OUTPUT)
PG_KEYWORD("reference", SQL_REFERENCE)
PG_KEYWORD("returned_length", SQL_RETURNED_LENGTH)
PG_KEYWORD("returned_octet_length", SQL_RETURNED_OCTET_LENGTH)
PG_KEYWORD("scale", SQL_SCALE)
PG_KEYWORD("section", SQL_SECTION)
PG_KEYWORD("short", SQL_SHORT)
PG_KEYWORD("signed", SQL_SIGNED)
PG_KEYWORD("sqlerror", SQL_SQLERROR)
PG_KEYWORD("sqlprint", SQL_SQLPRINT)
PG_KEYWORD("sqlwarning", SQL_SQLWARNING)
PG_KEYWORD("stop", SQL_STOP)
PG_KEYWORD("struct", SQL_STRUCT)
PG_KEYWORD("unsigned", SQL_UNSIGNED)
PG_KEYWORD("var", SQL_VAR)
PG_KEYWORD("whenever", SQL_WHENEVER)
