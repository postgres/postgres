/*-------------------------------------------------------------------------
 *
 * pg_shadow.h
 *	  definition of the system "shadow" relation (pg_shadow)
 *	  along with the relation's initial contents.
 *		  pg_user is now a public accessible view on pg_shadow.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_shadow.h,v 1.24 2003/08/04 02:40:12 momjian Exp $
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


/* ----------------
 *		pg_shadow definition.  cpp turns this into
 *		typedef struct FormData_pg_shadow
 * ----------------
 */
CATALOG(pg_shadow) BOOTSTRAP BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	NameData	usename;
	int4		usesysid;
	bool		usecreatedb;
	bool		usesuper;		/* read this field via superuser() only */
	bool		usecatupd;

	/* remaining fields may be null; use heap_getattr to read them! */
	text		passwd;
	int4		valuntil;		/* actually abstime */
	text		useconfig[1];
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
#define Anum_pg_shadow_usesuper			4
#define Anum_pg_shadow_usecatupd		5
#define Anum_pg_shadow_passwd			6
#define Anum_pg_shadow_valuntil			7
#define Anum_pg_shadow_useconfig		8

/* ----------------
 *		initial contents of pg_shadow
 *
 * The uppercase quantities will be replaced at initdb time with
 * user choices.
 * ----------------
 */
DATA(insert ( "POSTGRES" PGUID t t t _null_ _null_ _null_ ));

#define BOOTSTRAP_USESYSID 1

#endif   /* PG_SHADOW_H */
