/*-------------------------------------------------------------------------
 *
 * pg_magic.h--
 *	  definition of the system "magic" relation (pg_magic)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_magic.h,v 1.3 1997/09/07 04:56:57 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_MAGIC_H
#define PG_MAGIC_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_magic definition.  cpp turns this into
 *		typedef struct FormData_pg_magic
 * ----------------
 */
CATALOG(pg_magic) BOOTSTRAP
{
	NameData		magname;
	NameData		magvalue;
} FormData_pg_magic;

/* ----------------
 *		Form_pg_magic corresponds to a pointer to a tuple with
 *		the format of pg_magic relation.
 * ----------------
 */
typedef FormData_pg_magic *Form_pg_magic;

/* ----------------
 *		compiler constants for pg_magic
 * ----------------
 */
#define Natts_pg_magic					2
#define Anum_pg_magic_magname			1
#define Anum_pg_magic_magvalue			2

#endif							/* PG_MAGIC_H */
