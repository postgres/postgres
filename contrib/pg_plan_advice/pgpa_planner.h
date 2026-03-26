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

#ifdef USE_ASSERT_CHECKING
	/* Relation identifiers computed for baserels at this query level. */
	pgpa_identifier *rid_array;
	int			rid_array_size;
#endif

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

#endif
