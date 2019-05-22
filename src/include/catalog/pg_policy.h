/*-------------------------------------------------------------------------
 *
 * pg_policy.h
 *	  definition of the "policy" system catalog (pg_policy)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_policy.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_POLICY_H
#define PG_POLICY_H

#include "catalog/genbki.h"
#include "catalog/pg_policy_d.h"

/* ----------------
 *		pg_policy definition. cpp turns this into
 *		typedef struct FormData_pg_policy
 * ----------------
 */
CATALOG(pg_policy,3256,PolicyRelationId)
{
	Oid			oid;			/* oid */
	NameData	polname;		/* Policy name. */
	Oid			polrelid;		/* Oid of the relation with policy. */
	char		polcmd;			/* One of ACL_*_CHR, or '*' for all */
	bool		polpermissive;	/* restrictive or permissive policy */

#ifdef CATALOG_VARLEN
	Oid			polroles[1] BKI_FORCE_NOT_NULL; /* Roles associated with
												 * policy */
	pg_node_tree polqual;		/* Policy quals. */
	pg_node_tree polwithcheck;	/* WITH CHECK quals. */
#endif
} FormData_pg_policy;

/* ----------------
 *		Form_pg_policy corresponds to a pointer to a row with
 *		the format of pg_policy relation.
 * ----------------
 */
typedef FormData_pg_policy *Form_pg_policy;

#endif							/* PG_POLICY_H */
