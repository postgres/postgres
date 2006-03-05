/*-------------------------------------------------------------------------
 *
 * pg_inherits.h
 *	  definition of the system "inherits" relation (pg_inherits)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_inherits.h,v 1.22 2006/03/05 15:58:54 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INHERITS_H
#define PG_INHERITS_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_inherits definition.  cpp turns this into
 *		typedef struct FormData_pg_inherits
 * ----------------
 */
#define InheritsRelationId	2611

CATALOG(pg_inherits,2611) BKI_WITHOUT_OIDS
{
	Oid			inhrelid;
	Oid			inhparent;
	int4		inhseqno;
} FormData_pg_inherits;

/* ----------------
 *		Form_pg_inherits corresponds to a pointer to a tuple with
 *		the format of pg_inherits relation.
 * ----------------
 */
typedef FormData_pg_inherits *Form_pg_inherits;

/* ----------------
 *		compiler constants for pg_inherits
 * ----------------
 */
#define Natts_pg_inherits				3
#define Anum_pg_inherits_inhrelid		1
#define Anum_pg_inherits_inhparent		2
#define Anum_pg_inherits_inhseqno		3

#endif   /* PG_INHERITS_H */
