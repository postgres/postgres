/*-------------------------------------------------------------------------
 *
 * pg_largeobject.h
 *	  definition of the system "largeobject" relation (pg_largeobject)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_largeobject.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LARGEOBJECT_H
#define PG_LARGEOBJECT_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_largeobject definition.	cpp turns this into
 *		typedef struct FormData_pg_largeobject
 * ----------------
 */
#define LargeObjectRelationId  2613

CATALOG(pg_largeobject,2613) BKI_WITHOUT_OIDS
{
	Oid			loid;			/* Identifier of large object */
	int32		pageno;			/* Page number (starting from 0) */

	/* data has variable length, but we allow direct access; see inv_api.c */
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

extern Oid	LargeObjectCreate(Oid loid);
extern void LargeObjectDrop(Oid loid);
extern void LargeObjectAlterOwner(Oid loid, Oid newOwnerId);
extern bool LargeObjectExists(Oid loid);

#endif   /* PG_LARGEOBJECT_H */
