/*-------------------------------------------------------------------------
 *
 * pg_shadow.h
 *	  definition of the system "shadow" relation (pg_shadow)
 *	  along with the relation's initial contents.
 *
 *	  pg_user is now a publicly accessible view on pg_shadow.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_shadow.h,v 1.28 2005/04/14 01:38:21 tgl Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
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
#define ShadowRelationId  1260

CATALOG(pg_shadow,1260) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
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
#define Natts_pg_shadow					8
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
