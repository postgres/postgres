/*-------------------------------------------------------------------------
 *
 * pg_index.h
 *	  definition of the system "index" relation (pg_index)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_index.h,v 1.32 2003/08/04 02:40:12 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INDEX_H
#define PG_INDEX_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_index definition.  cpp turns this into
 *		typedef struct FormData_pg_index.
 * ----------------
 */
CATALOG(pg_index) BKI_WITHOUT_OIDS
{
	Oid			indexrelid;		/* OID of the index */
	Oid			indrelid;		/* OID of the relation it indexes */
	int2vector	indkey;			/* column numbers of indexed cols, or 0 */
	oidvector	indclass;		/* opclass identifiers */
	int2		indnatts;		/* number of columns in index */
	bool		indisunique;	/* is this a unique index? */
	bool		indisprimary;	/* is this index for primary key? */
	bool		indisclustered; /* is this the index last clustered by? */

	/* VARIABLE LENGTH FIELDS: */
	text		indexprs;		/* expression trees for index attributes
								 * that are not simple column references;
								 * one for each zero entry in indkey[] */
	text		indpred;		/* expression tree for predicate, if a
								 * partial index; else NULL */
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
#define Natts_pg_index					10
#define Anum_pg_index_indexrelid		1
#define Anum_pg_index_indrelid			2
#define Anum_pg_index_indkey			3
#define Anum_pg_index_indclass			4
#define Anum_pg_index_indnatts			5
#define Anum_pg_index_indisunique		6
#define Anum_pg_index_indisprimary		7
#define Anum_pg_index_indisclustered	8
#define Anum_pg_index_indexprs			9
#define Anum_pg_index_indpred			10

#endif   /* PG_INDEX_H */
