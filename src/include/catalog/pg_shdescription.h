/*-------------------------------------------------------------------------
 *
 * pg_shdescription.h
 *	  definition of the system "shared description" relation
 *	  (pg_shdescription)
 *
 * NOTE: an object is identified by the OID of the row that primarily
 * defines the object, plus the OID of the table that that row appears in.
 * For example, a database is identified by the OID of its pg_database row
 * plus the pg_class OID of table pg_database.	This allows unique
 * identification of objects without assuming that OIDs are unique
 * across tables.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_shdescription.h
 *
 * NOTES
 *		the genbki.pl script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHDESCRIPTION_H
#define PG_SHDESCRIPTION_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_shdescription definition.	cpp turns this into
 *		typedef struct FormData_pg_shdescription
 * ----------------
 */
#define SharedDescriptionRelationId  2396

CATALOG(pg_shdescription,2396) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	Oid			objoid;			/* OID of object itself */
	Oid			classoid;		/* OID of table containing object */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		description;	/* description of object */
#endif
} FormData_pg_shdescription;

/* ----------------
 *		Form_pg_shdescription corresponds to a pointer to a tuple with
 *		the format of pg_shdescription relation.
 * ----------------
 */
typedef FormData_pg_shdescription *Form_pg_shdescription;

/* ----------------
 *		compiler constants for pg_shdescription
 * ----------------
 */
#define Natts_pg_shdescription			3
#define Anum_pg_shdescription_objoid		1
#define Anum_pg_shdescription_classoid	2
#define Anum_pg_shdescription_description 3

/* ----------------
 *		initial contents of pg_shdescription
 * ----------------
 */

/*
 *	Because the contents of this table are taken from the other *.h files,
 *	there is no initialization here.  The initial contents are extracted
 *	by genbki.pl and loaded during initdb.
 */

#endif   /* PG_SHDESCRIPTION_H */
