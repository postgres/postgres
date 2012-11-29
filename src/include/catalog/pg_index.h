/*-------------------------------------------------------------------------
 *
 * pg_index.h
 *	  definition of the system "index" relation (pg_index)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_index.h,v 1.50 2010/01/05 01:06:56 tgl Exp $
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INDEX_H
#define PG_INDEX_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_index definition.  cpp turns this into
 *		typedef struct FormData_pg_index.
 * ----------------
 */
#define IndexRelationId  2610

CATALOG(pg_index,2610) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
	Oid			indexrelid;		/* OID of the index */
	Oid			indrelid;		/* OID of the relation it indexes */
	int2		indnatts;		/* number of columns in index */
	bool		indisunique;	/* is this a unique index? */
	bool		indisprimary;	/* is this index for primary key? */
	bool		indimmediate;	/* is uniqueness enforced immediately? */
	bool		indisclustered; /* is this the index last clustered by? */
	bool		indisvalid;		/* is this index valid for use by queries? */
	bool		indcheckxmin;	/* must we wait for xmin to be old? */
	bool		indisready;		/* is this index ready for inserts? */

	/* VARIABLE LENGTH FIELDS: */
	int2vector	indkey;			/* column numbers of indexed cols, or 0 */
	oidvector	indclass;		/* opclass identifiers */
	int2vector	indoption;		/* per-column flags (AM-specific meanings) */
	text		indexprs;		/* expression trees for index attributes that
								 * are not simple column references; one for
								 * each zero entry in indkey[] */
	text		indpred;		/* expression tree for predicate, if a partial
								 * index; else NULL */
} FormData_pg_index;

/* ----------------
 *		Form_pg_index corresponds to a pointer to a tuple with
 *		the format of pg_index relation.
 * ----------------
 */
typedef FormData_pg_index *Form_pg_index;

/* ----------------
 *		compiler constants for pg_index
 * ----------------
 */
#define Natts_pg_index					15
#define Anum_pg_index_indexrelid		1
#define Anum_pg_index_indrelid			2
#define Anum_pg_index_indnatts			3
#define Anum_pg_index_indisunique		4
#define Anum_pg_index_indisprimary		5
#define Anum_pg_index_indimmediate		6
#define Anum_pg_index_indisclustered	7
#define Anum_pg_index_indisvalid		8
#define Anum_pg_index_indcheckxmin		9
#define Anum_pg_index_indisready		10
#define Anum_pg_index_indkey			11
#define Anum_pg_index_indclass			12
#define Anum_pg_index_indoption			13
#define Anum_pg_index_indexprs			14
#define Anum_pg_index_indpred			15

/*
 * Index AMs that support ordered scans must support these two indoption
 * bits.  Otherwise, the content of the per-column indoption fields is
 * open for future definition.
 */
#define INDOPTION_DESC			0x0001	/* values are in reverse order */
#define INDOPTION_NULLS_FIRST	0x0002	/* NULLs are first instead of last */

/*
 * Use of these macros is recommended over direct examination of the state
 * flag columns where possible; this allows source code compatibility with
 * 9.2 and up.
 */
#define IndexIsValid(indexForm) ((indexForm)->indisvalid)
#define IndexIsReady(indexForm) ((indexForm)->indisready)

#endif   /* PG_INDEX_H */
