/*-------------------------------------------------------------------------
 *
 * pg_index.h--
 *	  definition of the system "index" relation (pg_index)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_index.h,v 1.7 1998/09/01 03:27:51 momjian Exp $
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
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_index definition.  cpp turns this into
 *		typedef struct FormData_pg_index.  The oid of the index relation
 *		is stored in indexrelid; the oid of the indexed relation is stored
 *		in indrelid.
 * ----------------
 */

/*
 * it seems that all variable length fields should go at the _end_,
 * because the system cache routines only copy the fields up to the
 * first variable length field.  so I moved indislossy, indhaskeytype,
 * and indisunique before indpred.	--djm 8/20/96
 */
CATALOG(pg_index)
{
	Oid			indexrelid;
	Oid			indrelid;
	Oid			indproc;		/* registered procedure for functional
								 * index */
	int28		indkey;
	oid8		indclass;
	bool		indisclustered;
	bool		indislossy;		/* do we fetch false tuples (lossy
								 * compression)? */
	bool		indhaskeytype;	/* does key type != attribute type? */
	bool		indisunique;	/* is this a unique index? */
	text		indpred;		/* query plan for partial index predicate */
} FormData_pg_index;

#define INDEX_MAX_KEYS 8		/* maximum number of keys in an index
								 * definition */

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
#define Anum_pg_index_indislossy		7
#define Anum_pg_index_indhaskeytype		8
#define Anum_pg_index_indisunique		8
#define Anum_pg_index_indpred			10

#endif							/* PG_INDEX_H */
