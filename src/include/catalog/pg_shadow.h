/*-------------------------------------------------------------------------
 *
 * pg_shadow.h
 *	  definition of the system "shadow" relation (pg_shadow)
 *	  along with the relation's initial contents.
 *		  pg_user is now a public accessible view on pg_shadow.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_shadow.h,v 1.4 1999/02/13 23:21:14 momjian Exp $
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
#define Natts_pg_shadow				8
#define Anum_pg_shadow_usename			1
#define Anum_pg_shadow_usesysid			2
#define Anum_pg_shadow_usecreatedb		3
#define Anum_pg_shadow_usetrace			4
#define Anum_pg_shadow_usesuper			5
#define Anum_pg_shadow_usecatupd		6
#define Anum_pg_shadow_passwd			7
#define Anum_pg_shadow_valuntil			8

/* ----------------
 *		initial contents of pg_shadow
 * ----------------
 */
DATA(insert OID = 0 ( postgres PGUID t t t t _null_ 2116994400 ));

BKI_BEGIN
#ifdef ALLOW_PG_GROUP
BKI_END

DATA(insert OID = 0 ( mike 799 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( mao 1806 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( hellers 1089 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( joey 5209 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( jolly 5443 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( sunita 6559 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( paxson 3029 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( marc 2435 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( jiangwu 6124 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( aoki 2360 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( avi 31080 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( kristin 1123 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( andrew 5229 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( nobuko 5493 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( hartzell 6676 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( devine 6724 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( boris 6396 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( sklower 354 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( marcel 31113 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( ginger 3692 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( woodruff 31026 t t t t _null_ 2116994400 ));
DATA(insert OID = 0 ( searcher 8261 t t t t _null_ 2116994400 ));

BKI_BEGIN
#endif	 /* ALLOW_PG_GROUP */
BKI_END

#endif	 /* PG_SHADOW_H */
