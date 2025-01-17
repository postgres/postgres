/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Post-processing of a completed plan tree: fix references to subplan
 *	  vars, compute regproc values for operators, etc
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/setrefs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "tcop/utility.h"
#include "utils/syscache.h"


typedef enum
{
	NRM_EQUAL,					/* expect exact match of nullingrels */
	NRM_SUBSET,					/* actual Var may have a subset of input */
	NRM_SUPERSET,				/* actual Var may have a superset of input */
} NullingRelsMatch;

typedef struct
{
	int			varno;			/* RT index of Var */
	AttrNumber	varattno;		/* attr number of Var */
	AttrNumber	resno;			/* TLE position of Var */
	Bitmapset  *varnullingrels; /* Var's varnullingrels */
} tlist_vinfo;

typedef struct
{
	List	   *tlist;			/* underlying target list */
	int			num_vars;		/* number of plain Var tlist entries */
	bool		has_ph_vars;	/* are there PlaceHolderVar entries? */
	bool		has_non_vars;	/* are there other entries? */
	tlist_vinfo vars[FLEXIBLE_ARRAY_MEMBER];	/* has num_vars entries */
} indexed_tlist;

typedef struct
{
	PlannerInfo *root;
	int			rtoffset;
	double		num_exec;
} fix_scan_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;
	Index		acceptable_rel;
	int			rtoffset;
	NullingRelsMatch nrm_match;
	double		num_exec;
} fix_join_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *subplan_itlist;
	int			newvarno;
	int			rtoffset;
	NullingRelsMatch nrm_match;
	double		num_exec;
} fix_upper_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *subplan_itlist;
	int			newvarno;
} fix_windowagg_cond_context;

/* Context info for flatten_rtes_walker() */
typedef struct
{
	PlannerGlobal *glob;
	Query	   *query;
} flatten_rtes_walker_context;

/*
 * Selecting the best alternative in an AlternativeSubPlan expression requires
 * estimating how many times that expression will be evaluated.  For an
 * expression in a plan node's targetlist, the plan's estimated number of
 * output rows is clearly what to use, but for an expression in a qual it's
 * far less clear.  Since AlternativeSubPlans aren't heavily used, we don't
 * want to expend a lot of cycles making such estimates.  What we use is twice
 * the number of output rows.  That's not entirely unfounded: we know that
 * clause_selectivity() would fall back to a default selectivity estimate
 * of 0.5 for any SubPlan, so if the qual containing the SubPlan is the last
 * to be applied (which it likely would be, thanks to order_qual_clauses()),
 * this matches what we could have estimated in a far more laborious fashion.
 * Obviously there are many other scenarios, but it's probably not worth the
 * trouble to try to improve on this estimate, especially not when we don't
 * have a better estimate for the selectivity of the SubPlan qual itself.
 */
#define NUM_EXEC_TLIST(parentplan)  ((parentplan)->plan_rows)
#define NUM_EXEC_QUAL(parentplan)   ((parentplan)->plan_rows * 2.0)

/*
 * Check if a Const node is a regclass value.  We accept plain OID too,
 * since a regclass Const will get folded to that type if it's an argument
 * to oideq or similar operators.  (This might result in some extraneous
 * values in a plan's list of relation dependencies, but the worst result
 * would be occasional useless replans.)
 */
#define ISREGCLASSCONST(con) \
	(((con)->consttype == REGCLASSOID || (con)->consttype == OIDOID) && \
	 !(con)->constisnull)

#define fix_scan_list(root, lst, rtoffset, num_exec) \
	((List *) fix_scan_expr(root, (Node *) (lst), rtoffset, num_exec))

static void add_rtes_to_flat_rtable(PlannerInfo *root, bool recursing);
static void flatten_unplanned_rtes(PlannerGlobal *glob, RangeTblEntry *rte);
static bool flatten_rtes_walker(Node *node, flatten_rtes_walker_context *cxt);
static void add_rte_to_flat_rtable(PlannerGlobal *glob, List *rteperminfos,
								   RangeTblEntry *rte);
static Plan *set_plan_refs(PlannerInfo *root, Plan *plan, int rtoffset);
static Plan *set_indexonlyscan_references(PlannerInfo *root,
										  IndexOnlyScan *plan,
										  int rtoffset);
static Plan *set_subqueryscan_references(PlannerInfo *root,
										 SubqueryScan *plan,
										 int rtoffset);
static Plan *clean_up_removed_plan_level(Plan *parent, Plan *child);
static void set_foreignscan_references(PlannerInfo *root,
									   ForeignScan *fscan,
									   int rtoffset);
static void set_customscan_references(PlannerInfo *root,
									  CustomScan *cscan,
									  int rtoffset);
static Plan *set_append_references(PlannerInfo *root,
								   Append *aplan,
								   int rtoffset);
static Plan *set_mergeappend_references(PlannerInfo *root,
										MergeAppend *mplan,
										int rtoffset);
static void set_hash_references(PlannerInfo *root, Plan *plan, int rtoffset);
static Relids offset_relid_set(Relids relids, int rtoffset);
static Node *fix_scan_expr(PlannerInfo *root, Node *node,
						   int rtoffset, double num_exec);
static Node *fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context);
static bool fix_scan_expr_walker(Node *node, fix_scan_expr_context *context);
static void set_join_references(PlannerInfo *root, Join *join, int rtoffset);
static void set_upper_references(PlannerInfo *root, Plan *plan, int rtoffset);
static void set_param_references(PlannerInfo *root, Plan *plan);
static Node *convert_combining_aggrefs(Node *node, void *context);
static void set_dummy_tlist_references(Plan *plan, int rtoffset);
static indexed_tlist *build_tlist_index(List *tlist);
static Var *search_indexed_tlist_for_var(Var *var,
										 indexed_tlist *itlist,
										 int newvarno,
										 int rtoffset,
										 NullingRelsMatch nrm_match);
static Var *search_indexed_tlist_for_phv(PlaceHolderVar *phv,
										 indexed_tlist *itlist,
										 int newvarno,
										 NullingRelsMatch nrm_match);
static Var *search_indexed_tlist_for_non_var(Expr *node,
											 indexed_tlist *itlist,
											 int newvarno);
static Var *search_indexed_tlist_for_sortgroupref(Expr *node,
												  Index sortgroupref,
												  indexed_tlist *itlist,
												  int newvarno);
static List *fix_join_expr(PlannerInfo *root,
						   List *clauses,
						   indexed_tlist *outer_itlist,
						   indexed_tlist *inner_itlist,
						   Index acceptable_rel,
						   int rtoffset,
						   NullingRelsMatch nrm_match,
						   double num_exec);
static Node *fix_join_expr_mutator(Node *node,
								   fix_join_expr_context *context);
static Node *fix_upper_expr(PlannerInfo *root,
							Node *node,
							indexed_tlist *subplan_itlist,
							int newvarno,
							int rtoffset,
							NullingRelsMatch nrm_match,
							double num_exec);
static Node *fix_upper_expr_mutator(Node *node,
									fix_upper_expr_context *context);
static List *set_returning_clause_references(PlannerInfo *root,
											 List *rlist,
											 Plan *topplan,
											 Index resultRelation,
											 int rtoffset);
static List *set_windowagg_runcondition_references(PlannerInfo *root,
												   List *runcondition,
												   Plan *plan);


/*****************************************************************************
 *
 *		SUBPLAN REFERENCES
 *
 *****************************************************************************/

/*
 * set_plan_references
 *
 * This is the final processing pass of the planner/optimizer.  The plan
 * tree is complete; we just have to adjust some representational details
 * for the convenience of the executor:
 *
 * 1. We flatten the various subquery rangetables into a single list, and
 * zero out RangeTblEntry fields that are not useful to the executor.
 *
 * 2. We adjust Vars in scan nodes to be consistent with the flat rangetable.
 *
 * 3. We adjust Vars in upper plan nodes to refer to the outputs of their
 * subplans.
 *
 * 4. Aggrefs in Agg plan nodes need to be adjusted in some cases involving
 * partial aggregation or minmax aggregate optimization.
 *
 * 5. PARAM_MULTIEXPR Params are replaced by regular PARAM_EXEC Params,
 * now that we have finished planning all MULTIEXPR subplans.
 *
 * 6. AlternativeSubPlan expressions are replaced by just one of their
 * alternatives, using an estimate of how many times they'll be executed.
 *
 * 7. We compute regproc OIDs for operators (ie, we look up the function
 * that implements each op).
 *
 * 8. We create lists of specific objects that the plan depends on.
 * This will be used by plancache.c to drive invalidation of cached plans.
 * Relation dependencies are represented by OIDs, and everything else by
 * PlanInvalItems (this distinction is motivated by the shared-inval APIs).
 * Currently, relations, user-defined functions, and domains are the only
 * types of objects that are explicitly tracked this way.
 *
 * 9. We assign every plan node in the tree a unique ID.
 *
 * We also perform one final optimization step, which is to delete
 * SubqueryScan, Append, and MergeAppend plan nodes that aren't doing
 * anything useful.  The reason for doing this last is that
 * it can't readily be done before set_plan_references, because it would
 * break set_upper_references: the Vars in the child plan's top tlist
 * wouldn't match up with the Vars in the outer plan tree.  A SubqueryScan
 * serves a necessary function as a buffer between outer query and subquery
 * variable numbering ... but after we've flattened the rangetable this is
 * no longer a problem, since then there's only one rtindex namespace.
 * Likewise, Append and MergeAppend buffer between the parent and child vars
 * of an appendrel, but we don't need to worry about that once we've done
 * set_plan_references.
 *
 * set_plan_references recursively traverses the whole plan tree.
 *
 * The return value is normally the same Plan node passed in, but can be
 * different when the passed-in Plan is a node we decide isn't needed.
 *
 * The flattened rangetable entries are appended to root->glob->finalrtable.
 * Also, rowmarks entries are appended to root->glob->finalrowmarks, and the
 * RT indexes of ModifyTable result relations to root->glob->resultRelations,
 * and flattened AppendRelInfos are appended to root->glob->appendRelations.
 * Plan dependencies are appended to root->glob->relationOids (for relations)
 * and root->glob->invalItems (for everything else).
 *
 * Notice that we modify Plan nodes in-place, but use expression_tree_mutator
 * to process targetlist and qual expressions.  We can assume that the Plan
 * nodes were just built by the planner and are not multiply referenced, but
 * it's not so safe to assume that for expression tree nodes.
 */
Plan *
set_plan_references(PlannerInfo *root, Plan *plan)
{
	Plan	   *result;
	PlannerGlobal *glob = root->glob;
	int			rtoffset = list_length(glob->finalrtable);
	ListCell   *lc;

	/*
	 * Add all the query's RTEs to the flattened rangetable.  The live ones
	 * will have their rangetable indexes increased by rtoffset.  (Additional
	 * RTEs, not referenced by the Plan tree, might get added after those.)
	 */
	add_rtes_to_flat_rtable(root, false);

	/*
	 * Adjust RT indexes of PlanRowMarks and add to final rowmarks list
	 */
	foreach(lc, root->rowMarks)
	{
		PlanRowMark *rc = lfirst_node(PlanRowMark, lc);
		PlanRowMark *newrc;

		/* flat copy is enough since all fields are scalars */
		newrc = (PlanRowMark *) palloc(sizeof(PlanRowMark));
		memcpy(newrc, rc, sizeof(PlanRowMark));

		/* adjust indexes ... but *not* the rowmarkId */
		newrc->rti += rtoffset;
		newrc->prti += rtoffset;

		glob->finalrowmarks = lappend(glob->finalrowmarks, newrc);
	}

	/*
	 * Adjust RT indexes of AppendRelInfos and add to final appendrels list.
	 * We assume the AppendRelInfos were built during planning and don't need
	 * to be copied.
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);

		/* adjust RT indexes */
		appinfo->parent_relid += rtoffset;
		appinfo->child_relid += rtoffset;

		/*
		 * Rather than adjust the translated_vars entries, just drop 'em.
		 * Neither the executor nor EXPLAIN currently need that data.
		 */
		appinfo->translated_vars = NIL;

		glob->appendRelations = lappend(glob->appendRelations, appinfo);
	}

	/* If needed, create workspace for processing AlternativeSubPlans */
	if (root->hasAlternativeSubPlans)
	{
		root->isAltSubplan = (bool *)
			palloc0(list_length(glob->subplans) * sizeof(bool));
		root->isUsedSubplan = (bool *)
			palloc0(list_length(glob->subplans) * sizeof(bool));
	}

	/* Now fix the Plan tree */
	result = set_plan_refs(root, plan, rtoffset);

	/*
	 * If we have AlternativeSubPlans, it is likely that we now have some
	 * unreferenced subplans in glob->subplans.  To avoid expending cycles on
	 * those subplans later, get rid of them by setting those list entries to
	 * NULL.  (Note: we can't do this immediately upon processing an
	 * AlternativeSubPlan, because there may be multiple copies of the
	 * AlternativeSubPlan, and they can get resolved differently.)
	 */
	if (root->hasAlternativeSubPlans)
	{
		foreach(lc, glob->subplans)
		{
			int			ndx = foreach_current_index(lc);

			/*
			 * If it was used by some AlternativeSubPlan in this query level,
			 * but wasn't selected as best by any AlternativeSubPlan, then we
			 * don't need it.  Do not touch subplans that aren't parts of
			 * AlternativeSubPlans.
			 */
			if (root->isAltSubplan[ndx] && !root->isUsedSubplan[ndx])
				lfirst(lc) = NULL;
		}
	}

	return result;
}

/*
 * Extract RangeTblEntries from the plan's rangetable, and add to flat rtable
 *
 * This can recurse into subquery plans; "recursing" is true if so.
 *
 * This also seems like a good place to add the query's RTEPermissionInfos to
 * the flat rteperminfos.
 */
static void
add_rtes_to_flat_rtable(PlannerInfo *root, bool recursing)
{
	PlannerGlobal *glob = root->glob;
	Index		rti;
	ListCell   *lc;

	/*
	 * Add the query's own RTEs to the flattened rangetable.
	 *
	 * At top level, we must add all RTEs so that their indexes in the
	 * flattened rangetable match up with their original indexes.  When
	 * recursing, we only care about extracting relation RTEs (and subquery
	 * RTEs that were once relation RTEs).
	 */
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (!recursing || rte->rtekind == RTE_RELATION ||
			(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid)))
			add_rte_to_flat_rtable(glob, root->parse->rteperminfos, rte);
	}

	/*
	 * If there are any dead subqueries, they are not referenced in the Plan
	 * tree, so we must add RTEs contained in them to the flattened rtable
	 * separately.  (If we failed to do this, the executor would not perform
	 * expected permission checks for tables mentioned in such subqueries.)
	 *
	 * Note: this pass over the rangetable can't be combined with the previous
	 * one, because that would mess up the numbering of the live RTEs in the
	 * flattened rangetable.
	 */
	rti = 1;
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		/*
		 * We should ignore inheritance-parent RTEs: their contents have been
		 * pulled up into our rangetable already.  Also ignore any subquery
		 * RTEs without matching RelOptInfos, as they likewise have been
		 * pulled up.
		 */
		if (rte->rtekind == RTE_SUBQUERY && !rte->inh &&
			rti < root->simple_rel_array_size)
		{
			RelOptInfo *rel = root->simple_rel_array[rti];

			if (rel != NULL)
			{
				Assert(rel->relid == rti);	/* sanity check on array */

				/*
				 * The subquery might never have been planned at all, if it
				 * was excluded on the basis of self-contradictory constraints
				 * in our query level.  In this case apply
				 * flatten_unplanned_rtes.
				 *
				 * If it was planned but the result rel is dummy, we assume
				 * that it has been omitted from our plan tree (see
				 * set_subquery_pathlist), and recurse to pull up its RTEs.
				 *
				 * Otherwise, it should be represented by a SubqueryScan node
				 * somewhere in our plan tree, and we'll pull up its RTEs when
				 * we process that plan node.
				 *
				 * However, if we're recursing, then we should pull up RTEs
				 * whether the subquery is dummy or not, because we've found
				 * that some upper query level is treating this one as dummy,
				 * and so we won't scan this level's plan tree at all.
				 */
				if (rel->subroot == NULL)
					flatten_unplanned_rtes(glob, rte);
				else if (recursing ||
						 IS_DUMMY_REL(fetch_upper_rel(rel->subroot,
													  UPPERREL_FINAL, NULL)))
					add_rtes_to_flat_rtable(rel->subroot, true);
			}
		}
		rti++;
	}
}

/*
 * Extract RangeTblEntries from a subquery that was never planned at all
 */

static void
flatten_unplanned_rtes(PlannerGlobal *glob, RangeTblEntry *rte)
{
	flatten_rtes_walker_context cxt = {glob, rte->subquery};

	/* Use query_tree_walker to find all RTEs in the parse tree */
	(void) query_tree_walker(rte->subquery,
							 flatten_rtes_walker,
							 &cxt,
							 QTW_EXAMINE_RTES_BEFORE);
}

static bool
flatten_rtes_walker(Node *node, flatten_rtes_walker_context *cxt)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/* As above, we need only save relation RTEs and former relations */
		if (rte->rtekind == RTE_RELATION ||
			(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid)))
			add_rte_to_flat_rtable(cxt->glob, cxt->query->rteperminfos, rte);
		return false;
	}
	if (IsA(node, Query))
	{
		/*
		 * Recurse into subselects.  Must update cxt->query to this query so
		 * that the rtable and rteperminfos correspond with each other.
		 */
		Query	   *save_query = cxt->query;
		bool		result;

		cxt->query = (Query *) node;
		result = query_tree_walker((Query *) node,
								   flatten_rtes_walker,
								   cxt,
								   QTW_EXAMINE_RTES_BEFORE);
		cxt->query = save_query;
		return result;
	}
	return expression_tree_walker(node, flatten_rtes_walker, cxt);
}

/*
 * Add (a copy of) the given RTE to the final rangetable and also the
 * corresponding RTEPermissionInfo, if any, to final rteperminfos.
 *
 * In the flat rangetable, we zero out substructure pointers that are not
 * needed by the executor; this reduces the storage space and copying cost
 * for cached plans.  We keep only the ctename, alias, eref Alias fields,
 * which are needed by EXPLAIN, and perminfoindex which is needed by the
 * executor to fetch the RTE's RTEPermissionInfo.
 */
static void
add_rte_to_flat_rtable(PlannerGlobal *glob, List *rteperminfos,
					   RangeTblEntry *rte)
{
	RangeTblEntry *newrte;

	/* flat copy to duplicate all the scalar fields */
	newrte = (RangeTblEntry *) palloc(sizeof(RangeTblEntry));
	memcpy(newrte, rte, sizeof(RangeTblEntry));

	/* zap unneeded sub-structure */
	newrte->tablesample = NULL;
	newrte->subquery = NULL;
	newrte->joinaliasvars = NIL;
	newrte->joinleftcols = NIL;
	newrte->joinrightcols = NIL;
	newrte->join_using_alias = NULL;
	newrte->functions = NIL;
	newrte->tablefunc = NULL;
	newrte->values_lists = NIL;
	newrte->coltypes = NIL;
	newrte->coltypmods = NIL;
	newrte->colcollations = NIL;
	newrte->groupexprs = NIL;
	newrte->securityQuals = NIL;

	glob->finalrtable = lappend(glob->finalrtable, newrte);

	/*
	 * If it's a plain relation RTE (or a subquery that was once a view
	 * reference), add the relation OID to relationOids.
	 *
	 * We do this even though the RTE might be unreferenced in the plan tree;
	 * this would correspond to cases such as views that were expanded, child
	 * tables that were eliminated by constraint exclusion, etc. Schema
	 * invalidation on such a rel must still force rebuilding of the plan.
	 *
	 * Note we don't bother to avoid making duplicate list entries.  We could,
	 * but it would probably cost more cycles than it would save.
	 */
	if (newrte->rtekind == RTE_RELATION ||
		(newrte->rtekind == RTE_SUBQUERY && OidIsValid(newrte->relid)))
		glob->relationOids = lappend_oid(glob->relationOids, newrte->relid);

	/*
	 * Add a copy of the RTEPermissionInfo, if any, corresponding to this RTE
	 * to the flattened global list.
	 */
	if (rte->perminfoindex > 0)
	{
		RTEPermissionInfo *perminfo;
		RTEPermissionInfo *newperminfo;

		/* Get the existing one from this query's rteperminfos. */
		perminfo = getRTEPermissionInfo(rteperminfos, newrte);

		/*
		 * Add a new one to finalrteperminfos and copy the contents of the
		 * existing one into it.  Note that addRTEPermissionInfo() also
		 * updates newrte->perminfoindex to point to newperminfo in
		 * finalrteperminfos.
		 */
		newrte->perminfoindex = 0;	/* expected by addRTEPermissionInfo() */
		newperminfo = addRTEPermissionInfo(&glob->finalrteperminfos, newrte);
		memcpy(newperminfo, perminfo, sizeof(RTEPermissionInfo));
	}
}

/*
 * set_plan_refs: recurse through the Plan nodes of a single subquery level
 */
static Plan *
set_plan_refs(PlannerInfo *root, Plan *plan, int rtoffset)
{
	ListCell   *l;

	if (plan == NULL)
		return NULL;

	/* Assign this node a unique ID. */
	plan->plan_node_id = root->glob->lastPlanNodeId++;

	/*
	 * Plan-type-specific fixes
	 */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			{
				SeqScan    *splan = (SeqScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_SampleScan:
			{
				SampleScan *splan = (SampleScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tablesample = (TableSampleClause *)
					fix_scan_expr(root, (Node *) splan->tablesample,
								  rtoffset, 1);
			}
			break;
		case T_IndexScan:
			{
				IndexScan  *splan = (IndexScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->indexqual =
					fix_scan_list(root, splan->indexqual,
								  rtoffset, 1);
				splan->indexqualorig =
					fix_scan_list(root, splan->indexqualorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->indexorderby =
					fix_scan_list(root, splan->indexorderby,
								  rtoffset, 1);
				splan->indexorderbyorig =
					fix_scan_list(root, splan->indexorderbyorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *splan = (IndexOnlyScan *) plan;

				return set_indexonlyscan_references(root, splan, rtoffset);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *splan = (BitmapIndexScan *) plan;

				splan->scan.scanrelid += rtoffset;
				/* no need to fix targetlist and qual */
				Assert(splan->scan.plan.targetlist == NIL);
				Assert(splan->scan.plan.qual == NIL);
				splan->indexqual =
					fix_scan_list(root, splan->indexqual, rtoffset, 1);
				splan->indexqualorig =
					fix_scan_list(root, splan->indexqualorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_BitmapHeapScan:
			{
				BitmapHeapScan *splan = (BitmapHeapScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->bitmapqualorig =
					fix_scan_list(root, splan->bitmapqualorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_TidScan:
			{
				TidScan    *splan = (TidScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tidquals =
					fix_scan_list(root, splan->tidquals,
								  rtoffset, 1);
			}
			break;
		case T_TidRangeScan:
			{
				TidRangeScan *splan = (TidRangeScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tidrangequals =
					fix_scan_list(root, splan->tidrangequals,
								  rtoffset, 1);
			}
			break;
		case T_SubqueryScan:
			/* Needs special treatment, see comments below */
			return set_subqueryscan_references(root,
											   (SubqueryScan *) plan,
											   rtoffset);
		case T_FunctionScan:
			{
				FunctionScan *splan = (FunctionScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->functions =
					fix_scan_list(root, splan->functions, rtoffset, 1);
			}
			break;
		case T_TableFuncScan:
			{
				TableFuncScan *splan = (TableFuncScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tablefunc = (TableFunc *)
					fix_scan_expr(root, (Node *) splan->tablefunc,
								  rtoffset, 1);
			}
			break;
		case T_ValuesScan:
			{
				ValuesScan *splan = (ValuesScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->values_lists =
					fix_scan_list(root, splan->values_lists,
								  rtoffset, 1);
			}
			break;
		case T_CteScan:
			{
				CteScan    *splan = (CteScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_NamedTuplestoreScan:
			{
				NamedTuplestoreScan *splan = (NamedTuplestoreScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_WorkTableScan:
			{
				WorkTableScan *splan = (WorkTableScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_ForeignScan:
			set_foreignscan_references(root, (ForeignScan *) plan, rtoffset);
			break;
		case T_CustomScan:
			set_customscan_references(root, (CustomScan *) plan, rtoffset);
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			set_join_references(root, (Join *) plan, rtoffset);
			break;

		case T_Gather:
		case T_GatherMerge:
			{
				set_upper_references(root, plan, rtoffset);
				set_param_references(root, plan);
			}
			break;

		case T_Hash:
			set_hash_references(root, plan, rtoffset);
			break;

		case T_Memoize:
			{
				Memoize    *mplan = (Memoize *) plan;

				/*
				 * Memoize does not evaluate its targetlist.  It just uses the
				 * same targetlist from its outer subnode.
				 */
				set_dummy_tlist_references(plan, rtoffset);

				mplan->param_exprs = fix_scan_list(root, mplan->param_exprs,
												   rtoffset,
												   NUM_EXEC_TLIST(plan));
				break;
			}

		case T_Material:
		case T_Sort:
		case T_IncrementalSort:
		case T_Unique:
		case T_SetOp:

			/*
			 * These plan types don't actually bother to evaluate their
			 * targetlists, because they just return their unmodified input
			 * tuples.  Even though the targetlist won't be used by the
			 * executor, we fix it up for possible use by EXPLAIN (not to
			 * mention ease of debugging --- wrong varnos are very confusing).
			 */
			set_dummy_tlist_references(plan, rtoffset);

			/*
			 * Since these plan types don't check quals either, we should not
			 * find any qual expression attached to them.
			 */
			Assert(plan->qual == NIL);
			break;
		case T_LockRows:
			{
				LockRows   *splan = (LockRows *) plan;

				/*
				 * Like the plan types above, LockRows doesn't evaluate its
				 * tlist or quals.  But we have to fix up the RT indexes in
				 * its rowmarks.
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				foreach(l, splan->rowMarks)
				{
					PlanRowMark *rc = (PlanRowMark *) lfirst(l);

					rc->rti += rtoffset;
					rc->prti += rtoffset;
				}
			}
			break;
		case T_Limit:
			{
				Limit	   *splan = (Limit *) plan;

				/*
				 * Like the plan types above, Limit doesn't evaluate its tlist
				 * or quals.  It does have live expressions for limit/offset,
				 * however; and those cannot contain subplan variable refs, so
				 * fix_scan_expr works for them.
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				splan->limitOffset =
					fix_scan_expr(root, splan->limitOffset, rtoffset, 1);
				splan->limitCount =
					fix_scan_expr(root, splan->limitCount, rtoffset, 1);
			}
			break;
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				/*
				 * If this node is combining partial-aggregation results, we
				 * must convert its Aggrefs to contain references to the
				 * partial-aggregate subexpressions that will be available
				 * from the child plan node.
				 */
				if (DO_AGGSPLIT_COMBINE(agg->aggsplit))
				{
					plan->targetlist = (List *)
						convert_combining_aggrefs((Node *) plan->targetlist,
												  NULL);
					plan->qual = (List *)
						convert_combining_aggrefs((Node *) plan->qual,
												  NULL);
				}

				set_upper_references(root, plan, rtoffset);
			}
			break;
		case T_Group:
			set_upper_references(root, plan, rtoffset);
			break;
		case T_WindowAgg:
			{
				WindowAgg  *wplan = (WindowAgg *) plan;

				/*
				 * Adjust the WindowAgg's run conditions by swapping the
				 * WindowFuncs references out to instead reference the Var in
				 * the scan slot so that when the executor evaluates the
				 * runCondition, it receives the WindowFunc's value from the
				 * slot that the result has just been stored into rather than
				 * evaluating the WindowFunc all over again.
				 */
				wplan->runCondition = set_windowagg_runcondition_references(root,
																			wplan->runCondition,
																			(Plan *) wplan);

				set_upper_references(root, plan, rtoffset);

				/*
				 * Like Limit node limit/offset expressions, WindowAgg has
				 * frame offset expressions, which cannot contain subplan
				 * variable refs, so fix_scan_expr works for them.
				 */
				wplan->startOffset =
					fix_scan_expr(root, wplan->startOffset, rtoffset, 1);
				wplan->endOffset =
					fix_scan_expr(root, wplan->endOffset, rtoffset, 1);
				wplan->runCondition = fix_scan_list(root,
													wplan->runCondition,
													rtoffset,
													NUM_EXEC_TLIST(plan));
				wplan->runConditionOrig = fix_scan_list(root,
														wplan->runConditionOrig,
														rtoffset,
														NUM_EXEC_TLIST(plan));
			}
			break;
		case T_Result:
			{
				Result	   *splan = (Result *) plan;

				/*
				 * Result may or may not have a subplan; if not, it's more
				 * like a scan node than an upper node.
				 */
				if (splan->plan.lefttree != NULL)
					set_upper_references(root, plan, rtoffset);
				else
				{
					/*
					 * The tlist of a childless Result could contain
					 * unresolved ROWID_VAR Vars, in case it's representing a
					 * target relation which is completely empty because of
					 * constraint exclusion.  Replace any such Vars by null
					 * constants, as though they'd been resolved for a leaf
					 * scan node that doesn't support them.  We could have
					 * fix_scan_expr do this, but since the case is only
					 * expected to occur here, it seems safer to special-case
					 * it here and keep the assertions that ROWID_VARs
					 * shouldn't be seen by fix_scan_expr.
					 */
					foreach(l, splan->plan.targetlist)
					{
						TargetEntry *tle = (TargetEntry *) lfirst(l);
						Var		   *var = (Var *) tle->expr;

						if (var && IsA(var, Var) && var->varno == ROWID_VAR)
							tle->expr = (Expr *) makeNullConst(var->vartype,
															   var->vartypmod,
															   var->varcollid);
					}

					splan->plan.targetlist =
						fix_scan_list(root, splan->plan.targetlist,
									  rtoffset, NUM_EXEC_TLIST(plan));
					splan->plan.qual =
						fix_scan_list(root, splan->plan.qual,
									  rtoffset, NUM_EXEC_QUAL(plan));
				}
				/* resconstantqual can't contain any subplan variable refs */
				splan->resconstantqual =
					fix_scan_expr(root, splan->resconstantqual, rtoffset, 1);
			}
			break;
		case T_ProjectSet:
			set_upper_references(root, plan, rtoffset);
			break;
		case T_ModifyTable:
			{
				ModifyTable *splan = (ModifyTable *) plan;
				Plan	   *subplan = outerPlan(splan);

				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);

				splan->withCheckOptionLists =
					fix_scan_list(root, splan->withCheckOptionLists,
								  rtoffset, 1);

				if (splan->returningLists)
				{
					List	   *newRL = NIL;
					ListCell   *lcrl,
							   *lcrr;

					/*
					 * Pass each per-resultrel returningList through
					 * set_returning_clause_references().
					 */
					Assert(list_length(splan->returningLists) == list_length(splan->resultRelations));
					forboth(lcrl, splan->returningLists,
							lcrr, splan->resultRelations)
					{
						List	   *rlist = (List *) lfirst(lcrl);
						Index		resultrel = lfirst_int(lcrr);

						rlist = set_returning_clause_references(root,
																rlist,
																subplan,
																resultrel,
																rtoffset);
						newRL = lappend(newRL, rlist);
					}
					splan->returningLists = newRL;

					/*
					 * Set up the visible plan targetlist as being the same as
					 * the first RETURNING list. This is for the use of
					 * EXPLAIN; the executor won't pay any attention to the
					 * targetlist.  We postpone this step until here so that
					 * we don't have to do set_returning_clause_references()
					 * twice on identical targetlists.
					 */
					splan->plan.targetlist = copyObject(linitial(newRL));
				}

				/*
				 * We treat ModifyTable with ON CONFLICT as a form of 'pseudo
				 * join', where the inner side is the EXCLUDED tuple.
				 * Therefore use fix_join_expr to setup the relevant variables
				 * to INNER_VAR. We explicitly don't create any OUTER_VARs as
				 * those are already used by RETURNING and it seems better to
				 * be non-conflicting.
				 */
				if (splan->onConflictSet)
				{
					indexed_tlist *itlist;

					itlist = build_tlist_index(splan->exclRelTlist);

					splan->onConflictSet =
						fix_join_expr(root, splan->onConflictSet,
									  NULL, itlist,
									  linitial_int(splan->resultRelations),
									  rtoffset, NRM_EQUAL, NUM_EXEC_QUAL(plan));

					splan->onConflictWhere = (Node *)
						fix_join_expr(root, (List *) splan->onConflictWhere,
									  NULL, itlist,
									  linitial_int(splan->resultRelations),
									  rtoffset, NRM_EQUAL, NUM_EXEC_QUAL(plan));

					pfree(itlist);

					splan->exclRelTlist =
						fix_scan_list(root, splan->exclRelTlist, rtoffset, 1);
				}

				/*
				 * The MERGE statement produces the target rows by performing
				 * a right join between the target relation and the source
				 * relation (which could be a plain relation or a subquery).
				 * The INSERT and UPDATE actions of the MERGE statement
				 * require access to the columns from the source relation. We
				 * arrange things so that the source relation attributes are
				 * available as INNER_VAR and the target relation attributes
				 * are available from the scan tuple.
				 */
				if (splan->mergeActionLists != NIL)
				{
					List	   *newMJC = NIL;
					ListCell   *lca,
							   *lcj,
							   *lcr;

					/*
					 * Fix the targetList of individual action nodes so that
					 * the so-called "source relation" Vars are referenced as
					 * INNER_VAR.  Note that for this to work correctly during
					 * execution, the ecxt_innertuple must be set to the tuple
					 * obtained by executing the subplan, which is what
					 * constitutes the "source relation".
					 *
					 * We leave the Vars from the result relation (i.e. the
					 * target relation) unchanged i.e. those Vars would be
					 * picked from the scan slot. So during execution, we must
					 * ensure that ecxt_scantuple is setup correctly to refer
					 * to the tuple from the target relation.
					 */
					indexed_tlist *itlist;

					itlist = build_tlist_index(subplan->targetlist);

					forthree(lca, splan->mergeActionLists,
							 lcj, splan->mergeJoinConditions,
							 lcr, splan->resultRelations)
					{
						List	   *mergeActionList = lfirst(lca);
						Node	   *mergeJoinCondition = lfirst(lcj);
						Index		resultrel = lfirst_int(lcr);

						foreach(l, mergeActionList)
						{
							MergeAction *action = (MergeAction *) lfirst(l);

							/* Fix targetList of each action. */
							action->targetList = fix_join_expr(root,
															   action->targetList,
															   NULL, itlist,
															   resultrel,
															   rtoffset,
															   NRM_EQUAL,
															   NUM_EXEC_TLIST(plan));

							/* Fix quals too. */
							action->qual = (Node *) fix_join_expr(root,
																  (List *) action->qual,
																  NULL, itlist,
																  resultrel,
																  rtoffset,
																  NRM_EQUAL,
																  NUM_EXEC_QUAL(plan));
						}

						/* Fix join condition too. */
						mergeJoinCondition = (Node *)
							fix_join_expr(root,
										  (List *) mergeJoinCondition,
										  NULL, itlist,
										  resultrel,
										  rtoffset,
										  NRM_EQUAL,
										  NUM_EXEC_QUAL(plan));
						newMJC = lappend(newMJC, mergeJoinCondition);
					}
					splan->mergeJoinConditions = newMJC;
				}

				splan->nominalRelation += rtoffset;
				if (splan->rootRelation)
					splan->rootRelation += rtoffset;
				splan->exclRelRTI += rtoffset;

				foreach(l, splan->resultRelations)
				{
					lfirst_int(l) += rtoffset;
				}
				foreach(l, splan->rowMarks)
				{
					PlanRowMark *rc = (PlanRowMark *) lfirst(l);

					rc->rti += rtoffset;
					rc->prti += rtoffset;
				}

				/*
				 * Append this ModifyTable node's final result relation RT
				 * index(es) to the global list for the plan.
				 */
				root->glob->resultRelations =
					list_concat(root->glob->resultRelations,
								splan->resultRelations);
				if (splan->rootRelation)
				{
					root->glob->resultRelations =
						lappend_int(root->glob->resultRelations,
									splan->rootRelation);
				}
			}
			break;
		case T_Append:
			/* Needs special treatment, see comments below */
			return set_append_references(root,
										 (Append *) plan,
										 rtoffset);
		case T_MergeAppend:
			/* Needs special treatment, see comments below */
			return set_mergeappend_references(root,
											  (MergeAppend *) plan,
											  rtoffset);
		case T_RecursiveUnion:
			/* This doesn't evaluate targetlist or check quals either */
			set_dummy_tlist_references(plan, rtoffset);
			Assert(plan->qual == NIL);
			break;
		case T_BitmapAnd:
			{
				BitmapAnd  *splan = (BitmapAnd *) plan;

				/* BitmapAnd works like Append, but has no tlist */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(root,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		case T_BitmapOr:
			{
				BitmapOr   *splan = (BitmapOr *) plan;

				/* BitmapOr works like Append, but has no tlist */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(root,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}

	/*
	 * Now recurse into child plans, if any
	 *
	 * NOTE: it is essential that we recurse into child plans AFTER we set
	 * subplan references in this plan's tlist and quals.  If we did the
	 * reference-adjustments bottom-up, then we would fail to match this
	 * plan's var nodes against the already-modified nodes of the children.
	 */
	plan->lefttree = set_plan_refs(root, plan->lefttree, rtoffset);
	plan->righttree = set_plan_refs(root, plan->righttree, rtoffset);

	return plan;
}

/*
 * set_indexonlyscan_references
 *		Do set_plan_references processing on an IndexOnlyScan
 *
 * This is unlike the handling of a plain IndexScan because we have to
 * convert Vars referencing the heap into Vars referencing the index.
 * We can use the fix_upper_expr machinery for that, by working from a
 * targetlist describing the index columns.
 */
static Plan *
set_indexonlyscan_references(PlannerInfo *root,
							 IndexOnlyScan *plan,
							 int rtoffset)
{
	indexed_tlist *index_itlist;
	List	   *stripped_indextlist;
	ListCell   *lc;

	/*
	 * Vars in the plan node's targetlist, qual, and recheckqual must only
	 * reference columns that the index AM can actually return.  To ensure
	 * this, remove non-returnable columns (which are marked as resjunk) from
	 * the indexed tlist.  We can just drop them because the indexed_tlist
	 * machinery pays attention to TLE resnos, not physical list position.
	 */
	stripped_indextlist = NIL;
	foreach(lc, plan->indextlist)
	{
		TargetEntry *indextle = (TargetEntry *) lfirst(lc);

		if (!indextle->resjunk)
			stripped_indextlist = lappend(stripped_indextlist, indextle);
	}

	index_itlist = build_tlist_index(stripped_indextlist);

	plan->scan.scanrelid += rtoffset;
	plan->scan.plan.targetlist = (List *)
		fix_upper_expr(root,
					   (Node *) plan->scan.plan.targetlist,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_TLIST((Plan *) plan));
	plan->scan.plan.qual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->scan.plan.qual,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL((Plan *) plan));
	plan->recheckqual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->recheckqual,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL((Plan *) plan));
	/* indexqual is already transformed to reference index columns */
	plan->indexqual = fix_scan_list(root, plan->indexqual,
									rtoffset, 1);
	/* indexorderby is already transformed to reference index columns */
	plan->indexorderby = fix_scan_list(root, plan->indexorderby,
									   rtoffset, 1);
	/* indextlist must NOT be transformed to reference index columns */
	plan->indextlist = fix_scan_list(root, plan->indextlist,
									 rtoffset, NUM_EXEC_TLIST((Plan *) plan));

	pfree(index_itlist);

	return (Plan *) plan;
}

/*
 * set_subqueryscan_references
 *		Do set_plan_references processing on a SubqueryScan
 *
 * We try to strip out the SubqueryScan entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_subqueryscan_references(PlannerInfo *root,
							SubqueryScan *plan,
							int rtoffset)
{
	RelOptInfo *rel;
	Plan	   *result;

	/* Need to look up the subquery's RelOptInfo, since we need its subroot */
	rel = find_base_rel(root, plan->scan.scanrelid);

	/* Recursively process the subplan */
	plan->subplan = set_plan_references(rel->subroot, plan->subplan);

	if (trivial_subqueryscan(plan))
	{
		/*
		 * We can omit the SubqueryScan node and just pull up the subplan.
		 */
		result = clean_up_removed_plan_level((Plan *) plan, plan->subplan);
	}
	else
	{
		/*
		 * Keep the SubqueryScan node.  We have to do the processing that
		 * set_plan_references would otherwise have done on it.  Notice we do
		 * not do set_upper_references() here, because a SubqueryScan will
		 * always have been created with correct references to its subplan's
		 * outputs to begin with.
		 */
		plan->scan.scanrelid += rtoffset;
		plan->scan.plan.targetlist =
			fix_scan_list(root, plan->scan.plan.targetlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) plan));
		plan->scan.plan.qual =
			fix_scan_list(root, plan->scan.plan.qual,
						  rtoffset, NUM_EXEC_QUAL((Plan *) plan));

		result = (Plan *) plan;
	}

	return result;
}

/*
 * trivial_subqueryscan
 *		Detect whether a SubqueryScan can be deleted from the plan tree.
 *
 * We can delete it if it has no qual to check and the targetlist just
 * regurgitates the output of the child plan.
 *
 * This can be called from mark_async_capable_plan(), a helper function for
 * create_append_plan(), before set_subqueryscan_references(), to determine
 * triviality of a SubqueryScan that is a child of an Append node.  So we
 * cache the result in the SubqueryScan node to avoid repeated computation.
 *
 * Note: when called from mark_async_capable_plan(), we determine the result
 * before running finalize_plan() on the SubqueryScan node (if needed) and
 * set_plan_references() on the subplan tree, but this would be safe, because
 * 1) finalize_plan() doesn't modify the tlist or quals for the SubqueryScan
 *	  node (or that for any plan node in the subplan tree), and
 * 2) set_plan_references() modifies the tlist for every plan node in the
 *	  subplan tree, but keeps const/resjunk columns as const/resjunk ones and
 *	  preserves the length and order of the tlist, and
 * 3) set_plan_references() might delete the topmost plan node like an Append
 *	  or MergeAppend from the subplan tree and pull up the child plan node,
 *	  but in that case, the tlist for the child plan node exactly matches the
 *	  parent.
 */
bool
trivial_subqueryscan(SubqueryScan *plan)
{
	int			attrno;
	ListCell   *lp,
			   *lc;

	/* We might have detected this already; in which case reuse the result */
	if (plan->scanstatus == SUBQUERY_SCAN_TRIVIAL)
		return true;
	if (plan->scanstatus == SUBQUERY_SCAN_NONTRIVIAL)
		return false;
	Assert(plan->scanstatus == SUBQUERY_SCAN_UNKNOWN);
	/* Initially, mark the SubqueryScan as non-deletable from the plan tree */
	plan->scanstatus = SUBQUERY_SCAN_NONTRIVIAL;

	if (plan->scan.plan.qual != NIL)
		return false;

	if (list_length(plan->scan.plan.targetlist) !=
		list_length(plan->subplan->targetlist))
		return false;			/* tlists not same length */

	attrno = 1;
	forboth(lp, plan->scan.plan.targetlist, lc, plan->subplan->targetlist)
	{
		TargetEntry *ptle = (TargetEntry *) lfirst(lp);
		TargetEntry *ctle = (TargetEntry *) lfirst(lc);

		if (ptle->resjunk != ctle->resjunk)
			return false;		/* tlist doesn't match junk status */

		/*
		 * We accept either a Var referencing the corresponding element of the
		 * subplan tlist, or a Const equaling the subplan element. See
		 * generate_setop_tlist() for motivation.
		 */
		if (ptle->expr && IsA(ptle->expr, Var))
		{
			Var		   *var = (Var *) ptle->expr;

			Assert(var->varno == plan->scan.scanrelid);
			Assert(var->varlevelsup == 0);
			if (var->varattno != attrno)
				return false;	/* out of order */
		}
		else if (ptle->expr && IsA(ptle->expr, Const))
		{
			if (!equal(ptle->expr, ctle->expr))
				return false;
		}
		else
			return false;

		attrno++;
	}

	/* Re-mark the SubqueryScan as deletable from the plan tree */
	plan->scanstatus = SUBQUERY_SCAN_TRIVIAL;

	return true;
}

/*
 * clean_up_removed_plan_level
 *		Do necessary cleanup when we strip out a SubqueryScan, Append, etc
 *
 * We are dropping the "parent" plan in favor of returning just its "child".
 * A few small tweaks are needed.
 */
static Plan *
clean_up_removed_plan_level(Plan *parent, Plan *child)
{
	/*
	 * We have to be sure we don't lose any initplans, so move any that were
	 * attached to the parent plan to the child.  If any are parallel-unsafe,
	 * the child is no longer parallel-safe.  As a cosmetic matter, also add
	 * the initplans' run costs to the child's costs.
	 */
	if (parent->initPlan)
	{
		Cost		initplan_cost;
		bool		unsafe_initplans;

		SS_compute_initplan_cost(parent->initPlan,
								 &initplan_cost, &unsafe_initplans);
		child->startup_cost += initplan_cost;
		child->total_cost += initplan_cost;
		if (unsafe_initplans)
			child->parallel_safe = false;

		/*
		 * Attach plans this way so that parent's initplans are processed
		 * before any pre-existing initplans of the child.  Probably doesn't
		 * matter, but let's preserve the ordering just in case.
		 */
		child->initPlan = list_concat(parent->initPlan,
									  child->initPlan);
	}

	/*
	 * We also have to transfer the parent's column labeling info into the
	 * child, else columns sent to client will be improperly labeled if this
	 * is the topmost plan level.  resjunk and so on may be important too.
	 */
	apply_tlist_labeling(child->targetlist, parent->targetlist);

	return child;
}

/*
 * set_foreignscan_references
 *	   Do set_plan_references processing on a ForeignScan
 */
static void
set_foreignscan_references(PlannerInfo *root,
						   ForeignScan *fscan,
						   int rtoffset)
{
	/* Adjust scanrelid if it's valid */
	if (fscan->scan.scanrelid > 0)
		fscan->scan.scanrelid += rtoffset;

	if (fscan->fdw_scan_tlist != NIL || fscan->scan.scanrelid == 0)
	{
		/*
		 * Adjust tlist, qual, fdw_exprs, fdw_recheck_quals to reference
		 * foreign scan tuple
		 */
		indexed_tlist *itlist = build_tlist_index(fscan->fdw_scan_tlist);

		fscan->scan.plan.targetlist = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->scan.plan.targetlist,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_TLIST((Plan *) fscan));
		fscan->scan.plan.qual = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->scan.plan.qual,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_exprs = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->fdw_exprs,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_recheck_quals = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->fdw_recheck_quals,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) fscan));
		pfree(itlist);
		/* fdw_scan_tlist itself just needs fix_scan_list() adjustments */
		fscan->fdw_scan_tlist =
			fix_scan_list(root, fscan->fdw_scan_tlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) fscan));
	}
	else
	{
		/*
		 * Adjust tlist, qual, fdw_exprs, fdw_recheck_quals in the standard
		 * way
		 */
		fscan->scan.plan.targetlist =
			fix_scan_list(root, fscan->scan.plan.targetlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) fscan));
		fscan->scan.plan.qual =
			fix_scan_list(root, fscan->scan.plan.qual,
						  rtoffset, NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_exprs =
			fix_scan_list(root, fscan->fdw_exprs,
						  rtoffset, NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_recheck_quals =
			fix_scan_list(root, fscan->fdw_recheck_quals,
						  rtoffset, NUM_EXEC_QUAL((Plan *) fscan));
	}

	fscan->fs_relids = offset_relid_set(fscan->fs_relids, rtoffset);
	fscan->fs_base_relids = offset_relid_set(fscan->fs_base_relids, rtoffset);

	/* Adjust resultRelation if it's valid */
	if (fscan->resultRelation > 0)
		fscan->resultRelation += rtoffset;
}

/*
 * set_customscan_references
 *	   Do set_plan_references processing on a CustomScan
 */
static void
set_customscan_references(PlannerInfo *root,
						  CustomScan *cscan,
						  int rtoffset)
{
	ListCell   *lc;

	/* Adjust scanrelid if it's valid */
	if (cscan->scan.scanrelid > 0)
		cscan->scan.scanrelid += rtoffset;

	if (cscan->custom_scan_tlist != NIL || cscan->scan.scanrelid == 0)
	{
		/* Adjust tlist, qual, custom_exprs to reference custom scan tuple */
		indexed_tlist *itlist = build_tlist_index(cscan->custom_scan_tlist);

		cscan->scan.plan.targetlist = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->scan.plan.targetlist,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_TLIST((Plan *) cscan));
		cscan->scan.plan.qual = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->scan.plan.qual,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) cscan));
		cscan->custom_exprs = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->custom_exprs,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) cscan));
		pfree(itlist);
		/* custom_scan_tlist itself just needs fix_scan_list() adjustments */
		cscan->custom_scan_tlist =
			fix_scan_list(root, cscan->custom_scan_tlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) cscan));
	}
	else
	{
		/* Adjust tlist, qual, custom_exprs in the standard way */
		cscan->scan.plan.targetlist =
			fix_scan_list(root, cscan->scan.plan.targetlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) cscan));
		cscan->scan.plan.qual =
			fix_scan_list(root, cscan->scan.plan.qual,
						  rtoffset, NUM_EXEC_QUAL((Plan *) cscan));
		cscan->custom_exprs =
			fix_scan_list(root, cscan->custom_exprs,
						  rtoffset, NUM_EXEC_QUAL((Plan *) cscan));
	}

	/* Adjust child plan-nodes recursively, if needed */
	foreach(lc, cscan->custom_plans)
	{
		lfirst(lc) = set_plan_refs(root, (Plan *) lfirst(lc), rtoffset);
	}

	cscan->custom_relids = offset_relid_set(cscan->custom_relids, rtoffset);
}

/*
 * set_append_references
 *		Do set_plan_references processing on an Append
 *
 * We try to strip out the Append entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_append_references(PlannerInfo *root,
					  Append *aplan,
					  int rtoffset)
{
	ListCell   *l;

	/*
	 * Append, like Sort et al, doesn't actually evaluate its targetlist or
	 * check quals.  If it's got exactly one child plan, then it's not doing
	 * anything useful at all, and we can strip it out.
	 */
	Assert(aplan->plan.qual == NIL);

	/* First, we gotta recurse on the children */
	foreach(l, aplan->appendplans)
	{
		lfirst(l) = set_plan_refs(root, (Plan *) lfirst(l), rtoffset);
	}

	/*
	 * See if it's safe to get rid of the Append entirely.  For this to be
	 * safe, there must be only one child plan and that child plan's parallel
	 * awareness must match the Append's.  The reason for the latter is that
	 * if the Append is parallel aware and the child is not, then the calling
	 * plan may execute the non-parallel aware child multiple times.  (If you
	 * change these rules, update create_append_path to match.)
	 */
	if (list_length(aplan->appendplans) == 1)
	{
		Plan	   *p = (Plan *) linitial(aplan->appendplans);

		if (p->parallel_aware == aplan->plan.parallel_aware)
			return clean_up_removed_plan_level((Plan *) aplan, p);
	}

	/*
	 * Otherwise, clean up the Append as needed.  It's okay to do this after
	 * recursing to the children, because set_dummy_tlist_references doesn't
	 * look at those.
	 */
	set_dummy_tlist_references((Plan *) aplan, rtoffset);

	aplan->apprelids = offset_relid_set(aplan->apprelids, rtoffset);

	if (aplan->part_prune_info)
	{
		foreach(l, aplan->part_prune_info->prune_infos)
		{
			List	   *prune_infos = lfirst(l);
			ListCell   *l2;

			foreach(l2, prune_infos)
			{
				PartitionedRelPruneInfo *pinfo = lfirst(l2);

				pinfo->rtindex += rtoffset;
				pinfo->initial_pruning_steps =
					fix_scan_list(root, pinfo->initial_pruning_steps,
								  rtoffset, 1);
				pinfo->exec_pruning_steps =
					fix_scan_list(root, pinfo->exec_pruning_steps,
								  rtoffset, 1);
			}
		}
	}

	/* We don't need to recurse to lefttree or righttree ... */
	Assert(aplan->plan.lefttree == NULL);
	Assert(aplan->plan.righttree == NULL);

	return (Plan *) aplan;
}

/*
 * set_mergeappend_references
 *		Do set_plan_references processing on a MergeAppend
 *
 * We try to strip out the MergeAppend entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_mergeappend_references(PlannerInfo *root,
						   MergeAppend *mplan,
						   int rtoffset)
{
	ListCell   *l;

	/*
	 * MergeAppend, like Sort et al, doesn't actually evaluate its targetlist
	 * or check quals.  If it's got exactly one child plan, then it's not
	 * doing anything useful at all, and we can strip it out.
	 */
	Assert(mplan->plan.qual == NIL);

	/* First, we gotta recurse on the children */
	foreach(l, mplan->mergeplans)
	{
		lfirst(l) = set_plan_refs(root, (Plan *) lfirst(l), rtoffset);
	}

	/*
	 * See if it's safe to get rid of the MergeAppend entirely.  For this to
	 * be safe, there must be only one child plan and that child plan's
	 * parallel awareness must match the MergeAppend's.  The reason for the
	 * latter is that if the MergeAppend is parallel aware and the child is
	 * not, then the calling plan may execute the non-parallel aware child
	 * multiple times.  (If you change these rules, update
	 * create_merge_append_path to match.)
	 */
	if (list_length(mplan->mergeplans) == 1)
	{
		Plan	   *p = (Plan *) linitial(mplan->mergeplans);

		if (p->parallel_aware == mplan->plan.parallel_aware)
			return clean_up_removed_plan_level((Plan *) mplan, p);
	}

	/*
	 * Otherwise, clean up the MergeAppend as needed.  It's okay to do this
	 * after recursing to the children, because set_dummy_tlist_references
	 * doesn't look at those.
	 */
	set_dummy_tlist_references((Plan *) mplan, rtoffset);

	mplan->apprelids = offset_relid_set(mplan->apprelids, rtoffset);

	if (mplan->part_prune_info)
	{
		foreach(l, mplan->part_prune_info->prune_infos)
		{
			List	   *prune_infos = lfirst(l);
			ListCell   *l2;

			foreach(l2, prune_infos)
			{
				PartitionedRelPruneInfo *pinfo = lfirst(l2);

				pinfo->rtindex += rtoffset;
				pinfo->initial_pruning_steps =
					fix_scan_list(root, pinfo->initial_pruning_steps,
								  rtoffset, 1);
				pinfo->exec_pruning_steps =
					fix_scan_list(root, pinfo->exec_pruning_steps,
								  rtoffset, 1);
			}
		}
	}

	/* We don't need to recurse to lefttree or righttree ... */
	Assert(mplan->plan.lefttree == NULL);
	Assert(mplan->plan.righttree == NULL);

	return (Plan *) mplan;
}

/*
 * set_hash_references
 *	   Do set_plan_references processing on a Hash node
 */
static void
set_hash_references(PlannerInfo *root, Plan *plan, int rtoffset)
{
	Hash	   *hplan = (Hash *) plan;
	Plan	   *outer_plan = plan->lefttree;
	indexed_tlist *outer_itlist;

	/*
	 * Hash's hashkeys are used when feeding tuples into the hashtable,
	 * therefore have them reference Hash's outer plan (which itself is the
	 * inner plan of the HashJoin).
	 */
	outer_itlist = build_tlist_index(outer_plan->targetlist);
	hplan->hashkeys = (List *)
		fix_upper_expr(root,
					   (Node *) hplan->hashkeys,
					   outer_itlist,
					   OUTER_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL(plan));

	/* Hash doesn't project */
	set_dummy_tlist_references(plan, rtoffset);

	/* Hash nodes don't have their own quals */
	Assert(plan->qual == NIL);
}

/*
 * offset_relid_set
 *		Apply rtoffset to the members of a Relids set.
 */
static Relids
offset_relid_set(Relids relids, int rtoffset)
{
	Relids		result = NULL;
	int			rtindex;

	/* If there's no offset to apply, we needn't recompute the value */
	if (rtoffset == 0)
		return relids;
	rtindex = -1;
	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
		result = bms_add_member(result, rtindex + rtoffset);
	return result;
}

/*
 * copyVar
 *		Copy a Var node.
 *
 * fix_scan_expr and friends do this enough times that it's worth having
 * a bespoke routine instead of using the generic copyObject() function.
 */
static inline Var *
copyVar(Var *var)
{
	Var		   *newvar = (Var *) palloc(sizeof(Var));

	*newvar = *var;
	return newvar;
}

/*
 * fix_expr_common
 *		Do generic set_plan_references processing on an expression node
 *
 * This is code that is common to all variants of expression-fixing.
 * We must look up operator opcode info for OpExpr and related nodes,
 * add OIDs from regclass Const nodes into root->glob->relationOids, and
 * add PlanInvalItems for user-defined functions into root->glob->invalItems.
 * We also fill in column index lists for GROUPING() expressions.
 *
 * We assume it's okay to update opcode info in-place.  So this could possibly
 * scribble on the planner's input data structures, but it's OK.
 */
static void
fix_expr_common(PlannerInfo *root, Node *node)
{
	/* We assume callers won't call us on a NULL pointer */
	if (IsA(node, Aggref))
	{
		record_plan_function_dependency(root,
										((Aggref *) node)->aggfnoid);
	}
	else if (IsA(node, WindowFunc))
	{
		record_plan_function_dependency(root,
										((WindowFunc *) node)->winfnoid);
	}
	else if (IsA(node, FuncExpr))
	{
		record_plan_function_dependency(root,
										((FuncExpr *) node)->funcid);
	}
	else if (IsA(node, OpExpr))
	{
		set_opfuncid((OpExpr *) node);
		record_plan_function_dependency(root,
										((OpExpr *) node)->opfuncid);
	}
	else if (IsA(node, DistinctExpr))
	{
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
		record_plan_function_dependency(root,
										((DistinctExpr *) node)->opfuncid);
	}
	else if (IsA(node, NullIfExpr))
	{
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
		record_plan_function_dependency(root,
										((NullIfExpr *) node)->opfuncid);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

		set_sa_opfuncid(saop);
		record_plan_function_dependency(root, saop->opfuncid);

		if (OidIsValid(saop->hashfuncid))
			record_plan_function_dependency(root, saop->hashfuncid);

		if (OidIsValid(saop->negfuncid))
			record_plan_function_dependency(root, saop->negfuncid);
	}
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* Check for regclass reference */
		if (ISREGCLASSCONST(con))
			root->glob->relationOids =
				lappend_oid(root->glob->relationOids,
							DatumGetObjectId(con->constvalue));
	}
	else if (IsA(node, GroupingFunc))
	{
		GroupingFunc *g = (GroupingFunc *) node;
		AttrNumber *grouping_map = root->grouping_map;

		/* If there are no grouping sets, we don't need this. */

		Assert(grouping_map || g->cols == NIL);

		if (grouping_map)
		{
			ListCell   *lc;
			List	   *cols = NIL;

			foreach(lc, g->refs)
			{
				cols = lappend_int(cols, grouping_map[lfirst_int(lc)]);
			}

			Assert(!g->cols || equal(cols, g->cols));

			if (!g->cols)
				g->cols = cols;
		}
	}
}

/*
 * fix_param_node
 *		Do set_plan_references processing on a Param
 *
 * If it's a PARAM_MULTIEXPR, replace it with the appropriate Param from
 * root->multiexpr_params; otherwise no change is needed.
 * Just for paranoia's sake, we make a copy of the node in either case.
 */
static Node *
fix_param_node(PlannerInfo *root, Param *p)
{
	if (p->paramkind == PARAM_MULTIEXPR)
	{
		int			subqueryid = p->paramid >> 16;
		int			colno = p->paramid & 0xFFFF;
		List	   *params;

		if (subqueryid <= 0 ||
			subqueryid > list_length(root->multiexpr_params))
			elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
		params = (List *) list_nth(root->multiexpr_params, subqueryid - 1);
		if (colno <= 0 || colno > list_length(params))
			elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
		return copyObject(list_nth(params, colno - 1));
	}
	return (Node *) copyObject(p);
}

/*
 * fix_alternative_subplan
 *		Do set_plan_references processing on an AlternativeSubPlan
 *
 * Choose one of the alternative implementations and return just that one,
 * discarding the rest of the AlternativeSubPlan structure.
 * Note: caller must still recurse into the result!
 *
 * We don't make any attempt to fix up cost estimates in the parent plan
 * node or higher-level nodes.
 */
static Node *
fix_alternative_subplan(PlannerInfo *root, AlternativeSubPlan *asplan,
						double num_exec)
{
	SubPlan    *bestplan = NULL;
	Cost		bestcost = 0;
	ListCell   *lc;

	/*
	 * Compute the estimated cost of each subplan assuming num_exec
	 * executions, and keep the cheapest one.  In event of exact equality of
	 * estimates, we prefer the later plan; this is a bit arbitrary, but in
	 * current usage it biases us to break ties against fast-start subplans.
	 */
	Assert(asplan->subplans != NIL);

	foreach(lc, asplan->subplans)
	{
		SubPlan    *curplan = (SubPlan *) lfirst(lc);
		Cost		curcost;

		curcost = curplan->startup_cost + num_exec * curplan->per_call_cost;
		if (bestplan == NULL || curcost <= bestcost)
		{
			bestplan = curplan;
			bestcost = curcost;
		}

		/* Also mark all subplans that are in AlternativeSubPlans */
		root->isAltSubplan[curplan->plan_id - 1] = true;
	}

	/* Mark the subplan we selected */
	root->isUsedSubplan[bestplan->plan_id - 1] = true;

	return (Node *) bestplan;
}

/*
 * fix_scan_expr
 *		Do set_plan_references processing on a scan-level expression
 *
 * This consists of incrementing all Vars' varnos by rtoffset,
 * replacing PARAM_MULTIEXPR Params, expanding PlaceHolderVars,
 * replacing Aggref nodes that should be replaced by initplan output Params,
 * choosing the best implementation for AlternativeSubPlans,
 * looking up operator opcode info for OpExpr and related nodes,
 * and adding OIDs from regclass Const nodes into root->glob->relationOids.
 *
 * 'node': the expression to be modified
 * 'rtoffset': how much to increment varnos by
 * 'num_exec': estimated number of executions of expression
 *
 * The expression tree is either copied-and-modified, or modified in-place
 * if that seems safe.
 */
static Node *
fix_scan_expr(PlannerInfo *root, Node *node, int rtoffset, double num_exec)
{
	fix_scan_expr_context context;

	context.root = root;
	context.rtoffset = rtoffset;
	context.num_exec = num_exec;

	if (rtoffset != 0 ||
		root->multiexpr_params != NIL ||
		root->glob->lastPHId != 0 ||
		root->minmax_aggs != NIL ||
		root->hasAlternativeSubPlans)
	{
		return fix_scan_expr_mutator(node, &context);
	}
	else
	{
		/*
		 * If rtoffset == 0, we don't need to change any Vars, and if there
		 * are no MULTIEXPR subqueries then we don't need to replace
		 * PARAM_MULTIEXPR Params, and if there are no placeholders anywhere
		 * we won't need to remove them, and if there are no minmax Aggrefs we
		 * won't need to replace them, and if there are no AlternativeSubPlans
		 * we won't need to remove them.  Then it's OK to just scribble on the
		 * input node tree instead of copying (since the only change, filling
		 * in any unset opfuncid fields, is harmless).  This saves just enough
		 * cycles to be noticeable on trivial queries.
		 */
		(void) fix_scan_expr_walker(node, &context);
		return node;
	}
}

static Node *
fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = copyVar((Var *) node);

		Assert(var->varlevelsup == 0);

		/*
		 * We should not see Vars marked INNER_VAR, OUTER_VAR, or ROWID_VAR.
		 * But an indexqual expression could contain INDEX_VAR Vars.
		 */
		Assert(var->varno != INNER_VAR);
		Assert(var->varno != OUTER_VAR);
		Assert(var->varno != ROWID_VAR);
		if (!IS_SPECIAL_VARNO(var->varno))
			var->varno += context->rtoffset;
		if (var->varnosyn > 0)
			var->varnosyn += context->rtoffset;
		return (Node *) var;
	}
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		/* See if the Aggref should be replaced by a Param */
		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
		{
			/* Make a copy of the Param for paranoia's sake */
			return (Node *) copyObject(aggparam);
		}
		/* If no match, just fall through to process it normally */
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		Assert(!IS_SPECIAL_VARNO(cexpr->cvarno));
		cexpr->cvarno += context->rtoffset;
		return (Node *) cexpr;
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* At scan level, we should always just evaluate the contained expr */
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* XXX can we assert something about phnullingrels? */
		return fix_scan_expr_mutator((Node *) phv->phexpr, context);
	}
	if (IsA(node, AlternativeSubPlan))
		return fix_scan_expr_mutator(fix_alternative_subplan(context->root,
															 (AlternativeSubPlan *) node,
															 context->num_exec),
									 context);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node, fix_scan_expr_mutator, context);
}

static bool
fix_scan_expr_walker(Node *node, fix_scan_expr_context *context)
{
	if (node == NULL)
		return false;
	Assert(!(IsA(node, Var) && ((Var *) node)->varno == ROWID_VAR));
	Assert(!IsA(node, PlaceHolderVar));
	Assert(!IsA(node, AlternativeSubPlan));
	fix_expr_common(context->root, node);
	return expression_tree_walker(node, fix_scan_expr_walker, context);
}

/*
 * set_join_references
 *	  Modify the target list and quals of a join node to reference its
 *	  subplans, by setting the varnos to OUTER_VAR or INNER_VAR and setting
 *	  attno values to the result domain number of either the corresponding
 *	  outer or inner join tuple item.  Also perform opcode lookup for these
 *	  expressions, and add regclass OIDs to root->glob->relationOids.
 */
static void
set_join_references(PlannerInfo *root, Join *join, int rtoffset)
{
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;

	outer_itlist = build_tlist_index(outer_plan->targetlist);
	inner_itlist = build_tlist_index(inner_plan->targetlist);

	/*
	 * First process the joinquals (including merge or hash clauses).  These
	 * are logically below the join so they can always use all values
	 * available from the input tlists.  It's okay to also handle
	 * NestLoopParams now, because those couldn't refer to nullable
	 * subexpressions.
	 */
	join->joinqual = fix_join_expr(root,
								   join->joinqual,
								   outer_itlist,
								   inner_itlist,
								   (Index) 0,
								   rtoffset,
								   NRM_EQUAL,
								   NUM_EXEC_QUAL((Plan *) join));

	/* Now do join-type-specific stuff */
	if (IsA(join, NestLoop))
	{
		NestLoop   *nl = (NestLoop *) join;
		ListCell   *lc;

		foreach(lc, nl->nestParams)
		{
			NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);

			/*
			 * Because we don't reparameterize parameterized paths to match
			 * the outer-join level at which they are used, Vars seen in the
			 * NestLoopParam expression may have nullingrels that are just a
			 * subset of those in the Vars actually available from the outer
			 * side.  (Lateral references can also cause this, as explained in
			 * the comments for identify_current_nestloop_params.)  Not
			 * checking this exactly is a bit grotty, but the work needed to
			 * make things match up perfectly seems well out of proportion to
			 * the value.
			 */
			nlp->paramval = (Var *) fix_upper_expr(root,
												   (Node *) nlp->paramval,
												   outer_itlist,
												   OUTER_VAR,
												   rtoffset,
												   NRM_SUBSET,
												   NUM_EXEC_TLIST(outer_plan));
			/* Check we replaced any PlaceHolderVar with simple Var */
			if (!(IsA(nlp->paramval, Var) &&
				  nlp->paramval->varno == OUTER_VAR))
				elog(ERROR, "NestLoopParam was not reduced to a simple Var");
		}
	}
	else if (IsA(join, MergeJoin))
	{
		MergeJoin  *mj = (MergeJoin *) join;

		mj->mergeclauses = fix_join_expr(root,
										 mj->mergeclauses,
										 outer_itlist,
										 inner_itlist,
										 (Index) 0,
										 rtoffset,
										 NRM_EQUAL,
										 NUM_EXEC_QUAL((Plan *) join));
	}
	else if (IsA(join, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) join;

		hj->hashclauses = fix_join_expr(root,
										hj->hashclauses,
										outer_itlist,
										inner_itlist,
										(Index) 0,
										rtoffset,
										NRM_EQUAL,
										NUM_EXEC_QUAL((Plan *) join));

		/*
		 * HashJoin's hashkeys are used to look for matching tuples from its
		 * outer plan (not the Hash node!) in the hashtable.
		 */
		hj->hashkeys = (List *) fix_upper_expr(root,
											   (Node *) hj->hashkeys,
											   outer_itlist,
											   OUTER_VAR,
											   rtoffset,
											   NRM_EQUAL,
											   NUM_EXEC_QUAL((Plan *) join));
	}

	/*
	 * Now we need to fix up the targetlist and qpqual, which are logically
	 * above the join.  This means that, if it's not an inner join, any Vars
	 * and PHVs appearing here should have nullingrels that include the
	 * effects of the outer join, ie they will have nullingrels equal to the
	 * input Vars' nullingrels plus the bit added by the outer join.  We don't
	 * currently have enough info available here to identify what that should
	 * be, so we just tell fix_join_expr to accept superset nullingrels
	 * matches instead of exact ones.
	 */
	join->plan.targetlist = fix_join_expr(root,
										  join->plan.targetlist,
										  outer_itlist,
										  inner_itlist,
										  (Index) 0,
										  rtoffset,
										  (join->jointype == JOIN_INNER ? NRM_EQUAL : NRM_SUPERSET),
										  NUM_EXEC_TLIST((Plan *) join));
	join->plan.qual = fix_join_expr(root,
									join->plan.qual,
									outer_itlist,
									inner_itlist,
									(Index) 0,
									rtoffset,
									(join->jointype == JOIN_INNER ? NRM_EQUAL : NRM_SUPERSET),
									NUM_EXEC_QUAL((Plan *) join));

	pfree(outer_itlist);
	pfree(inner_itlist);
}

/*
 * set_upper_references
 *	  Update the targetlist and quals of an upper-level plan node
 *	  to refer to the tuples returned by its lefttree subplan.
 *	  Also perform opcode lookup for these expressions, and
 *	  add regclass OIDs to root->glob->relationOids.
 *
 * This is used for single-input plan types like Agg, Group, Result.
 *
 * In most cases, we have to match up individual Vars in the tlist and
 * qual expressions with elements of the subplan's tlist (which was
 * generated by flattening these selfsame expressions, so it should have all
 * the required variables).  There is an important exception, however:
 * depending on where we are in the plan tree, sort/group columns may have
 * been pushed into the subplan tlist unflattened.  If these values are also
 * needed in the output then we want to reference the subplan tlist element
 * rather than recomputing the expression.
 */
static void
set_upper_references(PlannerInfo *root, Plan *plan, int rtoffset)
{
	Plan	   *subplan = plan->lefttree;
	indexed_tlist *subplan_itlist;
	List	   *output_targetlist;
	ListCell   *l;

	subplan_itlist = build_tlist_index(subplan->targetlist);

	/*
	 * If it's a grouping node with grouping sets, any Vars and PHVs appearing
	 * in the targetlist and quals should have nullingrels that include the
	 * effects of the grouping step, ie they will have nullingrels equal to
	 * the input Vars/PHVs' nullingrels plus the RT index of the grouping
	 * step.  In order to perform exact nullingrels matches, we remove the RT
	 * index of the grouping step first.
	 */
	if (IsA(plan, Agg) &&
		root->group_rtindex > 0 &&
		((Agg *) plan)->groupingSets)
	{
		plan->targetlist = (List *)
			remove_nulling_relids((Node *) plan->targetlist,
								  bms_make_singleton(root->group_rtindex),
								  NULL);
		plan->qual = (List *)
			remove_nulling_relids((Node *) plan->qual,
								  bms_make_singleton(root->group_rtindex),
								  NULL);
	}

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *newexpr;

		/* If it's a sort/group item, first try to match by sortref */
		if (tle->ressortgroupref != 0)
		{
			newexpr = (Node *)
				search_indexed_tlist_for_sortgroupref(tle->expr,
													  tle->ressortgroupref,
													  subplan_itlist,
													  OUTER_VAR);
			if (!newexpr)
				newexpr = fix_upper_expr(root,
										 (Node *) tle->expr,
										 subplan_itlist,
										 OUTER_VAR,
										 rtoffset,
										 NRM_EQUAL,
										 NUM_EXEC_TLIST(plan));
		}
		else
			newexpr = fix_upper_expr(root,
									 (Node *) tle->expr,
									 subplan_itlist,
									 OUTER_VAR,
									 rtoffset,
									 NRM_EQUAL,
									 NUM_EXEC_TLIST(plan));
		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newexpr;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	plan->qual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->qual,
					   subplan_itlist,
					   OUTER_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL(plan));

	pfree(subplan_itlist);
}

/*
 * set_param_references
 *	  Initialize the initParam list in Gather or Gather merge node such that
 *	  it contains reference of all the params that needs to be evaluated
 *	  before execution of the node.  It contains the initplan params that are
 *	  being passed to the plan nodes below it.
 */
static void
set_param_references(PlannerInfo *root, Plan *plan)
{
	Assert(IsA(plan, Gather) || IsA(plan, GatherMerge));

	if (plan->lefttree->extParam)
	{
		PlannerInfo *proot;
		Bitmapset  *initSetParam = NULL;
		ListCell   *l;

		for (proot = root; proot != NULL; proot = proot->parent_root)
		{
			foreach(l, proot->init_plans)
			{
				SubPlan    *initsubplan = (SubPlan *) lfirst(l);
				ListCell   *l2;

				foreach(l2, initsubplan->setParam)
				{
					initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
				}
			}
		}

		/*
		 * Remember the list of all external initplan params that are used by
		 * the children of Gather or Gather merge node.
		 */
		if (IsA(plan, Gather))
			((Gather *) plan)->initParam =
				bms_intersect(plan->lefttree->extParam, initSetParam);
		else
			((GatherMerge *) plan)->initParam =
				bms_intersect(plan->lefttree->extParam, initSetParam);
	}
}

/*
 * Recursively scan an expression tree and convert Aggrefs to the proper
 * intermediate form for combining aggregates.  This means (1) replacing each
 * one's argument list with a single argument that is the original Aggref
 * modified to show partial aggregation and (2) changing the upper Aggref to
 * show combining aggregation.
 *
 * After this step, set_upper_references will replace the partial Aggrefs
 * with Vars referencing the lower Agg plan node's outputs, so that the final
 * form seen by the executor is a combining Aggref with a Var as input.
 *
 * It's rather messy to postpone this step until setrefs.c; ideally it'd be
 * done in createplan.c.  The difficulty is that once we modify the Aggref
 * expressions, they will no longer be equal() to their original form and
 * so cross-plan-node-level matches will fail.  So this has to happen after
 * the plan node above the Agg has resolved its subplan references.
 */
static Node *
convert_combining_aggrefs(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *orig_agg = (Aggref *) node;
		Aggref	   *child_agg;
		Aggref	   *parent_agg;

		/* Assert we've not chosen to partial-ize any unsupported cases */
		Assert(orig_agg->aggorder == NIL);
		Assert(orig_agg->aggdistinct == NIL);

		/*
		 * Since aggregate calls can't be nested, we needn't recurse into the
		 * arguments.  But for safety, flat-copy the Aggref node itself rather
		 * than modifying it in-place.
		 */
		child_agg = makeNode(Aggref);
		memcpy(child_agg, orig_agg, sizeof(Aggref));

		/*
		 * For the parent Aggref, we want to copy all the fields of the
		 * original aggregate *except* the args list, which we'll replace
		 * below, and the aggfilter expression, which should be applied only
		 * by the child not the parent.  Rather than explicitly knowing about
		 * all the other fields here, we can momentarily modify child_agg to
		 * provide a suitable source for copyObject.
		 */
		child_agg->args = NIL;
		child_agg->aggfilter = NULL;
		parent_agg = copyObject(child_agg);
		child_agg->args = orig_agg->args;
		child_agg->aggfilter = orig_agg->aggfilter;

		/*
		 * Now, set up child_agg to represent the first phase of partial
		 * aggregation.  For now, assume serialization is required.
		 */
		mark_partial_aggref(child_agg, AGGSPLIT_INITIAL_SERIAL);

		/*
		 * And set up parent_agg to represent the second phase.
		 */
		parent_agg->args = list_make1(makeTargetEntry((Expr *) child_agg,
													  1, NULL, false));
		mark_partial_aggref(parent_agg, AGGSPLIT_FINAL_DESERIAL);

		return (Node *) parent_agg;
	}
	return expression_tree_mutator(node, convert_combining_aggrefs, context);
}

/*
 * set_dummy_tlist_references
 *	  Replace the targetlist of an upper-level plan node with a simple
 *	  list of OUTER_VAR references to its child.
 *
 * This is used for plan types like Sort and Append that don't evaluate
 * their targetlists.  Although the executor doesn't care at all what's in
 * the tlist, EXPLAIN needs it to be realistic.
 *
 * Note: we could almost use set_upper_references() here, but it fails for
 * Append for lack of a lefttree subplan.  Single-purpose code is faster
 * anyway.
 */
static void
set_dummy_tlist_references(Plan *plan, int rtoffset)
{
	List	   *output_targetlist;
	ListCell   *l;

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Var		   *oldvar = (Var *) tle->expr;
		Var		   *newvar;

		/*
		 * As in search_indexed_tlist_for_non_var(), we prefer to keep Consts
		 * as Consts, not Vars referencing Consts.  Here, there's no speed
		 * advantage to be had, but it makes EXPLAIN output look cleaner, and
		 * again it avoids confusing the executor.
		 */
		if (IsA(oldvar, Const))
		{
			/* just reuse the existing TLE node */
			output_targetlist = lappend(output_targetlist, tle);
			continue;
		}

		newvar = makeVar(OUTER_VAR,
						 tle->resno,
						 exprType((Node *) oldvar),
						 exprTypmod((Node *) oldvar),
						 exprCollation((Node *) oldvar),
						 0);
		if (IsA(oldvar, Var) &&
			oldvar->varnosyn > 0)
		{
			newvar->varnosyn = oldvar->varnosyn + rtoffset;
			newvar->varattnosyn = oldvar->varattnosyn;
		}
		else
		{
			newvar->varnosyn = 0;	/* wasn't ever a plain Var */
			newvar->varattnosyn = 0;
		}

		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newvar;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	/* We don't touch plan->qual here */
}


/*
 * build_tlist_index --- build an index data structure for a child tlist
 *
 * In most cases, subplan tlists will be "flat" tlists with only Vars,
 * so we try to optimize that case by extracting information about Vars
 * in advance.  Matching a parent tlist to a child is still an O(N^2)
 * operation, but at least with a much smaller constant factor than plain
 * tlist_member() searches.
 *
 * The result of this function is an indexed_tlist struct to pass to
 * search_indexed_tlist_for_var() and siblings.
 * When done, the indexed_tlist may be freed with a single pfree().
 */
static indexed_tlist *
build_tlist_index(List *tlist)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* Create data structure with enough slots for all tlist entries */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_ph_vars = false;
	itlist->has_non_vars = false;

	/* Find the Vars and fill in the index array */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			vinfo->varno = var->varno;
			vinfo->varattno = var->varattno;
			vinfo->resno = tle->resno;
			vinfo->varnullingrels = var->varnullingrels;
			vinfo++;
		}
		else if (tle->expr && IsA(tle->expr, PlaceHolderVar))
			itlist->has_ph_vars = true;
		else
			itlist->has_non_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * build_tlist_index_other_vars --- build a restricted tlist index
 *
 * This is like build_tlist_index, but we only index tlist entries that
 * are Vars belonging to some rel other than the one specified.  We will set
 * has_ph_vars (allowing PlaceHolderVars to be matched), but not has_non_vars
 * (so nothing other than Vars and PlaceHolderVars can be matched).
 */
static indexed_tlist *
build_tlist_index_other_vars(List *tlist, int ignore_rel)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* Create data structure with enough slots for all tlist entries */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_ph_vars = false;
	itlist->has_non_vars = false;

	/* Find the desired Vars and fill in the index array */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			if (var->varno != ignore_rel)
			{
				vinfo->varno = var->varno;
				vinfo->varattno = var->varattno;
				vinfo->resno = tle->resno;
				vinfo->varnullingrels = var->varnullingrels;
				vinfo++;
			}
		}
		else if (tle->expr && IsA(tle->expr, PlaceHolderVar))
			itlist->has_ph_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * search_indexed_tlist_for_var --- find a Var in an indexed tlist
 *
 * If a match is found, return a copy of the given Var with suitably
 * modified varno/varattno (to wit, newvarno and the resno of the TLE entry).
 * Also ensure that varnosyn is incremented by rtoffset.
 * If no match, return NULL.
 *
 * We cross-check the varnullingrels of the subplan output Var based on
 * nrm_match.  Most call sites should pass NRM_EQUAL indicating we expect
 * an exact match.  However, there are places where we haven't cleaned
 * things up completely, and we have to settle for allowing subset or
 * superset matches.
 */
static Var *
search_indexed_tlist_for_var(Var *var, indexed_tlist *itlist,
							 int newvarno, int rtoffset,
							 NullingRelsMatch nrm_match)
{
	int			varno = var->varno;
	AttrNumber	varattno = var->varattno;
	tlist_vinfo *vinfo;
	int			i;

	vinfo = itlist->vars;
	i = itlist->num_vars;
	while (i-- > 0)
	{
		if (vinfo->varno == varno && vinfo->varattno == varattno)
		{
			/* Found a match */
			Var		   *newvar = copyVar(var);

			/*
			 * Verify that we kept all the nullingrels machinations straight.
			 *
			 * XXX we skip the check for system columns and whole-row Vars.
			 * That's because such Vars might be row identity Vars, which are
			 * generated without any varnullingrels.  It'd be hard to do
			 * otherwise, since they're normally made very early in planning,
			 * when we haven't looked at the jointree yet and don't know which
			 * joins might null such Vars.  Doesn't seem worth the expense to
			 * make them fully valid.  (While it's slightly annoying that we
			 * thereby lose checking for user-written references to such
			 * columns, it seems unlikely that a bug in nullingrels logic
			 * would affect only system columns.)
			 */
			if (!(varattno <= 0 ||
				  (nrm_match == NRM_SUBSET ?
				   bms_is_subset(var->varnullingrels, vinfo->varnullingrels) :
				   nrm_match == NRM_SUPERSET ?
				   bms_is_subset(vinfo->varnullingrels, var->varnullingrels) :
				   bms_equal(vinfo->varnullingrels, var->varnullingrels))))
				elog(ERROR, "wrong varnullingrels %s (expected %s) for Var %d/%d",
					 bmsToString(var->varnullingrels),
					 bmsToString(vinfo->varnullingrels),
					 varno, varattno);

			newvar->varno = newvarno;
			newvar->varattno = vinfo->resno;
			if (newvar->varnosyn > 0)
				newvar->varnosyn += rtoffset;
			return newvar;
		}
		vinfo++;
	}
	return NULL;				/* no match */
}

/*
 * search_indexed_tlist_for_phv --- find a PlaceHolderVar in an indexed tlist
 *
 * If a match is found, return a Var constructed to reference the tlist item.
 * If no match, return NULL.
 *
 * Cross-check phnullingrels as in search_indexed_tlist_for_var.
 *
 * NOTE: it is a waste of time to call this unless itlist->has_ph_vars.
 */
static Var *
search_indexed_tlist_for_phv(PlaceHolderVar *phv,
							 indexed_tlist *itlist, int newvarno,
							 NullingRelsMatch nrm_match)
{
	ListCell   *lc;

	foreach(lc, itlist->tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->expr && IsA(tle->expr, PlaceHolderVar))
		{
			PlaceHolderVar *subphv = (PlaceHolderVar *) tle->expr;
			Var		   *newvar;

			/*
			 * Analogously to search_indexed_tlist_for_var, we match on phid
			 * only.  We don't use equal(), partially for speed but mostly
			 * because phnullingrels might not be exactly equal.
			 */
			if (phv->phid != subphv->phid)
				continue;

			/* Verify that we kept all the nullingrels machinations straight */
			if (!(nrm_match == NRM_SUBSET ?
				  bms_is_subset(phv->phnullingrels, subphv->phnullingrels) :
				  nrm_match == NRM_SUPERSET ?
				  bms_is_subset(subphv->phnullingrels, phv->phnullingrels) :
				  bms_equal(subphv->phnullingrels, phv->phnullingrels)))
				elog(ERROR, "wrong phnullingrels %s (expected %s) for PlaceHolderVar %d",
					 bmsToString(phv->phnullingrels),
					 bmsToString(subphv->phnullingrels),
					 phv->phid);

			/* Found a matching subplan output expression */
			newvar = makeVarFromTargetEntry(newvarno, tle);
			newvar->varnosyn = 0;	/* wasn't ever a plain Var */
			newvar->varattnosyn = 0;
			return newvar;
		}
	}
	return NULL;				/* no match */
}

/*
 * search_indexed_tlist_for_non_var --- find a non-Var/PHV in an indexed tlist
 *
 * If a match is found, return a Var constructed to reference the tlist item.
 * If no match, return NULL.
 *
 * NOTE: it is a waste of time to call this unless itlist->has_non_vars.
 */
static Var *
search_indexed_tlist_for_non_var(Expr *node,
								 indexed_tlist *itlist, int newvarno)
{
	TargetEntry *tle;

	/*
	 * If it's a simple Const, replacing it with a Var is silly, even if there
	 * happens to be an identical Const below; a Var is more expensive to
	 * execute than a Const.  What's more, replacing it could confuse some
	 * places in the executor that expect to see simple Consts for, eg,
	 * dropped columns.
	 */
	if (IsA(node, Const))
		return NULL;

	tle = tlist_member(node, itlist->tlist);
	if (tle)
	{
		/* Found a matching subplan output expression */
		Var		   *newvar;

		newvar = makeVarFromTargetEntry(newvarno, tle);
		newvar->varnosyn = 0;	/* wasn't ever a plain Var */
		newvar->varattnosyn = 0;
		return newvar;
	}
	return NULL;				/* no match */
}

/*
 * search_indexed_tlist_for_sortgroupref --- find a sort/group expression
 *
 * If a match is found, return a Var constructed to reference the tlist item.
 * If no match, return NULL.
 *
 * This is needed to ensure that we select the right subplan TLE in cases
 * where there are multiple textually-equal()-but-volatile sort expressions.
 * And it's also faster than search_indexed_tlist_for_non_var.
 */
static Var *
search_indexed_tlist_for_sortgroupref(Expr *node,
									  Index sortgroupref,
									  indexed_tlist *itlist,
									  int newvarno)
{
	ListCell   *lc;

	foreach(lc, itlist->tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		/*
		 * Usually the equal() check is redundant, but in setop plans it may
		 * not be, since prepunion.c assigns ressortgroupref equal to the
		 * column resno without regard to whether that matches the topmost
		 * level's sortgrouprefs and without regard to whether any implicit
		 * coercions are added in the setop tree.  We might have to clean that
		 * up someday; but for now, just ignore any false matches.
		 */
		if (tle->ressortgroupref == sortgroupref &&
			equal(node, tle->expr))
		{
			/* Found a matching subplan output expression */
			Var		   *newvar;

			newvar = makeVarFromTargetEntry(newvarno, tle);
			newvar->varnosyn = 0;	/* wasn't ever a plain Var */
			newvar->varattnosyn = 0;
			return newvar;
		}
	}
	return NULL;				/* no match */
}

/*
 * fix_join_expr
 *	   Create a new set of targetlist entries or join qual clauses by
 *	   changing the varno/varattno values of variables in the clauses
 *	   to reference target list values from the outer and inner join
 *	   relation target lists.  Also perform opcode lookup and add
 *	   regclass OIDs to root->glob->relationOids.
 *
 * This is used in four different scenarios:
 * 1) a normal join clause, where all the Vars in the clause *must* be
 *	  replaced by OUTER_VAR or INNER_VAR references.  In this case
 *	  acceptable_rel should be zero so that any failure to match a Var will be
 *	  reported as an error.
 * 2) RETURNING clauses, which may contain both Vars of the target relation
 *	  and Vars of other relations. In this case we want to replace the
 *	  other-relation Vars by OUTER_VAR references, while leaving target Vars
 *	  alone. Thus inner_itlist = NULL and acceptable_rel = the ID of the
 *	  target relation should be passed.
 * 3) ON CONFLICT UPDATE SET/WHERE clauses.  Here references to EXCLUDED are
 *	  to be replaced with INNER_VAR references, while leaving target Vars (the
 *	  to-be-updated relation) alone. Correspondingly inner_itlist is to be
 *	  EXCLUDED elements, outer_itlist = NULL and acceptable_rel the target
 *	  relation.
 * 4) MERGE.  In this case, references to the source relation are to be
 *    replaced with INNER_VAR references, leaving Vars of the target
 *    relation (the to-be-modified relation) alone.  So inner_itlist is to be
 *    the source relation elements, outer_itlist = NULL and acceptable_rel
 *    the target relation.
 *
 * 'clauses' is the targetlist or list of join clauses
 * 'outer_itlist' is the indexed target list of the outer join relation,
 *		or NULL
 * 'inner_itlist' is the indexed target list of the inner join relation,
 *		or NULL
 * 'acceptable_rel' is either zero or the rangetable index of a relation
 *		whose Vars may appear in the clause without provoking an error
 * 'rtoffset': how much to increment varnos by
 * 'nrm_match': as for search_indexed_tlist_for_var()
 * 'num_exec': estimated number of executions of expression
 *
 * Returns the new expression tree.  The original clause structure is
 * not modified.
 */
static List *
fix_join_expr(PlannerInfo *root,
			  List *clauses,
			  indexed_tlist *outer_itlist,
			  indexed_tlist *inner_itlist,
			  Index acceptable_rel,
			  int rtoffset,
			  NullingRelsMatch nrm_match,
			  double num_exec)
{
	fix_join_expr_context context;

	context.root = root;
	context.outer_itlist = outer_itlist;
	context.inner_itlist = inner_itlist;
	context.acceptable_rel = acceptable_rel;
	context.rtoffset = rtoffset;
	context.nrm_match = nrm_match;
	context.num_exec = num_exec;
	return (List *) fix_join_expr_mutator((Node *) clauses, &context);
}

static Node *
fix_join_expr_mutator(Node *node, fix_join_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/*
		 * Verify that Vars with non-default varreturningtype only appear in
		 * the RETURNING list, and refer to the target relation.
		 */
		if (var->varreturningtype != VAR_RETURNING_DEFAULT)
		{
			if (context->inner_itlist != NULL ||
				context->outer_itlist == NULL ||
				context->acceptable_rel == 0)
				elog(ERROR, "variable returning old/new found outside RETURNING list");
			if (var->varno != context->acceptable_rel)
				elog(ERROR, "wrong varno %d (expected %d) for variable returning old/new",
					 var->varno, context->acceptable_rel);
		}

		/* Look for the var in the input tlists, first in the outer */
		if (context->outer_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->outer_itlist,
												  OUTER_VAR,
												  context->rtoffset,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}

		/* then in the inner. */
		if (context->inner_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->inner_itlist,
												  INNER_VAR,
												  context->rtoffset,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}

		/* If it's for acceptable_rel, adjust and return it */
		if (var->varno == context->acceptable_rel)
		{
			var = copyVar(var);
			var->varno += context->rtoffset;
			if (var->varnosyn > 0)
				var->varnosyn += context->rtoffset;
			return (Node *) var;
		}

		/* No referent found for Var */
		elog(ERROR, "variable not found in subplan target lists");
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* See if the PlaceHolderVar has bubbled up from a lower plan node */
		if (context->outer_itlist && context->outer_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_phv(phv,
												  context->outer_itlist,
												  OUTER_VAR,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}
		if (context->inner_itlist && context->inner_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_phv(phv,
												  context->inner_itlist,
												  INNER_VAR,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}

		/* If not supplied by input plans, evaluate the contained expr */
		/* XXX can we assert something about phnullingrels? */
		return fix_join_expr_mutator((Node *) phv->phexpr, context);
	}
	/* Try matching more complex expressions too, if tlists have any */
	if (context->outer_itlist && context->outer_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->outer_itlist,
												  OUTER_VAR);
		if (newvar)
			return (Node *) newvar;
	}
	if (context->inner_itlist && context->inner_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->inner_itlist,
												  INNER_VAR);
		if (newvar)
			return (Node *) newvar;
	}
	/* Special cases (apply only AFTER failing to match to lower tlist) */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, AlternativeSubPlan))
		return fix_join_expr_mutator(fix_alternative_subplan(context->root,
															 (AlternativeSubPlan *) node,
															 context->num_exec),
									 context);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node, fix_join_expr_mutator, context);
}

/*
 * fix_upper_expr
 *		Modifies an expression tree so that all Var nodes reference outputs
 *		of a subplan.  Also looks for Aggref nodes that should be replaced
 *		by initplan output Params.  Also performs opcode lookup, and adds
 *		regclass OIDs to root->glob->relationOids.
 *
 * This is used to fix up target and qual expressions of non-join upper-level
 * plan nodes, as well as index-only scan nodes.
 *
 * An error is raised if no matching var can be found in the subplan tlist
 * --- so this routine should only be applied to nodes whose subplans'
 * targetlists were generated by flattening the expressions used in the
 * parent node.
 *
 * If itlist->has_non_vars is true, then we try to match whole subexpressions
 * against elements of the subplan tlist, so that we can avoid recomputing
 * expressions that were already computed by the subplan.  (This is relatively
 * expensive, so we don't want to try it in the common case where the
 * subplan tlist is just a flattened list of Vars.)
 *
 * 'node': the tree to be fixed (a target item or qual)
 * 'subplan_itlist': indexed target list for subplan (or index)
 * 'newvarno': varno to use for Vars referencing tlist elements
 * 'rtoffset': how much to increment varnos by
 * 'nrm_match': as for search_indexed_tlist_for_var()
 * 'num_exec': estimated number of executions of expression
 *
 * The resulting tree is a copy of the original in which all Var nodes have
 * varno = newvarno, varattno = resno of corresponding targetlist element.
 * The original tree is not modified.
 */
static Node *
fix_upper_expr(PlannerInfo *root,
			   Node *node,
			   indexed_tlist *subplan_itlist,
			   int newvarno,
			   int rtoffset,
			   NullingRelsMatch nrm_match,
			   double num_exec)
{
	fix_upper_expr_context context;

	context.root = root;
	context.subplan_itlist = subplan_itlist;
	context.newvarno = newvarno;
	context.rtoffset = rtoffset;
	context.nrm_match = nrm_match;
	context.num_exec = num_exec;
	return fix_upper_expr_mutator(node, &context);
}

static Node *
fix_upper_expr_mutator(Node *node, fix_upper_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		newvar = search_indexed_tlist_for_var(var,
											  context->subplan_itlist,
											  context->newvarno,
											  context->rtoffset,
											  context->nrm_match);
		if (!newvar)
			elog(ERROR, "variable not found in subplan target list");
		return (Node *) newvar;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* See if the PlaceHolderVar has bubbled up from a lower plan node */
		if (context->subplan_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_phv(phv,
												  context->subplan_itlist,
												  context->newvarno,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}
		/* If not supplied by input plan, evaluate the contained expr */
		/* XXX can we assert something about phnullingrels? */
		return fix_upper_expr_mutator((Node *) phv->phexpr, context);
	}
	/* Try matching more complex expressions too, if tlist has any */
	if (context->subplan_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->subplan_itlist,
												  context->newvarno);
		if (newvar)
			return (Node *) newvar;
	}
	/* Special cases (apply only AFTER failing to match to lower tlist) */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		/* See if the Aggref should be replaced by a Param */
		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
		{
			/* Make a copy of the Param for paranoia's sake */
			return (Node *) copyObject(aggparam);
		}
		/* If no match, just fall through to process it normally */
	}
	if (IsA(node, AlternativeSubPlan))
		return fix_upper_expr_mutator(fix_alternative_subplan(context->root,
															  (AlternativeSubPlan *) node,
															  context->num_exec),
									  context);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node, fix_upper_expr_mutator, context);
}

/*
 * set_returning_clause_references
 *		Perform setrefs.c's work on a RETURNING targetlist
 *
 * If the query involves more than just the result table, we have to
 * adjust any Vars that refer to other tables to reference junk tlist
 * entries in the top subplan's targetlist.  Vars referencing the result
 * table should be left alone, however (the executor will evaluate them
 * using the actual heap tuple, after firing triggers if any).  In the
 * adjusted RETURNING list, result-table Vars will have their original
 * varno (plus rtoffset), but Vars for other rels will have varno OUTER_VAR.
 *
 * We also must perform opcode lookup and add regclass OIDs to
 * root->glob->relationOids.
 *
 * 'rlist': the RETURNING targetlist to be fixed
 * 'topplan': the top subplan node that will be just below the ModifyTable
 *		node (note it's not yet passed through set_plan_refs)
 * 'resultRelation': RT index of the associated result relation
 * 'rtoffset': how much to increment varnos by
 *
 * Note: the given 'root' is for the parent query level, not the 'topplan'.
 * This does not matter currently since we only access the dependency-item
 * lists in root->glob, but it would need some hacking if we wanted a root
 * that actually matches the subplan.
 *
 * Note: resultRelation is not yet adjusted by rtoffset.
 */
static List *
set_returning_clause_references(PlannerInfo *root,
								List *rlist,
								Plan *topplan,
								Index resultRelation,
								int rtoffset)
{
	indexed_tlist *itlist;

	/*
	 * We can perform the desired Var fixup by abusing the fix_join_expr
	 * machinery that formerly handled inner indexscan fixup.  We search the
	 * top plan's targetlist for Vars of non-result relations, and use
	 * fix_join_expr to convert RETURNING Vars into references to those tlist
	 * entries, while leaving result-rel Vars as-is.
	 *
	 * PlaceHolderVars will also be sought in the targetlist, but no
	 * more-complex expressions will be.  Note that it is not possible for a
	 * PlaceHolderVar to refer to the result relation, since the result is
	 * never below an outer join.  If that case could happen, we'd have to be
	 * prepared to pick apart the PlaceHolderVar and evaluate its contained
	 * expression instead.
	 */
	itlist = build_tlist_index_other_vars(topplan->targetlist, resultRelation);

	rlist = fix_join_expr(root,
						  rlist,
						  itlist,
						  NULL,
						  resultRelation,
						  rtoffset,
						  NRM_EQUAL,
						  NUM_EXEC_TLIST(topplan));

	pfree(itlist);

	return rlist;
}

/*
 * fix_windowagg_condition_expr_mutator
 *		Mutator function for replacing WindowFuncs with the corresponding Var
 *		in the targetlist which references that WindowFunc.
 */
static Node *
fix_windowagg_condition_expr_mutator(Node *node,
									 fix_windowagg_cond_context *context)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, WindowFunc))
	{
		Var		   *newvar;

		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->subplan_itlist,
												  context->newvarno);
		if (newvar)
			return (Node *) newvar;
		elog(ERROR, "WindowFunc not found in subplan target lists");
	}

	return expression_tree_mutator(node,
								   fix_windowagg_condition_expr_mutator,
								   context);
}

/*
 * fix_windowagg_condition_expr
 *		Converts references in 'runcondition' so that any WindowFunc
 *		references are swapped out for a Var which references the matching
 *		WindowFunc in 'subplan_itlist'.
 */
static List *
fix_windowagg_condition_expr(PlannerInfo *root,
							 List *runcondition,
							 indexed_tlist *subplan_itlist)
{
	fix_windowagg_cond_context context;

	context.root = root;
	context.subplan_itlist = subplan_itlist;
	context.newvarno = 0;

	return (List *) fix_windowagg_condition_expr_mutator((Node *) runcondition,
														 &context);
}

/*
 * set_windowagg_runcondition_references
 *		Converts references in 'runcondition' so that any WindowFunc
 *		references are swapped out for a Var which references the matching
 *		WindowFunc in 'plan' targetlist.
 */
static List *
set_windowagg_runcondition_references(PlannerInfo *root,
									  List *runcondition,
									  Plan *plan)
{
	List	   *newlist;
	indexed_tlist *itlist;

	itlist = build_tlist_index(plan->targetlist);

	newlist = fix_windowagg_condition_expr(root, runcondition, itlist);

	pfree(itlist);

	return newlist;
}

/*
 * find_minmax_agg_replacement_param
 *		If the given Aggref is one that we are optimizing into a subquery
 *		(cf. planagg.c), then return the Param that should replace it.
 *		Else return NULL.
 *
 * This is exported so that SS_finalize_plan can use it before setrefs.c runs.
 * Note that it will not find anything until we have built a Plan from a
 * MinMaxAggPath, as root->minmax_aggs will never be filled otherwise.
 */
Param *
find_minmax_agg_replacement_param(PlannerInfo *root, Aggref *aggref)
{
	if (root->minmax_aggs != NIL &&
		list_length(aggref->args) == 1)
	{
		TargetEntry *curTarget = (TargetEntry *) linitial(aggref->args);
		ListCell   *lc;

		foreach(lc, root->minmax_aggs)
		{
			MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

			if (mminfo->aggfnoid == aggref->aggfnoid &&
				equal(mminfo->target, curTarget->expr))
				return mminfo->param;
		}
	}
	return NULL;
}


/*****************************************************************************
 *					QUERY DEPENDENCY MANAGEMENT
 *****************************************************************************/

/*
 * record_plan_function_dependency
 *		Mark the current plan as depending on a particular function.
 *
 * This is exported so that the function-inlining code can record a
 * dependency on a function that it's removed from the plan tree.
 */
void
record_plan_function_dependency(PlannerInfo *root, Oid funcid)
{
	/*
	 * For performance reasons, we don't bother to track built-in functions;
	 * we just assume they'll never change (or at least not in ways that'd
	 * invalidate plans using them).  For this purpose we can consider a
	 * built-in function to be one with OID less than FirstUnpinnedObjectId.
	 * Note that the OID generator guarantees never to generate such an OID
	 * after startup, even at OID wraparound.
	 */
	if (funcid >= (Oid) FirstUnpinnedObjectId)
	{
		PlanInvalItem *inval_item = makeNode(PlanInvalItem);

		/*
		 * It would work to use any syscache on pg_proc, but the easiest is
		 * PROCOID since we already have the function's OID at hand.  Note
		 * that plancache.c knows we use PROCOID.
		 */
		inval_item->cacheId = PROCOID;
		inval_item->hashValue = GetSysCacheHashValue1(PROCOID,
													  ObjectIdGetDatum(funcid));

		root->glob->invalItems = lappend(root->glob->invalItems, inval_item);
	}
}

/*
 * record_plan_type_dependency
 *		Mark the current plan as depending on a particular type.
 *
 * This is exported so that eval_const_expressions can record a
 * dependency on a domain that it's removed a CoerceToDomain node for.
 *
 * We don't currently need to record dependencies on domains that the
 * plan contains CoerceToDomain nodes for, though that might change in
 * future.  Hence, this isn't actually called in this module, though
 * someday fix_expr_common might call it.
 */
void
record_plan_type_dependency(PlannerInfo *root, Oid typid)
{
	/*
	 * As in record_plan_function_dependency, ignore the possibility that
	 * someone would change a built-in domain.
	 */
	if (typid >= (Oid) FirstUnpinnedObjectId)
	{
		PlanInvalItem *inval_item = makeNode(PlanInvalItem);

		/*
		 * It would work to use any syscache on pg_type, but the easiest is
		 * TYPEOID since we already have the type's OID at hand.  Note that
		 * plancache.c knows we use TYPEOID.
		 */
		inval_item->cacheId = TYPEOID;
		inval_item->hashValue = GetSysCacheHashValue1(TYPEOID,
													  ObjectIdGetDatum(typid));

		root->glob->invalItems = lappend(root->glob->invalItems, inval_item);
	}
}

/*
 * extract_query_dependencies
 *		Given a rewritten, but not yet planned, query or queries
 *		(i.e. a Query node or list of Query nodes), extract dependencies
 *		just as set_plan_references would do.  Also detect whether any
 *		rewrite steps were affected by RLS.
 *
 * This is needed by plancache.c to handle invalidation of cached unplanned
 * queries.
 *
 * Note: this does not go through eval_const_expressions, and hence doesn't
 * reflect its additions of inlined functions and elided CoerceToDomain nodes
 * to the invalItems list.  This is obviously OK for functions, since we'll
 * see them in the original query tree anyway.  For domains, it's OK because
 * we don't care about domains unless they get elided.  That is, a plan might
 * have domain dependencies that the query tree doesn't.
 */
void
extract_query_dependencies(Node *query,
						   List **relationOids,
						   List **invalItems,
						   bool *hasRowSecurity)
{
	PlannerGlobal glob;
	PlannerInfo root;

	/* Make up dummy planner state so we can use this module's machinery */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;
	/* Hack: we use glob.dependsOnRole to collect hasRowSecurity flags */
	glob.dependsOnRole = false;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	(void) extract_query_dependencies_walker(query, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;
	*hasRowSecurity = glob.dependsOnRole;
}

/*
 * Tree walker for extract_query_dependencies.
 *
 * This is exported so that expression_planner_with_deps can call it on
 * simple expressions (post-planning, not before planning, in that case).
 * In that usage, glob.dependsOnRole isn't meaningful, but the relationOids
 * and invalItems lists are added to as needed.
 */
bool
extract_query_dependencies_walker(Node *node, PlannerInfo *context)
{
	if (node == NULL)
		return false;
	Assert(!IsA(node, PlaceHolderVar));
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;

		if (query->commandType == CMD_UTILITY)
		{
			/*
			 * This logic must handle any utility command for which parse
			 * analysis was nontrivial (cf. stmt_requires_parse_analysis).
			 *
			 * Notably, CALL requires its own processing.
			 */
			if (IsA(query->utilityStmt, CallStmt))
			{
				CallStmt   *callstmt = (CallStmt *) query->utilityStmt;

				/* We need not examine funccall, just the transformed exprs */
				(void) extract_query_dependencies_walker((Node *) callstmt->funcexpr,
														 context);
				(void) extract_query_dependencies_walker((Node *) callstmt->outargs,
														 context);
				return false;
			}

			/*
			 * Ignore other utility statements, except those (such as EXPLAIN)
			 * that contain a parsed-but-not-planned query.  For those, we
			 * just need to transfer our attention to the contained query.
			 */
			query = UtilityContainsQuery(query->utilityStmt);
			if (query == NULL)
				return false;
		}

		/* Remember if any Query has RLS quals applied by rewriter */
		if (query->hasRowSecurity)
			context->glob->dependsOnRole = true;

		/* Collect relation OIDs in this Query's rtable */
		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_RELATION ||
				(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid)) ||
				(rte->rtekind == RTE_NAMEDTUPLESTORE && OidIsValid(rte->relid)))
				context->glob->relationOids =
					lappend_oid(context->glob->relationOids, rte->relid);
		}

		/* And recurse into the query's subexpressions */
		return query_tree_walker(query, extract_query_dependencies_walker,
								 context, 0);
	}
	/* Extract function dependencies and check for regclass Consts */
	fix_expr_common(context, node);
	return expression_tree_walker(node, extract_query_dependencies_walker,
								  context);
}
