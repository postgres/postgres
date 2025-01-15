/*-------------------------------------------------------------------------
 *
 * pg_rewrite.h
 *	  definition of the "rewrite rule" system catalog (pg_rewrite)
 *
 * As of Postgres 7.3, the primary key for this table is <ev_class, rulename>
 * --- ie, rule names are only unique among the rules of a given table.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_rewrite.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWRITE_H
#define PG_REWRITE_H

#include "catalog/genbki.h"
#include "catalog/pg_rewrite_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_rewrite definition.  cpp turns this into
 *		typedef struct FormData_pg_rewrite
 * ----------------
 */
CATALOG(pg_rewrite,2618,RewriteRelationId)
{
	Oid			oid;			/* oid */
	NameData	rulename;
	Oid			ev_class BKI_LOOKUP(pg_class);
	char		ev_type;
	char		ev_enabled;
	bool		is_instead;

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	pg_node_tree ev_qual BKI_FORCE_NOT_NULL;
	pg_node_tree ev_action BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_rewrite;

/* ----------------
 *		Form_pg_rewrite corresponds to a pointer to a tuple with
 *		the format of pg_rewrite relation.
 * ----------------
 */
typedef FormData_pg_rewrite *Form_pg_rewrite;

DECLARE_TOAST(pg_rewrite, 2838, 2839);

DECLARE_UNIQUE_INDEX_PKEY(pg_rewrite_oid_index, 2692, RewriteOidIndexId, pg_rewrite, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_rewrite_rel_rulename_index, 2693, RewriteRelRulenameIndexId, pg_rewrite, btree(ev_class oid_ops, rulename name_ops));

MAKE_SYSCACHE(RULERELNAME, pg_rewrite_rel_rulename_index, 8);

#endif							/* PG_REWRITE_H */
