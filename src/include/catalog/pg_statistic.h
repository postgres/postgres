/*-------------------------------------------------------------------------
 *
 * pg_statistic.h
 *	  definition of the system "statistic" relation (pg_statistic)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_statistic.h,v 1.8 2000/01/26 05:57:58 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_STATISTIC_H
#define PG_STATISTIC_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_statistic definition.  cpp turns this into
 *		typedef struct FormData_pg_statistic
 * ----------------
 */
CATALOG(pg_statistic)
{
	/* These fields form the unique key for the entry: */
	Oid			starelid;		/* relation containing attribute */
	int2		staattnum;		/* attribute (column) stats are for */
	Oid			staop;			/* '<' comparison op used for lo/hi vals */
	/* Note: the current VACUUM code will never produce more than one entry
	 * per column, but in theory there could be multiple entries if a datatype
	 * has more than one useful ordering operator.  Also, the current code
	 * will not write an entry unless it found at least one non-NULL value
	 * in the column; so the remaining fields will never be NULL.
	 */

	/* These fields contain the stats about the column indicated by the key */
	float4		stanullfrac;	/* the fraction of the entries that are NULL */
	float4		stacommonfrac;	/* the fraction that are the most common val */

	/* THE REST OF THESE ARE VARIABLE LENGTH FIELDS.
	 * They cannot be accessed as C struct entries; you have to use the
	 * full field access machinery (heap_getattr) for them.
	 *
	 * All three of these are text representations of data values of the
	 * column's data type.  To re-create the actual Datum, do
	 * datatypein(textout(givenvalue)).
	 */
	text		stacommonval;	/* most common non-null value in column */
	text		staloval;		/* smallest non-null value in column */
	text		stahival;		/* largest non-null value in column */
} FormData_pg_statistic;

/* ----------------
 *		Form_pg_statistic corresponds to a pointer to a tuple with
 *		the format of pg_statistic relation.
 * ----------------
 */
typedef FormData_pg_statistic *Form_pg_statistic;

/* ----------------
 *		compiler constants for pg_statistic
 * ----------------
 */
#define Natts_pg_statistic				8
#define Anum_pg_statistic_starelid		1
#define Anum_pg_statistic_staattnum		2
#define Anum_pg_statistic_staop			3
#define Anum_pg_statistic_stanullfrac	4
#define Anum_pg_statistic_stacommonfrac	5
#define Anum_pg_statistic_stacommonval	6
#define Anum_pg_statistic_staloval		7
#define Anum_pg_statistic_stahival		8

#endif	 /* PG_STATISTIC_H */
