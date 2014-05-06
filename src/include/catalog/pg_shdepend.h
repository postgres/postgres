/*-------------------------------------------------------------------------
 *
 * pg_shdepend.h
 *	  definition of the system "shared dependency" relation (pg_shdepend)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_shdepend.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHDEPEND_H
#define PG_SHDEPEND_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_shdepend definition.  cpp turns this into
 *		typedef struct FormData_pg_shdepend
 * ----------------
 */
#define SharedDependRelationId	1214

CATALOG(pg_shdepend,1214) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	/*
	 * Identification of the dependent (referencing) object.
	 *
	 * These fields are all zeroes for a DEPENDENCY_PIN entry.  Also, dbid can
	 * be zero to denote a shared object.
	 */
	Oid			dbid;			/* OID of database containing object */
	Oid			classid;		/* OID of table containing object */
	Oid			objid;			/* OID of object itself */
	int32		objsubid;		/* column number, or 0 if not used */

	/*
	 * Identification of the independent (referenced) object.  This is always
	 * a shared object, so we need no database ID field.  We don't bother with
	 * a sub-object ID either.
	 */
	Oid			refclassid;		/* OID of table containing object */
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

/* ----------------
 *		compiler constants for pg_shdepend
 * ----------------
 */
#define Natts_pg_shdepend			7
#define Anum_pg_shdepend_dbid		1
#define Anum_pg_shdepend_classid	2
#define Anum_pg_shdepend_objid		3
#define Anum_pg_shdepend_objsubid	4
#define Anum_pg_shdepend_refclassid 5
#define Anum_pg_shdepend_refobjid	6
#define Anum_pg_shdepend_deptype	7


/*
 * pg_shdepend has no preloaded contents; system-defined dependencies are
 * loaded into it during a late stage of the initdb process.
 *
 * NOTE: we do not represent all possible dependency pairs in pg_shdepend;
 * for example, there's not much value in creating an explicit dependency
 * from a relation to its database.  Currently, only dependencies on roles
 * are explicitly stored in pg_shdepend.
 */

#endif   /* PG_SHDEPEND_H */
