/*-------------------------------------------------------------------------
 *
 * pg_description.h
 *	  definition of the "description" system catalog (pg_description)
 *
 * Because the contents of this table are taken from the *.dat files
 * of other catalogs, there is no pg_description.dat file. The initial
 * contents are assembled by genbki.pl and loaded during initdb.
 *
 * NOTE: an object is identified by the OID of the row that primarily
 * defines the object, plus the OID of the table that that row appears in.
 * For example, a function is identified by the OID of its pg_proc row
 * plus the pg_class OID of table pg_proc.  This allows unique identification
 * of objects without assuming that OIDs are unique across tables.
 *
 * Since attributes don't have OIDs of their own, we identify an attribute
 * comment by the objoid+classoid of its parent table, plus an "objsubid"
 * giving the attribute column number.  "objsubid" must be zero in a comment
 * for a table itself, so that it is distinct from any column comment.
 * Currently, objsubid is unused and zero for all other kinds of objects,
 * but perhaps it might be useful someday to associate comments with
 * constituent elements of other kinds of objects (arguments of a function,
 * for example).
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_description.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DESCRIPTION_H
#define PG_DESCRIPTION_H

#include "catalog/genbki.h"
#include "catalog/pg_description_d.h"

/* ----------------
 *		pg_description definition.  cpp turns this into
 *		typedef struct FormData_pg_description
 * ----------------
 */
CATALOG(pg_description,2609,DescriptionRelationId)
{
	Oid			objoid;			/* OID of object itself */
	Oid			classoid;		/* OID of table containing object */
	int32		objsubid;		/* column number, or 0 if not used */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		description BKI_FORCE_NOT_NULL; /* description of object */
#endif
} FormData_pg_description;

/* ----------------
 *		Form_pg_description corresponds to a pointer to a tuple with
 *		the format of pg_description relation.
 * ----------------
 */
typedef FormData_pg_description * Form_pg_description;

DECLARE_TOAST(pg_description, 2834, 2835);

DECLARE_UNIQUE_INDEX_PKEY(pg_description_o_c_o_index, 2675, on pg_description using btree(objoid oid_ops, classoid oid_ops, objsubid int4_ops));
#define DescriptionObjIndexId  2675

/* We do not use BKI_LOOKUP here because it causes problems for genbki.pl */
DECLARE_FOREIGN_KEY((classoid), pg_class, (oid));

#endif							/* PG_DESCRIPTION_H */
