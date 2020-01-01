/*-------------------------------------------------------------------------
 *
 * pg_shdescription.h
 *	  definition of the "shared description" system catalog
 *	  (pg_shdescription)
 *
 * Because the contents of this table are taken from the *.dat files
 * of other catalogs, there is no pg_shdescription.dat file. The initial
 * contents are assembled by genbki.pl and loaded during initdb.
 *
 * NOTE: an object is identified by the OID of the row that primarily
 * defines the object, plus the OID of the table that that row appears in.
 * For example, a database is identified by the OID of its pg_database row
 * plus the pg_class OID of table pg_database.  This allows unique
 * identification of objects without assuming that OIDs are unique
 * across tables.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_shdescription.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHDESCRIPTION_H
#define PG_SHDESCRIPTION_H

#include "catalog/genbki.h"
#include "catalog/pg_shdescription_d.h"

/* ----------------
 *		pg_shdescription definition.    cpp turns this into
 *		typedef struct FormData_pg_shdescription
 * ----------------
 */
CATALOG(pg_shdescription,2396,SharedDescriptionRelationId) BKI_SHARED_RELATION
{
	Oid			objoid;			/* OID of object itself */
	Oid			classoid;		/* OID of table containing object */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		description BKI_FORCE_NOT_NULL; /* description of object */
#endif
} FormData_pg_shdescription;

/* ----------------
 *		Form_pg_shdescription corresponds to a pointer to a tuple with
 *		the format of pg_shdescription relation.
 * ----------------
 */
typedef FormData_pg_shdescription * Form_pg_shdescription;

#endif							/* PG_SHDESCRIPTION_H */
