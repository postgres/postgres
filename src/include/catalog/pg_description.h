/*-------------------------------------------------------------------------
 *
 * pg_description.h
 *	  definition of the system "description" relation (pg_description)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_description.h,v 1.8 1999/02/13 23:21:09 momjian Exp $
 *
 * NOTES
 *		the genbki.sh script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DESCRIPTION_H
#define PG_DESCRIPTION_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_description definition.	cpp turns this into
 *		typedef struct FormData_pg_description
 * ----------------
 */
CATALOG(pg_description)
{
	Oid			objoid;
	text		description;
} FormData_pg_description;

/* ----------------
 *		Form_pg_description corresponds to a pointer to a tuple with
 *		the format of pg_description relation.
 * ----------------
 */
typedef FormData_pg_description *Form_pg_description;

/* ----------------
 *		compiler constants for pg_descrpition
 * ----------------
 */
#define Natts_pg_description			2
#define Anum_pg_description_objoid		1
#define Anum_pg_description_description 2

/* ----------------
 *		initial contents of pg_description
 * ----------------
 */

/*
 *	Because the contents of this table are taken from the other *.h files,
 *	there is no initialization. It is loaded from initdb using a COPY
 *	statement.
 */

#endif	 /* PG_DESCRIPTION_H */
