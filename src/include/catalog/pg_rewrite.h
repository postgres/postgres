/*-------------------------------------------------------------------------
 *
 * pg_rewrite.h
 *	  definition of the system "rewrite-rule" relation (pg_rewrite)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_rewrite.h,v 1.13 2001/01/24 19:43:22 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWRITE_H
#define PG_REWRITE_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_rewrite definition.	cpp turns this into
 *		typedef struct FormData_pg_rewrite
 * ----------------
 */
CATALOG(pg_rewrite)
{
	NameData	rulename;
	char		ev_type;
	Oid			ev_class;
	int2		ev_attr;
	bool		is_instead;
	/* NB: remaining fields must be accessed via heap_getattr */
	text		ev_qual;
	text		ev_action;
} FormData_pg_rewrite;

/* ----------------
 *		Form_pg_rewrite corresponds to a pointer to a tuple with
 *		the format of pg_rewrite relation.
 * ----------------
 */
typedef FormData_pg_rewrite *Form_pg_rewrite;

/* ----------------
 *		compiler constants for pg_rewrite
 * ----------------
 */
#define Natts_pg_rewrite				7
#define Anum_pg_rewrite_rulename		1
#define Anum_pg_rewrite_ev_type			2
#define Anum_pg_rewrite_ev_class		3
#define Anum_pg_rewrite_ev_attr			4
#define Anum_pg_rewrite_is_instead		5
#define Anum_pg_rewrite_ev_qual			6
#define Anum_pg_rewrite_ev_action		7

#endif	 /* PG_REWRITE_H */
