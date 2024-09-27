/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/initsplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "parser/analyze.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"

/* These parameters are set by GUC */
int			from_collapse_limit;
int			join_collapse_limit;


/*
 * deconstruct_jointree requires multiple passes over the join tree, because we
 * need to finish computing JoinDomains before we start distributing quals.
 * As long as we have to do that, other information such as the relevant
 * qualscopes might as well be computed in the first pass too.
 *
 * deconstruct_recurse recursively examines the join tree and builds a List
 * (in depth-first traversal order) of JoinTreeItem structs, which are then
 * processed iteratively by deconstruct_distribute.  If there are outer
 * joins, non-degenerate outer join clauses are processed in a third pass
 * deconstruct_distribute_oj_quals.
 *
 * The JoinTreeItem structs themselves can be freed at the end of
 * deconstruct_jointree, but do not modify or free their substructure,
 * as the relid sets may also be pointed to by RestrictInfo and
 * SpecialJoinInfo nodes.
 */
typedef struct JoinTreeItem
{
	/* Fields filled during deconstruct_recurse: */
	Node	   *jtnode;			/* jointree node to examine */
	JoinDomain *jdomain;		/* join domain for its ON/WHERE clauses */
	struct JoinTreeItem *jti_parent;	/* JoinTreeItem for this node's
										 * parent, or NULL if it's the top */
	Relids		qualscope;		/* base+OJ Relids syntactically included in
								 * this jointree node */
	Relids		inner_join_rels;	/* base+OJ Relids syntactically included
									 * in inner joins appearing at or below
									 * this jointree node */
	Relids		left_rels;		/* if join node, Relids of the left side */
	Relids		right_rels;		/* if join node, Relids of the right side */
	Relids		nonnullable_rels;	/* if outer join, Relids of the
									 * non-nullable side */
	/* Fields filled during deconstruct_distribute: */
	SpecialJoinInfo *sjinfo;	/* if outer join, its SpecialJoinInfo */
	List	   *oj_joinclauses; /* outer join quals not yet distributed */
	List	   *lateral_clauses;	/* quals postponed from children due to
									 * lateral references */
} JoinTreeItem;


static void extract_lateral_references(PlannerInfo *root, RelOptInfo *brel,
									   Index rtindex);
static List *deconstruct_recurse(PlannerInfo *root, Node *jtnode,
								 JoinDomain *parent_domain,
								 JoinTreeItem *parent_jtitem,
								 List **item_list);
static void deconstruct_distribute(PlannerInfo *root, JoinTreeItem *jtitem);
static void process_security_barrier_quals(PlannerInfo *root,
										   int rti, JoinTreeItem *jtitem);
static void mark_rels_nulled_by_join(PlannerInfo *root, Index ojrelid,
									 Relids lower_rels);
static SpecialJoinInfo *make_outerjoininfo(PlannerInfo *root,
										   Relids left_rels, Relids right_rels,
										   Relids inner_join_rels,
										   JoinType jointype, Index ojrelid,
										   List *clause);
static void compute_semijoin_info(PlannerInfo *root, SpecialJoinInfo *sjinfo,
								  List *clause);
static void deconstruct_distribute_oj_quals(PlannerInfo *root,
											List *jtitems,
											JoinTreeItem *jtitem);
static void distribute_quals_to_rels(PlannerInfo *root, List *clauses,
									 JoinTreeItem *jtitem,
									 SpecialJoinInfo *sjinfo,
									 Index security_level,
									 Relids qualscope,
									 Relids ojscope,
									 Relids outerjoin_nonnullable,
									 Relids incompatible_relids,
									 bool allow_equivalence,
									 bool has_clone,
									 bool is_clone,
									 List **postponed_oj_qual_list);
static void distribute_qual_to_rels(PlannerInfo *root, Node *clause,
									JoinTreeItem *jtitem,
									SpecialJoinInfo *sjinfo,
									Index security_level,
									Relids qualscope,
									Relids ojscope,
									Relids outerjoin_nonnullable,
									Relids incompatible_relids,
									bool allow_equivalence,
									bool has_clone,
									bool is_clone,
									List **postponed_oj_qual_list);
static bool check_redundant_nullability_qual(PlannerInfo *root, Node *clause);
static Relids get_join_domain_min_rels(PlannerInfo *root, Relids domain_relids);
static void check_mergejoinable(RestrictInfo *restrictinfo);
static void check_hashjoinable(RestrictInfo *restrictinfo);
static void check_memoizable(RestrictInfo *restrictinfo);


/*****************************************************************************
 *
 *	 JOIN TREES
 *
 *****************************************************************************/

/*
 * add_base_rels_to_query
 *
 *	  Scan the query's jointree and create baserel RelOptInfos for all
 *	  the base relations (e.g., table, subquery, and function RTEs)
 *	  appearing in the jointree.
 *
 * The initial invocation must pass root->parse->jointree as the value of
 * jtnode.  Internally, the function recurses through the jointree.
 *
 * At the end of this process, there should be one baserel RelOptInfo for
 * every non-join RTE that is used in the query.  Some of the baserels
 * may be appendrel parents, which will require additional "otherrel"
 * RelOptInfos for their member rels, but those are added later.
 */
void
add_base_rels_to_query(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		(void) build_simple_rel(root, varno, NULL);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			add_base_rels_to_query(root, lfirst(l));
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		add_base_rels_to_query(root, j->larg);
		add_base_rels_to_query(root, j->rarg);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * add_other_rels_to_query
 *	  create "otherrel" RelOptInfos for the children of appendrel baserels
 *
 * At the end of this process, there should be RelOptInfos for all relations
 * that will be scanned by the query.
 */
void
add_other_rels_to_query(PlannerInfo *root)
{
	int			rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		RangeTblEntry *rte = root->simple_rte_array[rti];

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;

		/* Ignore any "otherrels" that were already added. */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		/* If it's marked as inheritable, look for children. */
		if (rte->inh)
			expand_inherited_rtentry(root, rel, rte, rti);
	}
}


/*****************************************************************************
 *
 *	 TARGET LISTS
 *
 *****************************************************************************/

/*
 * build_base_rel_tlists
 *	  Add targetlist entries for each var needed in the query's final tlist
 *	  (and HAVING clause, if any) to the appropriate base relations.
 *
 * We mark such vars as needed by "relation 0" to ensure that they will
 * propagate up through all join plan steps.
 */
void
build_base_rel_tlists(PlannerInfo *root, List *final_tlist)
{
	List	   *tlist_vars = pull_var_clause((Node *) final_tlist,
											 PVC_RECURSE_AGGREGATES |
											 PVC_RECURSE_WINDOWFUNCS |
											 PVC_INCLUDE_PLACEHOLDERS);

	if (tlist_vars != NIL)
	{
		add_vars_to_targetlist(root, tlist_vars, bms_make_singleton(0));
		list_free(tlist_vars);
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.  Note
	 * that HAVING can contain Aggrefs but not WindowFuncs.
	 */
	if (root->parse->havingQual)
	{
		List	   *having_vars = pull_var_clause(root->parse->havingQual,
												  PVC_RECURSE_AGGREGATES |
												  PVC_INCLUDE_PLACEHOLDERS);

		if (having_vars != NIL)
		{
			add_vars_to_targetlist(root, having_vars,
								   bms_make_singleton(0));
			list_free(having_vars);
		}
	}
}

/*
 * add_vars_to_targetlist
 *	  For each variable appearing in the list, add it to the owning
 *	  relation's targetlist if not already present, and mark the variable
 *	  as being needed for the indicated join (or for final output if
 *	  where_needed includes "relation 0").
 *
 *	  The list may also contain PlaceHolderVars.  These don't necessarily
 *	  have a single owning relation; we keep their attr_needed info in
 *	  root->placeholder_list instead.  Find or create the associated
 *	  PlaceHolderInfo entry, and update its ph_needed.
 *
 *	  See also add_vars_to_attr_needed.
 */
void
add_vars_to_targetlist(PlannerInfo *root, List *vars,
					   Relids where_needed)
{
	ListCell   *temp;

	Assert(!bms_is_empty(where_needed));

	foreach(temp, vars)
	{
		Node	   *node = (Node *) lfirst(temp);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RelOptInfo *rel = find_base_rel(root, var->varno);
			int			attno = var->varattno;

			if (bms_is_subset(where_needed, rel->relids))
				continue;
			Assert(attno >= rel->min_attr && attno <= rel->max_attr);
			attno -= rel->min_attr;
			if (rel->attr_needed[attno] == NULL)
			{
				/*
				 * Variable not yet requested, so add to rel's targetlist.
				 *
				 * The value available at the rel's scan level has not been
				 * nulled by any outer join, so drop its varnullingrels.
				 * (We'll put those back as we climb up the join tree.)
				 */
				var = copyObject(var);
				var->varnullingrels = NULL;
				rel->reltarget->exprs = lappend(rel->reltarget->exprs, var);
				/* reltarget cost and width will be computed later */
			}
			rel->attr_needed[attno] = bms_add_members(rel->attr_needed[attno],
													  where_needed);
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

			phinfo->ph_needed = bms_add_members(phinfo->ph_needed,
												where_needed);
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
}

/*
 * add_vars_to_attr_needed
 *	  This does a subset of what add_vars_to_targetlist does: it just
 *	  updates attr_needed for Vars and ph_needed for PlaceHolderVars.
 *	  We assume the Vars are already in their relations' targetlists.
 *
 *	  This is used to rebuild attr_needed/ph_needed sets after removal
 *	  of a useless outer join.  The removed join clause might have been
 *	  the only upper-level use of some other relation's Var, in which
 *	  case we can reduce that Var's attr_needed and thereby possibly
 *	  open the door to further join removals.  But we can't tell that
 *	  without tedious reconstruction of the attr_needed data.
 *
 *	  Note that if a Var's attr_needed is successfully reduced to empty,
 *	  it will still be in the relation's targetlist even though we do
 *	  not really need the scan plan node to emit it.  The extra plan
 *	  inefficiency seems tiny enough to not be worth spending planner
 *	  cycles to get rid of it.
 */
void
add_vars_to_attr_needed(PlannerInfo *root, List *vars,
						Relids where_needed)
{
	ListCell   *temp;

	Assert(!bms_is_empty(where_needed));

	foreach(temp, vars)
	{
		Node	   *node = (Node *) lfirst(temp);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RelOptInfo *rel = find_base_rel(root, var->varno);
			int			attno = var->varattno;

			if (bms_is_subset(where_needed, rel->relids))
				continue;
			Assert(attno >= rel->min_attr && attno <= rel->max_attr);
			attno -= rel->min_attr;
			rel->attr_needed[attno] = bms_add_members(rel->attr_needed[attno],
													  where_needed);
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

			phinfo->ph_needed = bms_add_members(phinfo->ph_needed,
												where_needed);
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
}


/*****************************************************************************
 *
 *	  LATERAL REFERENCES
 *
 *****************************************************************************/

/*
 * find_lateral_references
 *	  For each LATERAL subquery, extract all its references to Vars and
 *	  PlaceHolderVars of the current query level, and make sure those values
 *	  will be available for evaluation of the subquery.
 *
 * While later planning steps ensure that the Var/PHV source rels are on the
 * outside of nestloops relative to the LATERAL subquery, we also need to
 * ensure that the Vars/PHVs propagate up to the nestloop join level; this
 * means setting suitable where_needed values for them.
 *
 * Note that this only deals with lateral references in unflattened LATERAL
 * subqueries.  When we flatten a LATERAL subquery, its lateral references
 * become plain Vars in the parent query, but they may have to be wrapped in
 * PlaceHolderVars if they need to be forced NULL by outer joins that don't
 * also null the LATERAL subquery.  That's all handled elsewhere.
 *
 * This has to run before deconstruct_jointree, since it might result in
 * creation of PlaceHolderInfos.
 */
void
find_lateral_references(PlannerInfo *root)
{
	Index		rti;

	/* We need do nothing if the query contains no LATERAL RTEs */
	if (!root->hasLateralRTEs)
		return;

	/*
	 * Examine all baserels (the rel array has been set up by now).
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* sanity check on array */

		/*
		 * This bit is less obvious than it might look.  We ignore appendrel
		 * otherrels and consider only their parent baserels.  In a case where
		 * a LATERAL-containing UNION ALL subquery was pulled up, it is the
		 * otherrel that is actually going to be in the plan.  However, we
		 * want to mark all its lateral references as needed by the parent,
		 * because it is the parent's relid that will be used for join
		 * planning purposes.  And the parent's RTE will contain all the
		 * lateral references we need to know, since the pulled-up member is
		 * nothing but a copy of parts of the original RTE's subquery.  We
		 * could visit the parent's children instead and transform their
		 * references back to the parent's relid, but it would be much more
		 * complicated for no real gain.  (Important here is that the child
		 * members have not yet received any processing beyond being pulled
		 * up.)  Similarly, in appendrels created by inheritance expansion,
		 * it's sufficient to look at the parent relation.
		 */

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		extract_lateral_references(root, brel, rti);
	}
}

static void
extract_lateral_references(PlannerInfo *root, RelOptInfo *brel, Index rtindex)
{
	RangeTblEntry *rte = root->simple_rte_array[rtindex];
	List	   *vars;
	List	   *newvars;
	Relids		where_needed;
	ListCell   *lc;

	/* No cross-references are possible if it's not LATERAL */
	if (!rte->lateral)
		return;

	/* Fetch the appropriate variables */
	if (rte->rtekind == RTE_RELATION)
		vars = pull_vars_of_level((Node *) rte->tablesample, 0);
	else if (rte->rtekind == RTE_SUBQUERY)
		vars = pull_vars_of_level((Node *) rte->subquery, 1);
	else if (rte->rtekind == RTE_FUNCTION)
		vars = pull_vars_of_level((Node *) rte->functions, 0);
	else if (rte->rtekind == RTE_TABLEFUNC)
		vars = pull_vars_of_level((Node *) rte->tablefunc, 0);
	else if (rte->rtekind == RTE_VALUES)
		vars = pull_vars_of_level((Node *) rte->values_lists, 0);
	else
	{
		Assert(false);
		return;					/* keep compiler quiet */
	}

	if (vars == NIL)
		return;					/* nothing to do */

	/* Copy each Var (or PlaceHolderVar) and adjust it to match our level */
	newvars = NIL;
	foreach(lc, vars)
	{
		Node	   *node = (Node *) lfirst(lc);

		node = copyObject(node);
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;

			/* Adjustment is easy since it's just one node */
			var->varlevelsup = 0;
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			int			levelsup = phv->phlevelsup;

			/* Have to work harder to adjust the contained expression too */
			if (levelsup != 0)
				IncrementVarSublevelsUp(node, -levelsup, 0);

			/*
			 * If we pulled the PHV out of a subquery RTE, its expression
			 * needs to be preprocessed.  subquery_planner() already did this
			 * for level-zero PHVs in function and values RTEs, though.
			 */
			if (levelsup > 0)
				phv->phexpr = preprocess_phv_expression(root, phv->phexpr);
		}
		else
			Assert(false);
		newvars = lappend(newvars, node);
	}

	list_free(vars);

	/*
	 * We mark the Vars as being "needed" at the LATERAL RTE.  This is a bit
	 * of a cheat: a more formal approach would be to mark each one as needed
	 * at the join of the LATERAL RTE with its source RTE.  But it will work,
	 * and it's much less tedious than computing a separate where_needed for
	 * each Var.
	 */
	where_needed = bms_make_singleton(rtindex);

	/*
	 * Push Vars into their source relations' targetlists, and PHVs into
	 * root->placeholder_list.
	 */
	add_vars_to_targetlist(root, newvars, where_needed);

	/*
	 * Remember the lateral references for rebuild_lateral_attr_needed and
	 * create_lateral_join_info.
	 */
	brel->lateral_vars = newvars;
}

/*
 * rebuild_lateral_attr_needed
 *	  Put back attr_needed bits for Vars/PHVs needed for lateral references.
 *
 * This is used to rebuild attr_needed/ph_needed sets after removal of a
 * useless outer join.  It should match what find_lateral_references did,
 * except that we call add_vars_to_attr_needed not add_vars_to_targetlist.
 */
void
rebuild_lateral_attr_needed(PlannerInfo *root)
{
	Index		rti;

	/* We need do nothing if the query contains no LATERAL RTEs */
	if (!root->hasLateralRTEs)
		return;

	/* Examine the same baserels that find_lateral_references did */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		where_needed;

		if (brel == NULL)
			continue;
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		/*
		 * We don't need to repeat all of extract_lateral_references, since it
		 * kindly saved the extracted Vars/PHVs in lateral_vars.
		 */
		if (brel->lateral_vars == NIL)
			continue;

		where_needed = bms_make_singleton(rti);

		add_vars_to_attr_needed(root, brel->lateral_vars, where_needed);
	}
}

/*
 * create_lateral_join_info
 *	  Fill in the per-base-relation direct_lateral_relids, lateral_relids
 *	  and lateral_referencers sets.
 */
void
create_lateral_join_info(PlannerInfo *root)
{
	bool		found_laterals = false;
	Index		rti;
	ListCell   *lc;

	/* We need do nothing if the query contains no LATERAL RTEs */
	if (!root->hasLateralRTEs)
		return;

	/* We'll need to have the ph_eval_at values for PlaceHolderVars */
	Assert(root->placeholdersFrozen);

	/*
	 * Examine all baserels (the rel array has been set up by now).
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		lateral_relids;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		lateral_relids = NULL;

		/* consider each laterally-referenced Var or PHV */
		foreach(lc, brel->lateral_vars)
		{
			Node	   *node = (Node *) lfirst(lc);

			if (IsA(node, Var))
			{
				Var		   *var = (Var *) node;

				found_laterals = true;
				lateral_relids = bms_add_member(lateral_relids,
												var->varno);
			}
			else if (IsA(node, PlaceHolderVar))
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

				found_laterals = true;
				lateral_relids = bms_add_members(lateral_relids,
												 phinfo->ph_eval_at);
			}
			else
				Assert(false);
		}

		/* We now have all the simple lateral refs from this rel */
		brel->direct_lateral_relids = lateral_relids;
		brel->lateral_relids = bms_copy(lateral_relids);
	}

	/*
	 * Now check for lateral references within PlaceHolderVars, and mark their
	 * eval_at rels as having lateral references to the source rels.
	 *
	 * For a PHV that is due to be evaluated at a baserel, mark its source(s)
	 * as direct lateral dependencies of the baserel (adding onto the ones
	 * recorded above).  If it's due to be evaluated at a join, mark its
	 * source(s) as indirect lateral dependencies of each baserel in the join,
	 * ie put them into lateral_relids but not direct_lateral_relids.  This is
	 * appropriate because we can't put any such baserel on the outside of a
	 * join to one of the PHV's lateral dependencies, but on the other hand we
	 * also can't yet join it directly to the dependency.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		Relids		eval_at = phinfo->ph_eval_at;
		Relids		lateral_refs;
		int			varno;

		if (phinfo->ph_lateral == NULL)
			continue;			/* PHV is uninteresting if no lateral refs */

		found_laterals = true;

		/*
		 * Include only baserels not outer joins in the evaluation sites'
		 * lateral relids.  This avoids problems when outer join order gets
		 * rearranged, and it should still ensure that the lateral values are
		 * available when needed.
		 */
		lateral_refs = bms_intersect(phinfo->ph_lateral, root->all_baserels);
		Assert(!bms_is_empty(lateral_refs));

		if (bms_get_singleton_member(eval_at, &varno))
		{
			/* Evaluation site is a baserel */
			RelOptInfo *brel = find_base_rel(root, varno);

			brel->direct_lateral_relids =
				bms_add_members(brel->direct_lateral_relids,
								lateral_refs);
			brel->lateral_relids =
				bms_add_members(brel->lateral_relids,
								lateral_refs);
		}
		else
		{
			/* Evaluation site is a join */
			varno = -1;
			while ((varno = bms_next_member(eval_at, varno)) >= 0)
			{
				RelOptInfo *brel = find_base_rel_ignore_join(root, varno);

				if (brel == NULL)
					continue;	/* ignore outer joins in eval_at */
				brel->lateral_relids = bms_add_members(brel->lateral_relids,
													   lateral_refs);
			}
		}
	}

	/*
	 * If we found no actual lateral references, we're done; but reset the
	 * hasLateralRTEs flag to avoid useless work later.
	 */
	if (!found_laterals)
	{
		root->hasLateralRTEs = false;
		return;
	}

	/*
	 * Calculate the transitive closure of the lateral_relids sets, so that
	 * they describe both direct and indirect lateral references.  If relation
	 * X references Y laterally, and Y references Z laterally, then we will
	 * have to scan X on the inside of a nestloop with Z, so for all intents
	 * and purposes X is laterally dependent on Z too.
	 *
	 * This code is essentially Warshall's algorithm for transitive closure.
	 * The outer loop considers each baserel, and propagates its lateral
	 * dependencies to those baserels that have a lateral dependency on it.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		outer_lateral_relids;
		Index		rti2;

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* need not consider baserel further if it has no lateral refs */
		outer_lateral_relids = brel->lateral_relids;
		if (outer_lateral_relids == NULL)
			continue;

		/* else scan all baserels */
		for (rti2 = 1; rti2 < root->simple_rel_array_size; rti2++)
		{
			RelOptInfo *brel2 = root->simple_rel_array[rti2];

			if (brel2 == NULL || brel2->reloptkind != RELOPT_BASEREL)
				continue;

			/* if brel2 has lateral ref to brel, propagate brel's refs */
			if (bms_is_member(rti, brel2->lateral_relids))
				brel2->lateral_relids = bms_add_members(brel2->lateral_relids,
														outer_lateral_relids);
		}
	}

	/*
	 * Now that we've identified all lateral references, mark each baserel
	 * with the set of relids of rels that reference it laterally (possibly
	 * indirectly) --- that is, the inverse mapping of lateral_relids.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		lateral_relids;
		int			rti2;

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* Nothing to do at rels with no lateral refs */
		lateral_relids = brel->lateral_relids;
		if (bms_is_empty(lateral_relids))
			continue;

		/* No rel should have a lateral dependency on itself */
		Assert(!bms_is_member(rti, lateral_relids));

		/* Mark this rel's referencees */
		rti2 = -1;
		while ((rti2 = bms_next_member(lateral_relids, rti2)) >= 0)
		{
			RelOptInfo *brel2 = root->simple_rel_array[rti2];

			if (brel2 == NULL)
				continue;		/* must be an OJ */

			Assert(brel2->reloptkind == RELOPT_BASEREL);
			brel2->lateral_referencers =
				bms_add_member(brel2->lateral_referencers, rti);
		}
	}
}


/*****************************************************************************
 *
 *	  JOIN TREE PROCESSING
 *
 *****************************************************************************/

/*
 * deconstruct_jointree
 *	  Recursively scan the query's join tree for WHERE and JOIN/ON qual
 *	  clauses, and add these to the appropriate restrictinfo and joininfo
 *	  lists belonging to base RelOptInfos.  Also, add SpecialJoinInfo nodes
 *	  to root->join_info_list for any outer joins appearing in the query tree.
 *	  Return a "joinlist" data structure showing the join order decisions
 *	  that need to be made by make_one_rel().
 *
 * The "joinlist" result is a list of items that are either RangeTblRef
 * jointree nodes or sub-joinlists.  All the items at the same level of
 * joinlist must be joined in an order to be determined by make_one_rel()
 * (note that legal orders may be constrained by SpecialJoinInfo nodes).
 * A sub-joinlist represents a subproblem to be planned separately. Currently
 * sub-joinlists arise only from FULL OUTER JOIN or when collapsing of
 * subproblems is stopped by join_collapse_limit or from_collapse_limit.
 */
List *
deconstruct_jointree(PlannerInfo *root)
{
	List	   *result;
	JoinDomain *top_jdomain;
	List	   *item_list = NIL;
	ListCell   *lc;

	/*
	 * After this point, no more PlaceHolderInfos may be made, because
	 * make_outerjoininfo requires all active placeholders to be present in
	 * root->placeholder_list while we crawl up the join tree.
	 */
	root->placeholdersFrozen = true;

	/* Fetch the already-created top-level join domain for the query */
	top_jdomain = linitial_node(JoinDomain, root->join_domains);
	top_jdomain->jd_relids = NULL;	/* filled during deconstruct_recurse */

	/* Start recursion at top of jointree */
	Assert(root->parse->jointree != NULL &&
		   IsA(root->parse->jointree, FromExpr));

	/* These are filled as we scan the jointree */
	root->all_baserels = NULL;
	root->outer_join_rels = NULL;

	/* Perform the initial scan of the jointree */
	result = deconstruct_recurse(root, (Node *) root->parse->jointree,
								 top_jdomain, NULL,
								 &item_list);

	/* Now we can form the value of all_query_rels, too */
	root->all_query_rels = bms_union(root->all_baserels, root->outer_join_rels);

	/* ... which should match what we computed for the top join domain */
	Assert(bms_equal(root->all_query_rels, top_jdomain->jd_relids));

	/* Now scan all the jointree nodes again, and distribute quals */
	foreach(lc, item_list)
	{
		JoinTreeItem *jtitem = (JoinTreeItem *) lfirst(lc);

		deconstruct_distribute(root, jtitem);
	}

	/*
	 * If there were any special joins then we may have some postponed LEFT
	 * JOIN clauses to deal with.
	 */
	if (root->join_info_list)
	{
		foreach(lc, item_list)
		{
			JoinTreeItem *jtitem = (JoinTreeItem *) lfirst(lc);

			if (jtitem->oj_joinclauses != NIL)
				deconstruct_distribute_oj_quals(root, item_list, jtitem);
		}
	}

	/* Don't need the JoinTreeItems any more */
	list_free_deep(item_list);

	return result;
}

/*
 * deconstruct_recurse
 *	  One recursion level of deconstruct_jointree's initial jointree scan.
 *
 * jtnode is the jointree node to examine, and parent_domain is the
 * enclosing join domain.  (We must add all base+OJ relids appearing
 * here or below to parent_domain.)  parent_jtitem is the JoinTreeItem
 * for the parent jointree node, or NULL at the top of the recursion.
 *
 * item_list is an in/out parameter: we add a JoinTreeItem struct to
 * that list for each jointree node, in depth-first traversal order.
 * (Hence, after each call, the last list item corresponds to its jtnode.)
 *
 * Return value is the appropriate joinlist for this jointree node.
 */
static List *
deconstruct_recurse(PlannerInfo *root, Node *jtnode,
					JoinDomain *parent_domain,
					JoinTreeItem *parent_jtitem,
					List **item_list)
{
	List	   *joinlist;
	JoinTreeItem *jtitem;

	Assert(jtnode != NULL);

	/* Make the new JoinTreeItem, but don't add it to item_list yet */
	jtitem = palloc0_object(JoinTreeItem);
	jtitem->jtnode = jtnode;
	jtitem->jti_parent = parent_jtitem;

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* Fill all_baserels as we encounter baserel jointree nodes */
		root->all_baserels = bms_add_member(root->all_baserels, varno);
		/* This node belongs to parent_domain */
		jtitem->jdomain = parent_domain;
		parent_domain->jd_relids = bms_add_member(parent_domain->jd_relids,
												  varno);
		/* qualscope is just the one RTE */
		jtitem->qualscope = bms_make_singleton(varno);
		/* A single baserel does not create an inner join */
		jtitem->inner_join_rels = NULL;
		joinlist = list_make1(jtnode);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		int			remaining;
		ListCell   *l;

		/* This node belongs to parent_domain, as do its children */
		jtitem->jdomain = parent_domain;

		/*
		 * Recurse to handle child nodes, and compute output joinlist.  We
		 * collapse subproblems into a single joinlist whenever the resulting
		 * joinlist wouldn't exceed from_collapse_limit members.  Also, always
		 * collapse one-element subproblems, since that won't lengthen the
		 * joinlist anyway.
		 */
		jtitem->qualscope = NULL;
		jtitem->inner_join_rels = NULL;
		joinlist = NIL;
		remaining = list_length(f->fromlist);
		foreach(l, f->fromlist)
		{
			JoinTreeItem *sub_item;
			List	   *sub_joinlist;
			int			sub_members;

			sub_joinlist = deconstruct_recurse(root, lfirst(l),
											   parent_domain,
											   jtitem,
											   item_list);
			sub_item = (JoinTreeItem *) llast(*item_list);
			jtitem->qualscope = bms_add_members(jtitem->qualscope,
												sub_item->qualscope);
			jtitem->inner_join_rels = sub_item->inner_join_rels;
			sub_members = list_length(sub_joinlist);
			remaining--;
			if (sub_members <= 1 ||
				list_length(joinlist) + sub_members + remaining <= from_collapse_limit)
				joinlist = list_concat(joinlist, sub_joinlist);
			else
				joinlist = lappend(joinlist, sub_joinlist);
		}

		/*
		 * A FROM with more than one list element is an inner join subsuming
		 * all below it, so we should report inner_join_rels = qualscope. If
		 * there was exactly one element, we should (and already did) report
		 * whatever its inner_join_rels were.  If there were no elements (is
		 * that still possible?) the initialization before the loop fixed it.
		 */
		if (list_length(f->fromlist) > 1)
			jtitem->inner_join_rels = jtitem->qualscope;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		JoinDomain *child_domain,
				   *fj_domain;
		JoinTreeItem *left_item,
				   *right_item;
		List	   *leftjoinlist,
				   *rightjoinlist;

		switch (j->jointype)
		{
			case JOIN_INNER:
				/* This node belongs to parent_domain, as do its children */
				jtitem->jdomain = parent_domain;
				/* Recurse */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   parent_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													parent_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* Compute qualscope etc */
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				jtitem->inner_join_rels = jtitem->qualscope;
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				/* Inner join adds no restrictions for quals */
				jtitem->nonnullable_rels = NULL;
				break;
			case JOIN_LEFT:
			case JOIN_ANTI:
				/* Make new join domain for my quals and the RHS */
				child_domain = makeNode(JoinDomain);
				child_domain->jd_relids = NULL; /* filled by recursion */
				root->join_domains = lappend(root->join_domains, child_domain);
				jtitem->jdomain = child_domain;
				/* Recurse */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   parent_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													child_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* Compute join domain contents, qualscope etc */
				parent_domain->jd_relids =
					bms_add_members(parent_domain->jd_relids,
									child_domain->jd_relids);
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				/* caution: ANTI join derived from SEMI will lack rtindex */
				if (j->rtindex != 0)
				{
					parent_domain->jd_relids =
						bms_add_member(parent_domain->jd_relids,
									   j->rtindex);
					jtitem->qualscope = bms_add_member(jtitem->qualscope,
													   j->rtindex);
					root->outer_join_rels = bms_add_member(root->outer_join_rels,
														   j->rtindex);
					mark_rels_nulled_by_join(root, j->rtindex,
											 right_item->qualscope);
				}
				jtitem->inner_join_rels = bms_union(left_item->inner_join_rels,
													right_item->inner_join_rels);
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				jtitem->nonnullable_rels = left_item->qualscope;
				break;
			case JOIN_SEMI:
				/* This node belongs to parent_domain, as do its children */
				jtitem->jdomain = parent_domain;
				/* Recurse */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   parent_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													parent_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* Compute qualscope etc */
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				/* SEMI join never has rtindex, so don't add to anything */
				Assert(j->rtindex == 0);
				jtitem->inner_join_rels = bms_union(left_item->inner_join_rels,
													right_item->inner_join_rels);
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				/* Semi join adds no restrictions for quals */
				jtitem->nonnullable_rels = NULL;
				break;
			case JOIN_FULL:
				/* The FULL JOIN's quals need their very own domain */
				fj_domain = makeNode(JoinDomain);
				root->join_domains = lappend(root->join_domains, fj_domain);
				jtitem->jdomain = fj_domain;
				/* Recurse, giving each side its own join domain */
				child_domain = makeNode(JoinDomain);
				child_domain->jd_relids = NULL; /* filled by recursion */
				root->join_domains = lappend(root->join_domains, child_domain);
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   child_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				fj_domain->jd_relids = bms_copy(child_domain->jd_relids);
				child_domain = makeNode(JoinDomain);
				child_domain->jd_relids = NULL; /* filled by recursion */
				root->join_domains = lappend(root->join_domains, child_domain);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													child_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* Compute qualscope etc */
				fj_domain->jd_relids = bms_add_members(fj_domain->jd_relids,
													   child_domain->jd_relids);
				parent_domain->jd_relids = bms_add_members(parent_domain->jd_relids,
														   fj_domain->jd_relids);
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				Assert(j->rtindex != 0);
				parent_domain->jd_relids = bms_add_member(parent_domain->jd_relids,
														  j->rtindex);
				jtitem->qualscope = bms_add_member(jtitem->qualscope,
												   j->rtindex);
				root->outer_join_rels = bms_add_member(root->outer_join_rels,
													   j->rtindex);
				mark_rels_nulled_by_join(root, j->rtindex,
										 left_item->qualscope);
				mark_rels_nulled_by_join(root, j->rtindex,
										 right_item->qualscope);
				jtitem->inner_join_rels = bms_union(left_item->inner_join_rels,
													right_item->inner_join_rels);
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				/* each side is both outer and inner */
				jtitem->nonnullable_rels = jtitem->qualscope;
				break;
			default:
				/* JOIN_RIGHT was eliminated during reduce_outer_joins() */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				leftjoinlist = rightjoinlist = NIL; /* keep compiler quiet */
				break;
		}

		/*
		 * Compute the output joinlist.  We fold subproblems together except
		 * at a FULL JOIN or where join_collapse_limit would be exceeded.
		 */
		if (j->jointype == JOIN_FULL)
		{
			/* force the join order exactly at this node */
			joinlist = list_make1(list_make2(leftjoinlist, rightjoinlist));
		}
		else if (list_length(leftjoinlist) + list_length(rightjoinlist) <=
				 join_collapse_limit)
		{
			/* OK to combine subproblems */
			joinlist = list_concat(leftjoinlist, rightjoinlist);
		}
		else
		{
			/* can't combine, but needn't force join order above here */
			Node	   *leftpart,
					   *rightpart;

			/* avoid creating useless 1-element sublists */
			if (list_length(leftjoinlist) == 1)
				leftpart = (Node *) linitial(leftjoinlist);
			else
				leftpart = (Node *) leftjoinlist;
			if (list_length(rightjoinlist) == 1)
				rightpart = (Node *) linitial(rightjoinlist);
			else
				rightpart = (Node *) rightjoinlist;
			joinlist = list_make2(leftpart, rightpart);
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
		joinlist = NIL;			/* keep compiler quiet */
	}

	/* Finally, we can add the new JoinTreeItem to item_list */
	*item_list = lappend(*item_list, jtitem);

	return joinlist;
}

/*
 * deconstruct_distribute
 *	  Process one jointree node in phase 2 of deconstruct_jointree processing.
 *
 * Distribute quals of the node to appropriate restriction and join lists.
 * In addition, entries will be added to root->join_info_list for outer joins.
 */
static void
deconstruct_distribute(PlannerInfo *root, JoinTreeItem *jtitem)
{
	Node	   *jtnode = jtitem->jtnode;

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* Deal with any securityQuals attached to the RTE */
		if (root->qual_security_level > 0)
			process_security_barrier_quals(root,
										   varno,
										   jtitem);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;

		/*
		 * Process any lateral-referencing quals that were postponed to this
		 * level by children.
		 */
		distribute_quals_to_rels(root, jtitem->lateral_clauses,
								 jtitem,
								 NULL,
								 root->qual_security_level,
								 jtitem->qualscope,
								 NULL, NULL, NULL,
								 true, false, false,
								 NULL);

		/*
		 * Now process the top-level quals.
		 */
		distribute_quals_to_rels(root, (List *) f->quals,
								 jtitem,
								 NULL,
								 root->qual_security_level,
								 jtitem->qualscope,
								 NULL, NULL, NULL,
								 true, false, false,
								 NULL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		ojscope;
		List	   *my_quals;
		SpecialJoinInfo *sjinfo;
		List	  **postponed_oj_qual_list;

		/*
		 * Include lateral-referencing quals postponed from children in
		 * my_quals, so that they'll be handled properly in
		 * make_outerjoininfo.  (This is destructive to
		 * jtitem->lateral_clauses, but we won't use that again.)
		 */
		my_quals = list_concat(jtitem->lateral_clauses,
							   (List *) j->quals);

		/*
		 * For an OJ, form the SpecialJoinInfo now, so that we can pass it to
		 * distribute_qual_to_rels.  We must compute its ojscope too.
		 *
		 * Semijoins are a bit of a hybrid: we build a SpecialJoinInfo, but we
		 * want ojscope = NULL for distribute_qual_to_rels.
		 */
		if (j->jointype != JOIN_INNER)
		{
			sjinfo = make_outerjoininfo(root,
										jtitem->left_rels,
										jtitem->right_rels,
										jtitem->inner_join_rels,
										j->jointype,
										j->rtindex,
										my_quals);
			jtitem->sjinfo = sjinfo;
			if (j->jointype == JOIN_SEMI)
				ojscope = NULL;
			else
				ojscope = bms_union(sjinfo->min_lefthand,
									sjinfo->min_righthand);
		}
		else
		{
			sjinfo = NULL;
			ojscope = NULL;
		}

		/*
		 * If it's a left join with a join clause that is strict for the LHS,
		 * then we need to postpone handling of any non-degenerate join
		 * clauses, in case the join is able to commute with another left join
		 * per identity 3.  (Degenerate clauses need not be postponed, since
		 * they will drop down below this join anyway.)
		 */
		if (j->jointype == JOIN_LEFT && sjinfo->lhs_strict)
		{
			postponed_oj_qual_list = &jtitem->oj_joinclauses;

			/*
			 * Add back any commutable lower OJ relids that were removed from
			 * min_lefthand or min_righthand, else the ojscope cross-check in
			 * distribute_qual_to_rels will complain.  Since we are postponing
			 * processing of non-degenerate clauses, this addition doesn't
			 * affect anything except that cross-check.  Real clause
			 * positioning decisions will be made later, when we revisit the
			 * postponed clauses.
			 */
			ojscope = bms_add_members(ojscope, sjinfo->commute_below_l);
			ojscope = bms_add_members(ojscope, sjinfo->commute_below_r);
		}
		else
			postponed_oj_qual_list = NULL;

		/* Process the JOIN's qual clauses */
		distribute_quals_to_rels(root, my_quals,
								 jtitem,
								 sjinfo,
								 root->qual_security_level,
								 jtitem->qualscope,
								 ojscope, jtitem->nonnullable_rels,
								 NULL,	/* incompatible_relids */
								 true,	/* allow_equivalence */
								 false, false,	/* not clones */
								 postponed_oj_qual_list);

		/* And add the SpecialJoinInfo to join_info_list */
		if (sjinfo)
			root->join_info_list = lappend(root->join_info_list, sjinfo);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	}
}

/*
 * process_security_barrier_quals
 *	  Transfer security-barrier quals into relation's baserestrictinfo list.
 *
 * The rewriter put any relevant security-barrier conditions into the RTE's
 * securityQuals field, but it's now time to copy them into the rel's
 * baserestrictinfo.
 *
 * In inheritance cases, we only consider quals attached to the parent rel
 * here; they will be valid for all children too, so it's okay to consider
 * them for purposes like equivalence class creation.  Quals attached to
 * individual child rels will be dealt with during path creation.
 */
static void
process_security_barrier_quals(PlannerInfo *root,
							   int rti, JoinTreeItem *jtitem)
{
	RangeTblEntry *rte = root->simple_rte_array[rti];
	Index		security_level = 0;
	ListCell   *lc;

	/*
	 * Each element of the securityQuals list has been preprocessed into an
	 * implicitly-ANDed list of clauses.  All the clauses in a given sublist
	 * should get the same security level, but successive sublists get higher
	 * levels.
	 */
	foreach(lc, rte->securityQuals)
	{
		List	   *qualset = (List *) lfirst(lc);

		/*
		 * We cheat to the extent of passing ojscope = qualscope rather than
		 * its more logical value of NULL.  The only effect this has is to
		 * force a Var-free qual to be evaluated at the rel rather than being
		 * pushed up to top of tree, which we don't want.
		 */
		distribute_quals_to_rels(root, qualset,
								 jtitem,
								 NULL,
								 security_level,
								 jtitem->qualscope,
								 jtitem->qualscope,
								 NULL,
								 NULL,
								 true,
								 false, false,	/* not clones */
								 NULL);
		security_level++;
	}

	/* Assert that qual_security_level is higher than anything we just used */
	Assert(security_level <= root->qual_security_level);
}

/*
 * mark_rels_nulled_by_join
 *	  Fill RelOptInfo.nulling_relids of baserels nulled by this outer join
 *
 * Inputs:
 *	ojrelid: RT index of the join RTE (must not be 0)
 *	lower_rels: the base+OJ Relids syntactically below nullable side of join
 */
static void
mark_rels_nulled_by_join(PlannerInfo *root, Index ojrelid,
						 Relids lower_rels)
{
	int			relid = -1;

	while ((relid = bms_next_member(lower_rels, relid)) > 0)
	{
		RelOptInfo *rel = root->simple_rel_array[relid];

		/* ignore the RTE_GROUP RTE */
		if (relid == root->group_rtindex)
			continue;

		if (rel == NULL)		/* must be an outer join */
		{
			Assert(bms_is_member(relid, root->outer_join_rels));
			continue;
		}
		rel->nulling_relids = bms_add_member(rel->nulling_relids, ojrelid);
	}
}

/*
 * make_outerjoininfo
 *	  Build a SpecialJoinInfo for the current outer join
 *
 * Inputs:
 *	left_rels: the base+OJ Relids syntactically on outer side of join
 *	right_rels: the base+OJ Relids syntactically on inner side of join
 *	inner_join_rels: base+OJ Relids participating in inner joins below this one
 *	jointype: what it says (must always be LEFT, FULL, SEMI, or ANTI)
 *	ojrelid: RT index of the join RTE (0 for SEMI, which isn't in the RT list)
 *	clause: the outer join's join condition (in implicit-AND format)
 *
 * The node should eventually be appended to root->join_info_list, but we
 * do not do that here.
 *
 * Note: we assume that this function is invoked bottom-up, so that
 * root->join_info_list already contains entries for all outer joins that are
 * syntactically below this one.
 */
static SpecialJoinInfo *
make_outerjoininfo(PlannerInfo *root,
				   Relids left_rels, Relids right_rels,
				   Relids inner_join_rels,
				   JoinType jointype, Index ojrelid,
				   List *clause)
{
	SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
	Relids		clause_relids;
	Relids		strict_relids;
	Relids		min_lefthand;
	Relids		min_righthand;
	Relids		commute_below_l;
	Relids		commute_below_r;
	ListCell   *l;

	/*
	 * We should not see RIGHT JOIN here because left/right were switched
	 * earlier
	 */
	Assert(jointype != JOIN_INNER);
	Assert(jointype != JOIN_RIGHT);

	/*
	 * Presently the executor cannot support FOR [KEY] UPDATE/SHARE marking of
	 * rels appearing on the nullable side of an outer join. (It's somewhat
	 * unclear what that would mean, anyway: what should we mark when a result
	 * row is generated from no element of the nullable relation?)	So,
	 * complain if any nullable rel is FOR [KEY] UPDATE/SHARE.
	 *
	 * You might be wondering why this test isn't made far upstream in the
	 * parser.  It's because the parser hasn't got enough info --- consider
	 * FOR UPDATE applied to a view.  Only after rewriting and flattening do
	 * we know whether the view contains an outer join.
	 *
	 * We use the original RowMarkClause list here; the PlanRowMark list would
	 * list everything.
	 */
	foreach(l, root->parse->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (bms_is_member(rc->rti, right_rels) ||
			(jointype == JOIN_FULL && bms_is_member(rc->rti, left_rels)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*------
			 translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s cannot be applied to the nullable side of an outer join",
							LCS_asString(rc->strength))));
	}

	sjinfo->syn_lefthand = left_rels;
	sjinfo->syn_righthand = right_rels;
	sjinfo->jointype = jointype;
	sjinfo->ojrelid = ojrelid;
	/* these fields may get added to later: */
	sjinfo->commute_above_l = NULL;
	sjinfo->commute_above_r = NULL;
	sjinfo->commute_below_l = NULL;
	sjinfo->commute_below_r = NULL;

	compute_semijoin_info(root, sjinfo, clause);

	/* If it's a full join, no need to be very smart */
	if (jointype == JOIN_FULL)
	{
		sjinfo->min_lefthand = bms_copy(left_rels);
		sjinfo->min_righthand = bms_copy(right_rels);
		sjinfo->lhs_strict = false; /* don't care about this */
		return sjinfo;
	}

	/*
	 * Retrieve all relids mentioned within the join clause.
	 */
	clause_relids = pull_varnos(root, (Node *) clause);

	/*
	 * For which relids is the clause strict, ie, it cannot succeed if the
	 * rel's columns are all NULL?
	 */
	strict_relids = find_nonnullable_rels((Node *) clause);

	/* Remember whether the clause is strict for any LHS relations */
	sjinfo->lhs_strict = bms_overlap(strict_relids, left_rels);

	/*
	 * Required LHS always includes the LHS rels mentioned in the clause. We
	 * may have to add more rels based on lower outer joins; see below.
	 */
	min_lefthand = bms_intersect(clause_relids, left_rels);

	/*
	 * Similarly for required RHS.  But here, we must also include any lower
	 * inner joins, to ensure we don't try to commute with any of them.
	 */
	min_righthand = bms_int_members(bms_union(clause_relids, inner_join_rels),
									right_rels);

	/*
	 * Now check previous outer joins for ordering restrictions.
	 *
	 * commute_below_l and commute_below_r accumulate the relids of lower
	 * outer joins that we think this one can commute with.  These decisions
	 * are just tentative within this loop, since we might find an
	 * intermediate outer join that prevents commutation.  Surviving relids
	 * will get merged into the SpecialJoinInfo structs afterwards.
	 */
	commute_below_l = commute_below_r = NULL;
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *otherinfo = (SpecialJoinInfo *) lfirst(l);
		bool		have_unsafe_phvs;

		/*
		 * A full join is an optimization barrier: we can't associate into or
		 * out of it.  Hence, if it overlaps either LHS or RHS of the current
		 * rel, expand that side's min relset to cover the whole full join.
		 */
		if (otherinfo->jointype == JOIN_FULL)
		{
			Assert(otherinfo->ojrelid != 0);
			if (bms_overlap(left_rels, otherinfo->syn_lefthand) ||
				bms_overlap(left_rels, otherinfo->syn_righthand))
			{
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_lefthand);
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_righthand);
				min_lefthand = bms_add_member(min_lefthand,
											  otherinfo->ojrelid);
			}
			if (bms_overlap(right_rels, otherinfo->syn_lefthand) ||
				bms_overlap(right_rels, otherinfo->syn_righthand))
			{
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_lefthand);
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_righthand);
				min_righthand = bms_add_member(min_righthand,
											   otherinfo->ojrelid);
			}
			/* Needn't do anything else with the full join */
			continue;
		}

		/*
		 * If our join condition contains any PlaceHolderVars that need to be
		 * evaluated above the lower OJ, then we can't commute with it.
		 */
		if (otherinfo->ojrelid != 0)
			have_unsafe_phvs =
				contain_placeholder_references_to(root,
												  (Node *) clause,
												  otherinfo->ojrelid);
		else
			have_unsafe_phvs = false;

		/*
		 * For a lower OJ in our LHS, if our join condition uses the lower
		 * join's RHS and is not strict for that rel, we must preserve the
		 * ordering of the two OJs, so add lower OJ's full syntactic relset to
		 * min_lefthand.  (We must use its full syntactic relset, not just its
		 * min_lefthand + min_righthand.  This is because there might be other
		 * OJs below this one that this one can commute with, but we cannot
		 * commute with them if we don't with this one.)  Also, if we have
		 * unsafe PHVs or the current join is a semijoin or antijoin, we must
		 * preserve ordering regardless of strictness.
		 *
		 * Note: I believe we have to insist on being strict for at least one
		 * rel in the lower OJ's min_righthand, not its whole syn_righthand.
		 *
		 * When we don't need to preserve ordering, check to see if outer join
		 * identity 3 applies, and if so, remove the lower OJ's ojrelid from
		 * our min_lefthand so that commutation is allowed.
		 */
		if (bms_overlap(left_rels, otherinfo->syn_righthand))
		{
			if (bms_overlap(clause_relids, otherinfo->syn_righthand) &&
				(have_unsafe_phvs ||
				 jointype == JOIN_SEMI || jointype == JOIN_ANTI ||
				 !bms_overlap(strict_relids, otherinfo->min_righthand)))
			{
				/* Preserve ordering */
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_lefthand);
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_righthand);
				if (otherinfo->ojrelid != 0)
					min_lefthand = bms_add_member(min_lefthand,
												  otherinfo->ojrelid);
			}
			else if (jointype == JOIN_LEFT &&
					 otherinfo->jointype == JOIN_LEFT &&
					 bms_overlap(strict_relids, otherinfo->min_righthand) &&
					 !bms_overlap(clause_relids, otherinfo->syn_lefthand))
			{
				/* Identity 3 applies, so remove the ordering restriction */
				min_lefthand = bms_del_member(min_lefthand, otherinfo->ojrelid);
				/* Record the (still tentative) commutability relationship */
				commute_below_l =
					bms_add_member(commute_below_l, otherinfo->ojrelid);
			}
		}

		/*
		 * For a lower OJ in our RHS, if our join condition does not use the
		 * lower join's RHS and the lower OJ's join condition is strict, we
		 * can interchange the ordering of the two OJs; otherwise we must add
		 * the lower OJ's full syntactic relset to min_righthand.
		 *
		 * Also, if our join condition does not use the lower join's LHS
		 * either, force the ordering to be preserved.  Otherwise we can end
		 * up with SpecialJoinInfos with identical min_righthands, which can
		 * confuse join_is_legal (see discussion in backend/optimizer/README).
		 *
		 * Also, we must preserve ordering anyway if we have unsafe PHVs, or
		 * if either this join or the lower OJ is a semijoin or antijoin.
		 *
		 * When we don't need to preserve ordering, check to see if outer join
		 * identity 3 applies, and if so, remove the lower OJ's ojrelid from
		 * our min_righthand so that commutation is allowed.
		 */
		if (bms_overlap(right_rels, otherinfo->syn_righthand))
		{
			if (bms_overlap(clause_relids, otherinfo->syn_righthand) ||
				!bms_overlap(clause_relids, otherinfo->min_lefthand) ||
				have_unsafe_phvs ||
				jointype == JOIN_SEMI ||
				jointype == JOIN_ANTI ||
				otherinfo->jointype == JOIN_SEMI ||
				otherinfo->jointype == JOIN_ANTI ||
				!otherinfo->lhs_strict)
			{
				/* Preserve ordering */
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_lefthand);
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_righthand);
				if (otherinfo->ojrelid != 0)
					min_righthand = bms_add_member(min_righthand,
												   otherinfo->ojrelid);
			}
			else if (jointype == JOIN_LEFT &&
					 otherinfo->jointype == JOIN_LEFT &&
					 otherinfo->lhs_strict)
			{
				/* Identity 3 applies, so remove the ordering restriction */
				min_righthand = bms_del_member(min_righthand,
											   otherinfo->ojrelid);
				/* Record the (still tentative) commutability relationship */
				commute_below_r =
					bms_add_member(commute_below_r, otherinfo->ojrelid);
			}
		}
	}

	/*
	 * Examine PlaceHolderVars.  If a PHV is supposed to be evaluated within
	 * this join's nullable side, then ensure that min_righthand contains the
	 * full eval_at set of the PHV.  This ensures that the PHV actually can be
	 * evaluated within the RHS.  Note that this works only because we should
	 * already have determined the final eval_at level for any PHV
	 * syntactically within this join.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);
		Relids		ph_syn_level = phinfo->ph_var->phrels;

		/* Ignore placeholder if it didn't syntactically come from RHS */
		if (!bms_is_subset(ph_syn_level, right_rels))
			continue;

		/* Else, prevent join from being formed before we eval the PHV */
		min_righthand = bms_add_members(min_righthand, phinfo->ph_eval_at);
	}

	/*
	 * If we found nothing to put in min_lefthand, punt and make it the full
	 * LHS, to avoid having an empty min_lefthand which will confuse later
	 * processing. (We don't try to be smart about such cases, just correct.)
	 * Likewise for min_righthand.
	 */
	if (bms_is_empty(min_lefthand))
		min_lefthand = bms_copy(left_rels);
	if (bms_is_empty(min_righthand))
		min_righthand = bms_copy(right_rels);

	/* Now they'd better be nonempty */
	Assert(!bms_is_empty(min_lefthand));
	Assert(!bms_is_empty(min_righthand));
	/* Shouldn't overlap either */
	Assert(!bms_overlap(min_lefthand, min_righthand));

	sjinfo->min_lefthand = min_lefthand;
	sjinfo->min_righthand = min_righthand;

	/*
	 * Now that we've identified the correct min_lefthand and min_righthand,
	 * any commute_below_l or commute_below_r relids that have not gotten
	 * added back into those sets (due to intervening outer joins) are indeed
	 * commutable with this one.
	 *
	 * First, delete any subsequently-added-back relids (this is easier than
	 * maintaining commute_below_l/r precisely through all the above).
	 */
	commute_below_l = bms_del_members(commute_below_l, min_lefthand);
	commute_below_r = bms_del_members(commute_below_r, min_righthand);

	/* Anything left? */
	if (commute_below_l || commute_below_r)
	{
		/* Yup, so we must update the derived data in the SpecialJoinInfos */
		sjinfo->commute_below_l = commute_below_l;
		sjinfo->commute_below_r = commute_below_r;
		foreach(l, root->join_info_list)
		{
			SpecialJoinInfo *otherinfo = (SpecialJoinInfo *) lfirst(l);

			if (bms_is_member(otherinfo->ojrelid, commute_below_l))
				otherinfo->commute_above_l =
					bms_add_member(otherinfo->commute_above_l, ojrelid);
			else if (bms_is_member(otherinfo->ojrelid, commute_below_r))
				otherinfo->commute_above_r =
					bms_add_member(otherinfo->commute_above_r, ojrelid);
		}
	}

	return sjinfo;
}

/*
 * compute_semijoin_info
 *	  Fill semijoin-related fields of a new SpecialJoinInfo
 *
 * Note: this relies on only the jointype and syn_righthand fields of the
 * SpecialJoinInfo; the rest may not be set yet.
 */
static void
compute_semijoin_info(PlannerInfo *root, SpecialJoinInfo *sjinfo, List *clause)
{
	List	   *semi_operators;
	List	   *semi_rhs_exprs;
	bool		all_btree;
	bool		all_hash;
	ListCell   *lc;

	/* Initialize semijoin-related fields in case we can't unique-ify */
	sjinfo->semi_can_btree = false;
	sjinfo->semi_can_hash = false;
	sjinfo->semi_operators = NIL;
	sjinfo->semi_rhs_exprs = NIL;

	/* Nothing more to do if it's not a semijoin */
	if (sjinfo->jointype != JOIN_SEMI)
		return;

	/*
	 * Look to see whether the semijoin's join quals consist of AND'ed
	 * equality operators, with (only) RHS variables on only one side of each
	 * one.  If so, we can figure out how to enforce uniqueness for the RHS.
	 *
	 * Note that the input clause list is the list of quals that are
	 * *syntactically* associated with the semijoin, which in practice means
	 * the synthesized comparison list for an IN or the WHERE of an EXISTS.
	 * Particularly in the latter case, it might contain clauses that aren't
	 * *semantically* associated with the join, but refer to just one side or
	 * the other.  We can ignore such clauses here, as they will just drop
	 * down to be processed within one side or the other.  (It is okay to
	 * consider only the syntactically-associated clauses here because for a
	 * semijoin, no higher-level quals could refer to the RHS, and so there
	 * can be no other quals that are semantically associated with this join.
	 * We do things this way because it is useful to have the set of potential
	 * unique-ification expressions before we can extract the list of quals
	 * that are actually semantically associated with the particular join.)
	 *
	 * Note that the semi_operators list consists of the joinqual operators
	 * themselves (but commuted if needed to put the RHS value on the right).
	 * These could be cross-type operators, in which case the operator
	 * actually needed for uniqueness is a related single-type operator. We
	 * assume here that that operator will be available from the btree or hash
	 * opclass when the time comes ... if not, create_unique_plan() will fail.
	 */
	semi_operators = NIL;
	semi_rhs_exprs = NIL;
	all_btree = true;
	all_hash = enable_hashagg;	/* don't consider hash if not enabled */
	foreach(lc, clause)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);
		Oid			opno;
		Node	   *left_expr;
		Node	   *right_expr;
		Relids		left_varnos;
		Relids		right_varnos;
		Relids		all_varnos;
		Oid			opinputtype;

		/* Is it a binary opclause? */
		if (!IsA(op, OpExpr) ||
			list_length(op->args) != 2)
		{
			/* No, but does it reference both sides? */
			all_varnos = pull_varnos(root, (Node *) op);
			if (!bms_overlap(all_varnos, sjinfo->syn_righthand) ||
				bms_is_subset(all_varnos, sjinfo->syn_righthand))
			{
				/*
				 * Clause refers to only one rel, so ignore it --- unless it
				 * contains volatile functions, in which case we'd better
				 * punt.
				 */
				if (contain_volatile_functions((Node *) op))
					return;
				continue;
			}
			/* Non-operator clause referencing both sides, must punt */
			return;
		}

		/* Extract data from binary opclause */
		opno = op->opno;
		left_expr = linitial(op->args);
		right_expr = lsecond(op->args);
		left_varnos = pull_varnos(root, left_expr);
		right_varnos = pull_varnos(root, right_expr);
		all_varnos = bms_union(left_varnos, right_varnos);
		opinputtype = exprType(left_expr);

		/* Does it reference both sides? */
		if (!bms_overlap(all_varnos, sjinfo->syn_righthand) ||
			bms_is_subset(all_varnos, sjinfo->syn_righthand))
		{
			/*
			 * Clause refers to only one rel, so ignore it --- unless it
			 * contains volatile functions, in which case we'd better punt.
			 */
			if (contain_volatile_functions((Node *) op))
				return;
			continue;
		}

		/* check rel membership of arguments */
		if (!bms_is_empty(right_varnos) &&
			bms_is_subset(right_varnos, sjinfo->syn_righthand) &&
			!bms_overlap(left_varnos, sjinfo->syn_righthand))
		{
			/* typical case, right_expr is RHS variable */
		}
		else if (!bms_is_empty(left_varnos) &&
				 bms_is_subset(left_varnos, sjinfo->syn_righthand) &&
				 !bms_overlap(right_varnos, sjinfo->syn_righthand))
		{
			/* flipped case, left_expr is RHS variable */
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return;
			right_expr = left_expr;
		}
		else
		{
			/* mixed membership of args, punt */
			return;
		}

		/* all operators must be btree equality or hash equality */
		if (all_btree)
		{
			/* oprcanmerge is considered a hint... */
			if (!op_mergejoinable(opno, opinputtype) ||
				get_mergejoin_opfamilies(opno) == NIL)
				all_btree = false;
		}
		if (all_hash)
		{
			/* ... but oprcanhash had better be correct */
			if (!op_hashjoinable(opno, opinputtype))
				all_hash = false;
		}
		if (!(all_btree || all_hash))
			return;

		/* so far so good, keep building lists */
		semi_operators = lappend_oid(semi_operators, opno);
		semi_rhs_exprs = lappend(semi_rhs_exprs, copyObject(right_expr));
	}

	/* Punt if we didn't find at least one column to unique-ify */
	if (semi_rhs_exprs == NIL)
		return;

	/*
	 * The expressions we'd need to unique-ify mustn't be volatile.
	 */
	if (contain_volatile_functions((Node *) semi_rhs_exprs))
		return;

	/*
	 * If we get here, we can unique-ify the semijoin's RHS using at least one
	 * of sorting and hashing.  Save the information about how to do that.
	 */
	sjinfo->semi_can_btree = all_btree;
	sjinfo->semi_can_hash = all_hash;
	sjinfo->semi_operators = semi_operators;
	sjinfo->semi_rhs_exprs = semi_rhs_exprs;
}

/*
 * deconstruct_distribute_oj_quals
 *	  Adjust LEFT JOIN quals to be suitable for commuted-left-join cases,
 *	  then push them into the joinqual lists and EquivalenceClass structures.
 *
 * This runs immediately after we've completed the deconstruct_distribute scan.
 * jtitems contains all the JoinTreeItems (in depth-first order), and jtitem
 * is one that has postponed oj_joinclauses to deal with.
 */
static void
deconstruct_distribute_oj_quals(PlannerInfo *root,
								List *jtitems,
								JoinTreeItem *jtitem)
{
	SpecialJoinInfo *sjinfo = jtitem->sjinfo;
	Relids		qualscope,
				ojscope,
				nonnullable_rels;

	/* Recompute syntactic and semantic scopes of this left join */
	qualscope = bms_union(sjinfo->syn_lefthand, sjinfo->syn_righthand);
	qualscope = bms_add_member(qualscope, sjinfo->ojrelid);
	ojscope = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	nonnullable_rels = sjinfo->syn_lefthand;

	/*
	 * If this join can commute with any other ones per outer-join identity 3,
	 * and it is the one providing the join clause with flexible semantics,
	 * then we have to generate variants of the join clause with different
	 * nullingrels labeling.  Otherwise, just push out the postponed clause
	 * as-is.
	 */
	Assert(sjinfo->lhs_strict); /* else we shouldn't be here */
	if (sjinfo->commute_above_r || sjinfo->commute_below_l)
	{
		Relids		joins_above;
		Relids		joins_below;
		Relids		incompatible_joins;
		Relids		joins_so_far;
		List	   *quals;
		int			save_last_rinfo_serial;
		ListCell   *lc;

		/* Identify the outer joins this one commutes with */
		joins_above = sjinfo->commute_above_r;
		joins_below = sjinfo->commute_below_l;

		/*
		 * Generate qual variants with different sets of nullingrels bits.
		 *
		 * We only need bit-sets that correspond to the successively less
		 * deeply syntactically-nested subsets of this join and its
		 * commutators.  That's true first because obviously only those forms
		 * of the Vars and PHVs could appear elsewhere in the query, and
		 * second because the outer join identities do not provide a way to
		 * re-order such joins in a way that would require different marking.
		 * (That is, while the current join may commute with several others,
		 * none of those others can commute with each other.)  To visit the
		 * interesting joins in syntactic nesting order, we rely on the
		 * jtitems list to be ordered that way.
		 *
		 * We first strip out all the nullingrels bits corresponding to
		 * commuting joins below this one, and then successively put them back
		 * as we crawl up the join stack.
		 */
		quals = jtitem->oj_joinclauses;
		if (!bms_is_empty(joins_below))
			quals = (List *) remove_nulling_relids((Node *) quals,
												   joins_below,
												   NULL);

		/*
		 * We'll need to mark the lower versions of the quals as not safe to
		 * apply above not-yet-processed joins of the stack.  This prevents
		 * possibly applying a cloned qual at the wrong join level.
		 */
		incompatible_joins = bms_union(joins_below, joins_above);
		incompatible_joins = bms_add_member(incompatible_joins,
											sjinfo->ojrelid);

		/*
		 * Each time we produce RestrictInfo(s) from these quals, reset the
		 * last_rinfo_serial counter, so that the RestrictInfos for the "same"
		 * qual condition get identical serial numbers.  (This relies on the
		 * fact that we're not changing the qual list in any way that'd affect
		 * the number of RestrictInfos built from it.) This'll allow us to
		 * detect duplicative qual usage later.
		 */
		save_last_rinfo_serial = root->last_rinfo_serial;

		joins_so_far = NULL;
		foreach(lc, jtitems)
		{
			JoinTreeItem *otherjtitem = (JoinTreeItem *) lfirst(lc);
			SpecialJoinInfo *othersj = otherjtitem->sjinfo;
			bool		below_sjinfo = false;
			bool		above_sjinfo = false;
			Relids		this_qualscope;
			Relids		this_ojscope;
			bool		allow_equivalence,
						has_clone,
						is_clone;

			if (othersj == NULL)
				continue;		/* not an outer-join item, ignore */

			if (bms_is_member(othersj->ojrelid, joins_below))
			{
				/* othersj commutes with sjinfo from below left */
				below_sjinfo = true;
			}
			else if (othersj == sjinfo)
			{
				/* found our join in syntactic order */
				Assert(bms_equal(joins_so_far, joins_below));
			}
			else if (bms_is_member(othersj->ojrelid, joins_above))
			{
				/* othersj commutes with sjinfo from above */
				above_sjinfo = true;
			}
			else
			{
				/* othersj is not relevant, ignore */
				continue;
			}

			/* Reset serial counter for this version of the quals */
			root->last_rinfo_serial = save_last_rinfo_serial;

			/*
			 * When we are looking at joins above sjinfo, we are envisioning
			 * pushing sjinfo to above othersj, so add othersj's nulling bit
			 * before distributing the quals.  We should add it to Vars coming
			 * from the current join's LHS: we want to transform the second
			 * form of OJ identity 3 to the first form, in which Vars of
			 * relation B will appear nulled by the syntactically-upper OJ
			 * within the Pbc clause, but those of relation C will not.  (In
			 * the notation used by optimizer/README, we're converting a qual
			 * of the form Pbc to Pb*c.)  Of course, we must also remove that
			 * bit from the incompatible_joins value, else we'll make a qual
			 * that can't be placed anywhere.
			 */
			if (above_sjinfo)
			{
				quals = (List *)
					add_nulling_relids((Node *) quals,
									   sjinfo->syn_lefthand,
									   bms_make_singleton(othersj->ojrelid));
				incompatible_joins = bms_del_member(incompatible_joins,
													othersj->ojrelid);
			}

			/* Compute qualscope and ojscope for this join level */
			this_qualscope = bms_union(qualscope, joins_so_far);
			this_ojscope = bms_union(ojscope, joins_so_far);
			if (above_sjinfo)
			{
				/* othersj is not yet in joins_so_far, but we need it */
				this_qualscope = bms_add_member(this_qualscope,
												othersj->ojrelid);
				this_ojscope = bms_add_member(this_ojscope,
											  othersj->ojrelid);
				/* sjinfo is in joins_so_far, and we don't want it */
				this_ojscope = bms_del_member(this_ojscope,
											  sjinfo->ojrelid);
			}

			/*
			 * We generate EquivalenceClasses only from the first form of the
			 * quals, with the fewest nullingrels bits set.  An EC made from
			 * this version of the quals can be useful below the outer-join
			 * nest, whereas versions with some nullingrels bits set would not
			 * be.  We cannot generate ECs from more than one version, or
			 * we'll make nonsensical conclusions that Vars with nullingrels
			 * bits set are equal to their versions without.  Fortunately,
			 * such ECs wouldn't be very useful anyway, because they'd equate
			 * values not observable outside the join nest.  (See
			 * optimizer/README.)
			 *
			 * The first form of the quals is also the only one marked as
			 * has_clone rather than is_clone.
			 */
			allow_equivalence = (joins_so_far == NULL);
			has_clone = allow_equivalence;
			is_clone = !has_clone;

			distribute_quals_to_rels(root, quals,
									 otherjtitem,
									 sjinfo,
									 root->qual_security_level,
									 this_qualscope,
									 this_ojscope, nonnullable_rels,
									 bms_copy(incompatible_joins),
									 allow_equivalence,
									 has_clone,
									 is_clone,
									 NULL); /* no more postponement */

			/*
			 * Adjust qual nulling bits for next level up, if needed.  We
			 * don't want to put sjinfo's own bit in at all, and if we're
			 * above sjinfo then we did it already.  Here, we should mark all
			 * Vars coming from the lower join's RHS.  (Again, we are
			 * converting a qual of the form Pbc to Pb*c, but now we are
			 * putting back bits that were there in the parser output and were
			 * temporarily stripped above.)  Update incompatible_joins too.
			 */
			if (below_sjinfo)
			{
				quals = (List *)
					add_nulling_relids((Node *) quals,
									   othersj->syn_righthand,
									   bms_make_singleton(othersj->ojrelid));
				incompatible_joins = bms_del_member(incompatible_joins,
													othersj->ojrelid);
			}

			/* ... and track joins processed so far */
			joins_so_far = bms_add_member(joins_so_far, othersj->ojrelid);
		}
	}
	else
	{
		/* No commutation possible, just process the postponed clauses */
		distribute_quals_to_rels(root, jtitem->oj_joinclauses,
								 jtitem,
								 sjinfo,
								 root->qual_security_level,
								 qualscope,
								 ojscope, nonnullable_rels,
								 NULL,	/* incompatible_relids */
								 true,	/* allow_equivalence */
								 false, false,	/* not clones */
								 NULL); /* no more postponement */
	}
}


/*****************************************************************************
 *
 *	  QUALIFICATIONS
 *
 *****************************************************************************/

/*
 * distribute_quals_to_rels
 *	  Convenience routine to apply distribute_qual_to_rels to each element
 *	  of an AND'ed list of clauses.
 */
static void
distribute_quals_to_rels(PlannerInfo *root, List *clauses,
						 JoinTreeItem *jtitem,
						 SpecialJoinInfo *sjinfo,
						 Index security_level,
						 Relids qualscope,
						 Relids ojscope,
						 Relids outerjoin_nonnullable,
						 Relids incompatible_relids,
						 bool allow_equivalence,
						 bool has_clone,
						 bool is_clone,
						 List **postponed_oj_qual_list)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);

		distribute_qual_to_rels(root, clause,
								jtitem,
								sjinfo,
								security_level,
								qualscope,
								ojscope,
								outerjoin_nonnullable,
								incompatible_relids,
								allow_equivalence,
								has_clone,
								is_clone,
								postponed_oj_qual_list);
	}
}

/*
 * distribute_qual_to_rels
 *	  Add clause information to either the baserestrictinfo or joininfo list
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.  A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Alternatively, if the clause uses a
 *	  mergejoinable operator, enter its left- and right-side expressions into
 *	  the query's EquivalenceClasses.
 *
 * In some cases, quals will be added to parent jtitems' lateral_clauses
 * or to postponed_oj_qual_list instead of being processed right away.
 * These will be dealt with in later calls of deconstruct_distribute.
 *
 * 'clause': the qual clause to be distributed
 * 'jtitem': the JoinTreeItem for the containing jointree node
 * 'sjinfo': join's SpecialJoinInfo (NULL for an inner join or WHERE clause)
 * 'security_level': security_level to assign to the qual
 * 'qualscope': set of base+OJ rels the qual's syntactic scope covers
 * 'ojscope': NULL if not an outer-join qual, else the minimum set of base+OJ
 *		rels needed to form this join
 * 'outerjoin_nonnullable': NULL if not an outer-join qual, else the set of
 *		base+OJ rels appearing on the outer (nonnullable) side of the join
 *		(for FULL JOIN this includes both sides of the join, and must in fact
 *		equal qualscope)
 * 'incompatible_relids': the set of outer-join relid(s) that must not be
 *		computed below this qual.  We only bother to compute this for
 *		"clone" quals, otherwise it can be left NULL.
 * 'allow_equivalence': true if it's okay to convert clause into an
 *		EquivalenceClass
 * 'has_clone': has_clone property to assign to the qual
 * 'is_clone': is_clone property to assign to the qual
 * 'postponed_oj_qual_list': if not NULL, non-degenerate outer join clauses
 *		should be added to this list instead of being processed (list entries
 *		are just the bare clauses)
 *
 * 'qualscope' identifies what level of JOIN the qual came from syntactically.
 * 'ojscope' is needed if we decide to force the qual up to the outer-join
 * level, which will be ojscope not necessarily qualscope.
 *
 * At the time this is called, root->join_info_list must contain entries for
 * at least those special joins that are syntactically below this qual.
 * (We now need that only for detection of redundant IS NULL quals.)
 */
static void
distribute_qual_to_rels(PlannerInfo *root, Node *clause,
						JoinTreeItem *jtitem,
						SpecialJoinInfo *sjinfo,
						Index security_level,
						Relids qualscope,
						Relids ojscope,
						Relids outerjoin_nonnullable,
						Relids incompatible_relids,
						bool allow_equivalence,
						bool has_clone,
						bool is_clone,
						List **postponed_oj_qual_list)
{
	Relids		relids;
	bool		is_pushed_down;
	bool		pseudoconstant = false;
	bool		maybe_equivalence;
	bool		maybe_outer_join;
	RestrictInfo *restrictinfo;

	/*
	 * Retrieve all relids mentioned within the clause.
	 */
	relids = pull_varnos(root, clause);

	/*
	 * In ordinary SQL, a WHERE or JOIN/ON clause can't reference any rels
	 * that aren't within its syntactic scope; however, if we pulled up a
	 * LATERAL subquery then we might find such references in quals that have
	 * been pulled up.  We need to treat such quals as belonging to the join
	 * level that includes every rel they reference.  Although we could make
	 * pull_up_subqueries() place such quals correctly to begin with, it's
	 * easier to handle it here.  When we find a clause that contains Vars
	 * outside its syntactic scope, locate the nearest parent join level that
	 * includes all the required rels and add the clause to that level's
	 * lateral_clauses list.  We'll process it when we reach that join level.
	 */
	if (!bms_is_subset(relids, qualscope))
	{
		JoinTreeItem *pitem;

		Assert(root->hasLateralRTEs);	/* shouldn't happen otherwise */
		Assert(sjinfo == NULL); /* mustn't postpone past outer join */
		for (pitem = jtitem->jti_parent; pitem; pitem = pitem->jti_parent)
		{
			if (bms_is_subset(relids, pitem->qualscope))
			{
				pitem->lateral_clauses = lappend(pitem->lateral_clauses,
												 clause);
				return;
			}

			/*
			 * We should not be postponing any quals past an outer join.  If
			 * this Assert fires, pull_up_subqueries() messed up.
			 */
			Assert(pitem->sjinfo == NULL);
		}
		elog(ERROR, "failed to postpone qual containing lateral reference");
	}

	/*
	 * If it's an outer-join clause, also check that relids is a subset of
	 * ojscope.  (This should not fail if the syntactic scope check passed.)
	 */
	if (ojscope && !bms_is_subset(relids, ojscope))
		elog(ERROR, "JOIN qualification cannot refer to other relations");

	/*
	 * If the clause is variable-free, our normal heuristic for pushing it
	 * down to just the mentioned rels doesn't work, because there are none.
	 *
	 * If the clause is an outer-join clause, we must force it to the OJ's
	 * semantic level to preserve semantics.
	 *
	 * Otherwise, when the clause contains volatile functions, we force it to
	 * be evaluated at its original syntactic level.  This preserves the
	 * expected semantics.
	 *
	 * When the clause contains no volatile functions either, it is actually a
	 * pseudoconstant clause that will not change value during any one
	 * execution of the plan, and hence can be used as a one-time qual in a
	 * gating Result plan node.  We put such a clause into the regular
	 * RestrictInfo lists for the moment, but eventually createplan.c will
	 * pull it out and make a gating Result node immediately above whatever
	 * plan node the pseudoconstant clause is assigned to.  It's usually best
	 * to put a gating node as high in the plan tree as possible.
	 */
	if (bms_is_empty(relids))
	{
		if (ojscope)
		{
			/* clause is attached to outer join, eval it there */
			relids = bms_copy(ojscope);
			/* mustn't use as gating qual, so don't mark pseudoconstant */
		}
		else if (contain_volatile_functions(clause))
		{
			/* eval at original syntactic level */
			relids = bms_copy(qualscope);
			/* again, can't mark pseudoconstant */
		}
		else
		{
			/*
			 * If we are in the top-level join domain, we can push the qual to
			 * the top of the plan tree.  Otherwise, be conservative and eval
			 * it at original syntactic level.  (Ideally we'd push it to the
			 * top of the current join domain in all cases, but that causes
			 * problems if we later rearrange outer-join evaluation order.
			 * Pseudoconstant quals below the top level are a pretty odd case,
			 * so it's not clear that it's worth working hard on.)
			 */
			if (jtitem->jdomain == (JoinDomain *) linitial(root->join_domains))
				relids = bms_copy(jtitem->jdomain->jd_relids);
			else
				relids = bms_copy(qualscope);
			/* mark as gating qual */
			pseudoconstant = true;
			/* tell createplan.c to check for gating quals */
			root->hasPseudoConstantQuals = true;
		}
	}

	/*----------
	 * Check to see if clause application must be delayed by outer-join
	 * considerations.
	 *
	 * A word about is_pushed_down: we mark the qual as "pushed down" if
	 * it is (potentially) applicable at a level different from its original
	 * syntactic level.  This flag is used to distinguish OUTER JOIN ON quals
	 * from other quals pushed down to the same joinrel.  The rules are:
	 *		WHERE quals and INNER JOIN quals: is_pushed_down = true.
	 *		Non-degenerate OUTER JOIN quals: is_pushed_down = false.
	 *		Degenerate OUTER JOIN quals: is_pushed_down = true.
	 * A "degenerate" OUTER JOIN qual is one that doesn't mention the
	 * non-nullable side, and hence can be pushed down into the nullable side
	 * without changing the join result.  It is correct to treat it as a
	 * regular filter condition at the level where it is evaluated.
	 *
	 * Note: it is not immediately obvious that a simple boolean is enough
	 * for this: if for some reason we were to attach a degenerate qual to
	 * its original join level, it would need to be treated as an outer join
	 * qual there.  However, this cannot happen, because all the rels the
	 * clause mentions must be in the outer join's min_righthand, therefore
	 * the join it needs must be formed before the outer join; and we always
	 * attach quals to the lowest level where they can be evaluated.  But
	 * if we were ever to re-introduce a mechanism for delaying evaluation
	 * of "expensive" quals, this area would need work.
	 *
	 * Note: generally, use of is_pushed_down has to go through the macro
	 * RINFO_IS_PUSHED_DOWN, because that flag alone is not always sufficient
	 * to tell whether a clause must be treated as pushed-down in context.
	 * This seems like another reason why it should perhaps be rethought.
	 *----------
	 */
	if (bms_overlap(relids, outerjoin_nonnullable))
	{
		/*
		 * The qual is attached to an outer join and mentions (some of the)
		 * rels on the nonnullable side, so it's not degenerate.  If the
		 * caller wants to postpone handling such clauses, just add it to
		 * postponed_oj_qual_list and return.  (The work we've done up to here
		 * will have to be redone later, but there's not much of it.)
		 */
		if (postponed_oj_qual_list != NULL)
		{
			*postponed_oj_qual_list = lappend(*postponed_oj_qual_list, clause);
			return;
		}

		/*
		 * We can't use such a clause to deduce equivalence (the left and
		 * right sides might be unequal above the join because one of them has
		 * gone to NULL) ... but we might be able to use it for more limited
		 * deductions, if it is mergejoinable.  So consider adding it to the
		 * lists of set-aside outer-join clauses.
		 */
		is_pushed_down = false;
		maybe_equivalence = false;
		maybe_outer_join = true;

		/*
		 * Now force the qual to be evaluated exactly at the level of joining
		 * corresponding to the outer join.  We cannot let it get pushed down
		 * into the nonnullable side, since then we'd produce no output rows,
		 * rather than the intended single null-extended row, for any
		 * nonnullable-side rows failing the qual.
		 */
		Assert(ojscope);
		relids = ojscope;
		Assert(!pseudoconstant);
	}
	else
	{
		/*
		 * Normal qual clause or degenerate outer-join clause.  Either way, we
		 * can mark it as pushed-down.
		 */
		is_pushed_down = true;

		/*
		 * It's possible that this is an IS NULL clause that's redundant with
		 * a lower antijoin; if so we can just discard it.  We need not test
		 * in any of the other cases, because this will only be possible for
		 * pushed-down clauses.
		 */
		if (check_redundant_nullability_qual(root, clause))
			return;

		/* Feed qual to the equivalence machinery, if allowed by caller */
		maybe_equivalence = allow_equivalence;

		/*
		 * Since it doesn't mention the LHS, it's certainly not useful as a
		 * set-aside OJ clause, even if it's in an OJ.
		 */
		maybe_outer_join = false;
	}

	/*
	 * Build the RestrictInfo node itself.
	 */
	restrictinfo = make_restrictinfo(root,
									 (Expr *) clause,
									 is_pushed_down,
									 has_clone,
									 is_clone,
									 pseudoconstant,
									 security_level,
									 relids,
									 incompatible_relids,
									 outerjoin_nonnullable);

	/*
	 * If it's a join clause, add vars used in the clause to targetlists of
	 * their relations, so that they will be emitted by the plan nodes that
	 * scan those relations (else they won't be available at the join node!).
	 *
	 * Normally we mark the vars as needed at the join identified by "relids".
	 * However, if this is a clone clause then ignore the outer-join relids in
	 * that set.  Otherwise, vars appearing in a cloned clause would end up
	 * marked as having to propagate to the highest one of the commuting
	 * joins, which would often be an overestimate.  For such clauses, correct
	 * var propagation is ensured by making ojscope include input rels from
	 * both sides of the join.
	 *
	 * See also rebuild_joinclause_attr_needed, which has to partially repeat
	 * this work after removal of an outer join.
	 *
	 * Note: if the clause gets absorbed into an EquivalenceClass then this
	 * may be unnecessary, but for now we have to do it to cover the case
	 * where the EC becomes ec_broken and we end up reinserting the original
	 * clauses into the plan.
	 */
	if (bms_membership(relids) == BMS_MULTIPLE)
	{
		List	   *vars = pull_var_clause(clause,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);
		Relids		where_needed;

		if (is_clone)
			where_needed = bms_intersect(relids, root->all_baserels);
		else
			where_needed = relids;
		add_vars_to_targetlist(root, vars, where_needed);
		list_free(vars);
	}

	/*
	 * We check "mergejoinability" of every clause, not only join clauses,
	 * because we want to know about equivalences between vars of the same
	 * relation, or between vars and consts.
	 */
	check_mergejoinable(restrictinfo);

	/*
	 * If it is a true equivalence clause, send it to the EquivalenceClass
	 * machinery.  We do *not* attach it directly to any restriction or join
	 * lists.  The EC code will propagate it to the appropriate places later.
	 *
	 * If the clause has a mergejoinable operator, yet isn't an equivalence
	 * because it is an outer-join clause, the EC code may still be able to do
	 * something with it.  We add it to appropriate lists for further
	 * consideration later.  Specifically:
	 *
	 * If it is a left or right outer-join qualification that relates the two
	 * sides of the outer join (no funny business like leftvar1 = leftvar2 +
	 * rightvar), we add it to root->left_join_clauses or
	 * root->right_join_clauses according to which side the nonnullable
	 * variable appears on.
	 *
	 * If it is a full outer-join qualification, we add it to
	 * root->full_join_clauses.  (Ideally we'd discard cases that aren't
	 * leftvar = rightvar, as we do for left/right joins, but this routine
	 * doesn't have the info needed to do that; and the current usage of the
	 * full_join_clauses list doesn't require that, so it's not currently
	 * worth complicating this routine's API to make it possible.)
	 *
	 * If none of the above hold, pass it off to
	 * distribute_restrictinfo_to_rels().
	 *
	 * In all cases, it's important to initialize the left_ec and right_ec
	 * fields of a mergejoinable clause, so that all possibly mergejoinable
	 * expressions have representations in EquivalenceClasses.  If
	 * process_equivalence is successful, it will take care of that;
	 * otherwise, we have to call initialize_mergeclause_eclasses to do it.
	 */
	if (restrictinfo->mergeopfamilies)
	{
		if (maybe_equivalence)
		{
			if (process_equivalence(root, &restrictinfo, jtitem->jdomain))
				return;
			/* EC rejected it, so set left_ec/right_ec the hard way ... */
			if (restrictinfo->mergeopfamilies)	/* EC might have changed this */
				initialize_mergeclause_eclasses(root, restrictinfo);
			/* ... and fall through to distribute_restrictinfo_to_rels */
		}
		else if (maybe_outer_join && restrictinfo->can_join)
		{
			/* we need to set up left_ec/right_ec the hard way */
			initialize_mergeclause_eclasses(root, restrictinfo);
			/* now see if it should go to any outer-join lists */
			Assert(sjinfo != NULL);
			if (bms_is_subset(restrictinfo->left_relids,
							  outerjoin_nonnullable) &&
				!bms_overlap(restrictinfo->right_relids,
							 outerjoin_nonnullable))
			{
				/* we have outervar = innervar */
				OuterJoinClauseInfo *ojcinfo = makeNode(OuterJoinClauseInfo);

				ojcinfo->rinfo = restrictinfo;
				ojcinfo->sjinfo = sjinfo;
				root->left_join_clauses = lappend(root->left_join_clauses,
												  ojcinfo);
				return;
			}
			if (bms_is_subset(restrictinfo->right_relids,
							  outerjoin_nonnullable) &&
				!bms_overlap(restrictinfo->left_relids,
							 outerjoin_nonnullable))
			{
				/* we have innervar = outervar */
				OuterJoinClauseInfo *ojcinfo = makeNode(OuterJoinClauseInfo);

				ojcinfo->rinfo = restrictinfo;
				ojcinfo->sjinfo = sjinfo;
				root->right_join_clauses = lappend(root->right_join_clauses,
												   ojcinfo);
				return;
			}
			if (sjinfo->jointype == JOIN_FULL)
			{
				/* FULL JOIN (above tests cannot match in this case) */
				OuterJoinClauseInfo *ojcinfo = makeNode(OuterJoinClauseInfo);

				ojcinfo->rinfo = restrictinfo;
				ojcinfo->sjinfo = sjinfo;
				root->full_join_clauses = lappend(root->full_join_clauses,
												  ojcinfo);
				return;
			}
			/* nope, so fall through to distribute_restrictinfo_to_rels */
		}
		else
		{
			/* we still need to set up left_ec/right_ec */
			initialize_mergeclause_eclasses(root, restrictinfo);
		}
	}

	/* No EC special case applies, so push it into the clause lists */
	distribute_restrictinfo_to_rels(root, restrictinfo);
}

/*
 * check_redundant_nullability_qual
 *	  Check to see if the qual is an IS NULL qual that is redundant with
 *	  a lower JOIN_ANTI join.
 *
 * We want to suppress redundant IS NULL quals, not so much to save cycles
 * as to avoid generating bogus selectivity estimates for them.  So if
 * redundancy is detected here, distribute_qual_to_rels() just throws away
 * the qual.
 */
static bool
check_redundant_nullability_qual(PlannerInfo *root, Node *clause)
{
	Var		   *forced_null_var;
	ListCell   *lc;

	/* Check for IS NULL, and identify the Var forced to NULL */
	forced_null_var = find_forced_null_var(clause);
	if (forced_null_var == NULL)
		return false;

	/*
	 * If the Var comes from the nullable side of a lower antijoin, the IS
	 * NULL condition is necessarily true.  If it's not nulled by anything,
	 * there is no point in searching the join_info_list.  Otherwise, we need
	 * to find out whether the nulling rel is an antijoin.
	 */
	if (forced_null_var->varnullingrels == NULL)
		return false;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		/*
		 * This test will not succeed if sjinfo->ojrelid is zero, which is
		 * possible for an antijoin that was converted from a semijoin; but in
		 * such a case the Var couldn't have come from its nullable side.
		 */
		if (sjinfo->jointype == JOIN_ANTI && sjinfo->ojrelid != 0 &&
			bms_is_member(sjinfo->ojrelid, forced_null_var->varnullingrels))
			return true;
	}

	return false;
}

/*
 * add_base_clause_to_rel
 *		Add 'restrictinfo' as a baserestrictinfo to the base relation denoted
 *		by 'relid'.  We offer some simple prechecks to try to determine if the
 *		qual is always true, in which case we ignore it rather than add it.
 *		If we detect the qual is always false, we replace it with
 *		constant-FALSE.
 */
static void
add_base_clause_to_rel(PlannerInfo *root, Index relid,
					   RestrictInfo *restrictinfo)
{
	RelOptInfo *rel = find_base_rel(root, relid);
	RangeTblEntry *rte = root->simple_rte_array[relid];

	Assert(bms_membership(restrictinfo->required_relids) == BMS_SINGLETON);

	/*
	 * For inheritance parent tables, we must always record the RestrictInfo
	 * in baserestrictinfo as is.  If we were to transform or skip adding it,
	 * then the original wouldn't be available in apply_child_basequals. Since
	 * there are two RangeTblEntries for inheritance parents, one with
	 * inh==true and the other with inh==false, we're still able to apply this
	 * optimization to the inh==false one.  The inh==true one is what
	 * apply_child_basequals() sees, whereas the inh==false one is what's used
	 * for the scan node in the final plan.
	 *
	 * We make an exception to this for partitioned tables.  For these, we
	 * always apply the constant-TRUE and constant-FALSE transformations.  A
	 * qual which is either of these for a partitioned table must also be that
	 * for all of its child partitions.
	 */
	if (!rte->inh || rte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* Don't add the clause if it is always true */
		if (restriction_is_always_true(root, restrictinfo))
			return;

		/*
		 * Substitute the origin qual with constant-FALSE if it is provably
		 * always false.  Note that we keep the same rinfo_serial.
		 */
		if (restriction_is_always_false(root, restrictinfo))
		{
			int			save_rinfo_serial = restrictinfo->rinfo_serial;

			restrictinfo = make_restrictinfo(root,
											 (Expr *) makeBoolConst(false, false),
											 restrictinfo->is_pushed_down,
											 restrictinfo->has_clone,
											 restrictinfo->is_clone,
											 restrictinfo->pseudoconstant,
											 0, /* security_level */
											 restrictinfo->required_relids,
											 restrictinfo->incompatible_relids,
											 restrictinfo->outer_relids);
			restrictinfo->rinfo_serial = save_rinfo_serial;
		}
	}

	/* Add clause to rel's restriction list */
	rel->baserestrictinfo = lappend(rel->baserestrictinfo, restrictinfo);

	/* Update security level info */
	rel->baserestrict_min_security = Min(rel->baserestrict_min_security,
										 restrictinfo->security_level);
}

/*
 * expr_is_nonnullable
 *	  Check to see if the Expr cannot be NULL
 *
 * If the Expr is a simple Var that is defined NOT NULL and meanwhile is not
 * nulled by any outer joins, then we can know that it cannot be NULL.
 */
static bool
expr_is_nonnullable(PlannerInfo *root, Expr *expr)
{
	RelOptInfo *rel;
	Var		   *var;

	/* For now only check simple Vars */
	if (!IsA(expr, Var))
		return false;

	var = (Var *) expr;

	/* could the Var be nulled by any outer joins? */
	if (!bms_is_empty(var->varnullingrels))
		return false;

	/* system columns cannot be NULL */
	if (var->varattno < 0)
		return true;

	/* is the column defined NOT NULL? */
	rel = find_base_rel(root, var->varno);
	if (var->varattno > 0 &&
		bms_is_member(var->varattno, rel->notnullattnums))
		return true;

	return false;
}

/*
 * restriction_is_always_true
 *	  Check to see if the RestrictInfo is always true.
 *
 * Currently we only check for NullTest quals and OR clauses that include
 * NullTest quals.  We may extend it in the future.
 */
bool
restriction_is_always_true(PlannerInfo *root,
						   RestrictInfo *restrictinfo)
{
	/* Check for NullTest qual */
	if (IsA(restrictinfo->clause, NullTest))
	{
		NullTest   *nulltest = (NullTest *) restrictinfo->clause;

		/* is this NullTest an IS_NOT_NULL qual? */
		if (nulltest->nulltesttype != IS_NOT_NULL)
			return false;

		return expr_is_nonnullable(root, nulltest->arg);
	}

	/* If it's an OR, check its sub-clauses */
	if (restriction_is_or_clause(restrictinfo))
	{
		ListCell   *lc;

		Assert(is_orclause(restrictinfo->orclause));

		/*
		 * if any of the given OR branches is provably always true then the
		 * entire condition is true.
		 */
		foreach(lc, ((BoolExpr *) restrictinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(lc);

			if (!IsA(orarg, RestrictInfo))
				continue;

			if (restriction_is_always_true(root, (RestrictInfo *) orarg))
				return true;
		}
	}

	return false;
}

/*
 * restriction_is_always_false
 *	  Check to see if the RestrictInfo is always false.
 *
 * Currently we only check for NullTest quals and OR clauses that include
 * NullTest quals.  We may extend it in the future.
 */
bool
restriction_is_always_false(PlannerInfo *root,
							RestrictInfo *restrictinfo)
{
	/* Check for NullTest qual */
	if (IsA(restrictinfo->clause, NullTest))
	{
		NullTest   *nulltest = (NullTest *) restrictinfo->clause;

		/* is this NullTest an IS_NULL qual? */
		if (nulltest->nulltesttype != IS_NULL)
			return false;

		return expr_is_nonnullable(root, nulltest->arg);
	}

	/* If it's an OR, check its sub-clauses */
	if (restriction_is_or_clause(restrictinfo))
	{
		ListCell   *lc;

		Assert(is_orclause(restrictinfo->orclause));

		/*
		 * Currently, when processing OR expressions, we only return true when
		 * all of the OR branches are always false.  This could perhaps be
		 * expanded to remove OR branches that are provably false.  This may
		 * be a useful thing to do as it could result in the OR being left
		 * with a single arg.  That's useful as it would allow the OR
		 * condition to be replaced with its single argument which may allow
		 * use of an index for faster filtering on the remaining condition.
		 */
		foreach(lc, ((BoolExpr *) restrictinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(lc);

			if (!IsA(orarg, RestrictInfo) ||
				!restriction_is_always_false(root, (RestrictInfo *) orarg))
				return false;
		}
		return true;
	}

	return false;
}

/*
 * distribute_restrictinfo_to_rels
 *	  Push a completed RestrictInfo into the proper restriction or join
 *	  clause list(s).
 *
 * This is the last step of distribute_qual_to_rels() for ordinary qual
 * clauses.  Clauses that are interesting for equivalence-class processing
 * are diverted to the EC machinery, but may ultimately get fed back here.
 */
void
distribute_restrictinfo_to_rels(PlannerInfo *root,
								RestrictInfo *restrictinfo)
{
	Relids		relids = restrictinfo->required_relids;

	if (!bms_is_empty(relids))
	{
		int			relid;

		if (bms_get_singleton_member(relids, &relid))
		{
			/*
			 * There is only one relation participating in the clause, so it
			 * is a restriction clause for that relation.
			 */
			add_base_clause_to_rel(root, relid, restrictinfo);
		}
		else
		{
			/*
			 * The clause is a join clause, since there is more than one rel
			 * in its relid set.
			 */

			/*
			 * Check for hashjoinable operators.  (We don't bother setting the
			 * hashjoin info except in true join clauses.)
			 */
			check_hashjoinable(restrictinfo);

			/*
			 * Likewise, check if the clause is suitable to be used with a
			 * Memoize node to cache inner tuples during a parameterized
			 * nested loop.
			 */
			check_memoizable(restrictinfo);

			/*
			 * Add clause to the join lists of all the relevant relations.
			 */
			add_join_clause_to_rels(root, restrictinfo, relids);
		}
	}
	else
	{
		/*
		 * clause references no rels, and therefore we have no place to attach
		 * it.  Shouldn't get here if callers are working properly.
		 */
		elog(ERROR, "cannot cope with variable-free clause");
	}
}

/*
 * process_implied_equality
 *	  Create a restrictinfo item that says "item1 op item2", and push it
 *	  into the appropriate lists.  (In practice opno is always a btree
 *	  equality operator.)
 *
 * "qualscope" is the nominal syntactic level to impute to the restrictinfo.
 * This must contain at least all the rels used in the expressions, but it
 * is used only to set the qual application level when both exprs are
 * variable-free.  (Hence, it should usually match the join domain in which
 * the clause applies.)  Otherwise the qual is applied at the lowest join
 * level that provides all its variables.
 *
 * "security_level" is the security level to assign to the new restrictinfo.
 *
 * "both_const" indicates whether both items are known pseudo-constant;
 * in this case it is worth applying eval_const_expressions() in case we
 * can produce constant TRUE or constant FALSE.  (Otherwise it's not,
 * because the expressions went through eval_const_expressions already.)
 *
 * Returns the generated RestrictInfo, if any.  The result will be NULL
 * if both_const is true and we successfully reduced the clause to
 * constant TRUE.
 *
 * Note: this function will copy item1 and item2, but it is caller's
 * responsibility to make sure that the Relids parameters are fresh copies
 * not shared with other uses.
 *
 * Note: we do not do initialize_mergeclause_eclasses() here.  It is
 * caller's responsibility that left_ec/right_ec be set as necessary.
 */
RestrictInfo *
process_implied_equality(PlannerInfo *root,
						 Oid opno,
						 Oid collation,
						 Expr *item1,
						 Expr *item2,
						 Relids qualscope,
						 Index security_level,
						 bool both_const)
{
	RestrictInfo *restrictinfo;
	Node	   *clause;
	Relids		relids;
	bool		pseudoconstant = false;

	/*
	 * Build the new clause.  Copy to ensure it shares no substructure with
	 * original (this is necessary in case there are subselects in there...)
	 */
	clause = (Node *) make_opclause(opno,
									BOOLOID,	/* opresulttype */
									false,	/* opretset */
									copyObject(item1),
									copyObject(item2),
									InvalidOid,
									collation);

	/* If both constant, try to reduce to a boolean constant. */
	if (both_const)
	{
		clause = eval_const_expressions(root, clause);

		/* If we produced const TRUE, just drop the clause */
		if (clause && IsA(clause, Const))
		{
			Const	   *cclause = (Const *) clause;

			Assert(cclause->consttype == BOOLOID);
			if (!cclause->constisnull && DatumGetBool(cclause->constvalue))
				return NULL;
		}
	}

	/*
	 * The rest of this is a very cut-down version of distribute_qual_to_rels.
	 * We can skip most of the work therein, but there are a couple of special
	 * cases we still have to handle.
	 *
	 * Retrieve all relids mentioned within the possibly-simplified clause.
	 */
	relids = pull_varnos(root, clause);
	Assert(bms_is_subset(relids, qualscope));

	/*
	 * If the clause is variable-free, our normal heuristic for pushing it
	 * down to just the mentioned rels doesn't work, because there are none.
	 * Apply it as a gating qual at the appropriate level (see comments for
	 * get_join_domain_min_rels).
	 */
	if (bms_is_empty(relids))
	{
		/* eval at join domain's safe level */
		relids = get_join_domain_min_rels(root, qualscope);
		/* mark as gating qual */
		pseudoconstant = true;
		/* tell createplan.c to check for gating quals */
		root->hasPseudoConstantQuals = true;
	}

	/*
	 * Build the RestrictInfo node itself.
	 */
	restrictinfo = make_restrictinfo(root,
									 (Expr *) clause,
									 true,	/* is_pushed_down */
									 false, /* !has_clone */
									 false, /* !is_clone */
									 pseudoconstant,
									 security_level,
									 relids,
									 NULL,	/* incompatible_relids */
									 NULL); /* outer_relids */

	/*
	 * If it's a join clause, add vars used in the clause to targetlists of
	 * their relations, so that they will be emitted by the plan nodes that
	 * scan those relations (else they won't be available at the join node!).
	 *
	 * Typically, we'd have already done this when the component expressions
	 * were first seen by distribute_qual_to_rels; but it is possible that
	 * some of the Vars could have missed having that done because they only
	 * appeared in single-relation clauses originally.  So do it here for
	 * safety.
	 *
	 * See also rebuild_joinclause_attr_needed, which has to partially repeat
	 * this work after removal of an outer join.  (Since we will put this
	 * clause into the joininfo lists, that function needn't do any extra work
	 * to find it.)
	 */
	if (bms_membership(relids) == BMS_MULTIPLE)
	{
		List	   *vars = pull_var_clause(clause,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, relids);
		list_free(vars);
	}

	/*
	 * Check mergejoinability.  This will usually succeed, since the op came
	 * from an EquivalenceClass; but we could have reduced the original clause
	 * to a constant.
	 */
	check_mergejoinable(restrictinfo);

	/*
	 * Note we don't do initialize_mergeclause_eclasses(); the caller can
	 * handle that much more cheaply than we can.  It's okay to call
	 * distribute_restrictinfo_to_rels() before that happens.
	 */

	/*
	 * Push the new clause into all the appropriate restrictinfo lists.
	 */
	distribute_restrictinfo_to_rels(root, restrictinfo);

	return restrictinfo;
}

/*
 * build_implied_join_equality --- build a RestrictInfo for a derived equality
 *
 * This overlaps the functionality of process_implied_equality(), but we
 * must not push the RestrictInfo into the joininfo tree.
 *
 * Note: this function will copy item1 and item2, but it is caller's
 * responsibility to make sure that the Relids parameters are fresh copies
 * not shared with other uses.
 *
 * Note: we do not do initialize_mergeclause_eclasses() here.  It is
 * caller's responsibility that left_ec/right_ec be set as necessary.
 */
RestrictInfo *
build_implied_join_equality(PlannerInfo *root,
							Oid opno,
							Oid collation,
							Expr *item1,
							Expr *item2,
							Relids qualscope,
							Index security_level)
{
	RestrictInfo *restrictinfo;
	Expr	   *clause;

	/*
	 * Build the new clause.  Copy to ensure it shares no substructure with
	 * original (this is necessary in case there are subselects in there...)
	 */
	clause = make_opclause(opno,
						   BOOLOID, /* opresulttype */
						   false,	/* opretset */
						   copyObject(item1),
						   copyObject(item2),
						   InvalidOid,
						   collation);

	/*
	 * Build the RestrictInfo node itself.
	 */
	restrictinfo = make_restrictinfo(root,
									 clause,
									 true,	/* is_pushed_down */
									 false, /* !has_clone */
									 false, /* !is_clone */
									 false, /* pseudoconstant */
									 security_level,	/* security_level */
									 qualscope, /* required_relids */
									 NULL,	/* incompatible_relids */
									 NULL); /* outer_relids */

	/* Set mergejoinability/hashjoinability flags */
	check_mergejoinable(restrictinfo);
	check_hashjoinable(restrictinfo);
	check_memoizable(restrictinfo);

	return restrictinfo;
}

/*
 * get_join_domain_min_rels
 *	  Identify the appropriate join level for derived quals belonging
 *	  to the join domain with the given relids.
 *
 * When we derive a pseudoconstant (Var-free) clause from an EquivalenceClass,
 * we'd ideally apply the clause at the top level of the EC's join domain.
 * However, if there are any outer joins inside that domain that get commuted
 * with joins outside it, that leads to not finding a correct place to apply
 * the clause.  Instead, remove any lower outer joins from the relid set,
 * and apply the clause to just the remaining rels.  This still results in a
 * correct answer, since if the clause produces FALSE then the LHS of these
 * joins will be empty leading to an empty join result.
 *
 * However, there's no need to remove outer joins if this is the top-level
 * join domain of the query, since then there's nothing else to commute with.
 *
 * Note: it's tempting to use this in distribute_qual_to_rels where it's
 * dealing with pseudoconstant quals; but we can't because the necessary
 * SpecialJoinInfos aren't all formed at that point.
 *
 * The result is always freshly palloc'd; we do not modify domain_relids.
 */
static Relids
get_join_domain_min_rels(PlannerInfo *root, Relids domain_relids)
{
	Relids		result = bms_copy(domain_relids);
	ListCell   *lc;

	/* Top-level join domain? */
	if (bms_equal(result, root->all_query_rels))
		return result;

	/* Nope, look for lower outer joins that could potentially commute out */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_LEFT &&
			bms_is_member(sjinfo->ojrelid, result))
		{
			result = bms_del_member(result, sjinfo->ojrelid);
			result = bms_del_members(result, sjinfo->syn_righthand);
		}
	}
	return result;
}


/*
 * rebuild_joinclause_attr_needed
 *	  Put back attr_needed bits for Vars/PHVs needed for join clauses.
 *
 * This is used to rebuild attr_needed/ph_needed sets after removal of a
 * useless outer join.  It should match what distribute_qual_to_rels did,
 * except that we call add_vars_to_attr_needed not add_vars_to_targetlist.
 */
void
rebuild_joinclause_attr_needed(PlannerInfo *root)
{
	/*
	 * We must examine all join clauses, but there's no value in processing
	 * any join clause more than once.  So it's slightly annoying that we have
	 * to find them via the per-base-relation joininfo lists.  Avoid duplicate
	 * processing by tracking the rinfo_serial numbers of join clauses we've
	 * already seen.  (This doesn't work for is_clone clauses, so we must
	 * waste effort on them.)
	 */
	Bitmapset  *seen_serials = NULL;
	Index		rti;

	/* Scan all baserels for join clauses */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		ListCell   *lc;

		if (brel == NULL)
			continue;
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		foreach(lc, brel->joininfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
			Relids		relids = rinfo->required_relids;

			if (!rinfo->is_clone)	/* else serial number is not unique */
			{
				if (bms_is_member(rinfo->rinfo_serial, seen_serials))
					continue;	/* saw it already */
				seen_serials = bms_add_member(seen_serials,
											  rinfo->rinfo_serial);
			}

			if (bms_membership(relids) == BMS_MULTIPLE)
			{
				List	   *vars = pull_var_clause((Node *) rinfo->clause,
												   PVC_RECURSE_AGGREGATES |
												   PVC_RECURSE_WINDOWFUNCS |
												   PVC_INCLUDE_PLACEHOLDERS);
				Relids		where_needed;

				if (rinfo->is_clone)
					where_needed = bms_intersect(relids, root->all_baserels);
				else
					where_needed = relids;
				add_vars_to_attr_needed(root, vars, where_needed);
				list_free(vars);
			}
		}
	}
}


/*
 * match_foreign_keys_to_quals
 *		Match foreign-key constraints to equivalence classes and join quals
 *
 * The idea here is to see which query join conditions match equality
 * constraints of a foreign-key relationship.  For such join conditions,
 * we can use the FK semantics to make selectivity estimates that are more
 * reliable than estimating from statistics, especially for multiple-column
 * FKs, where the normal assumption of independent conditions tends to fail.
 *
 * In this function we annotate the ForeignKeyOptInfos in root->fkey_list
 * with info about which eclasses and join qual clauses they match, and
 * discard any ForeignKeyOptInfos that are irrelevant for the query.
 */
void
match_foreign_keys_to_quals(PlannerInfo *root)
{
	List	   *newlist = NIL;
	ListCell   *lc;

	foreach(lc, root->fkey_list)
	{
		ForeignKeyOptInfo *fkinfo = (ForeignKeyOptInfo *) lfirst(lc);
		RelOptInfo *con_rel;
		RelOptInfo *ref_rel;
		int			colno;

		/*
		 * Either relid might identify a rel that is in the query's rtable but
		 * isn't referenced by the jointree, or has been removed by join
		 * removal, so that it won't have a RelOptInfo.  Hence don't use
		 * find_base_rel() here.  We can ignore such FKs.
		 */
		if (fkinfo->con_relid >= root->simple_rel_array_size ||
			fkinfo->ref_relid >= root->simple_rel_array_size)
			continue;			/* just paranoia */
		con_rel = root->simple_rel_array[fkinfo->con_relid];
		if (con_rel == NULL)
			continue;
		ref_rel = root->simple_rel_array[fkinfo->ref_relid];
		if (ref_rel == NULL)
			continue;

		/*
		 * Ignore FK unless both rels are baserels.  This gets rid of FKs that
		 * link to inheritance child rels (otherrels).
		 */
		if (con_rel->reloptkind != RELOPT_BASEREL ||
			ref_rel->reloptkind != RELOPT_BASEREL)
			continue;

		/*
		 * Scan the columns and try to match them to eclasses and quals.
		 *
		 * Note: for simple inner joins, any match should be in an eclass.
		 * "Loose" quals that syntactically match an FK equality must have
		 * been rejected for EC status because they are outer-join quals or
		 * similar.  We can still consider them to match the FK.
		 */
		for (colno = 0; colno < fkinfo->nkeys; colno++)
		{
			EquivalenceClass *ec;
			AttrNumber	con_attno,
						ref_attno;
			Oid			fpeqop;
			ListCell   *lc2;

			ec = match_eclasses_to_foreign_key_col(root, fkinfo, colno);
			/* Don't bother looking for loose quals if we got an EC match */
			if (ec != NULL)
			{
				fkinfo->nmatched_ec++;
				if (ec->ec_has_const)
					fkinfo->nconst_ec++;
				continue;
			}

			/*
			 * Scan joininfo list for relevant clauses.  Either rel's joininfo
			 * list would do equally well; we use con_rel's.
			 */
			con_attno = fkinfo->conkey[colno];
			ref_attno = fkinfo->confkey[colno];
			fpeqop = InvalidOid;	/* we'll look this up only if needed */

			foreach(lc2, con_rel->joininfo)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc2);
				OpExpr	   *clause = (OpExpr *) rinfo->clause;
				Var		   *leftvar;
				Var		   *rightvar;

				/* Only binary OpExprs are useful for consideration */
				if (!IsA(clause, OpExpr) ||
					list_length(clause->args) != 2)
					continue;
				leftvar = (Var *) get_leftop((Expr *) clause);
				rightvar = (Var *) get_rightop((Expr *) clause);

				/* Operands must be Vars, possibly with RelabelType */
				while (leftvar && IsA(leftvar, RelabelType))
					leftvar = (Var *) ((RelabelType *) leftvar)->arg;
				if (!(leftvar && IsA(leftvar, Var)))
					continue;
				while (rightvar && IsA(rightvar, RelabelType))
					rightvar = (Var *) ((RelabelType *) rightvar)->arg;
				if (!(rightvar && IsA(rightvar, Var)))
					continue;

				/* Now try to match the vars to the current foreign key cols */
				if (fkinfo->ref_relid == leftvar->varno &&
					ref_attno == leftvar->varattno &&
					fkinfo->con_relid == rightvar->varno &&
					con_attno == rightvar->varattno)
				{
					/* Vars match, but is it the right operator? */
					if (clause->opno == fkinfo->conpfeqop[colno])
					{
						fkinfo->rinfos[colno] = lappend(fkinfo->rinfos[colno],
														rinfo);
						fkinfo->nmatched_ri++;
					}
				}
				else if (fkinfo->ref_relid == rightvar->varno &&
						 ref_attno == rightvar->varattno &&
						 fkinfo->con_relid == leftvar->varno &&
						 con_attno == leftvar->varattno)
				{
					/*
					 * Reverse match, must check commutator operator.  Look it
					 * up if we didn't already.  (In the worst case we might
					 * do multiple lookups here, but that would require an FK
					 * equality operator without commutator, which is
					 * unlikely.)
					 */
					if (!OidIsValid(fpeqop))
						fpeqop = get_commutator(fkinfo->conpfeqop[colno]);
					if (clause->opno == fpeqop)
					{
						fkinfo->rinfos[colno] = lappend(fkinfo->rinfos[colno],
														rinfo);
						fkinfo->nmatched_ri++;
					}
				}
			}
			/* If we found any matching loose quals, count col as matched */
			if (fkinfo->rinfos[colno])
				fkinfo->nmatched_rcols++;
		}

		/*
		 * Currently, we drop multicolumn FKs that aren't fully matched to the
		 * query.  Later we might figure out how to derive some sort of
		 * estimate from them, in which case this test should be weakened to
		 * "if ((fkinfo->nmatched_ec + fkinfo->nmatched_rcols) > 0)".
		 */
		if ((fkinfo->nmatched_ec + fkinfo->nmatched_rcols) == fkinfo->nkeys)
			newlist = lappend(newlist, fkinfo);
	}
	/* Replace fkey_list, thereby discarding any useless entries */
	root->fkey_list = newlist;
}


/*****************************************************************************
 *
 *	 CHECKS FOR MERGEJOINABLE AND HASHJOINABLE CLAUSES
 *
 *****************************************************************************/

/*
 * check_mergejoinable
 *	  If the restrictinfo's clause is mergejoinable, set the mergejoin
 *	  info fields in the restrictinfo.
 *
 *	  Currently, we support mergejoin for binary opclauses where
 *	  the operator is a mergejoinable operator.  The arguments can be
 *	  anything --- as long as there are no volatile functions in them.
 */
static void
check_mergejoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;
	Node	   *leftarg;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;
	leftarg = linitial(((OpExpr *) clause)->args);

	if (op_mergejoinable(opno, exprType(leftarg)) &&
		!contain_volatile_functions((Node *) restrictinfo))
		restrictinfo->mergeopfamilies = get_mergejoin_opfamilies(opno);

	/*
	 * Note: op_mergejoinable is just a hint; if we fail to find the operator
	 * in any btree opfamilies, mergeopfamilies remains NIL and so the clause
	 * is not treated as mergejoinable.
	 */
}

/*
 * check_hashjoinable
 *	  If the restrictinfo's clause is hashjoinable, set the hashjoin
 *	  info fields in the restrictinfo.
 *
 *	  Currently, we support hashjoin for binary opclauses where
 *	  the operator is a hashjoinable operator.  The arguments can be
 *	  anything --- as long as there are no volatile functions in them.
 */
static void
check_hashjoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;
	Node	   *leftarg;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;
	leftarg = linitial(((OpExpr *) clause)->args);

	if (op_hashjoinable(opno, exprType(leftarg)) &&
		!contain_volatile_functions((Node *) restrictinfo))
		restrictinfo->hashjoinoperator = opno;
}

/*
 * check_memoizable
 *	  If the restrictinfo's clause is suitable to be used for a Memoize node,
 *	  set the left_hasheqoperator and right_hasheqoperator to the hash equality
 *	  operator that will be needed during caching.
 */
static void
check_memoizable(RestrictInfo *restrictinfo)
{
	TypeCacheEntry *typentry;
	Expr	   *clause = restrictinfo->clause;
	Oid			lefttype;
	Oid			righttype;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	lefttype = exprType(linitial(((OpExpr *) clause)->args));

	typentry = lookup_type_cache(lefttype, TYPECACHE_HASH_PROC |
								 TYPECACHE_EQ_OPR);

	if (OidIsValid(typentry->hash_proc) && OidIsValid(typentry->eq_opr))
		restrictinfo->left_hasheqoperator = typentry->eq_opr;

	righttype = exprType(lsecond(((OpExpr *) clause)->args));

	/*
	 * Lookup the right type, unless it's the same as the left type, in which
	 * case typentry is already pointing to the required TypeCacheEntry.
	 */
	if (lefttype != righttype)
		typentry = lookup_type_cache(righttype, TYPECACHE_HASH_PROC |
									 TYPECACHE_EQ_OPR);

	if (OidIsValid(typentry->hash_proc) && OidIsValid(typentry->eq_opr))
		restrictinfo->right_hasheqoperator = typentry->eq_opr;
}
