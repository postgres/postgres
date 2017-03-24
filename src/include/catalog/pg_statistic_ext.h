/*-------------------------------------------------------------------------
 *
 * pg_statistic_ext.h
 *	  definition of the system "extended statistic" relation (pg_statistic_ext)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_statistic_ext.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_STATISTIC_EXT_H
#define PG_STATISTIC_EXT_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_statistic_ext definition.  cpp turns this into
 *		typedef struct FormData_pg_statistic_ext
 * ----------------
 */
#define StatisticExtRelationId	3381

CATALOG(pg_statistic_ext,3381)
{
	/* These fields form the unique key for the entry: */
	Oid			starelid;		/* relation containing attributes */
	NameData	staname;		/* statistics name */
	Oid			stanamespace;	/* OID of namespace containing this statistics */
	Oid			staowner;		/* statistics owner */

	/*
	 * variable-length fields start here, but we allow direct access to
	 * stakeys
	 */
	int2vector	stakeys;		/* array of column keys */

#ifdef CATALOG_VARLEN
	char		staenabled[1] BKI_FORCE_NOT_NULL;	/* statistic types
													 * requested to build */
	pg_ndistinct standistinct;	/* ndistinct coefficients (serialized) */
#endif

} FormData_pg_statistic_ext;

/* ----------------
 *		Form_pg_statistic_ext corresponds to a pointer to a tuple with
 *		the format of pg_statistic_ext relation.
 * ----------------
 */
typedef FormData_pg_statistic_ext *Form_pg_statistic_ext;

/* ----------------
 *		compiler constants for pg_statistic_ext
 * ----------------
 */
#define Natts_pg_statistic_ext					7
#define Anum_pg_statistic_ext_starelid			1
#define Anum_pg_statistic_ext_staname			2
#define Anum_pg_statistic_ext_stanamespace		3
#define Anum_pg_statistic_ext_staowner			4
#define Anum_pg_statistic_ext_stakeys			5
#define Anum_pg_statistic_ext_staenabled		6
#define Anum_pg_statistic_ext_standistinct		7

#define STATS_EXT_NDISTINCT		'd'

#endif   /* PG_STATISTIC_EXT_H */
