/*-------------------------------------------------------------------------
 *
 * pg_range.h
 *	  definition of the system "range" relation (pg_range)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_range.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *	  XXX do NOT break up DATA() statements into multiple lines!
 *		  the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RANGE_H
#define PG_RANGE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_range definition.  cpp turns this into
 *		typedef struct FormData_pg_range
 * ----------------
 */
#define RangeRelationId 3541

CATALOG(pg_range,3541) BKI_WITHOUT_OIDS
{
	Oid			rngtypid;		/* OID of owning range type */
	Oid			rngsubtype;		/* OID of range's element type (subtype) */
	Oid			rngcollation;	/* collation for this range type, or 0 */
	Oid			rngsubopc;		/* subtype's btree opclass */
	regproc		rngcanonical;	/* canonicalize range, or 0 */
	regproc		rngsubdiff;		/* subtype difference as a float8, or 0 */
} FormData_pg_range;

/* ----------------
 *		Form_pg_range corresponds to a pointer to a tuple with
 *		the format of pg_range relation.
 * ----------------
 */
typedef FormData_pg_range *Form_pg_range;

/* ----------------
 *		compiler constants for pg_range
 * ----------------
 */
#define Natts_pg_range					6
#define Anum_pg_range_rngtypid			1
#define Anum_pg_range_rngsubtype		2
#define Anum_pg_range_rngcollation		3
#define Anum_pg_range_rngsubopc			4
#define Anum_pg_range_rngcanonical		5
#define Anum_pg_range_rngsubdiff		6


/* ----------------
 *		initial contents of pg_range
 * ----------------
 */
DATA(insert ( 3904 23	0 1978 int4range_canonical int4range_subdiff));
DATA(insert ( 3906 1700 0 3125 - numrange_subdiff));
DATA(insert ( 3908 1114 0 3128 - tsrange_subdiff));
DATA(insert ( 3910 1184 0 3127 - tstzrange_subdiff));
DATA(insert ( 3912 1082 0 3122 daterange_canonical daterange_subdiff));
DATA(insert ( 3926 20	0 3124 int8range_canonical int8range_subdiff));


/*
 * prototypes for functions in pg_range.c
 */

extern void RangeCreate(Oid rangeTypeOid, Oid rangeSubType, Oid rangeCollation,
			Oid rangeSubOpclass, RegProcedure rangeCanonical,
			RegProcedure rangeSubDiff);
extern void RangeDelete(Oid rangeTypeOid);

#endif   /* PG_RANGE_H */
