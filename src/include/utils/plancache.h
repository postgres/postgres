/*-------------------------------------------------------------------------
 *
 * plancache.h
 *	  Plan cache definitions.
 *
 * See plancache.c for comments.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/plancache.h,v 1.18 2010/02/26 02:01:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCACHE_H
#define PLANCACHE_H

#include "access/tupdesc.h"
#include "nodes/params.h"

/*
 * CachedPlanSource represents the portion of a cached plan that persists
 * across invalidation/replan cycles.  It stores a raw parse tree (required),
 * the original source text (also required, as of 8.4), and adjunct data.
 *
 * Normally, both the struct itself and the subsidiary data live in the
 * context denoted by the context field, while the linked-to CachedPlan, if
 * any, has its own context.  Thus an invalidated CachedPlan can be dropped
 * when no longer needed, and conversely a CachedPlanSource can be dropped
 * without worrying whether any portals depend on particular instances of
 * its plan.
 *
 * But for entries created by FastCreateCachedPlan, the CachedPlanSource
 * and the initial version of the CachedPlan share the same memory context.
 * In this case, we treat the memory context as belonging to the CachedPlan.
 * The CachedPlanSource has an extra reference-counted link (orig_plan)
 * to the CachedPlan, and the memory context goes away when the CachedPlan's
 * reference count goes to zero.  This arrangement saves overhead for plans
 * that aren't expected to live long enough to need replanning, while not
 * losing any flexibility if a replan turns out to be necessary.
 *
 * Note: the string referenced by commandTag is not subsidiary storage;
 * it is assumed to be a compile-time-constant string.  As with portals,
 * commandTag shall be NULL if and only if the original query string (before
 * rewriting) was an empty string.
 */
typedef struct CachedPlanSource
{
	Node	   *raw_parse_tree; /* output of raw_parser() */
	char	   *query_string;	/* text of query (as of 8.4, never NULL) */
	const char *commandTag;		/* command tag (a constant!), or NULL */
	Oid		   *param_types;	/* array of parameter type OIDs, or NULL */
	int			num_params;		/* length of param_types array */
	ParserSetupHook parserSetup;	/* alternative parameter spec method */
	void	   *parserSetupArg;
	int			cursor_options; /* cursor options used for planning */
	bool		fully_planned;	/* do we cache planner or rewriter output? */
	bool		fixed_result;	/* disallow change in result tupdesc? */
	struct OverrideSearchPath *search_path;		/* saved search_path */
	int			generation;		/* counter, starting at 1, for replans */
	TupleDesc	resultDesc;		/* result type; NULL = doesn't return tuples */
	struct CachedPlan *plan;	/* link to plan, or NULL if not valid */
	MemoryContext context;		/* context containing this CachedPlanSource */
	struct CachedPlan *orig_plan;		/* link to plan owning my context */
} CachedPlanSource;

/*
 * CachedPlan represents the portion of a cached plan that is discarded when
 * invalidation occurs.  The reference count includes both the link(s) from the
 * parent CachedPlanSource, and any active plan executions, so the plan can be
 * discarded exactly when refcount goes to zero.  Both the struct itself and
 * the subsidiary data live in the context denoted by the context field.
 * This makes it easy to free a no-longer-needed cached plan.
 */
typedef struct CachedPlan
{
	List	   *stmt_list;		/* list of statement or Query nodes */
	bool		fully_planned;	/* do we cache planner or rewriter output? */
	bool		dead;			/* if true, do not use */
	TransactionId saved_xmin;	/* if valid, replan when TransactionXmin
								 * changes from this value */
	int			refcount;		/* count of live references to this struct */
	int			generation;		/* counter, starting at 1, for replans */
	MemoryContext context;		/* context containing this CachedPlan */
	/* These fields are used only in the not-fully-planned case: */
	List	   *relationOids;	/* OIDs of relations the stmts depend on */
	List	   *invalItems;		/* other dependencies, as PlanInvalItems */
} CachedPlan;


extern void InitPlanCache(void);
extern CachedPlanSource *CreateCachedPlan(Node *raw_parse_tree,
				 const char *query_string,
				 const char *commandTag,
				 Oid *param_types,
				 int num_params,
				 int cursor_options,
				 List *stmt_list,
				 bool fully_planned,
				 bool fixed_result);
extern CachedPlanSource *FastCreateCachedPlan(Node *raw_parse_tree,
					 char *query_string,
					 const char *commandTag,
					 Oid *param_types,
					 int num_params,
					 int cursor_options,
					 List *stmt_list,
					 bool fully_planned,
					 bool fixed_result,
					 MemoryContext context);
extern void CachedPlanSetParserHook(CachedPlanSource *plansource,
						ParserSetupHook parserSetup,
						void *parserSetupArg);
extern void DropCachedPlan(CachedPlanSource *plansource);
extern CachedPlan *RevalidateCachedPlan(CachedPlanSource *plansource,
					 bool useResOwner);
extern void ReleaseCachedPlan(CachedPlan *plan, bool useResOwner);
extern bool CachedPlanIsValid(CachedPlanSource *plansource);
extern TupleDesc PlanCacheComputeResultDesc(List *stmt_list);

extern void ResetPlanCache(void);

#endif   /* PLANCACHE_H */
