/*-------------------------------------------------------------------------
 *
 * pg_defaults.h--
 *	 definition of the system "defaults" relation (pg_defaults)
 *	 along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_defaults.h,v 1.3 1997/09/07 04:56:44 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DEFAULTS_H
#define PG_DEFAULTS_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_defaults definition.  cpp turns this into
 *		typedef struct FormData_pg_defaults
 * ----------------
 */
CATALOG(pg_defaults) BOOTSTRAP
{
	NameData		defname;
	NameData		defvalue;
} FormData_pg_defaults;

/* ----------------
 *		Form_pg_defaults corresponds to a pointer to a tuple with
 *		the format of pg_defaults relation.
 * ----------------
 */
typedef FormData_pg_defaults *Form_pg_defaults;

/* ----------------
 *		compiler constants for pg_defaults
 * ----------------
 */
#define Natts_pg_defaults				2
#define Anum_pg_defaults_defname		1
#define Anum_pg_defaults_defvalue		2


#endif							/* PG_DEFAULTS_H */
