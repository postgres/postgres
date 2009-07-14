/*-------------------------------------------------------------------------
 *
 * plancache.c
 *	  Plan cache management.
 *
 * We can store a cached plan in either fully-planned format, or just
 * parsed-and-rewritten if the caller wishes to postpone planning until
 * actual parameter values are available.  CachedPlanSource has the same
 * contents either way, but CachedPlan contains a list of PlannedStmts
 * and bare utility statements in the first case, or a list of Query nodes
 * in the second case.
 *
 * The plan cache manager itself is principally responsible for tracking
 * whether cached plans should be invalidated because of schema changes in
 * the objects they depend on.	When (and if) the next demand for a cached
 * plan occurs, the query will be replanned.  Note that this could result
 * in an error, for example if a column referenced by the query is no
 * longer present.	The creator of a cached plan can specify whether it
 * is allowable for the query to change output tupdesc on replan (this
 * could happen with "SELECT *" for example) --- if so, it's up to the
 * caller to notice changes and cope with them.
 *
 * Currently, we track exactly the dependencies of plans on relations and
 * user-defined functions.	On relcache invalidation events or pg_proc
 * syscache invalidation events, we invalidate just those plans that depend
 * on the particular object being modified.  (Note: this scheme assumes
 * that any table modification that requires replanning will generate a
 * relcache inval event.)  We also watch for inval events on certain other
 * system catalogs, such as pg_namespace; but for them, our response is
 * just to invalidate all plans.  We expect updates on those catalogs to
 * be infrequent enough that more-detailed tracking is not worth the effort.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/cache/plancache.c,v 1.27.2.1 2009/07/14 15:37:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/plancache.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planmain.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


static List *cached_plans_list = NIL;

static void StoreCachedPlan(CachedPlanSource *plansource, List *stmt_list,
				MemoryContext plan_context);
static void AcquireExecutorLocks(List *stmt_list, bool acquire);
static void AcquirePlannerLocks(List *stmt_list, bool acquire);
static void ScanQueryForLocks(Query *parsetree, bool acquire);
static bool ScanQueryWalker(Node *node, bool *acquire);
static bool rowmark_member(List *rowMarks, int rt_index);
static bool plan_list_is_transient(List *stmt_list);
static void PlanCacheRelCallback(Datum arg, Oid relid);
static void PlanCacheFuncCallback(Datum arg, int cacheid, ItemPointer tuplePtr);
static void PlanCacheSysCallback(Datum arg, int cacheid, ItemPointer tuplePtr);


/*
 * InitPlanCache: initialize module during InitPostgres.
 *
 * All we need to do is hook into inval.c's callback lists.
 */
void
InitPlanCache(void)
{
	CacheRegisterRelcacheCallback(PlanCacheRelCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(PROCOID, PlanCacheFuncCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(NAMESPACEOID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(OPEROID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(AMOPOPID, PlanCacheSysCallback, (Datum) 0);
}

/*
 * CreateCachedPlan: initially create a plan cache entry.
 *
 * The caller must already have successfully parsed/planned the query;
 * about all that we do here is copy it into permanent storage.
 *
 * raw_parse_tree: output of raw_parser()
 * query_string: original query text (as of PG 8.4, must not be NULL)
 * commandTag: compile-time-constant tag for query, or NULL if empty query
 * param_types: array of parameter type OIDs, or NULL if none
 * num_params: number of parameters
 * cursor_options: options bitmask that was/will be passed to planner
 * stmt_list: list of PlannedStmts/utility stmts, or list of Query trees
 * fully_planned: are we caching planner or rewriter output?
 * fixed_result: TRUE to disallow changes in result tupdesc
 */
CachedPlanSource *
CreateCachedPlan(Node *raw_parse_tree,
				 const char *query_string,
				 const char *commandTag,
				 Oid *param_types,
				 int num_params,
				 int cursor_options,
				 List *stmt_list,
				 bool fully_planned,
				 bool fixed_result)
{
	CachedPlanSource *plansource;
	OverrideSearchPath *search_path;
	MemoryContext source_context;
	MemoryContext oldcxt;

	Assert(query_string != NULL);		/* required as of 8.4 */

	/*
	 * Make a dedicated memory context for the CachedPlanSource and its
	 * subsidiary data.  We expect it can be pretty small.
	 */
	source_context = AllocSetContextCreate(CacheMemoryContext,
										   "CachedPlanSource",
										   ALLOCSET_SMALL_MINSIZE,
										   ALLOCSET_SMALL_INITSIZE,
										   ALLOCSET_SMALL_MAXSIZE);

	/*
	 * Fetch current search_path into new context, but do any recalculation
	 * work required in caller's context.
	 */
	search_path = GetOverrideSearchPath(source_context);

	/*
	 * Create and fill the CachedPlanSource struct within the new context.
	 */
	oldcxt = MemoryContextSwitchTo(source_context);
	plansource = (CachedPlanSource *) palloc(sizeof(CachedPlanSource));
	plansource->raw_parse_tree = copyObject(raw_parse_tree);
	plansource->query_string = pstrdup(query_string);
	plansource->commandTag = commandTag;		/* no copying needed */
	if (num_params > 0)
	{
		plansource->param_types = (Oid *) palloc(num_params * sizeof(Oid));
		memcpy(plansource->param_types, param_types, num_params * sizeof(Oid));
	}
	else
		plansource->param_types = NULL;
	plansource->num_params = num_params;
	plansource->cursor_options = cursor_options;
	plansource->fully_planned = fully_planned;
	plansource->fixed_result = fixed_result;
	plansource->search_path = search_path;
	plansource->generation = 0; /* StoreCachedPlan will increment */
	plansource->resultDesc = PlanCacheComputeResultDesc(stmt_list);
	plansource->plan = NULL;
	plansource->context = source_context;
	plansource->orig_plan = NULL;

	/*
	 * Copy the current output plans into the plancache entry.
	 */
	StoreCachedPlan(plansource, stmt_list, NULL);

	/*
	 * Now we can add the entry to the list of cached plans.  The List nodes
	 * live in CacheMemoryContext.
	 */
	MemoryContextSwitchTo(CacheMemoryContext);

	cached_plans_list = lappend(cached_plans_list, plansource);

	MemoryContextSwitchTo(oldcxt);

	return plansource;
}

/*
 * FastCreateCachedPlan: create a plan cache entry with minimal data copying.
 *
 * For plans that aren't expected to live very long, the copying overhead of
 * CreateCachedPlan is annoying.  We provide this variant entry point in which
 * the caller has already placed all the data in a suitable memory context.
 * The source data and completed plan are in the same context, since this
 * avoids extra copy steps during plan construction.  If the query ever does
 * need replanning, we'll generate a separate new CachedPlan at that time, but
 * the CachedPlanSource and the initial CachedPlan share the caller-provided
 * context and go away together when neither is needed any longer.	(Because
 * the parser and planner generate extra cruft in addition to their real
 * output, this approach means that the context probably contains a bunch of
 * useless junk as well as the useful trees.  Hence, this method is a
 * space-for-time tradeoff, which is worth making for plans expected to be
 * short-lived.)
 *
 * raw_parse_tree, query_string, param_types, and stmt_list must reside in the
 * given context, which must have adequate lifespan (recommendation: make it a
 * child of CacheMemoryContext).  Otherwise the API is the same as
 * CreateCachedPlan.
 */
CachedPlanSource *
FastCreateCachedPlan(Node *raw_parse_tree,
					 char *query_string,
					 const char *commandTag,
					 Oid *param_types,
					 int num_params,
					 int cursor_options,
					 List *stmt_list,
					 bool fully_planned,
					 bool fixed_result,
					 MemoryContext context)
{
	CachedPlanSource *plansource;
	OverrideSearchPath *search_path;
	MemoryContext oldcxt;

	Assert(query_string != NULL);		/* required as of 8.4 */

	/*
	 * Fetch current search_path into given context, but do any recalculation
	 * work required in caller's context.
	 */
	search_path = GetOverrideSearchPath(context);

	/*
	 * Create and fill the CachedPlanSource struct within the given context.
	 */
	oldcxt = MemoryContextSwitchTo(context);
	plansource = (CachedPlanSource *) palloc(sizeof(CachedPlanSource));
	plansource->raw_parse_tree = raw_parse_tree;
	plansource->query_string = query_string;
	plansource->commandTag = commandTag;		/* no copying needed */
	plansource->param_types = param_types;
	plansource->num_params = num_params;
	plansource->cursor_options = cursor_options;
	plansource->fully_planned = fully_planned;
	plansource->fixed_result = fixed_result;
	plansource->search_path = search_path;
	plansource->generation = 0; /* StoreCachedPlan will increment */
	plansource->resultDesc = PlanCacheComputeResultDesc(stmt_list);
	plansource->plan = NULL;
	plansource->context = context;
	plansource->orig_plan = NULL;

	/*
	 * Store the current output plans into the plancache entry.
	 */
	StoreCachedPlan(plansource, stmt_list, context);

	/*
	 * Since the context is owned by the CachedPlan, advance its refcount.
	 */
	plansource->orig_plan = plansource->plan;
	plansource->orig_plan->refcount++;

	/*
	 * Now we can add the entry to the list of cached plans.  The List nodes
	 * live in CacheMemoryContext.
	 */
	MemoryContextSwitchTo(CacheMemoryContext);

	cached_plans_list = lappend(cached_plans_list, plansource);

	MemoryContextSwitchTo(oldcxt);

	return plansource;
}

/*
 * StoreCachedPlan: store a built or rebuilt plan into a plancache entry.
 *
 * Common subroutine for CreateCachedPlan and RevalidateCachedPlan.
 */
static void
StoreCachedPlan(CachedPlanSource *plansource,
				List *stmt_list,
				MemoryContext plan_context)
{
	CachedPlan *plan;
	MemoryContext oldcxt;

	if (plan_context == NULL)
	{
		/*
		 * Make a dedicated memory context for the CachedPlan and its
		 * subsidiary data.  It's probably not going to be large, but just in
		 * case, use the default maxsize parameter.
		 */
		plan_context = AllocSetContextCreate(CacheMemoryContext,
											 "CachedPlan",
											 ALLOCSET_SMALL_MINSIZE,
											 ALLOCSET_SMALL_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Copy supplied data into the new context.
		 */
		oldcxt = MemoryContextSwitchTo(plan_context);

		stmt_list = (List *) copyObject(stmt_list);
	}
	else
	{
		/* Assume subsidiary data is in the given context */
		oldcxt = MemoryContextSwitchTo(plan_context);
	}

	/*
	 * Create and fill the CachedPlan struct within the new context.
	 */
	plan = (CachedPlan *) palloc(sizeof(CachedPlan));
	plan->stmt_list = stmt_list;
	plan->fully_planned = plansource->fully_planned;
	plan->dead = false;
	if (plansource->fully_planned && plan_list_is_transient(stmt_list))
	{
		Assert(TransactionIdIsNormal(TransactionXmin));
		plan->saved_xmin = TransactionXmin;
	}
	else
		plan->saved_xmin = InvalidTransactionId;
	plan->refcount = 1;			/* for the parent's link */
	plan->generation = ++(plansource->generation);
	plan->context = plan_context;
	if (plansource->fully_planned)
	{
		/* Planner already extracted dependencies, we don't have to */
		plan->relationOids = plan->invalItems = NIL;
	}
	else
	{
		/* Use the planner machinery to extract dependencies */
		extract_query_dependencies(stmt_list,
								   &plan->relationOids,
								   &plan->invalItems);
	}

	Assert(plansource->plan == NULL);
	plansource->plan = plan;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * DropCachedPlan: destroy a cached plan.
 *
 * Actually this only destroys the CachedPlanSource: the referenced CachedPlan
 * is released, but not destroyed until its refcount goes to zero.	That
 * handles the situation where DropCachedPlan is called while the plan is
 * still in use.
 */
void
DropCachedPlan(CachedPlanSource *plansource)
{
	/* Validity check that we were given a CachedPlanSource */
	Assert(list_member_ptr(cached_plans_list, plansource));

	/* Remove it from the list */
	cached_plans_list = list_delete_ptr(cached_plans_list, plansource);

	/* Decrement child CachePlan's refcount and drop if no longer needed */
	if (plansource->plan)
		ReleaseCachedPlan(plansource->plan, false);

	/*
	 * If CachedPlanSource has independent storage, just drop it.  Otherwise
	 * decrement the refcount on the CachePlan that owns the storage.
	 */
	if (plansource->orig_plan == NULL)
	{
		/* Remove the CachedPlanSource and all subsidiary data */
		MemoryContextDelete(plansource->context);
	}
	else
	{
		Assert(plansource->context == plansource->orig_plan->context);
		ReleaseCachedPlan(plansource->orig_plan, false);
	}
}

/*
 * RevalidateCachedPlan: prepare for re-use of a previously cached plan.
 *
 * What we do here is re-acquire locks and rebuild the plan if necessary.
 * On return, the plan is valid and we have sufficient locks to begin
 * execution (or planning, if not fully_planned).
 *
 * On return, the refcount of the plan has been incremented; a later
 * ReleaseCachedPlan() call is expected.  The refcount has been reported
 * to the CurrentResourceOwner if useResOwner is true.
 *
 * Note: if any replanning activity is required, the caller's memory context
 * is used for that work.
 */
CachedPlan *
RevalidateCachedPlan(CachedPlanSource *plansource, bool useResOwner)
{
	CachedPlan *plan;

	/* Validity check that we were given a CachedPlanSource */
	Assert(list_member_ptr(cached_plans_list, plansource));

	/*
	 * If the plan currently appears valid, acquire locks on the referenced
	 * objects; then check again.  We need to do it this way to cover the race
	 * condition that an invalidation message arrives before we get the lock.
	 */
	plan = plansource->plan;
	if (plan && !plan->dead)
	{
		/*
		 * Plan must have positive refcount because it is referenced by
		 * plansource; so no need to fear it disappears under us here.
		 */
		Assert(plan->refcount > 0);

		if (plan->fully_planned)
			AcquireExecutorLocks(plan->stmt_list, true);
		else
			AcquirePlannerLocks(plan->stmt_list, true);

		/*
		 * If plan was transient, check to see if TransactionXmin has
		 * advanced, and if so invalidate it.
		 */
		if (!plan->dead &&
			TransactionIdIsValid(plan->saved_xmin) &&
			!TransactionIdEquals(plan->saved_xmin, TransactionXmin))
			plan->dead = true;

		/*
		 * By now, if any invalidation has happened, the inval callback
		 * functions will have marked the plan dead.
		 */
		if (plan->dead)
		{
			/* Ooops, the race case happened.  Release useless locks. */
			if (plan->fully_planned)
				AcquireExecutorLocks(plan->stmt_list, false);
			else
				AcquirePlannerLocks(plan->stmt_list, false);
		}
	}

	/*
	 * If plan has been invalidated, unlink it from the parent and release it.
	 */
	if (plan && plan->dead)
	{
		plansource->plan = NULL;
		ReleaseCachedPlan(plan, false);
		plan = NULL;
	}

	/*
	 * Build a new plan if needed.
	 */
	if (!plan)
	{
		bool		snapshot_set = false;
		List	   *slist;
		TupleDesc	resultDesc;

		/*
		 * Restore the search_path that was in use when the plan was made.
		 * (XXX is there anything else we really need to restore?)
		 */
		PushOverrideSearchPath(plansource->search_path);

		/*
		 * If a snapshot is already set (the normal case), we can just use
		 * that for parsing/planning.  But if it isn't, install one.  Note: no
		 * point in checking whether parse analysis requires a snapshot;
		 * utility commands don't have invalidatable plans, so we'd not get
		 * here for such a command.
		 */
		if (!ActiveSnapshotSet())
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * Run parse analysis and rule rewriting.  The parser tends to
		 * scribble on its input, so we must copy the raw parse tree to
		 * prevent corruption of the cache.  Note that we do not use
		 * parse_analyze_varparams(), assuming that the caller never wants the
		 * parameter types to change from the original values.
		 */
		slist = pg_analyze_and_rewrite(copyObject(plansource->raw_parse_tree),
									   plansource->query_string,
									   plansource->param_types,
									   plansource->num_params);

		if (plansource->fully_planned)
		{
			/*
			 * Generate plans for queries.
			 *
			 * The planner may try to call SPI-using functions, which causes
			 * a problem if we're already inside one.  Rather than expect
			 * all SPI-using code to do SPI_push whenever a replan could
			 * happen, it seems best to take care of the case here.
			 */
			bool	pushed;

			pushed = SPI_push_conditional();

			slist = pg_plan_queries(slist, plansource->cursor_options, NULL);

			SPI_pop_conditional(pushed);
		}

		/*
		 * Check or update the result tupdesc.	XXX should we use a weaker
		 * condition than equalTupleDescs() here?
		 */
		resultDesc = PlanCacheComputeResultDesc(slist);
		if (resultDesc == NULL && plansource->resultDesc == NULL)
		{
			/* OK, doesn't return tuples */
		}
		else if (resultDesc == NULL || plansource->resultDesc == NULL ||
				 !equalTupleDescs(resultDesc, plansource->resultDesc))
		{
			MemoryContext oldcxt;

			/* can we give a better error message? */
			if (plansource->fixed_result)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cached plan must not change result type")));
			oldcxt = MemoryContextSwitchTo(plansource->context);
			if (resultDesc)
				resultDesc = CreateTupleDescCopy(resultDesc);
			if (plansource->resultDesc)
				FreeTupleDesc(plansource->resultDesc);
			plansource->resultDesc = resultDesc;
			MemoryContextSwitchTo(oldcxt);
		}

		/* Release snapshot if we got one */
		if (snapshot_set)
			PopActiveSnapshot();

		/* Now we can restore current search path */
		PopOverrideSearchPath();

		/*
		 * Store the plans into the plancache entry, advancing the generation
		 * count.
		 */
		StoreCachedPlan(plansource, slist, NULL);

		plan = plansource->plan;
	}

	/*
	 * Last step: flag the plan as in use by caller.
	 */
	if (useResOwner)
		ResourceOwnerEnlargePlanCacheRefs(CurrentResourceOwner);
	plan->refcount++;
	if (useResOwner)
		ResourceOwnerRememberPlanCacheRef(CurrentResourceOwner, plan);

	return plan;
}

/*
 * ReleaseCachedPlan: release active use of a cached plan.
 *
 * This decrements the reference count, and frees the plan if the count
 * has thereby gone to zero.  If useResOwner is true, it is assumed that
 * the reference count is managed by the CurrentResourceOwner.
 *
 * Note: useResOwner = false is used for releasing references that are in
 * persistent data structures, such as the parent CachedPlanSource or a
 * Portal.	Transient references should be protected by a resource owner.
 */
void
ReleaseCachedPlan(CachedPlan *plan, bool useResOwner)
{
	if (useResOwner)
		ResourceOwnerForgetPlanCacheRef(CurrentResourceOwner, plan);
	Assert(plan->refcount > 0);
	plan->refcount--;
	if (plan->refcount == 0)
		MemoryContextDelete(plan->context);
}

/*
 * CachedPlanIsValid: test whether the plan within a CachedPlanSource is
 * currently valid (that is, not marked as being in need of revalidation).
 *
 * This result is only trustworthy (ie, free from race conditions) if
 * the caller has acquired locks on all the relations used in the plan.
 */
bool
CachedPlanIsValid(CachedPlanSource *plansource)
{
	CachedPlan *plan;

	/* Validity check that we were given a CachedPlanSource */
	Assert(list_member_ptr(cached_plans_list, plansource));

	plan = plansource->plan;
	if (plan && !plan->dead)
	{
		/*
		 * Plan must have positive refcount because it is referenced by
		 * plansource; so no need to fear it disappears under us here.
		 */
		Assert(plan->refcount > 0);

		/*
		 * Although we don't want to acquire locks here, it still seems useful
		 * to check for expiration of a transient plan.
		 */
		if (TransactionIdIsValid(plan->saved_xmin) &&
			!TransactionIdEquals(plan->saved_xmin, TransactionXmin))
			plan->dead = true;
		else
			return true;
	}

	return false;
}

/*
 * AcquireExecutorLocks: acquire locks needed for execution of a fully-planned
 * cached plan; or release them if acquire is false.
 */
static void
AcquireExecutorLocks(List *stmt_list, bool acquire)
{
	ListCell   *lc1;

	foreach(lc1, stmt_list)
	{
		PlannedStmt *plannedstmt = (PlannedStmt *) lfirst(lc1);
		int			rt_index;
		ListCell   *lc2;

		Assert(!IsA(plannedstmt, Query));
		if (!IsA(plannedstmt, PlannedStmt))
			continue;			/* Ignore utility statements */

		rt_index = 0;
		foreach(lc2, plannedstmt->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);
			LOCKMODE	lockmode;

			rt_index++;

			if (rte->rtekind != RTE_RELATION)
				continue;

			/*
			 * Acquire the appropriate type of lock on each relation OID. Note
			 * that we don't actually try to open the rel, and hence will not
			 * fail if it's been dropped entirely --- we'll just transiently
			 * acquire a non-conflicting lock.
			 */
			if (list_member_int(plannedstmt->resultRelations, rt_index))
				lockmode = RowExclusiveLock;
			else if (rowmark_member(plannedstmt->rowMarks, rt_index))
				lockmode = RowShareLock;
			else
				lockmode = AccessShareLock;

			if (acquire)
				LockRelationOid(rte->relid, lockmode);
			else
				UnlockRelationOid(rte->relid, lockmode);
		}
	}
}

/*
 * AcquirePlannerLocks: acquire locks needed for planning and execution of a
 * not-fully-planned cached plan; or release them if acquire is false.
 *
 * Note that we don't actually try to open the relations, and hence will not
 * fail if one has been dropped entirely --- we'll just transiently acquire
 * a non-conflicting lock.
 */
static void
AcquirePlannerLocks(List *stmt_list, bool acquire)
{
	ListCell   *lc;

	foreach(lc, stmt_list)
	{
		Query	   *query = (Query *) lfirst(lc);

		Assert(IsA(query, Query));
		ScanQueryForLocks(query, acquire);
	}
}

/*
 * ScanQueryForLocks: recursively scan one Query for AcquirePlannerLocks.
 */
static void
ScanQueryForLocks(Query *parsetree, bool acquire)
{
	ListCell   *lc;
	int			rt_index;

	/*
	 * First, process RTEs of the current query level.
	 */
	rt_index = 0;
	foreach(lc, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		LOCKMODE	lockmode;

		rt_index++;
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				/* Acquire or release the appropriate type of lock */
				if (rt_index == parsetree->resultRelation)
					lockmode = RowExclusiveLock;
				else if (rowmark_member(parsetree->rowMarks, rt_index))
					lockmode = RowShareLock;
				else
					lockmode = AccessShareLock;
				if (acquire)
					LockRelationOid(rte->relid, lockmode);
				else
					UnlockRelationOid(rte->relid, lockmode);
				break;

			case RTE_SUBQUERY:
				/* Recurse into subquery-in-FROM */
				ScanQueryForLocks(rte->subquery, acquire);
				break;

			default:
				/* ignore other types of RTEs */
				break;
		}
	}

	/* Recurse into subquery-in-WITH */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		ScanQueryForLocks((Query *) cte->ctequery, acquire);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable and cteList.
	 */
	if (parsetree->hasSubLinks)
	{
		query_tree_walker(parsetree, ScanQueryWalker,
						  (void *) &acquire,
						  QTW_IGNORE_RC_SUBQUERIES);
	}
}

/*
 * Walker to find sublink subqueries for ScanQueryForLocks
 */
static bool
ScanQueryWalker(Node *node, bool *acquire)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		ScanQueryForLocks((Query *) sub->subselect, *acquire);
		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * Do NOT recurse into Query nodes, because ScanQueryForLocks already
	 * processed subselects of subselects for us.
	 */
	return expression_tree_walker(node, ScanQueryWalker,
								  (void *) acquire);
}

/*
 * rowmark_member: check whether an RT index appears in a RowMarkClause list.
 */
static bool
rowmark_member(List *rowMarks, int rt_index)
{
	ListCell   *l;

	foreach(l, rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (rc->rti == rt_index)
			return true;
	}
	return false;
}

/*
 * plan_list_is_transient: check if any of the plans in the list are transient.
 */
static bool
plan_list_is_transient(List *stmt_list)
{
	ListCell   *lc;

	foreach(lc, stmt_list)
	{
		PlannedStmt *plannedstmt = (PlannedStmt *) lfirst(lc);

		if (!IsA(plannedstmt, PlannedStmt))
			continue;			/* Ignore utility statements */

		if (plannedstmt->transientPlan)
			return true;
	}

	return false;
}

/*
 * PlanCacheComputeResultDesc: given a list of either fully-planned statements
 * or Queries, determine the result tupledesc it will produce.	Returns NULL
 * if the execution will not return tuples.
 *
 * Note: the result is created or copied into current memory context.
 */
TupleDesc
PlanCacheComputeResultDesc(List *stmt_list)
{
	Node	   *node;
	Query	   *query;
	PlannedStmt *pstmt;

	switch (ChoosePortalStrategy(stmt_list))
	{
		case PORTAL_ONE_SELECT:
			node = (Node *) linitial(stmt_list);
			if (IsA(node, Query))
			{
				query = (Query *) node;
				return ExecCleanTypeFromTL(query->targetList, false);
			}
			if (IsA(node, PlannedStmt))
			{
				pstmt = (PlannedStmt *) node;
				return ExecCleanTypeFromTL(pstmt->planTree->targetlist, false);
			}
			/* other cases shouldn't happen, but return NULL */
			break;

		case PORTAL_ONE_RETURNING:
			node = PortalListGetPrimaryStmt(stmt_list);
			if (IsA(node, Query))
			{
				query = (Query *) node;
				Assert(query->returningList);
				return ExecCleanTypeFromTL(query->returningList, false);
			}
			if (IsA(node, PlannedStmt))
			{
				pstmt = (PlannedStmt *) node;
				Assert(pstmt->returningLists);
				return ExecCleanTypeFromTL((List *) linitial(pstmt->returningLists), false);
			}
			/* other cases shouldn't happen, but return NULL */
			break;

		case PORTAL_UTIL_SELECT:
			node = (Node *) linitial(stmt_list);
			if (IsA(node, Query))
			{
				query = (Query *) node;
				Assert(query->utilityStmt);
				return UtilityTupleDescriptor(query->utilityStmt);
			}
			/* else it's a bare utility statement */
			return UtilityTupleDescriptor(node);

		case PORTAL_MULTI_QUERY:
			/* will not return tuples */
			break;
	}
	return NULL;
}

/*
 * PlanCacheRelCallback
 *		Relcache inval callback function
 *
 * Invalidate all plans mentioning the given rel, or all plans mentioning
 * any rel at all if relid == InvalidOid.
 */
static void
PlanCacheRelCallback(Datum arg, Oid relid)
{
	ListCell   *lc1;

	foreach(lc1, cached_plans_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc1);
		CachedPlan *plan = plansource->plan;

		/* No work if it's already invalidated */
		if (!plan || plan->dead)
			continue;
		if (plan->fully_planned)
		{
			/* Have to check the per-PlannedStmt relid lists */
			ListCell   *lc2;

			foreach(lc2, plan->stmt_list)
			{
				PlannedStmt *plannedstmt = (PlannedStmt *) lfirst(lc2);

				Assert(!IsA(plannedstmt, Query));
				if (!IsA(plannedstmt, PlannedStmt))
					continue;	/* Ignore utility statements */
				if ((relid == InvalidOid) ? plannedstmt->relationOids != NIL :
					list_member_oid(plannedstmt->relationOids, relid))
				{
					/* Invalidate the plan! */
					plan->dead = true;
					break;		/* out of stmt_list scan */
				}
			}
		}
		else
		{
			/* Otherwise check the single list we built ourselves */
			if ((relid == InvalidOid) ? plan->relationOids != NIL :
				list_member_oid(plan->relationOids, relid))
				plan->dead = true;
		}
	}
}

/*
 * PlanCacheFuncCallback
 *		Syscache inval callback function for PROCOID cache
 *
 * Invalidate all plans mentioning the given catalog entry, or all plans
 * mentioning any member of this cache if tuplePtr == NULL.
 *
 * Note that the coding would support use for multiple caches, but right
 * now only user-defined functions are tracked this way.
 */
static void
PlanCacheFuncCallback(Datum arg, int cacheid, ItemPointer tuplePtr)
{
	ListCell   *lc1;

	foreach(lc1, cached_plans_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc1);
		CachedPlan *plan = plansource->plan;

		/* No work if it's already invalidated */
		if (!plan || plan->dead)
			continue;
		if (plan->fully_planned)
		{
			/* Have to check the per-PlannedStmt inval-item lists */
			ListCell   *lc2;

			foreach(lc2, plan->stmt_list)
			{
				PlannedStmt *plannedstmt = (PlannedStmt *) lfirst(lc2);
				ListCell   *lc3;

				Assert(!IsA(plannedstmt, Query));
				if (!IsA(plannedstmt, PlannedStmt))
					continue;	/* Ignore utility statements */
				foreach(lc3, plannedstmt->invalItems)
				{
					PlanInvalItem *item = (PlanInvalItem *) lfirst(lc3);

					if (item->cacheId != cacheid)
						continue;
					if (tuplePtr == NULL ||
						ItemPointerEquals(tuplePtr, &item->tupleId))
					{
						/* Invalidate the plan! */
						plan->dead = true;
						break;	/* out of invalItems scan */
					}
				}
				if (plan->dead)
					break;		/* out of stmt_list scan */
			}
		}
		else
		{
			/* Otherwise check the single list we built ourselves */
			ListCell   *lc2;

			foreach(lc2, plan->invalItems)
			{
				PlanInvalItem *item = (PlanInvalItem *) lfirst(lc2);

				if (item->cacheId != cacheid)
					continue;
				if (tuplePtr == NULL ||
					ItemPointerEquals(tuplePtr, &item->tupleId))
				{
					/* Invalidate the plan! */
					plan->dead = true;
					break;
				}
			}
		}
	}
}

/*
 * PlanCacheSysCallback
 *		Syscache inval callback function for other caches
 *
 * Just invalidate everything...
 */
static void
PlanCacheSysCallback(Datum arg, int cacheid, ItemPointer tuplePtr)
{
	ResetPlanCache();
}

/*
 * ResetPlanCache: drop all cached plans.
 */
void
ResetPlanCache(void)
{
	ListCell   *lc;

	foreach(lc, cached_plans_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);
		CachedPlan *plan = plansource->plan;

		if (plan)
			plan->dead = true;
	}
}
