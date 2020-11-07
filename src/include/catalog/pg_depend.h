/*-------------------------------------------------------------------------
 *
 * pg_depend.h
 *	  definition of the "dependency" system catalog (pg_depend)
 *
 * pg_depend has no preloaded contents, so there is no pg_depend.dat
 * file; system-defined dependencies are loaded into it during a late stage
 * of the initdb process.
 *
 * NOTE: we do not represent all possible dependency pairs in pg_depend;
 * for example, there's not much value in creating an explicit dependency
 * from an attribute to its relation.  Usually we make a dependency for
 * cases where the relationship is conditional rather than essential
 * (for example, not all triggers are dependent on constraints, but all
 * attributes are dependent on relations) or where the dependency is not
 * convenient to find from the contents of other catalogs.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_depend.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DEPEND_H
#define PG_DEPEND_H

#include "catalog/genbki.h"
#include "catalog/pg_depend_d.h"

/* ----------------
 *		pg_depend definition.  cpp turns this into
 *		typedef struct FormData_pg_depend
 * ----------------
 */
CATALOG(pg_depend,2608,DependRelationId)
{
	/*
	 * Identification of the dependent (referencing) object.
	 *
	 * These fields are all zeroes for a DEPENDENCY_PIN entry.
	 */
	Oid			classid;		/* OID of table containing object */
	Oid			objid;			/* OID of object itself */
	int32		objsubid;		/* column number, or 0 if not used */

	/*
	 * Identification of the independent (referenced) object.
	 */
	Oid			refclassid;		/* OID of table containing object */
	Oid			refobjid;		/* OID of object itself */
	int32		refobjsubid;	/* column number, or 0 if not used */

	/*
	 * Precise semantics of the relationship are specified by the deptype
	 * field.  See DependencyType in catalog/dependency.h.
	 */
	char		deptype;		/* see codes in dependency.h */
#ifdef CATALOG_VARLEN
	text		refobjversion;	/* version of referenced object */
#endif
} FormData_pg_depend;

/* ----------------
 *		Form_pg_depend corresponds to a pointer to a row with
 *		the format of pg_depend relation.
 * ----------------
 */
typedef FormData_pg_depend *Form_pg_depend;

DECLARE_TOAST(pg_depend, 8888, 8889);

DECLARE_INDEX(pg_depend_depender_index, 2673, on pg_depend using btree(classid oid_ops, objid oid_ops, objsubid int4_ops));
#define DependDependerIndexId  2673
DECLARE_INDEX(pg_depend_reference_index, 2674, on pg_depend using btree(refclassid oid_ops, refobjid oid_ops, refobjsubid int4_ops));
#define DependReferenceIndexId	2674

#endif							/* PG_DEPEND_H */
