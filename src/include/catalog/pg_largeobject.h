/*-------------------------------------------------------------------------
 *
 * pg_largeobject.h
 *	  definition of the system "largeobject" relation (pg_largeobject)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_largeobject.h,v 1.22 2008/01/01 19:45:56 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LARGEOBJECT_H
#define PG_LARGEOBJECT_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_largeobject definition.	cpp turns this into
 *		typedef struct FormData_pg_largeobject
 * ----------------
 */
#define LargeObjectRelationId  2613

CATALOG(pg_largeobject,2613) BKI_WITHOUT_OIDS
{
	Oid			loid;			/* Identifier of large object */
	int4		pageno;			/* Page number (starting from 0) */
	bytea		data;			/* Data for page (may be zero-length) */
} FormData_pg_largeobject;

/* ----------------
 *		Form_pg_largeobject corresponds to a pointer to a tuple with
 *		the format of pg_largeobject relation.
 * ----------------
 */
typedef FormData_pg_largeobject *Form_pg_largeobject;

/* ----------------
 *		compiler constants for pg_largeobject
 * ----------------
 */
#define Natts_pg_largeobject			3
#define Anum_pg_largeobject_loid		1
#define Anum_pg_largeobject_pageno		2
#define Anum_pg_largeobject_data		3

extern void LargeObjectCreate(Oid loid);
extern void LargeObjectDrop(Oid loid);
extern bool LargeObjectExists(Oid loid);

#endif   /* PG_LARGEOBJECT_H */
