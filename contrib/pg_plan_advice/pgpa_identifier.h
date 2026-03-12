/*-------------------------------------------------------------------------
 *
 * pgpa_identifier.h
 *	  create appropriate identifiers for range table entries
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_identifier.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGPA_IDENTIFIER_H
#define PGPA_IDENTIFIER_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

typedef struct pgpa_identifier
{
	const char *alias_name;
	int			occurrence;
	const char *partnsp;
	const char *partrel;
	const char *plan_name;
} pgpa_identifier;

/* Convenience function for comparing possibly-NULL strings. */
static inline bool
strings_equal_or_both_null(const char *a, const char *b)
{
	if (a == b)
		return true;
	else if (a == NULL || b == NULL)
		return false;
	else
		return strcmp(a, b) == 0;
}

extern const char *pgpa_identifier_string(const pgpa_identifier *rid);
extern void pgpa_compute_identifier_by_rti(PlannerInfo *root, Index rti,
										   pgpa_identifier *rid);
extern int	pgpa_compute_identifiers_by_relids(PlannerInfo *root,
											   Bitmapset *relids,
											   pgpa_identifier *rids);
extern pgpa_identifier *pgpa_create_identifiers_for_planned_stmt(PlannedStmt *pstmt);

extern Index pgpa_compute_rti_from_identifier(int rtable_length,
											  pgpa_identifier *rt_identifiers,
											  pgpa_identifier *rid);

#endif
