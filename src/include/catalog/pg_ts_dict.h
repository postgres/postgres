/*-------------------------------------------------------------------------
 *
 * pg_ts_dict.h
 *	definition of dictionaries for tsearch
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_ts_dict.h,v 1.3 2008/01/01 19:45:57 momjian Exp $
 *
 * NOTES
 *		the genbki.sh script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TS_DICT_H
#define PG_TS_DICT_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_ts_dict definition.	cpp turns this into
 *		typedef struct FormData_pg_ts_dict
 * ----------------
 */
#define TSDictionaryRelationId	3600

CATALOG(pg_ts_dict,3600)
{
	NameData	dictname;		/* dictionary name */
	Oid			dictnamespace;	/* name space */
	Oid			dictowner;		/* owner */
	Oid			dicttemplate;	/* dictionary's template */
	text		dictinitoption; /* options passed to dict_init() */
} FormData_pg_ts_dict;

typedef FormData_pg_ts_dict *Form_pg_ts_dict;

/* ----------------
 *		compiler constants for pg_ts_dict
 * ----------------
 */
#define Natts_pg_ts_dict				5
#define Anum_pg_ts_dict_dictname		1
#define Anum_pg_ts_dict_dictnamespace	2
#define Anum_pg_ts_dict_dictowner		3
#define Anum_pg_ts_dict_dicttemplate	4
#define Anum_pg_ts_dict_dictinitoption	5

/* ----------------
 *		initial contents of pg_ts_dict
 * ----------------
 */

DATA(insert OID = 3765 ( "simple" PGNSP PGUID 3727 _null_));
DESCR("simple dictionary: just lower case and check for stopword");

#endif   /* PG_TS_DICT_H */
