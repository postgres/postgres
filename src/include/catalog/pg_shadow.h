/*-------------------------------------------------------------------------
 *
 * pg_shadow.h
 *	  definition of the system "shadow" relation (pg_shadow)
 *	  along with the relation's initial contents.
 *		  pg_user is now a public accessible view on pg_shadow.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_shadow.h,v 1.8 2000/06/09 15:51:00 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *		  WHENEVER the definition for pg_shadow changes, the
 *		  view creation of pg_user must be changed in initdb.sh!
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHADOW_H
#define PG_SHADOW_H


/* Prototype required for superuser() from superuser.c */

bool		superuser(void);

/* ----------------
 *		pg_shadow definition.  cpp turns this into
 *		typedef struct FormData_pg_shadow
 * ----------------
 */
CATALOG(pg_shadow) BOOTSTRAP
{
	NameData	usename;
	int4		usesysid;
	bool		usecreatedb;
	bool		usecreatetable;
	bool		uselocktable;
	bool		usetrace;
	bool		usesuper;
	bool		usecatupd;
	text		passwd;
	int4		valuntil;
} FormData_pg_shadow;

/* ----------------
 *		Form_pg_shadow corresponds to a pointer to a tuple with
 *		the format of pg_shadow relation.
 * ----------------
 */
typedef FormData_pg_shadow *Form_pg_shadow;

/* ----------------
 *		compiler constants for pg_shadow
 * ----------------
 */
#define Natts_pg_shadow				10
#define Anum_pg_shadow_usename			1
#define Anum_pg_shadow_usesysid			2
#define Anum_pg_shadow_usecreatedb		3
#define Anum_pg_shadow_usecreatetable		4
#define Anum_pg_shadow_uselocktable		5
#define Anum_pg_shadow_usetrace			6
#define Anum_pg_shadow_usesuper			7
#define Anum_pg_shadow_usecatupd		8
#define Anum_pg_shadow_passwd			9
#define Anum_pg_shadow_valuntil			10

/* ----------------
 *		initial contents of pg_shadow
 *
 * The uppercase quantities will be replaced at initdb time with
 * user choices.
 * ----------------
 */
DATA(insert OID = 0 ( POSTGRES PGUID t t t t t t _null_ _null_ ));

#endif	 /* PG_SHADOW_H */
