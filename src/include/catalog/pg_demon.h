/*-------------------------------------------------------------------------
 *
 * pg_demon.h--
 *	 definition of the system "demon" relation (pg_demon)
 *	 along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_demon.h,v 1.4 1997/09/08 02:35:06 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DEMON_H
#define PG_DEMON_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_demon definition.  cpp turns this into
 *		typedef struct FormData_pg_demon
 * ----------------
 */
CATALOG(pg_demon) BOOTSTRAP
{
	Oid			demserid;
	NameData	demname;
	Oid			demowner;
	regproc		demcode;
} FormData_pg_demon;

/* ----------------
 *		Form_pg_demon corresponds to a pointer to a tuple with
 *		the format of pg_demon relation.
 * ----------------
 */
typedef FormData_pg_demon *Form_pg_demon;

/* ----------------
 *		compiler constants for pg_demon
 * ----------------
 */
#define Natts_pg_demon					4
#define Anum_pg_demon_demserid			1
#define Anum_pg_demon_demname			2
#define Anum_pg_demon_demowner			3
#define Anum_pg_demon_demcode			4

#endif							/* PG_DEMON_H */
