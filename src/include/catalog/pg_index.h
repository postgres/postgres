/*-------------------------------------------------------------------------
 *
 * pg_index.h
 *	  definition of the system "index" relation (pg_index)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_index.h,v 1.30 2003/03/10 22:28:19 tgl Exp $
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
	regproc		indproc;		/* OID of function for functional index */
	int2vector	indkey;			/* column numbers of indexed attributes */
	oidvector	indclass;		/* opclass identifiers */
	bool		indisclustered; /* is this the index last clustered by? */
	bool		indisunique;	/* is this a unique index? */
	bool		indisprimary;	/* is this index for primary key? */
	Oid			indreference;	/* oid of index of referenced relation (ie
								 * - this index for foreign key) */

	/* VARIABLE LENGTH FIELD: */
	text		indpred;		/* expression tree for predicate, if a
								 * partial index */
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
#define Anum_pg_index_indproc			3
#define Anum_pg_index_indkey			4
#define Anum_pg_index_indclass			5
#define Anum_pg_index_indisclustered	6
#define Anum_pg_index_indisunique		7
#define Anum_pg_index_indisprimary		8
#define Anum_pg_index_indreference		9
#define Anum_pg_index_indpred			10

#endif   /* PG_INDEX_H */
