/*-------------------------------------------------------------------------
 *
 * pg_default_acl.h
 *	  definition of default ACLs for new objects.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_default_acl.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DEFAULT_ACL_H
#define PG_DEFAULT_ACL_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_default_acl definition.  cpp turns this into
 *		typedef struct FormData_pg_default_acl
 * ----------------
 */
#define DefaultAclRelationId	826

CATALOG(pg_default_acl,826)
{
	Oid			defaclrole;		/* OID of role owning this ACL */
	Oid			defaclnamespace;	/* OID of namespace, or 0 for all */
	char		defaclobjtype;	/* see DEFACLOBJ_xxx constants below */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		defaclacl[1];	/* permissions to add at CREATE time */
#endif
} FormData_pg_default_acl;

/* ----------------
 *		Form_pg_default_acl corresponds to a pointer to a tuple with
 *		the format of pg_default_acl relation.
 * ----------------
 */
typedef FormData_pg_default_acl *Form_pg_default_acl;

/* ----------------
 *		compiler constants for pg_default_acl
 * ----------------
 */

#define Natts_pg_default_acl					4
#define Anum_pg_default_acl_defaclrole			1
#define Anum_pg_default_acl_defaclnamespace		2
#define Anum_pg_default_acl_defaclobjtype		3
#define Anum_pg_default_acl_defaclacl			4

/* ----------------
 *		pg_default_acl has no initial contents
 * ----------------
 */

/*
 * Types of objects for which the user is allowed to specify default
 * permissions through pg_default_acl.  These codes are used in the
 * defaclobjtype column.
 */
#define DEFACLOBJ_RELATION		'r'		/* table, view */
#define DEFACLOBJ_SEQUENCE		'S'		/* sequence */
#define DEFACLOBJ_FUNCTION		'f'		/* function */
#define DEFACLOBJ_TYPE			'T'		/* type */

#endif   /* PG_DEFAULT_ACL_H */
