/*-------------------------------------------------------------------------
 *
 * pg_attrdef.h
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ATTRDEF_H
#define PG_ATTRDEF_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_attrdef definition.	cpp turns this into
 *		typedef struct FormData_pg_attrdef
 * ----------------
 */
CATALOG(pg_attrdef) BOOTSTRAP
{
	Oid			adrelid;
	int2		adnum;
	text		adbin;
	text		adsrc;
} FormData_pg_attrdef;

/* ----------------
 *		Form_pg_attrdef corresponds to a pointer to a tuple with
 *		the format of pg_attrdef relation.
 * ----------------
 */
typedef FormData_pg_attrdef *Form_pg_attrdef;

/* ----------------
 *		compiler constants for pg_attrdef
 * ----------------
 */
#define Natts_pg_attrdef				4
#define Anum_pg_attrdef_adrelid			1
#define Anum_pg_attrdef_adnum			2
#define Anum_pg_attrdef_adbin			3
#define Anum_pg_attrdef_adsrc			4


#endif	 /* PG_ATTRDEF_H */
