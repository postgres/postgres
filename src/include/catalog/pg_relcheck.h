/*-------------------------------------------------------------------------
 *
 * pg_relcheck.h
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RELCHECK_H
#define PG_RELCHECK_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_relcheck definition.  cpp turns this into
 *		typedef struct FormData_pg_relcheck
 * ----------------
 */
CATALOG(pg_relcheck) BOOTSTRAP
{
	Oid			rcrelid;
	NameData	rcname;
	text		rcbin;
	text		rcsrc;
} FormData_pg_relcheck;

/* ----------------
 *		Form_pg_relcheck corresponds to a pointer to a tuple with
 *		the format of pg_relcheck relation.
 * ----------------
 */
typedef FormData_pg_relcheck *Form_pg_relcheck;

/* ----------------
 *		compiler constants for pg_relcheck
 * ----------------
 */
#define Natts_pg_relcheck				4
#define Anum_pg_relcheck_rcrelid		1
#define Anum_pg_relcheck_rcname			2
#define Anum_pg_relcheck_rcbin			3
#define Anum_pg_relcheck_rcsrc			4

#endif	 /* PG_RELCHECK_H */
