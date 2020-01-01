/*-------------------------------------------------------------------------
 *
 * c_kwlist.h
 *
 * The keyword lists are kept in their own source files for use by
 * automatic tools.  The exact representation of a keyword is determined
 * by the PG_KEYWORD macro, which is not defined in this file; it can
 * be defined by the caller for special purposes.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/ecpg/preproc/c_kwlist.h
 *
 *-------------------------------------------------------------------------
 */

/* There is deliberately not an #ifndef C_KWLIST_H here. */

/*
 * List of (keyword-name, keyword-token-value) pairs.
 *
 * Note: gen_keywordlist.pl requires the entries to appear in ASCII order.
 */

/* name, value */
PG_KEYWORD("VARCHAR", VARCHAR)
PG_KEYWORD("auto", S_AUTO)
PG_KEYWORD("bool", SQL_BOOL)
PG_KEYWORD("char", CHAR_P)
PG_KEYWORD("const", S_CONST)
PG_KEYWORD("enum", ENUM_P)
PG_KEYWORD("extern", S_EXTERN)
PG_KEYWORD("float", FLOAT_P)
PG_KEYWORD("hour", HOUR_P)
PG_KEYWORD("int", INT_P)
PG_KEYWORD("long", SQL_LONG)
PG_KEYWORD("minute", MINUTE_P)
PG_KEYWORD("month", MONTH_P)
PG_KEYWORD("register", S_REGISTER)
PG_KEYWORD("second", SECOND_P)
PG_KEYWORD("short", SQL_SHORT)
PG_KEYWORD("signed", SQL_SIGNED)
PG_KEYWORD("static", S_STATIC)
PG_KEYWORD("struct", SQL_STRUCT)
PG_KEYWORD("to", TO)
PG_KEYWORD("typedef", S_TYPEDEF)
PG_KEYWORD("union", UNION)
PG_KEYWORD("unsigned", SQL_UNSIGNED)
PG_KEYWORD("varchar", VARCHAR)
PG_KEYWORD("volatile", S_VOLATILE)
PG_KEYWORD("year", YEAR_P)
