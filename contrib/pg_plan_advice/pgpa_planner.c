/*-------------------------------------------------------------------------
 *
 * pgpa_planner.c
 *	  Use planner hooks to observe and modify planner behavior
 *
 * All interaction with the core planner happens here. Much of it has to
 * do with enforcing supplied advice, but we also need these hooks to
 * generate advice strings (though the heavy lifting in that case is
 * mostly done by pgpa_walker.c).
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_planner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_plan_advice.h"
#include "pgpa_identifier.h"
#include "pgpa_output.h"
#include "pgpa_planner.h"
#include "pgpa_trove.h"
#include "pgpa_walker.h"

#include "commands/defrem.h"
#include "common/hashfn_unstable.h"
#include "nodes/makefuncs.h"
#include "optimizer/extendplan.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

typedef enum pgpa_jo_outcome
{
	PGPA_JO_PERMITTED,			/* permit this join order */
	PGPA_JO_DENIED,				/* deny this join order */
	PGPA_JO_INDIFFERENT			/* do neither */
} pgpa_jo_outcome;

typedef struct pgpa_planner_state
{
	MemoryContext mcxt;
	bool		generate_advice_feedback;
	bool		generate_advice_string;
	pgpa_trove *trove;
	List	   *proots;
	pgpa_planner_info *last_proot;
} pgpa_planner_state;

typedef struct pgpa_join_state
{
	/* Most-recently-considered outer rel. */
	RelOptInfo *outerrel;

	/* Most-recently-considered inner rel. */
	RelOptInfo *innerrel;

	/*
	 * Array of relation identifiers for all members of this joinrel, with
	 * outerrel identifiers before innerrel identifiers.
	 */
	pgpa_identifier *rids;

	/* Number of outer rel identifiers. */
	int			outer_count;

	/* Number of inner rel identifiers. */
	int			inner_count;

	/*
	 * Trove lookup results.
	 *
	 * join_entries and rel_entries are arrays of entries, and join_indexes
	 * and rel_indexes are the integer offsets within those arrays of entries
	 * potentially relevant to us. The "join" fields correspond to a lookup
	 * using PGPA_TROVE_LOOKUP_JOIN and the "rel" fields to a lookup using
	 * PGPA_TROVE_LOOKUP_REL.
	 */
	pgpa_trove_entry *join_entries;
	Bitmapset  *join_indexes;
	pgpa_trove_entry *rel_entries;
	Bitmapset  *rel_indexes;
} pgpa_join_state;

/* Saved hook values */
static build_simple_rel_hook_type prev_build_simple_rel = NULL;
static join_path_setup_hook_type prev_join_path_setup = NULL;
static joinrel_setup_hook_type prev_joinrel_setup = NULL;
static planner_setup_hook_type prev_planner_setup = NULL;
static planner_shutdown_hook_type prev_planner_shutdown = NULL;

/* Other global variables */
int			pgpa_planner_generate_advice = 0;
static int	planner_extension_id = -1;

/* Function prototypes. */
static void pgpa_planner_setup(PlannerGlobal *glob, Query *parse,
							   const char *query_string,
							   int cursorOptions,
							   double *tuple_fraction,
							   ExplainState *es);
static void pgpa_planner_shutdown(PlannerGlobal *glob, Query *parse,
								  const char *query_string, PlannedStmt *pstmt);
static void pgpa_build_simple_rel(PlannerInfo *root,
								  RelOptInfo *rel,
								  RangeTblEntry *rte);
static void pgpa_joinrel_setup(PlannerInfo *root,
							   RelOptInfo *joinrel,
							   RelOptInfo *outerrel,
							   RelOptInfo *innerrel,
							   SpecialJoinInfo *sjinfo,
							   List *restrictlist);
static void pgpa_join_path_setup(PlannerInfo *root,
								 RelOptInfo *joinrel,
								 RelOptInfo *outerrel,
								 RelOptInfo *innerrel,
								 JoinType jointype,
								 JoinPathExtraData *extra);
static pgpa_join_state *pgpa_get_join_state(PlannerInfo *root,
											RelOptInfo *joinrel,
											RelOptInfo *outerrel,
											RelOptInfo *innerrel);
static void pgpa_planner_apply_joinrel_advice(uint64 *pgs_mask_p,
											  char *plan_name,
											  pgpa_join_state *pjs);
static void pgpa_planner_apply_join_path_advice(JoinType jointype,
												uint64 *pgs_mask_p,
												char *plan_name,
												pgpa_join_state *pjs);
static void pgpa_planner_apply_scan_advice(RelOptInfo *rel,
										   pgpa_trove_entry *scan_entries,
										   Bitmapset *scan_indexes,
										   pgpa_trove_entry *rel_entries,
										   Bitmapset *rel_indexes);
static uint64 pgpa_join_strategy_mask_from_advice_tag(pgpa_advice_tag_type tag);
static pgpa_jo_outcome pgpa_join_order_permits_join(int outer_count,
													int inner_count,
													pgpa_identifier *rids,
													pgpa_trove_entry *entry);
static bool pgpa_join_method_permits_join(int outer_count, int inner_count,
										  pgpa_identifier *rids,
										  pgpa_trove_entry *entry,
										  bool *restrict_method);
static bool pgpa_opaque_join_permits_join(int outer_count, int inner_count,
										  pgpa_identifier *rids,
										  pgpa_trove_entry *entry,
										  bool *restrict_method);
static bool pgpa_semijoin_permits_join(int outer_count, int inner_count,
									   pgpa_identifier *rids,
									   pgpa_trove_entry *entry,
									   bool outer_is_nullable,
									   bool *restrict_method);

static List *pgpa_planner_append_feedback(List *list, pgpa_trove *trove,
										  pgpa_trove_lookup_type type,
										  pgpa_identifier *rt_identifiers,
										  pgpa_plan_walker_context *walker);
static void pgpa_planner_feedback_warning(List *feedback);

static pgpa_planner_info *pgpa_planner_get_proot(pgpa_planner_state *pps,
												 PlannerInfo *root);

static inline void pgpa_ri_checker_save(pgpa_planner_state *pps,
										PlannerInfo *root,
										RelOptInfo *rel);
static void pgpa_ri_checker_validate(pgpa_planner_state *pps,
									 PlannedStmt *pstmt);

static char *pgpa_bms_to_cstring(Bitmapset *bms);
static const char *pgpa_jointype_to_cstring(JoinType jointype);

/*
 * Install planner-related hooks.
 */
void
pgpa_planner_install_hooks(void)
{
	planner_extension_id = GetPlannerExtensionId("pg_plan_advice");
	prev_planner_setup = planner_setup_hook;
	planner_setup_hook = pgpa_planner_setup;
	prev_planner_shutdown = planner_shutdown_hook;
	planner_shutdown_hook = pgpa_planner_shutdown;
	prev_build_simple_rel = build_simple_rel_hook;
	build_simple_rel_hook = pgpa_build_simple_rel;
	prev_joinrel_setup = joinrel_setup_hook;
	joinrel_setup_hook = pgpa_joinrel_setup;
	prev_join_path_setup = join_path_setup_hook;
	join_path_setup_hook = pgpa_join_path_setup;
}

/*
 * Carry out whatever setup work we need to do before planning.
 */
static void
pgpa_planner_setup(PlannerGlobal *glob, Query *parse, const char *query_string,
				   int cursorOptions, double *tuple_fraction,
				   ExplainState *es)
{
	pgpa_trove *trove = NULL;
	pgpa_planner_state *pps;
	char	   *supplied_advice;
	bool		generate_advice_feedback = false;
	bool		generate_advice_string = false;
	bool		needs_pps = false;

	/*
	 * Decide whether we need to generate an advice string. We must do this if
	 * the user has told us to do it categorically, or if another loadable
	 * module has requested it, or if the user has requested it using the
	 * EXPLAIN (PLAN_ADVICE) option.
	 */
	generate_advice_string = (pg_plan_advice_always_store_advice_details ||
							  pgpa_planner_generate_advice ||
							  pg_plan_advice_should_explain(es));
	if (generate_advice_string)
		needs_pps = true;

	/*
	 * If any advice was provided, build a trove of advice for use during
	 * planning.
	 */
	supplied_advice = pg_plan_advice_get_supplied_query_advice(glob, parse,
															   query_string,
															   cursorOptions,
															   es);
	if (supplied_advice != NULL && supplied_advice[0] != '\0')
	{
		List	   *advice_items;
		char	   *error;

		/*
		 * If the supplied advice string comes from pg_plan_advice.advice,
		 * parsing shouldn't fail here, because we must have previously parsed
		 * successfully in pg_plan_advice_advice_check_hook. However, it might
		 * also come from a hook registered via pg_plan_advice_add_advisor,
		 * and we can't be sure whether that's valid. (Plus, having an error
		 * check here seems like a good idea anyway, just for safety.)
		 */
		advice_items = pgpa_parse(supplied_advice, &error);
		if (error)
			ereport(WARNING,
					errmsg("could not parse supplied advice: %s", error));

		/*
		 * It's possible that the advice string was non-empty but contained no
		 * actual advice, e.g. it was all whitespace.
		 */
		if (advice_items != NIL)
		{
			trove = pgpa_build_trove(advice_items);
			needs_pps = true;

			/*
			 * If we know that we're running under EXPLAIN, or if the user has
			 * told us to always do the work, generate advice feedback.
			 */
			if (es != NULL || pg_plan_advice_feedback_warnings ||
				pg_plan_advice_always_store_advice_details)
				generate_advice_feedback = true;
		}
	}

#ifdef USE_ASSERT_CHECKING

	/*
	 * If asserts are enabled, always build a private state object for
	 * cross-checks.
	 */
	needs_pps = true;
#endif

	/*
	 * We only create and initialize a private state object if it's needed for
	 * some purpose. That could be (1) recording that we will need to generate
	 * an advice string, (2) storing a trove of supplied advice, or (3)
	 * facilitating debugging cross-checks when asserts are enabled.
	 *
	 * Currently, the active memory context should be one that will last for
	 * the entire duration of query planning, but if GEQO is in use, it's
	 * possible that some of our callbacks may be invoked later with
	 * CurrentMemoryContext set to some shorter-lived context. So, record the
	 * context that should be used for allocations that need to live as long
	 * as the pgpa_planner_state itself.
	 */
	if (needs_pps)
	{
		pps = palloc0_object(pgpa_planner_state);
		pps->mcxt = CurrentMemoryContext;
		pps->generate_advice_feedback = generate_advice_feedback;
		pps->generate_advice_string = generate_advice_string;
		pps->trove = trove;
		SetPlannerGlobalExtensionState(glob, planner_extension_id, pps);
	}

	/* Pass call to previous hook. */
	if (prev_planner_setup)
		(*prev_planner_setup) (glob, parse, query_string, cursorOptions,
							   tuple_fraction, es);
}

/*
 * Carry out whatever work we want to do after planning is complete.
 */
static void
pgpa_planner_shutdown(PlannerGlobal *glob, Query *parse,
					  const char *query_string, PlannedStmt *pstmt)
{
	pgpa_planner_state *pps;
	pgpa_trove *trove = NULL;
	pgpa_plan_walker_context walker = {0};	/* placate compiler */
	bool		generate_advice_feedback = false;
	bool		generate_advice_string = false;
	List	   *pgpa_items = NIL;
	pgpa_identifier *rt_identifiers = NULL;

	/* Fetch our private state, set up by pgpa_planner_setup(). */
	pps = GetPlannerGlobalExtensionState(glob, planner_extension_id);
	if (pps != NULL)
	{
		trove = pps->trove;
		generate_advice_feedback = pps->generate_advice_feedback;
		generate_advice_string = pps->generate_advice_string;
	}

	/*
	 * If we're trying to generate an advice string or if we're trying to
	 * provide advice feedback, then we will need to create range table
	 * identifiers.
	 */
	if (generate_advice_string || generate_advice_feedback)
	{
		pgpa_plan_walker(&walker, pstmt, pps->proots);
		rt_identifiers = pgpa_create_identifiers_for_planned_stmt(pstmt);
	}

	/* Generate the advice string, if we need to do so. */
	if (generate_advice_string)
	{
		char	   *advice_string;
		StringInfoData buf;

		/* Generate a textual advice string. */
		initStringInfo(&buf);
		pgpa_output_advice(&buf, &walker, rt_identifiers);
		advice_string = buf.data;

		/* Save the advice string in the final plan. */
		pgpa_items = lappend(pgpa_items,
							 makeDefElem("advice_string",
										 (Node *) makeString(advice_string),
										 -1));
	}

	/*
	 * If we're trying to provide advice feedback, then we will need to
	 * analyze how successful the advice was.
	 */
	if (generate_advice_feedback)
	{
		List	   *feedback = NIL;

		/*
		 * Inject a Node-tree representation of all the trove-entry flags into
		 * the PlannedStmt.
		 */
		feedback = pgpa_planner_append_feedback(feedback,
												trove,
												PGPA_TROVE_LOOKUP_SCAN,
												rt_identifiers, &walker);
		feedback = pgpa_planner_append_feedback(feedback,
												trove,
												PGPA_TROVE_LOOKUP_JOIN,
												rt_identifiers, &walker);
		feedback = pgpa_planner_append_feedback(feedback,
												trove,
												PGPA_TROVE_LOOKUP_REL,
												rt_identifiers, &walker);

		pgpa_items = lappend(pgpa_items, makeDefElem("feedback",
													 (Node *) feedback, -1));

		/* If we were asked to generate feedback warnings, do so. */
		if (pg_plan_advice_feedback_warnings)
			pgpa_planner_feedback_warning(feedback);
	}

	/* Push whatever data we're saving into the PlannedStmt. */
	if (pgpa_items != NIL)
		pstmt->extension_state =
			lappend(pstmt->extension_state,
					makeDefElem("pg_plan_advice", (Node *) pgpa_items, -1));

	/*
	 * If assertions are enabled, cross-check the generated range table
	 * identifiers.
	 */
	if (pps != NULL)
		pgpa_ri_checker_validate(pps, pstmt);

	/* Pass call to previous hook. */
	if (prev_planner_shutdown)
		(*prev_planner_shutdown) (glob, parse, query_string, pstmt);
}

/*
 * Hook function for build_simple_rel().
 *
 * We can apply scan advice at this point, and we also use this as an
 * opportunity to do range-table identifier cross-checking in assert-enabled
 * builds.
 */
static void
pgpa_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	pgpa_planner_state *pps;

	/* Fetch our private state, set up by pgpa_planner_setup(). */
	pps = GetPlannerGlobalExtensionState(root->glob, planner_extension_id);

	/* Save details needed for range table identifier cross-checking. */
	if (pps != NULL)
		pgpa_ri_checker_save(pps, root, rel);

	/* If query advice was provided, search for relevant entries. */
	if (pps != NULL && pps->trove != NULL)
	{
		pgpa_identifier rid;
		pgpa_trove_result tresult_scan;
		pgpa_trove_result tresult_rel;

		/* Search for scan advice and general rel advice. */
		pgpa_compute_identifier_by_rti(root, rel->relid, &rid);
		pgpa_trove_lookup(pps->trove, PGPA_TROVE_LOOKUP_SCAN, 1, &rid,
						  &tresult_scan);
		pgpa_trove_lookup(pps->trove, PGPA_TROVE_LOOKUP_REL, 1, &rid,
						  &tresult_rel);

		/* If relevant entries were found, apply them. */
		if (tresult_scan.indexes != NULL || tresult_rel.indexes != NULL)
		{
			uint64		original_mask = rel->pgs_mask;

			pgpa_planner_apply_scan_advice(rel,
										   tresult_scan.entries,
										   tresult_scan.indexes,
										   tresult_rel.entries,
										   tresult_rel.indexes);

			/* Emit debugging message, if enabled. */
			if (pg_plan_advice_trace_mask && original_mask != rel->pgs_mask)
			{
				if (root->plan_name != NULL)
					ereport(WARNING,
							(errmsg("strategy mask for RTI %u in subplan \"%s\" changed from 0x%" PRIx64 " to 0x%" PRIx64,
									rel->relid, root->plan_name,
									original_mask, rel->pgs_mask)));
				else
					ereport(WARNING,
							(errmsg("strategy mask for RTI %u changed from 0x%" PRIx64 " to 0x%" PRIx64,
									rel->relid, original_mask,
									rel->pgs_mask)));
			}
		}
	}

	/* Pass call to previous hook. */
	if (prev_build_simple_rel)
		(*prev_build_simple_rel) (root, rel, rte);
}

/*
 * Enforce any provided advice that is relevant to any method of implementing
 * this join.
 *
 * Although we're passed the outerrel and innerrel here, those are just
 * whatever values happened to prompt the creation of this joinrel; they
 * shouldn't really influence our choice of what advice to apply.
 */
static void
pgpa_joinrel_setup(PlannerInfo *root, RelOptInfo *joinrel,
				   RelOptInfo *outerrel, RelOptInfo *innerrel,
				   SpecialJoinInfo *sjinfo, List *restrictlist)
{
	pgpa_join_state *pjs;

	Assert(bms_membership(joinrel->relids) == BMS_MULTIPLE);

	/* Get our private state information for this join. */
	pjs = pgpa_get_join_state(root, joinrel, outerrel, innerrel);

	/* If there is relevant advice, call a helper function to apply it. */
	if (pjs != NULL)
	{
		uint64		original_mask = joinrel->pgs_mask;

		pgpa_planner_apply_joinrel_advice(&joinrel->pgs_mask,
										  root->plan_name,
										  pjs);

		/* Emit debugging message, if enabled. */
		if (pg_plan_advice_trace_mask && original_mask != joinrel->pgs_mask)
		{
			if (root->plan_name != NULL)
				ereport(WARNING,
						(errmsg("strategy mask for join on RTIs %s in subplan \"%s\" changed from 0x%" PRIx64 " to 0x%" PRIx64,
								pgpa_bms_to_cstring(joinrel->relids),
								root->plan_name,
								original_mask,
								joinrel->pgs_mask)));
			else
				ereport(WARNING,
						(errmsg("strategy mask for join on RTIs %s changed from 0x%" PRIx64 " to 0x%" PRIx64,
								pgpa_bms_to_cstring(joinrel->relids),
								original_mask,
								joinrel->pgs_mask)));
		}
	}

	/* Pass call to previous hook. */
	if (prev_joinrel_setup)
		(*prev_joinrel_setup) (root, joinrel, outerrel, innerrel,
							   sjinfo, restrictlist);
}

/*
 * Enforce any provided advice that is relevant to this particular method of
 * implementing this particular join.
 */
static void
pgpa_join_path_setup(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 JoinType jointype, JoinPathExtraData *extra)
{
	pgpa_join_state *pjs;

	Assert(bms_membership(joinrel->relids) == BMS_MULTIPLE);

	/*
	 * If we're considering implementing a semijoin by making one side unique,
	 * make a note of it in the pgpa_planner_state.
	 */
	if (jointype == JOIN_UNIQUE_OUTER || jointype == JOIN_UNIQUE_INNER)
	{
		pgpa_planner_state *pps;
		RelOptInfo *uniquerel;

		uniquerel = jointype == JOIN_UNIQUE_OUTER ? outerrel : innerrel;
		pps = GetPlannerGlobalExtensionState(root->glob, planner_extension_id);
		if (pps != NULL &&
			(pps->generate_advice_string || pps->generate_advice_feedback))
		{
			pgpa_planner_info *proot;
			MemoryContext oldcontext;

			/*
			 * Get or create a pgpa_planner_info object, and then add the
			 * relids from the unique side to proot->sj_unique_rels.
			 *
			 * We must be careful here to use a sufficiently long-lived
			 * context, since we might have been called by GEQO. We want all
			 * the data we store here (including the proot, if we create it)
			 * to last for as long as the pgpa_planner_state.
			 */
			oldcontext = MemoryContextSwitchTo(pps->mcxt);
			proot = pgpa_planner_get_proot(pps, root);
			if (!list_member(proot->sj_unique_rels, uniquerel->relids))
				proot->sj_unique_rels = lappend(proot->sj_unique_rels,
												bms_copy(uniquerel->relids));
			MemoryContextSwitchTo(oldcontext);
		}
	}

	/* Get our private state information for this join. */
	pjs = pgpa_get_join_state(root, joinrel, outerrel, innerrel);

	/* If there is relevant advice, call a helper function to apply it. */
	if (pjs != NULL)
	{
		uint64		original_mask = extra->pgs_mask;

		pgpa_planner_apply_join_path_advice(jointype,
											&extra->pgs_mask,
											root->plan_name,
											pjs);

		/* Emit debugging message, if enabled. */
		if (pg_plan_advice_trace_mask && original_mask != extra->pgs_mask)
		{
			if (root->plan_name != NULL)
				ereport(WARNING,
						(errmsg("strategy mask for %s join on %s with outer %s and inner %s in subplan \"%s\" changed from 0x%" PRIx64 " to 0x%" PRIx64,
								pgpa_jointype_to_cstring(jointype),
								pgpa_bms_to_cstring(joinrel->relids),
								pgpa_bms_to_cstring(outerrel->relids),
								pgpa_bms_to_cstring(innerrel->relids),
								root->plan_name,
								original_mask,
								extra->pgs_mask)));
			else
				ereport(WARNING,
						(errmsg("strategy mask for %s join on %s with outer %s and inner %s changed from 0x%" PRIx64 " to 0x%" PRIx64,
								pgpa_jointype_to_cstring(jointype),
								pgpa_bms_to_cstring(joinrel->relids),
								pgpa_bms_to_cstring(outerrel->relids),
								pgpa_bms_to_cstring(innerrel->relids),
								original_mask,
								extra->pgs_mask)));
		}
	}

	/* Pass call to previous hook. */
	if (prev_join_path_setup)
		(*prev_join_path_setup) (root, joinrel, outerrel, innerrel,
								 jointype, extra);
}

/*
 * Search for advice pertaining to a proposed join.
 */
static pgpa_join_state *
pgpa_get_join_state(PlannerInfo *root, RelOptInfo *joinrel,
					RelOptInfo *outerrel, RelOptInfo *innerrel)
{
	pgpa_planner_state *pps;
	pgpa_join_state *pjs;
	bool		new_pjs = false;

	/* Fetch our private state, set up by pgpa_planner_setup(). */
	pps = GetPlannerGlobalExtensionState(root->glob, planner_extension_id);
	if (pps == NULL || pps->trove == NULL)
	{
		/* No advice applies to this query, hence none to this joinrel. */
		return NULL;
	}

	/*
	 * See whether we've previously associated a pgpa_join_state with this
	 * joinrel. If we have not, we need to try to construct one. If we have,
	 * then there are two cases: (a) if innerrel and outerrel are unchanged,
	 * we can simply use it, and (b) if they have changed, we need to rejigger
	 * the array of identifiers but can still skip the trove lookup.
	 */
	pjs = GetRelOptInfoExtensionState(joinrel, planner_extension_id);
	if (pjs != NULL)
	{
		if (pjs->join_indexes == NULL && pjs->rel_indexes == NULL)
		{
			/*
			 * If there's no potentially relevant advice, then the presence of
			 * this pgpa_join_state acts like a negative cache entry: it tells
			 * us not to bother searching the trove for advice, because we
			 * will not find any.
			 */
			return NULL;
		}

		if (pjs->outerrel == outerrel && pjs->innerrel == innerrel)
		{
			/* No updates required, so just return. */
			/* XXX. Does this need to do something different under GEQO? */
			return pjs;
		}
	}

	/*
	 * If there's no pgpa_join_state yet, we need to allocate one. Trove keys
	 * will not get built for RTE_JOIN RTEs, so the array may end up being
	 * larger than needed. It's not worth trying to compute a perfectly
	 * accurate count here.
	 */
	if (pjs == NULL)
	{
		int			pessimistic_count = bms_num_members(joinrel->relids);

		pjs = palloc0_object(pgpa_join_state);
		pjs->rids = palloc_array(pgpa_identifier, pessimistic_count);
		new_pjs = true;
	}

	/*
	 * Either we just allocated a new pgpa_join_state, or the existing one
	 * needs reconfiguring for a new innerrel and outerrel. The required array
	 * size can't change, so we can overwrite the existing one.
	 */
	pjs->outerrel = outerrel;
	pjs->innerrel = innerrel;
	pjs->outer_count =
		pgpa_compute_identifiers_by_relids(root, outerrel->relids, pjs->rids);
	pjs->inner_count =
		pgpa_compute_identifiers_by_relids(root, innerrel->relids,
										   pjs->rids + pjs->outer_count);

	/*
	 * If we allocated a new pgpa_join_state, search our trove of advice for
	 * relevant entries. The trove lookup will return the same results for
	 * every outerrel/innerrel combination, so we don't need to repeat that
	 * work every time.
	 */
	if (new_pjs)
	{
		pgpa_trove_result tresult;

		/* Find join entries. */
		pgpa_trove_lookup(pps->trove, PGPA_TROVE_LOOKUP_JOIN,
						  pjs->outer_count + pjs->inner_count,
						  pjs->rids, &tresult);
		pjs->join_entries = tresult.entries;
		pjs->join_indexes = tresult.indexes;

		/* Find rel entries. */
		pgpa_trove_lookup(pps->trove, PGPA_TROVE_LOOKUP_REL,
						  pjs->outer_count + pjs->inner_count,
						  pjs->rids, &tresult);
		pjs->rel_entries = tresult.entries;
		pjs->rel_indexes = tresult.indexes;

		/* Now that the new pgpa_join_state is fully valid, save a pointer. */
		SetRelOptInfoExtensionState(joinrel, planner_extension_id, pjs);

		/*
		 * If there was no relevant advice found, just return NULL. This
		 * pgpa_join_state will stick around as a sort of negative cache
		 * entry, so that future calls for this same joinrel quickly return
		 * NULL.
		 */
		if (pjs->join_indexes == NULL && pjs->rel_indexes == NULL)
			return NULL;
	}

	return pjs;
}

/*
 * Enforce overall restrictions on a join relation that apply uniformly
 * regardless of the choice of inner and outer rel.
 */
static void
pgpa_planner_apply_joinrel_advice(uint64 *pgs_mask_p, char *plan_name,
								  pgpa_join_state *pjs)
{
	int			i = -1;
	int			flags;
	bool		gather_conflict = false;
	uint64		gather_mask = 0;
	Bitmapset  *gather_partial_match = NULL;
	Bitmapset  *gather_full_match = NULL;
	bool		partitionwise_conflict = false;
	int			partitionwise_outcome = 0;
	Bitmapset  *partitionwise_partial_match = NULL;
	Bitmapset  *partitionwise_full_match = NULL;

	/* Iterate over all possibly-relevant advice. */
	while ((i = bms_next_member(pjs->rel_indexes, i)) >= 0)
	{
		pgpa_trove_entry *entry = &pjs->rel_entries[i];
		pgpa_itm_type itm;
		bool		full_match = false;
		uint64		my_gather_mask = 0;
		int			my_partitionwise_outcome = 0;	/* >0 yes, <0 no */

		/*
		 * For GATHER and GATHER_MERGE, if the specified relations exactly
		 * match this joinrel, do whatever the advice says; otherwise, don't
		 * allow Gather or Gather Merge at this level. For NO_GATHER, there
		 * must be a single target relation which must be included in this
		 * joinrel, so just don't allow Gather or Gather Merge here, full
		 * stop.
		 */
		if (entry->tag == PGPA_TAG_NO_GATHER)
		{
			my_gather_mask = PGS_CONSIDER_NONPARTIAL;
			full_match = true;
		}
		else
		{
			int			total_count;

			total_count = pjs->outer_count + pjs->inner_count;
			itm = pgpa_identifiers_match_target(total_count, pjs->rids,
												entry->target);
			Assert(itm != PGPA_ITM_DISJOINT);

			if (itm == PGPA_ITM_EQUAL)
			{
				full_match = true;
				if (entry->tag == PGPA_TAG_PARTITIONWISE)
					my_partitionwise_outcome = 1;
				else if (entry->tag == PGPA_TAG_GATHER)
					my_gather_mask = PGS_GATHER;
				else if (entry->tag == PGPA_TAG_GATHER_MERGE)
					my_gather_mask = PGS_GATHER_MERGE;
				else
					elog(ERROR, "unexpected advice tag: %d",
						 (int) entry->tag);
			}
			else
			{
				/*
				 * If specified relations don't exactly match this joinrel,
				 * then we should do the opposite of whatever the advice says.
				 * For instance, if we have PARTITIONWISE((a b c)) or
				 * GATHER((a b c)) and this joinrel covers {a, b} or {a, b, c,
				 * d} or {a, d}, we shouldn't plan it partitionwise or put a
				 * Gather or Gather Merge on it here.
				 *
				 * Also, we can't put a Gather or Gather Merge at this level
				 * if there is PARTITIONWISE advice that overlaps with it,
				 * unless the PARTITIONWISE advice covers a subset of the
				 * relations in the joinrel. To continue the previous example,
				 * PARTITIONWISE((a b c)) is logically incompatible with
				 * GATHER((a b)) or GATHER((a d)), but not with GATHER((a b c
				 * d)).
				 *
				 * Conversely, we can't proceed partitionwise at this level if
				 * there is overlapping GATHER or GATHER_MERGE advice, unless
				 * that advice covers a superset of the relations in this
				 * joinrel. This is just the flip side of the preceding point.
				 */
				if (entry->tag == PGPA_TAG_PARTITIONWISE)
				{
					my_partitionwise_outcome = -1;
					if (itm != PGPA_ITM_TARGETS_ARE_SUBSET)
						my_gather_mask = PGS_CONSIDER_NONPARTIAL;
				}
				else if (entry->tag == PGPA_TAG_GATHER ||
						 entry->tag == PGPA_TAG_GATHER_MERGE)
				{
					my_gather_mask = PGS_CONSIDER_NONPARTIAL;
					if (itm != PGPA_ITM_KEYS_ARE_SUBSET)
						my_partitionwise_outcome = -1;
				}
				else
					elog(ERROR, "unexpected advice tag: %d",
						 (int) entry->tag);
			}
		}

		/*
		 * If we set my_gather_mask up above, then we (1) make a note if the
		 * advice conflicted, (2) remember the mask value, and (3) remember
		 * whether this was a full or partial match.
		 */
		if (my_gather_mask != 0)
		{
			if (gather_mask != 0 && gather_mask != my_gather_mask)
				gather_conflict = true;
			gather_mask = my_gather_mask;
			if (full_match)
				gather_full_match = bms_add_member(gather_full_match, i);
			else
				gather_partial_match = bms_add_member(gather_partial_match, i);
		}

		/*
		 * Likewise, if we set my_partitionwise_outcome up above, then we (1)
		 * make a note if the advice conflicted, (2) remember what the desired
		 * outcome was, and (3) remember whether this was a full or partial
		 * match.
		 */
		if (my_partitionwise_outcome != 0)
		{
			if (partitionwise_outcome != 0 &&
				partitionwise_outcome != my_partitionwise_outcome)
				partitionwise_conflict = true;
			partitionwise_outcome = my_partitionwise_outcome;
			if (full_match)
				partitionwise_full_match =
					bms_add_member(partitionwise_full_match, i);
			else
				partitionwise_partial_match =
					bms_add_member(partitionwise_partial_match, i);
		}
	}

	/*
	 * Mark every Gather-related piece of advice as partially matched, and if
	 * the set of targets exactly matched this relation, fully matched. If
	 * there was a conflict, mark them all as conflicting.
	 */
	flags = PGPA_TE_MATCH_PARTIAL;
	if (gather_conflict)
		flags |= PGPA_TE_CONFLICTING;
	pgpa_trove_set_flags(pjs->rel_entries, gather_partial_match, flags);
	flags |= PGPA_TE_MATCH_FULL;
	pgpa_trove_set_flags(pjs->rel_entries, gather_full_match, flags);

	/* Likewise for partitionwise advice. */
	flags = PGPA_TE_MATCH_PARTIAL;
	if (partitionwise_conflict)
		flags |= PGPA_TE_CONFLICTING;
	pgpa_trove_set_flags(pjs->rel_entries, partitionwise_partial_match, flags);
	flags |= PGPA_TE_MATCH_FULL;
	pgpa_trove_set_flags(pjs->rel_entries, partitionwise_full_match, flags);

	/*
	 * Enforce restrictions on the Gather/Gather Merge.  Only clear bits here,
	 * so that we still respect the enable_* GUCs. Do nothing if the advice
	 * conflicts.
	 */
	if (gather_mask != 0 && !gather_conflict)
	{
		uint64		all_gather_mask;

		all_gather_mask =
			PGS_GATHER | PGS_GATHER_MERGE | PGS_CONSIDER_NONPARTIAL;
		*pgs_mask_p &= ~(all_gather_mask & ~gather_mask);
	}

	/*
	 * As above, but for partitionwise advice.
	 *
	 * To induce a partitionwise join, we disable all the ordinary means of
	 * performing a join, so that an Append or MergeAppend path will hopefully
	 * be chosen.
	 *
	 * To prevent one, we just disable Append and MergeAppend.  Note that we
	 * must not unset PGS_CONSIDER_PARTITIONWISE even when we don't want a
	 * partitionwise join here, because we might want one at a higher level
	 * that will construct its own paths using the ones from this level.
	 */
	if (partitionwise_outcome != 0 && !partitionwise_conflict)
	{
		if (partitionwise_outcome > 0)
			*pgs_mask_p = (*pgs_mask_p & ~PGS_JOIN_ANY);
		else
			*pgs_mask_p &= ~(PGS_APPEND | PGS_MERGE_APPEND);
	}
}

/*
 * Enforce restrictions on the join order or join method.
 */
static void
pgpa_planner_apply_join_path_advice(JoinType jointype, uint64 *pgs_mask_p,
									char *plan_name,
									pgpa_join_state *pjs)
{
	int			i = -1;
	Bitmapset  *jo_permit_indexes = NULL;
	Bitmapset  *jo_deny_indexes = NULL;
	Bitmapset  *jo_deny_rel_indexes = NULL;
	Bitmapset  *jm_indexes = NULL;
	bool		jm_conflict = false;
	uint64		join_mask = 0;
	Bitmapset  *sj_permit_indexes = NULL;
	Bitmapset  *sj_deny_indexes = NULL;

	/*
	 * Reconsider PARTITIONWISE(...) advice.
	 *
	 * We already thought about this for the joinrel as a whole, but in some
	 * cases, partitionwise advice can also constrain the join order. For
	 * instance, if the advice says PARTITIONWISE((t1 t2)), we shouldn't build
	 * join paths for any joinrel that includes t1 or t2 unless it also
	 * includes the other. In general, the partitionwise operation must have
	 * already been completed within one side of the current join or the
	 * other, else the join order is impermissible.
	 *
	 * NB: It might seem tempting to try to deal with PARTITIONWISE advice
	 * entirely in this function, but that doesn't work. Here, we can only
	 * affect the pgs_mask within a particular JoinPathExtraData, that is, for
	 * a particular choice of innerrel and outerrel. Partitionwise paths are
	 * not built that way, so we must set pgs_mask for the RelOptInfo, which
	 * is best done in pgpa_planner_apply_joinrel_advice.
	 */
	while ((i = bms_next_member(pjs->rel_indexes, i)) >= 0)
	{
		pgpa_trove_entry *entry = &pjs->rel_entries[i];
		pgpa_itm_type inner_itm;
		pgpa_itm_type outer_itm;

		if (entry->tag != PGPA_TAG_PARTITIONWISE)
			continue;

		outer_itm = pgpa_identifiers_match_target(pjs->outer_count,
												  pjs->rids, entry->target);
		if (outer_itm == PGPA_ITM_EQUAL ||
			outer_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
			continue;

		inner_itm = pgpa_identifiers_match_target(pjs->inner_count,
												  pjs->rids + pjs->outer_count,
												  entry->target);
		if (inner_itm == PGPA_ITM_EQUAL ||
			inner_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
			continue;

		jo_deny_rel_indexes = bms_add_member(jo_deny_rel_indexes, i);
	}

	/* Iterate over advice that pertains to the join order and method. */
	i = -1;
	while ((i = bms_next_member(pjs->join_indexes, i)) >= 0)
	{
		pgpa_trove_entry *entry = &pjs->join_entries[i];
		uint64		my_join_mask;

		/* Handle join order advice. */
		if (entry->tag == PGPA_TAG_JOIN_ORDER)
		{
			pgpa_jo_outcome jo_outcome;

			jo_outcome = pgpa_join_order_permits_join(pjs->outer_count,
													  pjs->inner_count,
													  pjs->rids,
													  entry);
			if (jo_outcome == PGPA_JO_PERMITTED)
				jo_permit_indexes = bms_add_member(jo_permit_indexes, i);
			else if (jo_outcome == PGPA_JO_DENIED)
				jo_deny_indexes = bms_add_member(jo_deny_indexes, i);
			continue;
		}

		/* Handle join method advice. */
		my_join_mask = pgpa_join_strategy_mask_from_advice_tag(entry->tag);
		if (my_join_mask != 0)
		{
			bool		permit;
			bool		restrict_method;

			if (entry->tag == PGPA_TAG_FOREIGN_JOIN)
				permit = pgpa_opaque_join_permits_join(pjs->outer_count,
													   pjs->inner_count,
													   pjs->rids,
													   entry,
													   &restrict_method);
			else
				permit = pgpa_join_method_permits_join(pjs->outer_count,
													   pjs->inner_count,
													   pjs->rids,
													   entry,
													   &restrict_method);
			if (!permit)
				jo_deny_indexes = bms_add_member(jo_deny_indexes, i);
			else if (restrict_method)
			{
				jm_indexes = bms_add_member(jm_indexes, i);
				if (join_mask != 0 && join_mask != my_join_mask)
					jm_conflict = true;
				join_mask = my_join_mask;
			}
			continue;
		}

		/* Handle semijoin uniqueness advice. */
		if (entry->tag == PGPA_TAG_SEMIJOIN_UNIQUE ||
			entry->tag == PGPA_TAG_SEMIJOIN_NON_UNIQUE)
		{
			bool		outer_side_nullable;
			bool		restrict_method;

			/* Planner has nullable side of the semijoin on the outer side? */
			outer_side_nullable = (jointype == JOIN_UNIQUE_OUTER ||
								   jointype == JOIN_RIGHT_SEMI);

			if (!pgpa_semijoin_permits_join(pjs->outer_count,
											pjs->inner_count,
											pjs->rids,
											entry,
											outer_side_nullable,
											&restrict_method))
				jo_deny_indexes = bms_add_member(jo_deny_indexes, i);
			else if (restrict_method)
			{
				bool		advice_unique;
				bool		jt_unique;
				bool		jt_non_unique;

				/* Advice wants to unique-ify and use a regular join? */
				advice_unique = (entry->tag == PGPA_TAG_SEMIJOIN_UNIQUE);

				/* Planner is trying to unique-ify and use a regular join? */
				jt_unique = (jointype == JOIN_UNIQUE_INNER ||
							 jointype == JOIN_UNIQUE_OUTER);

				/* Planner is trying a semi-join, without unique-ifying? */
				jt_non_unique = (jointype == JOIN_SEMI ||
								 jointype == JOIN_RIGHT_SEMI);

				if (!jt_unique && !jt_non_unique)
				{
					/*
					 * This doesn't seem to be a semijoin to which SJ_UNIQUE
					 * or SJ_NON_UNIQUE can be applied.
					 */
					entry->flags |= PGPA_TE_INAPPLICABLE;
				}
				else if (advice_unique != jt_unique)
					sj_deny_indexes = bms_add_member(sj_deny_indexes, i);
				else
					sj_permit_indexes = bms_add_member(sj_permit_indexes, i);
			}
			continue;
		}
	}

	/*
	 * If the advice indicates both that this join order is permissible and
	 * also that it isn't, then mark advice related to the join order as
	 * conflicting.
	 */
	if (jo_permit_indexes != NULL &&
		(jo_deny_indexes != NULL || jo_deny_rel_indexes != NULL))
	{
		pgpa_trove_set_flags(pjs->join_entries, jo_permit_indexes,
							 PGPA_TE_CONFLICTING);
		pgpa_trove_set_flags(pjs->join_entries, jo_deny_indexes,
							 PGPA_TE_CONFLICTING);
		pgpa_trove_set_flags(pjs->rel_entries, jo_deny_rel_indexes,
							 PGPA_TE_CONFLICTING);
	}

	/*
	 * If more than one join method specification is relevant here and they
	 * differ, mark them all as conflicting.
	 */
	if (jm_conflict)
		pgpa_trove_set_flags(pjs->join_entries, jm_indexes,
							 PGPA_TE_CONFLICTING);

	/* If semijoin advice says both yes and no, mark it all as conflicting. */
	if (sj_permit_indexes != NULL && sj_deny_indexes != NULL)
	{
		pgpa_trove_set_flags(pjs->join_entries, sj_permit_indexes,
							 PGPA_TE_CONFLICTING);
		pgpa_trove_set_flags(pjs->join_entries, sj_deny_indexes,
							 PGPA_TE_CONFLICTING);
	}

	/*
	 * Enforce restrictions on the join order and join method, and any
	 * semijoin-related restrictions. Only clear bits here, so that we still
	 * respect the enable_* GUCs. Do nothing in cases where the advice on a
	 * single topic conflicts.
	 */
	if ((jo_deny_indexes != NULL || jo_deny_rel_indexes != NULL) &&
		jo_permit_indexes == NULL)
		*pgs_mask_p &= ~PGS_JOIN_ANY;
	if (join_mask != 0 && !jm_conflict)
		*pgs_mask_p &= ~(PGS_JOIN_ANY & ~join_mask);
	if (sj_deny_indexes != NULL && sj_permit_indexes == NULL)
		*pgs_mask_p &= ~PGS_JOIN_ANY;
}

/*
 * Translate an advice tag into a path generation strategy mask.
 *
 * This function can be called with tag types that don't represent join
 * strategies. In such cases, we just return 0, which can't be confused with
 * a valid mask.
 */
static uint64
pgpa_join_strategy_mask_from_advice_tag(pgpa_advice_tag_type tag)
{
	switch (tag)
	{
		case PGPA_TAG_FOREIGN_JOIN:
			return PGS_FOREIGNJOIN;
		case PGPA_TAG_MERGE_JOIN_PLAIN:
			return PGS_MERGEJOIN_PLAIN;
		case PGPA_TAG_MERGE_JOIN_MATERIALIZE:
			return PGS_MERGEJOIN_MATERIALIZE;
		case PGPA_TAG_NESTED_LOOP_PLAIN:
			return PGS_NESTLOOP_PLAIN;
		case PGPA_TAG_NESTED_LOOP_MATERIALIZE:
			return PGS_NESTLOOP_MATERIALIZE;
		case PGPA_TAG_NESTED_LOOP_MEMOIZE:
			return PGS_NESTLOOP_MEMOIZE;
		case PGPA_TAG_HASH_JOIN:
			return PGS_HASHJOIN;
		default:
			return 0;
	}
}

/*
 * Does a certain item of join order advice permit a certain join?
 *
 * Returns PGPA_JO_DENIED if the advice is incompatible with the proposed
 * join order.
 *
 * Returns PGPA_JO_PERMITTED if the advice specifies exactly the proposed
 * join order. This implies that a partitionwise join should not be
 * performed at this level; rather, one of the traditional join methods
 * should be used.
 *
 * Returns PGPA_JO_INDIFFERENT if the advice does not care what happens.
 * We use this for unordered JOIN_ORDER sublists, which are compatible with
 * partitionwise join but do not mandate it.
 */
static pgpa_jo_outcome
pgpa_join_order_permits_join(int outer_count, int inner_count,
							 pgpa_identifier *rids,
							 pgpa_trove_entry *entry)
{
	bool		loop = true;
	bool		sublist = false;
	int			length;
	int			outer_length;
	pgpa_advice_target *target = entry->target;
	pgpa_advice_target *prefix_target;

	/* We definitely have at least a partial match for this trove entry. */
	entry->flags |= PGPA_TE_MATCH_PARTIAL;

	/*
	 * Find the innermost sublist that contains all keys; if no sublist does,
	 * then continue processing with the toplevel list.
	 *
	 * For example, if the advice says JOIN_ORDER(t1 t2 (t3 t4 t5)), then we
	 * should evaluate joins that only involve t3, t4, and/or t5 against the
	 * (t3 t4 t5) sublist, and others against the full list.
	 *
	 * Note that (1) outermost sublist is always ordered and (2) whenever we
	 * zoom into an unordered sublist, we instantly return
	 * PGPA_JO_INDIFFERENT.
	 */
	while (loop)
	{
		Assert(target->ttype == PGPA_TARGET_ORDERED_LIST);

		loop = false;
		foreach_ptr(pgpa_advice_target, child_target, target->children)
		{
			pgpa_itm_type itm;

			if (child_target->ttype == PGPA_TARGET_IDENTIFIER)
				continue;

			itm = pgpa_identifiers_match_target(outer_count + inner_count,
												rids, child_target);
			if (itm == PGPA_ITM_EQUAL || itm == PGPA_ITM_KEYS_ARE_SUBSET)
			{
				if (child_target->ttype == PGPA_TARGET_ORDERED_LIST)
				{
					target = child_target;
					sublist = true;
					loop = true;
					break;
				}
				else
				{
					Assert(child_target->ttype == PGPA_TARGET_UNORDERED_LIST);
					return PGPA_JO_INDIFFERENT;
				}
			}
		}
	}

	/*
	 * Try to find a prefix of the selected join order list that is exactly
	 * equal to the outer side of the proposed join.
	 */
	length = list_length(target->children);
	prefix_target = palloc0_object(pgpa_advice_target);
	prefix_target->ttype = PGPA_TARGET_ORDERED_LIST;
	for (outer_length = 1; outer_length <= length; ++outer_length)
	{
		pgpa_itm_type itm;

		/* Avoid leaking memory in every loop iteration. */
		if (prefix_target->children != NULL)
			list_free(prefix_target->children);
		prefix_target->children = list_copy_head(target->children,
												 outer_length);

		/* Search, hoping to find an exact match. */
		itm = pgpa_identifiers_match_target(outer_count, rids, prefix_target);
		if (itm == PGPA_ITM_EQUAL)
			break;

		/*
		 * If the prefix of the join order list that we're considering
		 * includes some but not all of the outer rels, we can make the prefix
		 * longer to find an exact match. But if the advice hasn't mentioned
		 * everything that's part of our outer rel yet, but has mentioned
		 * things that are not, then this join doesn't match the join order
		 * list.
		 */
		if (itm != PGPA_ITM_TARGETS_ARE_SUBSET)
			return PGPA_JO_DENIED;
	}

	/*
	 * If the previous loop stopped before the prefix_target included the
	 * entire join order list, then the next member of the join order list
	 * must exactly match the inner side of the join.
	 *
	 * Example: Given JOIN_ORDER(t1 t2 (t3 t4 t5)), if the outer side of the
	 * current join includes only t1, then the inner side must be exactly t2;
	 * if the outer side includes both t1 and t2, then the inner side must
	 * include exactly t3, t4, and t5.
	 */
	if (outer_length < length)
	{
		pgpa_advice_target *inner_target;
		pgpa_itm_type itm;

		inner_target = list_nth(target->children, outer_length);

		itm = pgpa_identifiers_match_target(inner_count, rids + outer_count,
											inner_target);

		/*
		 * Before returning, consider whether we need to mark this entry as
		 * fully matched. If we're considering the full list rather than a
		 * sublist, and if we found every item but one on the outer side of
		 * the join and the last item on the inner side of the join, then the
		 * answer is yes.
		 */
		if (!sublist && outer_length + 1 == length && itm == PGPA_ITM_EQUAL)
			entry->flags |= PGPA_TE_MATCH_FULL;

		return (itm == PGPA_ITM_EQUAL) ? PGPA_JO_PERMITTED : PGPA_JO_DENIED;
	}

	/*
	 * If we get here, then the outer side of the join includes the entirety
	 * of the join order list. In this case, we behave differently depending
	 * on whether we're looking at the top-level join order list or sublist.
	 * At the top-level, we treat the specified list as mandating that the
	 * actual join order has the given list as a prefix, but a sublist
	 * requires an exact match.
	 *
	 * Example: Given JOIN_ORDER(t1 t2 (t3 t4 t5)), we must start by joining
	 * all five of those relations and in that sequence, but once that is
	 * done, it's OK to join any other rels that are part of the join problem.
	 * This allows a user to specify the driving table and perhaps the first
	 * few things to which it should be joined while leaving the rest of the
	 * join order up the optimizer. But it seems like it would be surprising,
	 * given that specification, if the user could add t6 to the (t3 t4 t5)
	 * sub-join, so we don't allow that. If we did want to allow it, the logic
	 * earlier in this function would require substantial adjustment: we could
	 * allow the t3-t4-t5-t6 join to be built here, but the next step of
	 * joining t1-t2 to the result would still be rejected.
	 */
	if (!sublist)
		entry->flags |= PGPA_TE_MATCH_FULL;
	return sublist ? PGPA_JO_DENIED : PGPA_JO_PERMITTED;
}

/*
 * Does a certain item of join method advice permit a certain join?
 *
 * Advice such as HASH_JOIN((x y)) means that there should be a hash join with
 * exactly x and y on the inner side. Obviously, this means that if we are
 * considering a join with exactly x and y on the inner side, we should enforce
 * the use of a hash join. However, it also means that we must reject some
 * incompatible join orders entirely.  For example, a join with exactly x
 * and y on the outer side shouldn't be allowed, because such paths might win
 * over the advice-driven path on cost.
 *
 * To accommodate these requirements, this function returns true if the join
 * should be allowed and false if it should not. Furthermore, *restrict_method
 * is set to true if the join method should be enforced and false if not.
 */
static bool
pgpa_join_method_permits_join(int outer_count, int inner_count,
							  pgpa_identifier *rids,
							  pgpa_trove_entry *entry,
							  bool *restrict_method)
{
	pgpa_advice_target *target = entry->target;
	pgpa_itm_type inner_itm;
	pgpa_itm_type outer_itm;
	pgpa_itm_type join_itm;

	/* We definitely have at least a partial match for this trove entry. */
	entry->flags |= PGPA_TE_MATCH_PARTIAL;

	*restrict_method = false;

	/*
	 * If our inner rel mentions exactly the same relations as the advice
	 * target, allow the join and enforce the join method restriction.
	 *
	 * If our inner rel mentions a superset of the target relations, allow the
	 * join. The join we care about has already taken place, and this advice
	 * imposes no further restrictions.
	 */
	inner_itm = pgpa_identifiers_match_target(inner_count,
											  rids + outer_count,
											  target);
	if (inner_itm == PGPA_ITM_EQUAL)
	{
		entry->flags |= PGPA_TE_MATCH_FULL;
		*restrict_method = true;
		return true;
	}
	else if (inner_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
		return true;

	/*
	 * If our outer rel mentions a superset of the relations in the advice
	 * target, no restrictions apply, because the join we care about has
	 * already taken place.
	 *
	 * On the other hand, if our outer rel mentions exactly the relations
	 * mentioned in the advice target, the planner is trying to reverse the
	 * sides of the join as compared with our desired outcome. Reject that.
	 */
	outer_itm = pgpa_identifiers_match_target(outer_count,
											  rids, target);
	if (outer_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
		return true;
	else if (outer_itm == PGPA_ITM_EQUAL)
		return false;

	/*
	 * If the advice target mentions only a single relation, the test below
	 * cannot ever pass, so save some work by exiting now.
	 */
	if (target->ttype == PGPA_TARGET_IDENTIFIER)
		return false;

	/*
	 * If everything in the joinrel appears in the advice target, we're below
	 * the level of the join we want to control.
	 *
	 * For example, HASH_JOIN((x y)) doesn't restrict how x and y can be
	 * joined.
	 *
	 * This lookup shouldn't return PGPA_ITM_DISJOINT, because any such advice
	 * should not have been returned from the trove in the first place.
	 */
	join_itm = pgpa_identifiers_match_target(outer_count + inner_count,
											 rids, target);
	Assert(join_itm != PGPA_ITM_DISJOINT);
	if (join_itm == PGPA_ITM_KEYS_ARE_SUBSET ||
		join_itm == PGPA_ITM_EQUAL)
		return true;

	/*
	 * We've already permitted all allowable cases, so reject this.
	 *
	 * If we reach this point, then the advice overlaps with this join but
	 * isn't entirely contained within either side, and there's also at least
	 * one relation present in the join that isn't mentioned by the advice.
	 *
	 * For instance, in the HASH_JOIN((x y)) example, we would reach here if x
	 * were on one side of the join, y on the other, and at least one of the
	 * two sides also included some other relation, say t. In that case,
	 * accepting this join would allow the (x y t) joinrel to contain
	 * non-disabled paths that do not put (x y) on the inner side of a hash
	 * join; we could instead end up with something like (x JOIN t) JOIN y.
	 */
	return false;
}

/*
 * Does advice concerning an opaque join permit a certain join?
 *
 * By an opaque join, we mean one where the exact mechanism by which the
 * join is performed is not visible to PostgreSQL. Currently this is the
 * case only for foreign joins: FOREIGN_JOIN((x y z)) means that x, y, and
 * z are joined on the remote side, but we know nothing about the join order
 * or join methods used over there.
 *
 * The logic here needs to differ from pgpa_join_method_permits_join because,
 * for other join types, the advice target is the set of inner rels; here, it
 * includes both inner and outer rels.
 */
static bool
pgpa_opaque_join_permits_join(int outer_count, int inner_count,
							  pgpa_identifier *rids,
							  pgpa_trove_entry *entry,
							  bool *restrict_method)
{
	pgpa_advice_target *target = entry->target;
	pgpa_itm_type join_itm;

	/* We definitely have at least a partial match for this trove entry. */
	entry->flags |= PGPA_TE_MATCH_PARTIAL;

	*restrict_method = false;

	join_itm = pgpa_identifiers_match_target(outer_count + inner_count,
											 rids, target);
	if (join_itm == PGPA_ITM_EQUAL)
	{
		/*
		 * We have an exact match, and should therefore allow the join and
		 * enforce the use of the relevant opaque join method.
		 */
		entry->flags |= PGPA_TE_MATCH_FULL;
		*restrict_method = true;
		return true;
	}

	if (join_itm == PGPA_ITM_KEYS_ARE_SUBSET ||
		join_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
	{
		/*
		 * If join_itm == PGPA_ITM_TARGETS_ARE_SUBSET, then the join we care
		 * about has already taken place and no further restrictions apply.
		 *
		 * If join_itm == PGPA_ITM_KEYS_ARE_SUBSET, we're still building up to
		 * the join we care about and have not introduced any extraneous
		 * relations not named in the advice. Note that ForeignScan paths for
		 * joins are built up from ForeignScan paths from underlying joins and
		 * scans, so we must not disable this join when considering a subset
		 * of the relations we ultimately want.
		 */
		return true;
	}

	/*
	 * The advice overlaps the join, but at least one relation is present in
	 * the join that isn't mentioned by the advice. We want to disable such
	 * paths so that we actually push down the join as intended.
	 */
	return false;
}

/*
 * Does advice concerning a semijoin permit a certain join?
 *
 * Unlike join method advice, which lists the rels on the inner side of the
 * join, semijoin uniqueness advice lists the rels on the nullable side of the
 * join. Those can be the same, if the join type is JOIN_UNIQUE_INNER or
 * JOIN_SEMI, or they can be different, in case of JOIN_UNIQUE_OUTER or
 * JOIN_RIGHT_SEMI.
 *
 * We don't know here whether the caller specified SEMIJOIN_UNIQUE or
 * SEMIJOIN_NON_UNIQUE. The caller should check the join type against the
 * advice type if and only if we set *restrict_method to true.
 */
static bool
pgpa_semijoin_permits_join(int outer_count, int inner_count,
						   pgpa_identifier *rids,
						   pgpa_trove_entry *entry,
						   bool outer_is_nullable,
						   bool *restrict_method)
{
	pgpa_advice_target *target = entry->target;
	pgpa_itm_type join_itm;
	pgpa_itm_type inner_itm;
	pgpa_itm_type outer_itm;

	*restrict_method = false;

	/* We definitely have at least a partial match for this trove entry. */
	entry->flags |= PGPA_TE_MATCH_PARTIAL;

	/*
	 * If outer rel is the nullable side and contains exactly the same
	 * relations as the advice target, then the join order is allowable, but
	 * the caller must check whether the advice tag (either SEMIJOIN_UNIQUE or
	 * SEMIJOIN_NON_UNIQUE) matches the join type.
	 *
	 * If the outer rel is a superset of the target relations, the join we
	 * care about has already taken place, so we should impose no further
	 * restrictions.
	 */
	outer_itm = pgpa_identifiers_match_target(outer_count,
											  rids, target);
	if (outer_itm == PGPA_ITM_EQUAL)
	{
		entry->flags |= PGPA_TE_MATCH_FULL;
		if (outer_is_nullable)
		{
			*restrict_method = true;
			return true;
		}
	}
	else if (outer_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
		return true;

	/* As above, but for the inner rel. */
	inner_itm = pgpa_identifiers_match_target(inner_count,
											  rids + outer_count,
											  target);
	if (inner_itm == PGPA_ITM_EQUAL)
	{
		entry->flags |= PGPA_TE_MATCH_FULL;
		if (!outer_is_nullable)
		{
			*restrict_method = true;
			return true;
		}
	}
	else if (inner_itm == PGPA_ITM_TARGETS_ARE_SUBSET)
		return true;

	/*
	 * If everything in the joinrel appears in the advice target, we're below
	 * the level of the join we want to control.
	 */
	join_itm = pgpa_identifiers_match_target(outer_count + inner_count,
											 rids, target);
	Assert(join_itm != PGPA_ITM_DISJOINT);
	if (join_itm == PGPA_ITM_KEYS_ARE_SUBSET ||
		join_itm == PGPA_ITM_EQUAL)
		return true;

	/*
	 * We've tested for all allowable possibilities, and so must reject this
	 * join order. This can happen in two ways.
	 *
	 * First, we might be considering a semijoin that overlaps incompletely
	 * with one or both sides of the join. For example, if the user has
	 * specified SEMIJOIN_UNIQUE((t1 t2)) or SEMIJOIN_NON_UNIQUE((t1 t2)), we
	 * should reject a proposed t2-t3 join, since that could not result in a
	 * final plan compatible with the advice.
	 *
	 * Second, we might be considering a semijoin where the advice target
	 * perfectly matches one side of the join, but it's the wrong one. For
	 * example, in the example above, we might see a 3-way join between t1,
	 * t2, and t3, with (t1 t2) on the non-nullable side. That, too, would be
	 * incompatible with the advice.
	 */
	return false;
}

/*
 * Apply scan advice to a RelOptInfo.
 */
static void
pgpa_planner_apply_scan_advice(RelOptInfo *rel,
							   pgpa_trove_entry *scan_entries,
							   Bitmapset *scan_indexes,
							   pgpa_trove_entry *rel_entries,
							   Bitmapset *rel_indexes)
{
	bool		gather_conflict = false;
	Bitmapset  *gather_partial_match = NULL;
	Bitmapset  *gather_full_match = NULL;
	int			i = -1;
	pgpa_trove_entry *scan_entry = NULL;
	int			flags;
	bool		scan_type_conflict = false;
	Bitmapset  *scan_type_indexes = NULL;
	Bitmapset  *scan_type_rel_indexes = NULL;
	uint64		gather_mask = 0;
	uint64		scan_type = 0;

	/* Scrutinize available scan advice. */
	while ((i = bms_next_member(scan_indexes, i)) >= 0)
	{
		pgpa_trove_entry *my_entry = &scan_entries[i];
		uint64		my_scan_type = 0;

		/* Translate our advice tags to a scan strategy advice value. */
		if (my_entry->tag == PGPA_TAG_BITMAP_HEAP_SCAN)
		{
			/*
			 * Currently, PGS_CONSIDER_INDEXONLY can suppress Bitmap Heap
			 * Scans, so don't clear it when such a scan is requested. This
			 * happens because build_index_scan() thinks that the possibility
			 * of an index-only scan is a sufficient reason to consider using
			 * an otherwise-useless index, and get_index_paths() thinks that
			 * the same paths that are useful for index or index-only scans
			 * should also be considered for bitmap scans. Perhaps that logic
			 * should be tightened up, but until then we need to include
			 * PGS_CONSIDER_INDEXONLY in my_scan_type here.
			 */
			my_scan_type = PGS_BITMAPSCAN | PGS_CONSIDER_INDEXONLY;
		}
		else if (my_entry->tag == PGPA_TAG_INDEX_ONLY_SCAN)
			my_scan_type = PGS_INDEXONLYSCAN | PGS_CONSIDER_INDEXONLY;
		else if (my_entry->tag == PGPA_TAG_INDEX_SCAN)
			my_scan_type = PGS_INDEXSCAN;
		else if (my_entry->tag == PGPA_TAG_SEQ_SCAN)
			my_scan_type = PGS_SEQSCAN;
		else if (my_entry->tag == PGPA_TAG_TID_SCAN)
			my_scan_type = PGS_TIDSCAN;

		/*
		 * If this is understandable scan advice, hang on to the entry, the
		 * inferred scan type, and the index at which we found it.
		 *
		 * Also make a note if we see conflicting scan type advice. Note that
		 * we regard two index specifications as conflicting unless they match
		 * exactly. In theory, perhaps we could regard INDEX_SCAN(a c) and
		 * INDEX_SCAN(a b.c) as non-conflicting if it happens that the only
		 * index named c is in schema b, but it doesn't seem worth the code.
		 */
		if (my_scan_type != 0)
		{
			if (scan_type != 0 && scan_type != my_scan_type)
				scan_type_conflict = true;
			if (!scan_type_conflict && scan_entry != NULL &&
				my_entry->target->itarget != NULL &&
				scan_entry->target->itarget != NULL &&
				!pgpa_index_targets_equal(scan_entry->target->itarget,
										  my_entry->target->itarget))
				scan_type_conflict = true;
			scan_entry = my_entry;
			scan_type = my_scan_type;
			scan_type_indexes = bms_add_member(scan_type_indexes, i);
		}
	}

	/* Scrutinize available gather-related and partitionwise advice. */
	i = -1;
	while ((i = bms_next_member(rel_indexes, i)) >= 0)
	{
		pgpa_trove_entry *my_entry = &rel_entries[i];
		uint64		my_gather_mask = 0;
		bool		just_one_rel;

		just_one_rel = my_entry->target->ttype == PGPA_TARGET_IDENTIFIER
			|| list_length(my_entry->target->children) == 1;

		/*
		 * PARTITIONWISE behaves like a scan type, except that if there's more
		 * than one relation targeted, it has no effect at this level.
		 */
		if (my_entry->tag == PGPA_TAG_PARTITIONWISE)
		{
			if (just_one_rel)
			{
				const uint64 my_scan_type = PGS_APPEND | PGS_MERGE_APPEND;

				if (scan_type != 0 && scan_type != my_scan_type)
					scan_type_conflict = true;
				scan_entry = my_entry;
				scan_type = my_scan_type;
				scan_type_rel_indexes =
					bms_add_member(scan_type_rel_indexes, i);
			}
			continue;
		}

		/*
		 * GATHER and GATHER_MERGE applied to a single rel mean that we should
		 * use the corresponding strategy here, while applying either to more
		 * than one rel means we should not use those strategies here, but
		 * rather at the level of the joinrel that corresponds to what was
		 * specified. NO_GATHER can only be applied to single rels.
		 *
		 * Note that setting PGS_CONSIDER_NONPARTIAL in my_gather_mask is
		 * equivalent to allowing the non-use of either form of Gather here.
		 */
		if (my_entry->tag == PGPA_TAG_GATHER ||
			my_entry->tag == PGPA_TAG_GATHER_MERGE)
		{
			if (!just_one_rel)
				my_gather_mask = PGS_CONSIDER_NONPARTIAL;
			else if (my_entry->tag == PGPA_TAG_GATHER)
				my_gather_mask = PGS_GATHER;
			else
				my_gather_mask = PGS_GATHER_MERGE;
		}
		else if (my_entry->tag == PGPA_TAG_NO_GATHER)
		{
			Assert(just_one_rel);
			my_gather_mask = PGS_CONSIDER_NONPARTIAL;
		}

		/*
		 * If we set my_gather_mask up above, then we (1) make a note if the
		 * advice conflicted, (2) remember the mask value, and (3) remember
		 * whether this was a full or partial match.
		 */
		if (my_gather_mask != 0)
		{
			if (gather_mask != 0 && gather_mask != my_gather_mask)
				gather_conflict = true;
			gather_mask = my_gather_mask;
			if (just_one_rel)
				gather_full_match = bms_add_member(gather_full_match, i);
			else
				gather_partial_match = bms_add_member(gather_partial_match, i);
		}
	}

	/* Enforce choice of index. */
	if (scan_entry != NULL && !scan_type_conflict &&
		(scan_entry->tag == PGPA_TAG_INDEX_SCAN ||
		 scan_entry->tag == PGPA_TAG_INDEX_ONLY_SCAN))
	{
		pgpa_index_target *itarget = scan_entry->target->itarget;
		IndexOptInfo *matched_index = NULL;

		foreach_node(IndexOptInfo, index, rel->indexlist)
		{
			char	   *relname = get_rel_name(index->indexoid);
			Oid			nspoid = get_rel_namespace(index->indexoid);
			char	   *relnamespace = get_namespace_name_or_temp(nspoid);

			if (strcmp(itarget->indname, relname) == 0 &&
				(itarget->indnamespace == NULL ||
				 strcmp(itarget->indnamespace, relnamespace) == 0))
			{
				matched_index = index;
				break;
			}
		}

		if (matched_index == NULL)
		{
			/* Don't force the scan type if the index doesn't exist. */
			scan_type = 0;

			/* Mark advice as inapplicable. */
			pgpa_trove_set_flags(scan_entries, scan_type_indexes,
								 PGPA_TE_INAPPLICABLE);
		}
		else
		{
			/* Disable every other index. */
			foreach_node(IndexOptInfo, index, rel->indexlist)
			{
				if (index != matched_index)
					index->disabled = true;
			}
		}
	}

	/*
	 * Mark all the scan method entries as fully matched; and if they specify
	 * different things, mark them all as conflicting.
	 */
	flags = PGPA_TE_MATCH_PARTIAL | PGPA_TE_MATCH_FULL;
	if (scan_type_conflict)
		flags |= PGPA_TE_CONFLICTING;
	pgpa_trove_set_flags(scan_entries, scan_type_indexes, flags);
	pgpa_trove_set_flags(rel_entries, scan_type_rel_indexes, flags);

	/*
	 * Mark every Gather-related piece of advice as partially matched. Mark
	 * the ones that included this relation as a target by itself as fully
	 * matched. If there was a conflict, mark them all as conflicting.
	 */
	flags = PGPA_TE_MATCH_PARTIAL;
	if (gather_conflict)
		flags |= PGPA_TE_CONFLICTING;
	pgpa_trove_set_flags(rel_entries, gather_partial_match, flags);
	flags |= PGPA_TE_MATCH_FULL;
	pgpa_trove_set_flags(rel_entries, gather_full_match, flags);

	/*
	 * Enforce restrictions on the scan type and use of Gather/Gather Merge.
	 * Only clear bits here, so that we still respect the enable_* GUCs. Do
	 * nothing in cases where the advice on a single topic conflicts.
	 */
	if (scan_type != 0 && !scan_type_conflict)
	{
		uint64		all_scan_mask;

		all_scan_mask = PGS_SCAN_ANY | PGS_APPEND | PGS_MERGE_APPEND |
			PGS_CONSIDER_INDEXONLY;
		rel->pgs_mask &= ~(all_scan_mask & ~scan_type);
	}
	if (gather_mask != 0 && !gather_conflict)
	{
		uint64		all_gather_mask;

		all_gather_mask =
			PGS_GATHER | PGS_GATHER_MERGE | PGS_CONSIDER_NONPARTIAL;
		rel->pgs_mask &= ~(all_gather_mask & ~gather_mask);
	}
}

/*
 * Add feedback entries for one trove slice to the provided list and
 * return the resulting list.
 *
 * Feedback entries are generated from the trove entry's flags. It's assumed
 * that the caller has already set all relevant flags with the exception of
 * PGPA_TE_FAILED. We set that flag here if appropriate.
 */
static List *
pgpa_planner_append_feedback(List *list, pgpa_trove *trove,
							 pgpa_trove_lookup_type type,
							 pgpa_identifier *rt_identifiers,
							 pgpa_plan_walker_context *walker)
{
	pgpa_trove_entry *entries;
	int			nentries;

	pgpa_trove_lookup_all(trove, type, &entries, &nentries);
	for (int i = 0; i < nentries; ++i)
	{
		pgpa_trove_entry *entry = &entries[i];
		DefElem    *item;

		/*
		 * If this entry was fully matched, check whether generating advice
		 * from this plan would produce such an entry. If not, label the entry
		 * as failed.
		 */
		if ((entry->flags & PGPA_TE_MATCH_FULL) != 0 &&
			!pgpa_walker_would_advise(walker, rt_identifiers,
									  entry->tag, entry->target))
			entry->flags |= PGPA_TE_FAILED;

		item = makeDefElem(pgpa_cstring_trove_entry(entry),
						   (Node *) makeInteger(entry->flags), -1);
		list = lappend(list, item);
	}

	return list;
}

/*
 * Emit a WARNING to tell the user about a problem with the supplied plan
 * advice.
 */
static void
pgpa_planner_feedback_warning(List *feedback)
{
	StringInfoData detailbuf;
	StringInfoData flagbuf;

	/* Quick exit if there's no feedback. */
	if (feedback == NIL)
		return;

	/* Initialize buffers. */
	initStringInfo(&detailbuf);
	initStringInfo(&flagbuf);

	/* Main loop. */
	foreach_node(DefElem, item, feedback)
	{
		int			flags = defGetInt32(item);

		/*
		 * Don't emit anything if it was fully matched with no problems found.
		 *
		 * NB: Feedback should never be marked fully matched without also
		 * being marked partially matched.
		 */
		if (flags == (PGPA_TE_MATCH_PARTIAL | PGPA_TE_MATCH_FULL))
			continue;

		/*
		 * Terminate each detail line except the last with a newline. This is
		 * also a convenient place to reset flagbuf.
		 */
		if (detailbuf.len > 0)
		{
			appendStringInfoChar(&detailbuf, '\n');
			resetStringInfo(&flagbuf);
		}

		/* Generate output. */
		pgpa_trove_append_flags(&flagbuf, flags);
		appendStringInfo(&detailbuf, "advice %s feedback is \"%s\"",
						 item->defname, flagbuf.data);
	}

	/* Emit the warning, if any problems were found. */
	if (detailbuf.len > 0)
		ereport(WARNING,
				errmsg("supplied plan advice was not enforced"),
				errdetail("%s", detailbuf.data));
}

/*
 * Get or create the pgpa_planner_info for the given PlannerInfo.
 */
static pgpa_planner_info *
pgpa_planner_get_proot(pgpa_planner_state *pps, PlannerInfo *root)
{
	pgpa_planner_info *new_proot;

	/*
	 * If pps->last_proot isn't populated, there are no pgpa_planner_info
	 * objects yet, so we can drop through and create a new one. Otherwise,
	 * search for an object with a matching name, and drop through only if
	 * none is found.
	 */
	if (pps->last_proot != NULL)
	{
		if (root->plan_name == NULL)
		{
			if (pps->last_proot->plan_name == NULL)
				return pps->last_proot;

			foreach_ptr(pgpa_planner_info, proot, pps->proots)
			{
				if (proot->plan_name == NULL)
				{
					pps->last_proot = proot;
					return proot;
				}
			}
		}
		else
		{
			if (pps->last_proot->plan_name != NULL &&
				strcmp(pps->last_proot->plan_name, root->plan_name) == 0)
				return pps->last_proot;

			foreach_ptr(pgpa_planner_info, proot, pps->proots)
			{
				if (proot->plan_name != NULL &&
					strcmp(proot->plan_name, root->plan_name) == 0)
				{
					pps->last_proot = proot;
					return proot;
				}
			}
		}
	}

	/* Create new object, add to list, and make it most recently used. */
	new_proot = palloc0_object(pgpa_planner_info);
	new_proot->plan_name = root->plan_name;
	pps->proots = lappend(pps->proots, new_proot);
	pps->last_proot = new_proot;

	return new_proot;
}

/*
 * Save the range table identifier for one relation for future cross-checking.
 */
static void
pgpa_ri_checker_save(pgpa_planner_state *pps, PlannerInfo *root,
					 RelOptInfo *rel)
{
#ifdef USE_ASSERT_CHECKING
	pgpa_planner_info *proot;
	pgpa_identifier *rid;

	/* Get the pgpa_planner_info for this PlannerInfo. */
	proot = pgpa_planner_get_proot(pps, root);

	/* Allocate or extend the proot's rid_array as necessary. */
	if (proot->rid_array_size < rel->relid)
	{
		int			new_size = pg_nextpower2_32(Max(rel->relid, 8));

		if (proot->rid_array_size == 0)
			proot->rid_array = palloc0_array(pgpa_identifier, new_size);
		else
			proot->rid_array = repalloc0_array(proot->rid_array,
											   pgpa_identifier,
											   proot->rid_array_size,
											   new_size);
		proot->rid_array_size = new_size;
	}

	/* Save relation identifier details for this RTI if not already done. */
	rid = &proot->rid_array[rel->relid - 1];
	if (rid->alias_name == NULL)
		pgpa_compute_identifier_by_rti(root, rel->relid, rid);
#endif
}

/*
 * Validate that the range table identifiers we were able to generate during
 * planning match the ones we generated from the final plan.
 */
static void
pgpa_ri_checker_validate(pgpa_planner_state *pps, PlannedStmt *pstmt)
{
#ifdef USE_ASSERT_CHECKING
	pgpa_identifier *rt_identifiers;
	Index		rtable_length = list_length(pstmt->rtable);

	/* Create identifiers from the planned statement. */
	rt_identifiers = pgpa_create_identifiers_for_planned_stmt(pstmt);

	/* Iterate over identifiers created during planning, so we can compare. */
	foreach_ptr(pgpa_planner_info, proot, pps->proots)
	{
		int			rtoffset = 0;

		/*
		 * If there's no plan name associated with this entry, then the
		 * rtoffset is 0. Otherwise, we can search the SubPlanRTInfo list to
		 * find the rtoffset.
		 */
		if (proot->plan_name != NULL)
		{
			foreach_node(SubPlanRTInfo, rtinfo, pstmt->subrtinfos)
			{
				/*
				 * If rtinfo->dummy is set, then the subquery's range table
				 * will only have been partially copied to the final range
				 * table. Specifically, only RTE_RELATION entries and
				 * RTE_SUBQUERY entries that were once RTE_RELATION entries
				 * will be copied, as per add_rtes_to_flat_rtable. Therefore,
				 * there's no fixed rtoffset that we can apply to the RTIs
				 * used during planning to locate the corresponding relations
				 */
				if (strcmp(proot->plan_name, rtinfo->plan_name) == 0
					&& !rtinfo->dummy)
				{
					rtoffset = rtinfo->rtoffset;
					Assert(rtoffset > 0);
					break;
				}
			}

			/*
			 * It's not an error if we don't find the plan name: that just
			 * means that we planned a subplan by this name but it ended up
			 * being a dummy subplan and so wasn't included in the final plan
			 * tree.
			 */
			if (rtoffset == 0)
				continue;
		}

		for (int rti = 1; rti <= proot->rid_array_size; ++rti)
		{
			Index		flat_rti = rtoffset + rti;
			pgpa_identifier *rid1 = &proot->rid_array[rti - 1];
			pgpa_identifier *rid2;

			if (rid1->alias_name == NULL)
				continue;

			Assert(flat_rti <= rtable_length);
			rid2 = &rt_identifiers[flat_rti - 1];
			Assert(strcmp(rid1->alias_name, rid2->alias_name) == 0);
			Assert(rid1->occurrence == rid2->occurrence);
			Assert(strings_equal_or_both_null(rid1->partnsp, rid2->partnsp));
			Assert(strings_equal_or_both_null(rid1->partrel, rid2->partrel));
			Assert(strings_equal_or_both_null(rid1->plan_name,
											  rid2->plan_name));
		}
	}
#endif
}

/*
 * Convert a bitmapset to a C string of comma-separated integers.
 */
static char *
pgpa_bms_to_cstring(Bitmapset *bms)
{
	StringInfoData buf;
	int			x = -1;

	if (bms_is_empty(bms))
		return "none";

	initStringInfo(&buf);
	while ((x = bms_next_member(bms, x)) >= 0)
	{
		if (buf.len > 0)
			appendStringInfo(&buf, ", %d", x);
		else
			appendStringInfo(&buf, "%d", x);
	}

	return buf.data;
}

/*
 * Convert a JoinType to a C string.
 */
static const char *
pgpa_jointype_to_cstring(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
			return "inner";
		case JOIN_LEFT:
			return "left";
		case JOIN_FULL:
			return "full";
		case JOIN_RIGHT:
			return "right";
		case JOIN_SEMI:
			return "semi";
		case JOIN_ANTI:
			return "anti";
		case JOIN_RIGHT_SEMI:
			return "right semi";
		case JOIN_RIGHT_ANTI:
			return "right anti";
		case JOIN_UNIQUE_OUTER:
			return "unique outer";
		case JOIN_UNIQUE_INNER:
			return "unique inner";
	}
	return "???";
}
