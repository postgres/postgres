/*-------------------------------------------------------------------------
 *
 * pgpa_planner.h
 *	  planner integration for pg_plan_advice
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_PLANNER_H
#define PGPA_PLANNER_H

#include "pgpa_identifier.h"

extern void pgpa_planner_install_hooks(void);

/*
 * Per-PlannerInfo information that we gather during query planning.
 */
typedef struct pgpa_planner_info
{
	/* Plan name taken from the corresponding PlannerInfo; NULL at top level. */
	char	   *plan_name;

	/*
	 * If the corresponding PlannerInfo has an alternative_root, then this is
	 * the plan name from that PlannerInfo; otherwise, it is the same as
	 * plan_name.
	 *
	 * is_alternative_plan is set to true for every pgpa_planner_info that
	 * shares an alternative_plan_name with at least one other, and to false
	 * otherwise.
	 */
	char	   *alternative_plan_name;
	bool		is_alternative_plan;

	/* Relation identifiers computed for baserels at this query level. */
	pgpa_identifier *rid_array;
	int			rid_array_size;

	/*
	 * If has_rtoffset is true, then rtoffset is the offset required to align
	 * RTIs for this query level with RTIs from the final, flattened
	 * rangetable. If has_rtoffset is false, then this subquery's range table
	 * wasn't copied, or was only partially copied, into the final range
	 * table. (Note that we can't determine the rtoffset values until the
	 * final range table actually exists; before that time, has_rtoffset will
	 * be false everywhere except at the top level.)
	 */
	bool		has_rtoffset;
	Index		rtoffset;

	/*
	 * List of Bitmapset objects. Each represents the relid set of a relation
	 * that the planner considers making unique during semijoin planning.
	 *
	 * When generating advice, we should emit either SEMIJOIN_UNIQUE advice or
	 * SEMIJOIN_NON_UNIQUE advice for each semijoin depending on whether we
	 * chose to implement it as a semijoin or whether we instead chose to make
	 * the nullable side unique and then perform an inner join. When the
	 * make-unique strategy is not chosen, it's not easy to tell from the
	 * final plan tree whether it was considered. That's awkward, because we
	 * don't want to emit useless SEMIJOIN_NON_UNIQUE advice when there was no
	 * decision to be made. This list lets the plan tree walker know in which
	 * cases that approach was considered, so that it doesn't have to guess.
	 */
	List	   *sj_unique_rels;
} pgpa_planner_info;

/*
 * When set to a value greater than zero, indicates that advice should be
 * generated during query planning even in the absence of obvious reasons to
 * do so. See pg_plan_advice_request_advice_generation().
 */
extern int	pgpa_planner_generate_advice;

/* Must be exported for use by test_plan_advice */
extern PGDLLEXPORT void pgpa_planner_feedback_warning(List *feedback);

#endif
