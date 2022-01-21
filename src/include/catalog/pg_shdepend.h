/*-------------------------------------------------------------------------
 *
 * pg_shdepend.h
 *	  definition of the "shared dependency" system catalog (pg_shdepend)
 *
 * pg_shdepend has no preloaded contents, so there is no pg_shdepend.dat
 * file; dependencies for system-defined objects are loaded into it
 * on-the-fly during initdb.  Most built-in objects are pinned anyway,
 * and hence need no explicit entries in pg_shdepend.
 *
 * NOTE: we do not represent all possible dependency pairs in pg_shdepend;
 * for example, there's not much value in creating an explicit dependency
 * from a relation to its database.  Currently, only dependencies on roles
 * are explicitly stored in pg_shdepend.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_shdepend.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHDEPEND_H
#define PG_SHDEPEND_H

#include "catalog/genbki.h"
#include "catalog/pg_shdepend_d.h"

/* ----------------
 *		pg_shdepend definition.  cpp turns this into
 *		typedef struct FormData_pg_shdepend
 * ----------------
 */
CATALOG(pg_shdepend,1214,SharedDependRelationId) BKI_SHARED_RELATION
{
	/*
	 * Identification of the dependent (referencing) object.
	 *
	 * Note that dbid can be zero to denote a shared object.
	 */
	Oid			dbid BKI_LOOKUP_OPT(pg_database);	/* OID of database
													 * containing object */
	Oid			classid BKI_LOOKUP(pg_class);	/* OID of table containing
												 * object */
	Oid			objid;			/* OID of object itself */
	int32		objsubid;		/* column number, or 0 if not used */

	/*
	 * Identification of the independent (referenced) object.  This is always
	 * a shared object, so we need no database ID field.  We don't bother with
	 * a sub-object ID either.
	 */
	Oid			refclassid BKI_LOOKUP(pg_class);	/* OID of table containing
													 * object */
	Oid			refobjid;		/* OID of object itself */

	/*
	 * Precise semantics of the relationship are specified by the deptype
	 * field.  See SharedDependencyType in catalog/dependency.h.
	 */
	char		deptype;		/* see codes in dependency.h */
} FormData_pg_shdepend;

/* ----------------
 *		Form_pg_shdepend corresponds to a pointer to a row with
 *		the format of pg_shdepend relation.
 * ----------------
 */
typedef FormData_pg_shdepend *Form_pg_shdepend;

DECLARE_INDEX(pg_shdepend_depender_index, 1232, SharedDependDependerIndexId, on pg_shdepend using btree(dbid oid_ops, classid oid_ops, objid oid_ops, objsubid int4_ops));
DECLARE_INDEX(pg_shdepend_reference_index, 1233, SharedDependReferenceIndexId, on pg_shdepend using btree(refclassid oid_ops, refobjid oid_ops));

#endif							/* PG_SHDEPEND_H */
