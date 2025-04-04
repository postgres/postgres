/*-------------------------------------------------------------------------
 *
 * pg_default_acl.h
 *	  definition of the system catalog for default ACLs of new objects
 *	  (pg_default_acl)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_default_acl.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DEFAULT_ACL_H
#define PG_DEFAULT_ACL_H

#include "catalog/genbki.h"
#include "catalog/pg_default_acl_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_default_acl definition.  cpp turns this into
 *		typedef struct FormData_pg_default_acl
 * ----------------
 */
CATALOG(pg_default_acl,826,DefaultAclRelationId)
{
	Oid			oid;			/* oid */
	Oid			defaclrole BKI_LOOKUP(pg_authid);	/* OID of role owning this
													 * ACL */
	Oid			defaclnamespace BKI_LOOKUP_OPT(pg_namespace);	/* OID of namespace, or
																 * 0 for all */
	char		defaclobjtype;	/* see DEFACLOBJ_xxx constants below */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		defaclacl[1] BKI_FORCE_NOT_NULL;	/* permissions to add at
													 * CREATE time */
#endif
} FormData_pg_default_acl;

/* ----------------
 *		Form_pg_default_acl corresponds to a pointer to a tuple with
 *		the format of pg_default_acl relation.
 * ----------------
 */
typedef FormData_pg_default_acl *Form_pg_default_acl;

DECLARE_TOAST(pg_default_acl, 4143, 4144);

DECLARE_UNIQUE_INDEX(pg_default_acl_role_nsp_obj_index, 827, DefaultAclRoleNspObjIndexId, pg_default_acl, btree(defaclrole oid_ops, defaclnamespace oid_ops, defaclobjtype char_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_default_acl_oid_index, 828, DefaultAclOidIndexId, pg_default_acl, btree(oid oid_ops));

MAKE_SYSCACHE(DEFACLROLENSPOBJ, pg_default_acl_role_nsp_obj_index, 8);

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * Types of objects for which the user is allowed to specify default
 * permissions through pg_default_acl.  These codes are used in the
 * defaclobjtype column.
 */
#define DEFACLOBJ_RELATION		'r' /* table, view */
#define DEFACLOBJ_SEQUENCE		'S' /* sequence */
#define DEFACLOBJ_FUNCTION		'f' /* function */
#define DEFACLOBJ_TYPE			'T' /* type */
#define DEFACLOBJ_NAMESPACE		'n' /* namespace */
#define DEFACLOBJ_LARGEOBJECT	'L' /* large object */

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_DEFAULT_ACL_H */
