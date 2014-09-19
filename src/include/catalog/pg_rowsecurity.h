/*
 * pg_rowsecurity.h
 *   definition of the system catalog for row-security policy (pg_rowsecurity)
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 */
#ifndef PG_ROWSECURITY_H
#define PG_ROWSECURITY_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_rowsecurity definition. cpp turns this into
 *		typedef struct FormData_pg_rowsecurity
 * ----------------
 */
#define RowSecurityRelationId	3256

CATALOG(pg_rowsecurity,3256)
{
	NameData		rsecpolname;	/* Policy name. */
	Oid				rsecrelid;		/* Oid of the relation with policy. */
	char			rseccmd;		/* One of ACL_*_CHR, or \0 for all */

#ifdef CATALOG_VARLEN
	Oid				rsecroles[1]	/* Roles associated with policy, not-NULL */
	pg_node_tree	rsecqual;		/* Policy quals. */
	pg_node_tree	rsecwithcheck;	/* WITH CHECK quals. */
#endif
} FormData_pg_rowsecurity;

/* ----------------
 *		Form_pg_rowsecurity corresponds to a pointer to a row with
 *		the format of pg_rowsecurity relation.
 * ----------------
 */
typedef FormData_pg_rowsecurity *Form_pg_rowsecurity;

/* ----------------
 * 		compiler constants for pg_rowsecurity
 * ----------------
 */
#define Natts_pg_rowsecurity				6
#define Anum_pg_rowsecurity_rsecpolname		1
#define Anum_pg_rowsecurity_rsecrelid		2
#define Anum_pg_rowsecurity_rseccmd			3
#define Anum_pg_rowsecurity_rsecroles		4
#define Anum_pg_rowsecurity_rsecqual		5
#define Anum_pg_rowsecurity_rsecwithcheck	6

#endif  /* PG_ROWSECURITY_H */
