/*-------------------------------------------------------------------------
 *
 * prepjointree.c
 *	  Planner preprocessing for subqueries and join tree manipulation.
 *
 * NOTE: the intended sequence for invoking these operations is
 *		replace_empty_jointree
 *		pull_up_sublinks
 *		preprocess_function_rtes
 *		pull_up_subqueries
 *		flatten_simple_union_all
 *		do expression preprocessing (including flattening JOIN alias vars)
 *		reduce_outer_joins
 *		remove_useless_result_rtes
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepjointree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/multibitmapset.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/placeholder.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"


typedef struct pullup_replace_vars_context
{
	PlannerInfo *root;
	List	   *targetlist;		/* tlist of subquery being pulled up */
	RangeTblEntry *target_rte;	/* RTE of subquery */
	Relids		relids;			/* relids within subquery, as numbered after
								 * pullup (set only if target_rte->lateral) */
	bool	   *outer_hasSubLinks;	/* -> outer query's hasSubLinks */
	int			varno;			/* varno of subquery */
	bool		wrap_non_vars;	/* do we need all non-Var outputs to be PHVs? */
	Node	  **rv_cache;		/* cache for results with PHVs */
} pullup_replace_vars_context;

typedef struct reduce_outer_joins_pass1_state
{
	Relids		relids;			/* base relids within this subtree */
	bool		contains_outer; /* does subtree contain outer join(s)? */
	List	   *sub_states;		/* List of states for subtree components */
} reduce_outer_joins_pass1_state;

typedef struct reduce_outer_joins_pass2_state
{
	Relids		inner_reduced;	/* OJ relids reduced to plain inner joins */
	List	   *partial_reduced;	/* List of partially reduced FULL joins */
} reduce_outer_joins_pass2_state;

typedef struct reduce_outer_joins_partial_state
{
	int			full_join_rti;	/* RT index of a formerly-FULL join */
	Relids		unreduced_side; /* relids in its still-nullable side */
} reduce_outer_joins_partial_state;

static Node *pull_up_sublinks_jointree_recurse(PlannerInfo *root, Node *jtnode,
											   Relids *relids);
static Node *pull_up_sublinks_qual_recurse(PlannerInfo *root, Node *node,
										   Node **jtlink1, Relids available_rels1,
										   Node **jtlink2, Relids available_rels2);
static Node *pull_up_subqueries_recurse(PlannerInfo *root, Node *jtnode,
										JoinExpr *lowest_outer_join,
										AppendRelInfo *containing_appendrel);
static Node *pull_up_simple_subquery(PlannerInfo *root, Node *jtnode,
									 RangeTblEntry *rte,
									 JoinExpr *lowest_outer_join,
									 AppendRelInfo *containing_appendrel);
static Node *pull_up_simple_union_all(PlannerInfo *root, Node *jtnode,
									  RangeTblEntry *rte);
static void pull_up_union_leaf_queries(Node *setOp, PlannerInfo *root,
									   int parentRTindex, Query *setOpQuery,
									   int childRToffset);
static void make_setop_translation_list(Query *query, int newvarno,
										AppendRelInfo *appinfo);
static bool is_simple_subquery(PlannerInfo *root, Query *subquery,
							   RangeTblEntry *rte,
							   JoinExpr *lowest_outer_join);
static Node *pull_up_simple_values(PlannerInfo *root, Node *jtnode,
								   RangeTblEntry *rte);
static bool is_simple_values(PlannerInfo *root, RangeTblEntry *rte);
static Node *pull_up_constant_function(PlannerInfo *root, Node *jtnode,
									   RangeTblEntry *rte,
									   AppendRelInfo *containing_appendrel);
static bool is_simple_union_all(Query *subquery);
static bool is_simple_union_all_recurse(Node *setOp, Query *setOpQuery,
										List *colTypes);
static bool is_safe_append_member(Query *subquery);
static bool jointree_contains_lateral_outer_refs(PlannerInfo *root,
												 Node *jtnode, bool restricted,
												 Relids safe_upper_varnos);
static void perform_pullup_replace_vars(PlannerInfo *root,
										pullup_replace_vars_context *rvcontext,
										AppendRelInfo *containing_appendrel);
static void replace_vars_in_jointree(Node *jtnode,
									 pullup_replace_vars_context *context);
static Node *pullup_replace_vars(Node *expr,
								 pullup_replace_vars_context *context);
static Node *pullup_replace_vars_callback(Var *var,
										  replace_rte_variables_context *context);
static Query *pullup_replace_vars_subquery(Query *query,
										   pullup_replace_vars_context *context);
static reduce_outer_joins_pass1_state *reduce_outer_joins_pass1(Node *jtnode);
static void reduce_outer_joins_pass2(Node *jtnode,
									 reduce_outer_joins_pass1_state *state1,
									 reduce_outer_joins_pass2_state *state2,
									 PlannerInfo *root,
									 Relids nonnullable_rels,
									 List *forced_null_vars);
static void report_reduced_full_join(reduce_outer_joins_pass2_state *state2,
									 int rtindex, Relids relids);
static Node *remove_useless_results_recurse(PlannerInfo *root, Node *jtnode,
											Node **parent_quals,
											Relids *dropped_outer_joins);
static int	get_result_relid(PlannerInfo *root, Node *jtnode);
static void remove_result_refs(PlannerInfo *root, int varno, Node *newjtloc);
static bool find_dependent_phvs(PlannerInfo *root, int varno);
static bool find_dependent_phvs_in_jointree(PlannerInfo *root,
											Node *node, int varno);
static void substitute_phv_relids(Node *node,
								  int varno, Relids subrelids);
static void fix_append_rel_relids(PlannerInfo *root, int varno,
								  Relids subrelids);
static Node *find_jointree_node_for_rel(Node *jtnode, int relid);


/*
 * transform_MERGE_to_join
 *		Replace a MERGE's jointree to also include the target relation.
 */
void
transform_MERGE_to_join(Query *parse)
{
	RangeTblEntry *joinrte;
	JoinExpr   *joinexpr;
	bool		have_action[NUM_MERGE_MATCH_KINDS];
	JoinType	jointype;
	int			joinrti;
	List	   *vars;
	RangeTblRef *rtr;

	if (parse->commandType != CMD_MERGE)
		return;

	/* XXX probably bogus */
	vars = NIL;

	/*
	 * Work out what kind of join is required.  If there any WHEN NOT MATCHED
	 * BY SOURCE/TARGET actions, an outer join is required so that we process
	 * all unmatched tuples from the source and/or target relations.
	 * Otherwise, we can use an inner join.
	 */
	have_action[MERGE_WHEN_MATCHED] = false;
	have_action[MERGE_WHEN_NOT_MATCHED_BY_SOURCE] = false;
	have_action[MERGE_WHEN_NOT_MATCHED_BY_TARGET] = false;

	foreach_node(MergeAction, action, parse->mergeActionList)
	{
		if (action->commandType != CMD_NOTHING)
			have_action[action->matchKind] = true;
	}

	if (have_action[MERGE_WHEN_NOT_MATCHED_BY_SOURCE] &&
		have_action[MERGE_WHEN_NOT_MATCHED_BY_TARGET])
		jointype = JOIN_FULL;
	else if (have_action[MERGE_WHEN_NOT_MATCHED_BY_SOURCE])
		jointype = JOIN_LEFT;
	else if (have_action[MERGE_WHEN_NOT_MATCHED_BY_TARGET])
		jointype = JOIN_RIGHT;
	else
		jointype = JOIN_INNER;

	/* Manufacture a join RTE to use. */
	joinrte = makeNode(RangeTblEntry);
	joinrte->rtekind = RTE_JOIN;
	joinrte->jointype = jointype;
	joinrte->joinmergedcols = 0;
	joinrte->joinaliasvars = vars;
	joinrte->joinleftcols = NIL;	/* MERGE does not allow JOIN USING */
	joinrte->joinrightcols = NIL;	/* ditto */
	joinrte->join_using_alias = NULL;

	joinrte->alias = NULL;
	joinrte->eref = makeAlias("*MERGE*", NIL);
	joinrte->lateral = false;
	joinrte->inh = false;
	joinrte->inFromCl = true;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.
	 */
	parse->rtable = lappend(parse->rtable, joinrte);
	joinrti = list_length(parse->rtable);

	/*
	 * Create a JOIN between the target and the source relation.
	 *
	 * Here the target is identified by parse->mergeTargetRelation.  For a
	 * regular table, this will equal parse->resultRelation, but for a
	 * trigger-updatable view, it will be the expanded view subquery that we
	 * need to pull data from.
	 *
	 * The source relation is in parse->jointree->fromlist, but any quals in
	 * parse->jointree->quals are restrictions on the target relation (if the
	 * target relation is an auto-updatable view).
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = parse->mergeTargetRelation;
	joinexpr = makeNode(JoinExpr);
	joinexpr->jointype = jointype;
	joinexpr->isNatural = false;
	joinexpr->larg = (Node *) makeFromExpr(list_make1(rtr), parse->jointree->quals);
	joinexpr->rarg = linitial(parse->jointree->fromlist);	/* source rel */
	joinexpr->usingClause = NIL;
	joinexpr->join_using_alias = NULL;
	joinexpr->quals = parse->mergeJoinCondition;
	joinexpr->alias = NULL;
	joinexpr->rtindex = joinrti;

	/* Make the new join be the sole entry in the query's jointree */
	parse->jointree->fromlist = list_make1(joinexpr);
	parse->jointree->quals = NULL;

	/*
	 * If necessary, mark parse->targetlist entries that refer to the target
	 * as nullable by the join.  Normally the targetlist will be empty for a
	 * MERGE, but if the target is a trigger-updatable view, it will contain a
	 * whole-row Var referring to the expanded view query.
	 */
	if (parse->targetList != NIL &&
		(jointype == JOIN_RIGHT || jointype == JOIN_FULL))
		parse->targetList = (List *)
			add_nulling_relids((Node *) parse->targetList,
							   bms_make_singleton(parse->mergeTargetRelation),
							   bms_make_singleton(joinrti));

	/*
	 * If there are any WHEN NOT MATCHED BY SOURCE actions, the executor will
	 * use the join condition to distinguish between MATCHED and NOT MATCHED
	 * BY SOURCE cases.  Otherwise, it's no longer needed, and we set it to
	 * NULL, saving cycles during planning and execution.
	 */
	if (!have_action[MERGE_WHEN_NOT_MATCHED_BY_SOURCE])
		parse->mergeJoinCondition = NULL;
}

/*
 * replace_empty_jointree
 *		If the Query's jointree is empty, replace it with a dummy RTE_RESULT
 *		relation.
 *
 * By doing this, we can avoid a bunch of corner cases that formerly existed
 * for SELECTs with omitted FROM clauses.  An example is that a subquery
 * with empty jointree previously could not be pulled up, because that would
 * have resulted in an empty relid set, making the subquery not uniquely
 * identifiable for join or PlaceHolderVar processing.
 *
 * Unlike most other functions in this file, this function doesn't recurse;
 * we rely on other processing to invoke it on sub-queries at suitable times.
 */
void
replace_empty_jointree(Query *parse)
{
	RangeTblEntry *rte;
	Index		rti;
	RangeTblRef *rtr;

	/* Nothing to do if jointree is already nonempty */
	if (parse->jointree->fromlist != NIL)
		return;

	/* We mustn't change it in the top level of a setop tree, either */
	if (parse->setOperations)
		return;

	/* Create suitable RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);

	/* Add it to rangetable */
	parse->rtable = lappend(parse->rtable, rte);
	rti = list_length(parse->rtable);

	/* And jam a reference into the jointree */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rti;
	parse->jointree->fromlist = list_make1(rtr);
}

/*
 * pull_up_sublinks
 *		Attempt to pull up ANY and EXISTS SubLinks to be treated as
 *		semijoins or anti-semijoins.
 *
 * A clause "foo op ANY (sub-SELECT)" can be processed by pulling the
 * sub-SELECT up to become a rangetable entry and treating the implied
 * comparisons as quals of a semijoin.  However, this optimization *only*
 * works at the top level of WHERE or a JOIN/ON clause, because we cannot
 * distinguish whether the ANY ought to return FALSE or NULL in cases
 * involving NULL inputs.  Also, in an outer join's ON clause we can only
 * do this if the sublink is degenerate (ie, references only the nullable
 * side of the join).  In that case it is legal to push the semijoin
 * down into the nullable side of the join.  If the sublink references any
 * nonnullable-side variables then it would have to be evaluated as part
 * of the outer join, which makes things way too complicated.
 *
 * Under similar conditions, EXISTS and NOT EXISTS clauses can be handled
 * by pulling up the sub-SELECT and creating a semijoin or anti-semijoin.
 *
 * This routine searches for such clauses and does the necessary parsetree
 * transformations if any are found.
 *
 * This routine has to run before preprocess_expression(), so the quals
 * clauses are not yet reduced to implicit-AND format, and are not guaranteed
 * to be AND/OR-flat either.  That means we need to recursively search through
 * explicit AND clauses.  We stop as soon as we hit a non-AND item.
 */
void
pull_up_sublinks(PlannerInfo *root)
{
	Node	   *jtnode;
	Relids		relids;

	/* Begin recursion through the jointree */
	jtnode = pull_up_sublinks_jointree_recurse(root,
											   (Node *) root->parse->jointree,
											   &relids);

	/*
	 * root->parse->jointree must always be a FromExpr, so insert a dummy one
	 * if we got a bare RangeTblRef or JoinExpr out of the recursion.
	 */
	if (IsA(jtnode, FromExpr))
		root->parse->jointree = (FromExpr *) jtnode;
	else
		root->parse->jointree = makeFromExpr(list_make1(jtnode), NULL);
}

/*
 * Recurse through jointree nodes for pull_up_sublinks()
 *
 * In addition to returning the possibly-modified jointree node, we return
 * a relids set of the contained rels into *relids.
 */
static Node *
pull_up_sublinks_jointree_recurse(PlannerInfo *root, Node *jtnode,
								  Relids *relids)
{
	/* Since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (jtnode == NULL)
	{
		*relids = NULL;
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		*relids = bms_make_singleton(varno);
		/* jtnode is returned unmodified */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *newfromlist = NIL;
		Relids		frelids = NULL;
		FromExpr   *newf;
		Node	   *jtlink;
		ListCell   *l;

		/* First, recurse to process children and collect their relids */
		foreach(l, f->fromlist)
		{
			Node	   *newchild;
			Relids		childrelids;

			newchild = pull_up_sublinks_jointree_recurse(root,
														 lfirst(l),
														 &childrelids);
			newfromlist = lappend(newfromlist, newchild);
			frelids = bms_join(frelids, childrelids);
		}
		/* Build the replacement FromExpr; no quals yet */
		newf = makeFromExpr(newfromlist, NULL);
		/* Set up a link representing the rebuilt jointree */
		jtlink = (Node *) newf;
		/* Now process qual --- all children are available for use */
		newf->quals = pull_up_sublinks_qual_recurse(root, f->quals,
													&jtlink, frelids,
													NULL, NULL);

		/*
		 * Note that the result will be either newf, or a stack of JoinExprs
		 * with newf at the base.  We rely on subsequent optimization steps to
		 * flatten this and rearrange the joins as needed.
		 *
		 * Although we could include the pulled-up subqueries in the returned
		 * relids, there's no need since upper quals couldn't refer to their
		 * outputs anyway.
		 */
		*relids = frelids;
		jtnode = jtlink;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j;
		Relids		leftrelids;
		Relids		rightrelids;
		Node	   *jtlink;

		/*
		 * Make a modifiable copy of join node, but don't bother copying its
		 * subnodes (yet).
		 */
		j = (JoinExpr *) palloc(sizeof(JoinExpr));
		memcpy(j, jtnode, sizeof(JoinExpr));
		jtlink = (Node *) j;

		/* Recurse to process children and collect their relids */
		j->larg = pull_up_sublinks_jointree_recurse(root, j->larg,
													&leftrelids);
		j->rarg = pull_up_sublinks_jointree_recurse(root, j->rarg,
													&rightrelids);

		/*
		 * Now process qual, showing appropriate child relids as available,
		 * and attach any pulled-up jointree items at the right place. In the
		 * inner-join case we put new JoinExprs above the existing one (much
		 * as for a FromExpr-style join).  In outer-join cases the new
		 * JoinExprs must go into the nullable side of the outer join. The
		 * point of the available_rels machinations is to ensure that we only
		 * pull up quals for which that's okay.
		 *
		 * We don't expect to see any pre-existing JOIN_SEMI, JOIN_ANTI,
		 * JOIN_RIGHT_SEMI, or JOIN_RIGHT_ANTI jointypes here.
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &jtlink,
														 bms_union(leftrelids,
																   rightrelids),
														 NULL, NULL);
				break;
			case JOIN_LEFT:
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &j->rarg,
														 rightrelids,
														 NULL, NULL);
				break;
			case JOIN_FULL:
				/* can't do anything with full-join quals */
				break;
			case JOIN_RIGHT:
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &j->larg,
														 leftrelids,
														 NULL, NULL);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}

		/*
		 * Although we could include the pulled-up subqueries in the returned
		 * relids, there's no need since upper quals couldn't refer to their
		 * outputs anyway.  But we *do* need to include the join's own rtindex
		 * because we haven't yet collapsed join alias variables, so upper
		 * levels would mistakenly think they couldn't use references to this
		 * join.
		 */
		*relids = bms_join(leftrelids, rightrelids);
		if (j->rtindex)
			*relids = bms_add_member(*relids, j->rtindex);
		jtnode = jtlink;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * Recurse through top-level qual nodes for pull_up_sublinks()
 *
 * jtlink1 points to the link in the jointree where any new JoinExprs should
 * be inserted if they reference available_rels1 (i.e., available_rels1
 * denotes the relations present underneath jtlink1).  Optionally, jtlink2 can
 * point to a second link where new JoinExprs should be inserted if they
 * reference available_rels2 (pass NULL for both those arguments if not used).
 * Note that SubLinks referencing both sets of variables cannot be optimized.
 * If we find multiple pull-up-able SubLinks, they'll get stacked onto jtlink1
 * and/or jtlink2 in the order we encounter them.  We rely on subsequent
 * optimization to rearrange the stack if appropriate.
 *
 * Returns the replacement qual node, or NULL if the qual should be removed.
 */
static Node *
pull_up_sublinks_qual_recurse(PlannerInfo *root, Node *node,
							  Node **jtlink1, Relids available_rels1,
							  Node **jtlink2, Relids available_rels2)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		JoinExpr   *j;
		Relids		child_rels;

		/* Is it a convertible ANY or EXISTS clause? */
		if (sublink->subLinkType == ANY_SUBLINK)
		{
			if ((j = convert_ANY_sublink_to_join(root, sublink,
												 available_rels1)) != NULL)
			{
				/* Yes; insert the new join node into the join tree */
				j->larg = *jtlink1;
				*jtlink1 = (Node *) j;
				/* Recursively process pulled-up jointree nodes */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * Now recursively process the pulled-up quals.  Any inserted
				 * joins can get stacked onto either j->larg or j->rarg,
				 * depending on which rels they reference.
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels1,
														 &j->rarg,
														 child_rels);
				/* Return NULL representing constant TRUE */
				return NULL;
			}
			if (available_rels2 != NULL &&
				(j = convert_ANY_sublink_to_join(root, sublink,
												 available_rels2)) != NULL)
			{
				/* Yes; insert the new join node into the join tree */
				j->larg = *jtlink2;
				*jtlink2 = (Node *) j;
				/* Recursively process pulled-up jointree nodes */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * Now recursively process the pulled-up quals.  Any inserted
				 * joins can get stacked onto either j->larg or j->rarg,
				 * depending on which rels they reference.
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels2,
														 &j->rarg,
														 child_rels);
				/* Return NULL representing constant TRUE */
				return NULL;
			}
		}
		else if (sublink->subLinkType == EXISTS_SUBLINK)
		{
			if ((j = convert_EXISTS_sublink_to_join(root, sublink, false,
													available_rels1)) != NULL)
			{
				/* Yes; insert the new join node into the join tree */
				j->larg = *jtlink1;
				*jtlink1 = (Node *) j;
				/* Recursively process pulled-up jointree nodes */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * Now recursively process the pulled-up quals.  Any inserted
				 * joins can get stacked onto either j->larg or j->rarg,
				 * depending on which rels they reference.
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels1,
														 &j->rarg,
														 child_rels);
				/* Return NULL representing constant TRUE */
				return NULL;
			}
			if (available_rels2 != NULL &&
				(j = convert_EXISTS_sublink_to_join(root, sublink, false,
													available_rels2)) != NULL)
			{
				/* Yes; insert the new join node into the join tree */
				j->larg = *jtlink2;
				*jtlink2 = (Node *) j;
				/* Recursively process pulled-up jointree nodes */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * Now recursively process the pulled-up quals.  Any inserted
				 * joins can get stacked onto either j->larg or j->rarg,
				 * depending on which rels they reference.
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels2,
														 &j->rarg,
														 child_rels);
				/* Return NULL representing constant TRUE */
				return NULL;
			}
		}
		/* Else return it unmodified */
		return node;
	}
	if (is_notclause(node))
	{
		/* If the immediate argument of NOT is EXISTS, try to convert */
		SubLink    *sublink = (SubLink *) get_notclausearg((Expr *) node);
		JoinExpr   *j;
		Relids		child_rels;

		if (sublink && IsA(sublink, SubLink))
		{
			if (sublink->subLinkType == EXISTS_SUBLINK)
			{
				if ((j = convert_EXISTS_sublink_to_join(root, sublink, true,
														available_rels1)) != NULL)
				{
					/* Yes; insert the new join node into the join tree */
					j->larg = *jtlink1;
					*jtlink1 = (Node *) j;
					/* Recursively process pulled-up jointree nodes */
					j->rarg = pull_up_sublinks_jointree_recurse(root,
																j->rarg,
																&child_rels);

					/*
					 * Now recursively process the pulled-up quals.  Because
					 * we are underneath a NOT, we can't pull up sublinks that
					 * reference the left-hand stuff, but it's still okay to
					 * pull up sublinks referencing j->rarg.
					 */
					j->quals = pull_up_sublinks_qual_recurse(root,
															 j->quals,
															 &j->rarg,
															 child_rels,
															 NULL, NULL);
					/* Return NULL representing constant TRUE */
					return NULL;
				}
				if (available_rels2 != NULL &&
					(j = convert_EXISTS_sublink_to_join(root, sublink, true,
														available_rels2)) != NULL)
				{
					/* Yes; insert the new join node into the join tree */
					j->larg = *jtlink2;
					*jtlink2 = (Node *) j;
					/* Recursively process pulled-up jointree nodes */
					j->rarg = pull_up_sublinks_jointree_recurse(root,
																j->rarg,
																&child_rels);

					/*
					 * Now recursively process the pulled-up quals.  Because
					 * we are underneath a NOT, we can't pull up sublinks that
					 * reference the left-hand stuff, but it's still okay to
					 * pull up sublinks referencing j->rarg.
					 */
					j->quals = pull_up_sublinks_qual_recurse(root,
															 j->quals,
															 &j->rarg,
															 child_rels,
															 NULL, NULL);
					/* Return NULL representing constant TRUE */
					return NULL;
				}
			}
		}
		/* Else return it unmodified */
		return node;
	}
	if (is_andclause(node))
	{
		/* Recurse into AND clause */
		List	   *newclauses = NIL;
		ListCell   *l;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *oldclause = (Node *) lfirst(l);
			Node	   *newclause;

			newclause = pull_up_sublinks_qual_recurse(root,
													  oldclause,
													  jtlink1,
													  available_rels1,
													  jtlink2,
													  available_rels2);
			if (newclause)
				newclauses = lappend(newclauses, newclause);
		}
		/* We might have got back fewer clauses than we started with */
		if (newclauses == NIL)
			return NULL;
		else if (list_length(newclauses) == 1)
			return (Node *) linitial(newclauses);
		else
			return (Node *) make_andclause(newclauses);
	}
	/* Stop if not an AND */
	return node;
}

/*
 * preprocess_function_rtes
 *		Constant-simplify any FUNCTION RTEs in the FROM clause, and then
 *		attempt to "inline" any that are set-returning functions.
 *
 * If an RTE_FUNCTION rtable entry invokes a set-returning function that
 * contains just a simple SELECT, we can convert the rtable entry to an
 * RTE_SUBQUERY entry exposing the SELECT directly.  This is especially
 * useful if the subquery can then be "pulled up" for further optimization,
 * but we do it even if not, to reduce executor overhead.
 *
 * This has to be done before we have started to do any optimization of
 * subqueries, else any such steps wouldn't get applied to subqueries
 * obtained via inlining.  However, we do it after pull_up_sublinks
 * so that we can inline any functions used in SubLink subselects.
 *
 * The reason for applying const-simplification at this stage is that
 * (a) we'd need to do it anyway to inline a SRF, and (b) by doing it now,
 * we can be sure that pull_up_constant_function() will see constants
 * if there are constants to be seen.  This approach also guarantees
 * that every FUNCTION RTE has been const-simplified, allowing planner.c's
 * preprocess_expression() to skip doing it again.
 *
 * Like most of the planner, this feels free to scribble on its input data
 * structure.
 */
void
preprocess_function_rtes(PlannerInfo *root)
{
	ListCell   *rt;

	foreach(rt, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

		if (rte->rtekind == RTE_FUNCTION)
		{
			Query	   *funcquery;

			/* Apply const-simplification */
			rte->functions = (List *)
				eval_const_expressions(root, (Node *) rte->functions);

			/* Check safety of expansion, and expand if possible */
			funcquery = inline_set_returning_function(root, rte);
			if (funcquery)
			{
				/* Successful expansion, convert the RTE to a subquery */
				rte->rtekind = RTE_SUBQUERY;
				rte->subquery = funcquery;
				rte->security_barrier = false;
				/* Clear fields that should not be set in a subquery RTE */
				rte->functions = NIL;
				rte->funcordinality = false;
			}
		}
	}
}

/*
 * pull_up_subqueries
 *		Look for subqueries in the rangetable that can be pulled up into
 *		the parent query.  If the subquery has no special features like
 *		grouping/aggregation then we can merge it into the parent's jointree.
 *		Also, subqueries that are simple UNION ALL structures can be
 *		converted into "append relations".
 */
void
pull_up_subqueries(PlannerInfo *root)
{
	/* Top level of jointree must always be a FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
	/* Recursion starts with no containing join nor appendrel */
	root->parse->jointree = (FromExpr *)
		pull_up_subqueries_recurse(root, (Node *) root->parse->jointree,
								   NULL, NULL);
	/* We should still have a FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
}

/*
 * pull_up_subqueries_recurse
 *		Recursive guts of pull_up_subqueries.
 *
 * This recursively processes the jointree and returns a modified jointree.
 *
 * If this jointree node is within either side of an outer join, then
 * lowest_outer_join references the lowest such JoinExpr node; otherwise
 * it is NULL.  We use this to constrain the effects of LATERAL subqueries.
 *
 * If we are looking at a member subquery of an append relation,
 * containing_appendrel describes that relation; else it is NULL.
 * This forces use of the PlaceHolderVar mechanism for all non-Var targetlist
 * items, and puts some additional restrictions on what can be pulled up.
 *
 * A tricky aspect of this code is that if we pull up a subquery we have
 * to replace Vars that reference the subquery's outputs throughout the
 * parent query, including quals attached to jointree nodes above the one
 * we are currently processing!  We handle this by being careful to maintain
 * validity of the jointree structure while recursing, in the following sense:
 * whenever we recurse, all qual expressions in the tree must be reachable
 * from the top level, in case the recursive call needs to modify them.
 *
 * Notice also that we can't turn pullup_replace_vars loose on the whole
 * jointree, because it'd return a mutated copy of the tree; we have to
 * invoke it just on the quals, instead.  This behavior is what makes it
 * reasonable to pass lowest_outer_join as a pointer rather than some
 * more-indirect way of identifying the lowest OJ.  Likewise, we don't
 * replace append_rel_list members but only their substructure, so the
 * containing_appendrel reference is safe to use.
 */
static Node *
pull_up_subqueries_recurse(PlannerInfo *root, Node *jtnode,
						   JoinExpr *lowest_outer_join,
						   AppendRelInfo *containing_appendrel)
{
	/* Since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();
	/* Also, since it's a bit expensive, let's check for query cancel. */
	CHECK_FOR_INTERRUPTS();

	Assert(jtnode != NULL);
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, root->parse->rtable);

		/*
		 * Is this a subquery RTE, and if so, is the subquery simple enough to
		 * pull up?
		 *
		 * If we are looking at an append-relation member, we can't pull it up
		 * unless is_safe_append_member says so.
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_subquery(root, rte->subquery, rte, lowest_outer_join) &&
			(containing_appendrel == NULL ||
			 is_safe_append_member(rte->subquery)))
			return pull_up_simple_subquery(root, jtnode, rte,
										   lowest_outer_join,
										   containing_appendrel);

		/*
		 * Alternatively, is it a simple UNION ALL subquery?  If so, flatten
		 * into an "append relation".
		 *
		 * It's safe to do this regardless of whether this query is itself an
		 * appendrel member.  (If you're thinking we should try to flatten the
		 * two levels of appendrel together, you're right; but we handle that
		 * in set_append_rel_pathlist, not here.)
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_union_all(rte->subquery))
			return pull_up_simple_union_all(root, jtnode, rte);

		/*
		 * Or perhaps it's a simple VALUES RTE?
		 *
		 * We don't allow VALUES pullup below an outer join nor into an
		 * appendrel (such cases are impossible anyway at the moment).
		 */
		if (rte->rtekind == RTE_VALUES &&
			lowest_outer_join == NULL &&
			containing_appendrel == NULL &&
			is_simple_values(root, rte))
			return pull_up_simple_values(root, jtnode, rte);

		/*
		 * Or perhaps it's a FUNCTION RTE that we could inline?
		 */
		if (rte->rtekind == RTE_FUNCTION)
			return pull_up_constant_function(root, jtnode, rte,
											 containing_appendrel);

		/* Otherwise, do nothing at this node. */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		Assert(containing_appendrel == NULL);
		/* Recursively transform all the child nodes */
		foreach(l, f->fromlist)
		{
			lfirst(l) = pull_up_subqueries_recurse(root, lfirst(l),
												   lowest_outer_join,
												   NULL);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		Assert(containing_appendrel == NULL);
		/* Recurse, being careful to tell myself when inside outer join */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 lowest_outer_join,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 lowest_outer_join,
													 NULL);
				break;
			case JOIN_LEFT:
			case JOIN_SEMI:
			case JOIN_ANTI:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 NULL);
				break;
			case JOIN_FULL:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 NULL);
				break;
			case JOIN_RIGHT:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 NULL);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * pull_up_simple_subquery
 *		Attempt to pull up a single simple subquery.
 *
 * jtnode is a RangeTblRef that has been tentatively identified as a simple
 * subquery by pull_up_subqueries.  We return the replacement jointree node,
 * or jtnode itself if we determine that the subquery can't be pulled up
 * after all.
 *
 * rte is the RangeTblEntry referenced by jtnode.  Remaining parameters are
 * as for pull_up_subqueries_recurse.
 */
static Node *
pull_up_simple_subquery(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte,
						JoinExpr *lowest_outer_join,
						AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	Query	   *subquery;
	PlannerInfo *subroot;
	int			rtoffset;
	pullup_replace_vars_context rvcontext;
	ListCell   *lc;

	/*
	 * Make a modifiable copy of the subquery to hack on, so that the RTE will
	 * be left unchanged in case we decide below that we can't pull it up
	 * after all.
	 */
	subquery = copyObject(rte->subquery);

	/*
	 * Create a PlannerInfo data structure for this subquery.
	 *
	 * NOTE: the next few steps should match the first processing in
	 * subquery_planner().  Can we refactor to avoid code duplication, or
	 * would that just make things uglier?
	 */
	subroot = makeNode(PlannerInfo);
	subroot->parse = subquery;
	subroot->glob = root->glob;
	subroot->query_level = root->query_level;
	subroot->parent_root = root->parent_root;
	subroot->plan_params = NIL;
	subroot->outer_params = NULL;
	subroot->planner_cxt = CurrentMemoryContext;
	subroot->init_plans = NIL;
	subroot->cte_plan_ids = NIL;
	subroot->multiexpr_params = NIL;
	subroot->join_domains = NIL;
	subroot->eq_classes = NIL;
	subroot->ec_merging_done = false;
	subroot->last_rinfo_serial = 0;
	subroot->all_result_relids = NULL;
	subroot->leaf_result_relids = NULL;
	subroot->append_rel_list = NIL;
	subroot->row_identity_vars = NIL;
	subroot->rowMarks = NIL;
	memset(subroot->upper_rels, 0, sizeof(subroot->upper_rels));
	memset(subroot->upper_targets, 0, sizeof(subroot->upper_targets));
	subroot->processed_groupClause = NIL;
	subroot->processed_distinctClause = NIL;
	subroot->processed_tlist = NIL;
	subroot->update_colnos = NIL;
	subroot->grouping_map = NULL;
	subroot->minmax_aggs = NIL;
	subroot->qual_security_level = 0;
	subroot->placeholdersFrozen = false;
	subroot->hasRecursion = false;
	subroot->wt_param_id = -1;
	subroot->non_recursive_path = NULL;
	/* We don't currently need a top JoinDomain for the subroot */

	/* No CTEs to worry about */
	Assert(subquery->cteList == NIL);

	/*
	 * If the FROM clause is empty, replace it with a dummy RTE_RESULT RTE, so
	 * that we don't need so many special cases to deal with that situation.
	 */
	replace_empty_jointree(subquery);

	/*
	 * Pull up any SubLinks within the subquery's quals, so that we don't
	 * leave unoptimized SubLinks behind.
	 */
	if (subquery->hasSubLinks)
		pull_up_sublinks(subroot);

	/*
	 * Similarly, preprocess its function RTEs to inline any set-returning
	 * functions in its rangetable.
	 */
	preprocess_function_rtes(subroot);

	/*
	 * Recursively pull up the subquery's subqueries, so that
	 * pull_up_subqueries' processing is complete for its jointree and
	 * rangetable.
	 *
	 * Note: it's okay that the subquery's recursion starts with NULL for
	 * containing-join info, even if we are within an outer join in the upper
	 * query; the lower query starts with a clean slate for outer-join
	 * semantics.  Likewise, we needn't pass down appendrel state.
	 */
	pull_up_subqueries(subroot);

	/*
	 * Now we must recheck whether the subquery is still simple enough to pull
	 * up.  If not, abandon processing it.
	 *
	 * We don't really need to recheck all the conditions involved, but it's
	 * easier just to keep this "if" looking the same as the one in
	 * pull_up_subqueries_recurse.
	 */
	if (is_simple_subquery(root, subquery, rte, lowest_outer_join) &&
		(containing_appendrel == NULL || is_safe_append_member(subquery)))
	{
		/* good to go */
	}
	else
	{
		/*
		 * Give up, return unmodified RangeTblRef.
		 *
		 * Note: The work we just did will be redone when the subquery gets
		 * planned on its own.  Perhaps we could avoid that by storing the
		 * modified subquery back into the rangetable, but I'm not gonna risk
		 * it now.
		 */
		return jtnode;
	}

	/*
	 * We must flatten any join alias Vars in the subquery's targetlist,
	 * because pulling up the subquery's subqueries might have changed their
	 * expansions into arbitrary expressions, which could affect
	 * pullup_replace_vars' decisions about whether PlaceHolderVar wrappers
	 * are needed for tlist entries.  (Likely it'd be better to do
	 * flatten_join_alias_vars on the whole query tree at some earlier stage,
	 * maybe even in the rewriter; but for now let's just fix this case here.)
	 */
	subquery->targetList = (List *)
		flatten_join_alias_vars(subroot, subroot->parse,
								(Node *) subquery->targetList);

	/*
	 * Adjust level-0 varnos in subquery so that we can append its rangetable
	 * to upper query's.  We have to fix the subquery's append_rel_list as
	 * well.
	 */
	rtoffset = list_length(parse->rtable);
	OffsetVarNodes((Node *) subquery, rtoffset, 0);
	OffsetVarNodes((Node *) subroot->append_rel_list, rtoffset, 0);

	/*
	 * Upper-level vars in subquery are now one level closer to their parent
	 * than before.
	 */
	IncrementVarSublevelsUp((Node *) subquery, -1, 1);
	IncrementVarSublevelsUp((Node *) subroot->append_rel_list, -1, 1);

	/*
	 * The subquery's targetlist items are now in the appropriate form to
	 * insert into the top query, except that we may need to wrap them in
	 * PlaceHolderVars.  Set up required context data for pullup_replace_vars.
	 * (Note that we should include the subquery's inner joins in relids,
	 * since it may include join alias vars referencing them.)
	 */
	rvcontext.root = root;
	rvcontext.targetlist = subquery->targetList;
	rvcontext.target_rte = rte;
	if (rte->lateral)
		rvcontext.relids = get_relids_in_jointree((Node *) subquery->jointree,
												  true, true);
	else						/* won't need relids */
		rvcontext.relids = NULL;
	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = varno;
	/* this flag will be set below, if needed */
	rvcontext.wrap_non_vars = false;
	/* initialize cache array with indexes 0 .. length(tlist) */
	rvcontext.rv_cache = palloc0((list_length(subquery->targetList) + 1) *
								 sizeof(Node *));

	/*
	 * If we are dealing with an appendrel member then anything that's not a
	 * simple Var has to be turned into a PlaceHolderVar.  We force this to
	 * ensure that what we pull up doesn't get merged into a surrounding
	 * expression during later processing and then fail to match the
	 * expression actually available from the appendrel.
	 */
	if (containing_appendrel != NULL)
		rvcontext.wrap_non_vars = true;

	/*
	 * If the parent query uses grouping sets, we need a PlaceHolderVar for
	 * anything that's not a simple Var.  Again, this ensures that expressions
	 * retain their separate identity so that they will match grouping set
	 * columns when appropriate.  (It'd be sufficient to wrap values used in
	 * grouping set columns, and do so only in non-aggregated portions of the
	 * tlist and havingQual, but that would require a lot of infrastructure
	 * that pullup_replace_vars hasn't currently got.)
	 */
	if (parse->groupingSets)
		rvcontext.wrap_non_vars = true;

	/*
	 * Replace all of the top query's references to the subquery's outputs
	 * with copies of the adjusted subtlist items, being careful not to
	 * replace any of the jointree structure.
	 */
	perform_pullup_replace_vars(root, &rvcontext,
								containing_appendrel);

	/*
	 * If the subquery had a LATERAL marker, propagate that to any of its
	 * child RTEs that could possibly now contain lateral cross-references.
	 * The children might or might not contain any actual lateral
	 * cross-references, but we have to mark the pulled-up child RTEs so that
	 * later planner stages will check for such.
	 */
	if (rte->lateral)
	{
		foreach(lc, subquery->rtable)
		{
			RangeTblEntry *child_rte = (RangeTblEntry *) lfirst(lc);

			switch (child_rte->rtekind)
			{
				case RTE_RELATION:
					if (child_rte->tablesample)
						child_rte->lateral = true;
					break;
				case RTE_SUBQUERY:
				case RTE_FUNCTION:
				case RTE_VALUES:
				case RTE_TABLEFUNC:
					child_rte->lateral = true;
					break;
				case RTE_JOIN:
				case RTE_CTE:
				case RTE_NAMEDTUPLESTORE:
				case RTE_RESULT:
					/* these can't contain any lateral references */
					break;
			}
		}
	}

	/*
	 * Now append the adjusted rtable entries and their perminfos to upper
	 * query. (We hold off until after fixing the upper rtable entries; no
	 * point in running that code on the subquery ones too.)
	 */
	CombineRangeTables(&parse->rtable, &parse->rteperminfos,
					   subquery->rtable, subquery->rteperminfos);

	/*
	 * Pull up any FOR UPDATE/SHARE markers, too.  (OffsetVarNodes already
	 * adjusted the marker rtindexes, so just concat the lists.)
	 */
	parse->rowMarks = list_concat(parse->rowMarks, subquery->rowMarks);

	/*
	 * We also have to fix the relid sets of any PlaceHolderVar nodes in the
	 * parent query.  (This could perhaps be done by pullup_replace_vars(),
	 * but it seems cleaner to use two passes.)  Note in particular that any
	 * PlaceHolderVar nodes just created by pullup_replace_vars() will be
	 * adjusted, so having created them with the subquery's varno is correct.
	 *
	 * Likewise, relids appearing in AppendRelInfo nodes have to be fixed. We
	 * already checked that this won't require introducing multiple subrelids
	 * into the single-slot AppendRelInfo structs.
	 */
	if (root->glob->lastPHId != 0 || root->append_rel_list)
	{
		Relids		subrelids;

		subrelids = get_relids_in_jointree((Node *) subquery->jointree,
										   true, false);
		if (root->glob->lastPHId != 0)
			substitute_phv_relids((Node *) parse, varno, subrelids);
		fix_append_rel_relids(root, varno, subrelids);
	}

	/*
	 * And now add subquery's AppendRelInfos to our list.
	 */
	root->append_rel_list = list_concat(root->append_rel_list,
										subroot->append_rel_list);

	/*
	 * We don't have to do the equivalent bookkeeping for outer-join info,
	 * because that hasn't been set up yet.  placeholder_list likewise.
	 */
	Assert(root->join_info_list == NIL);
	Assert(subroot->join_info_list == NIL);
	Assert(root->placeholder_list == NIL);
	Assert(subroot->placeholder_list == NIL);

	/*
	 * We no longer need the RTE's copy of the subquery's query tree.  Getting
	 * rid of it saves nothing in particular so far as this level of query is
	 * concerned; but if this query level is in turn pulled up into a parent,
	 * we'd waste cycles copying the now-unused query tree.
	 */
	rte->subquery = NULL;

	/*
	 * Miscellaneous housekeeping.
	 *
	 * Although replace_rte_variables() faithfully updated parse->hasSubLinks
	 * if it copied any SubLinks out of the subquery's targetlist, we still
	 * could have SubLinks added to the query in the expressions of FUNCTION
	 * and VALUES RTEs copied up from the subquery.  So it's necessary to copy
	 * subquery->hasSubLinks anyway.  Perhaps this can be improved someday.
	 */
	parse->hasSubLinks |= subquery->hasSubLinks;

	/* If subquery had any RLS conditions, now main query does too */
	parse->hasRowSecurity |= subquery->hasRowSecurity;

	/*
	 * subquery won't be pulled up if it hasAggs, hasWindowFuncs, or
	 * hasTargetSRFs, so no work needed on those flags
	 */

	/*
	 * Return the adjusted subquery jointree to replace the RangeTblRef entry
	 * in parent's jointree; or, if the FromExpr is degenerate, just return
	 * its single member.
	 */
	Assert(IsA(subquery->jointree, FromExpr));
	Assert(subquery->jointree->fromlist != NIL);
	if (subquery->jointree->quals == NULL &&
		list_length(subquery->jointree->fromlist) == 1)
		return (Node *) linitial(subquery->jointree->fromlist);

	return (Node *) subquery->jointree;
}

/*
 * pull_up_simple_union_all
 *		Pull up a single simple UNION ALL subquery.
 *
 * jtnode is a RangeTblRef that has been identified as a simple UNION ALL
 * subquery by pull_up_subqueries.  We pull up the leaf subqueries and
 * build an "append relation" for the union set.  The result value is just
 * jtnode, since we don't actually need to change the query jointree.
 */
static Node *
pull_up_simple_union_all(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte)
{
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	Query	   *subquery = rte->subquery;
	int			rtoffset = list_length(root->parse->rtable);
	List	   *rtable;

	/*
	 * Make a modifiable copy of the subquery's rtable, so we can adjust
	 * upper-level Vars in it.  There are no such Vars in the setOperations
	 * tree proper, so fixing the rtable should be sufficient.
	 */
	rtable = copyObject(subquery->rtable);

	/*
	 * Upper-level vars in subquery are now one level closer to their parent
	 * than before.  We don't have to worry about offsetting varnos, though,
	 * because the UNION leaf queries can't cross-reference each other.
	 */
	IncrementVarSublevelsUp_rtable(rtable, -1, 1);

	/*
	 * If the UNION ALL subquery had a LATERAL marker, propagate that to all
	 * its children.  The individual children might or might not contain any
	 * actual lateral cross-references, but we have to mark the pulled-up
	 * child RTEs so that later planner stages will check for such.
	 */
	if (rte->lateral)
	{
		ListCell   *rt;

		foreach(rt, rtable)
		{
			RangeTblEntry *child_rte = (RangeTblEntry *) lfirst(rt);

			Assert(child_rte->rtekind == RTE_SUBQUERY);
			child_rte->lateral = true;
		}
	}

	/*
	 * Append child RTEs (and their perminfos) to parent rtable.
	 */
	CombineRangeTables(&root->parse->rtable, &root->parse->rteperminfos,
					   rtable, subquery->rteperminfos);

	/*
	 * Recursively scan the subquery's setOperations tree and add
	 * AppendRelInfo nodes for leaf subqueries to the parent's
	 * append_rel_list.  Also apply pull_up_subqueries to the leaf subqueries.
	 */
	Assert(subquery->setOperations);
	pull_up_union_leaf_queries(subquery->setOperations, root, varno, subquery,
							   rtoffset);

	/*
	 * Mark the parent as an append relation.
	 */
	rte->inh = true;

	return jtnode;
}

/*
 * pull_up_union_leaf_queries -- recursive guts of pull_up_simple_union_all
 *
 * Build an AppendRelInfo for each leaf query in the setop tree, and then
 * apply pull_up_subqueries to the leaf query.
 *
 * Note that setOpQuery is the Query containing the setOp node, whose tlist
 * contains references to all the setop output columns.  When called from
 * pull_up_simple_union_all, this is *not* the same as root->parse, which is
 * the parent Query we are pulling up into.
 *
 * parentRTindex is the appendrel parent's index in root->parse->rtable.
 *
 * The child RTEs have already been copied to the parent.  childRToffset
 * tells us where in the parent's range table they were copied.  When called
 * from flatten_simple_union_all, childRToffset is 0 since the child RTEs
 * were already in root->parse->rtable and no RT index adjustment is needed.
 */
static void
pull_up_union_leaf_queries(Node *setOp, PlannerInfo *root, int parentRTindex,
						   Query *setOpQuery, int childRToffset)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		int			childRTindex;
		AppendRelInfo *appinfo;

		/*
		 * Calculate the index in the parent's range table
		 */
		childRTindex = childRToffset + rtr->rtindex;

		/*
		 * Build a suitable AppendRelInfo, and attach to parent's list.
		 */
		appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = parentRTindex;
		appinfo->child_relid = childRTindex;
		appinfo->parent_reltype = InvalidOid;
		appinfo->child_reltype = InvalidOid;
		make_setop_translation_list(setOpQuery, childRTindex, appinfo);
		appinfo->parent_reloid = InvalidOid;
		root->append_rel_list = lappend(root->append_rel_list, appinfo);

		/*
		 * Recursively apply pull_up_subqueries to the new child RTE.  (We
		 * must build the AppendRelInfo first, because this will modify it;
		 * indeed, that's the only part of the upper query where Vars
		 * referencing childRTindex can exist at this point.)
		 *
		 * Note that we can pass NULL for containing-join info even if we're
		 * actually under an outer join, because the child's expressions
		 * aren't going to propagate up to the join.  Also, we ignore the
		 * possibility that pull_up_subqueries_recurse() returns a different
		 * jointree node than what we pass it; if it does, the important thing
		 * is that it replaced the child relid in the AppendRelInfo node.
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = childRTindex;
		(void) pull_up_subqueries_recurse(root, (Node *) rtr,
										  NULL, appinfo);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* Recurse to reach leaf queries */
		pull_up_union_leaf_queries(op->larg, root, parentRTindex, setOpQuery,
								   childRToffset);
		pull_up_union_leaf_queries(op->rarg, root, parentRTindex, setOpQuery,
								   childRToffset);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * make_setop_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  a UNION ALL member.  (At this point it's just a simple list of
 *	  referencing Vars, but if we succeed in pulling up the member
 *	  subquery, the Vars will get replaced by pulled-up expressions.)
 *	  Also create the rather trivial reverse-translation array.
 */
static void
make_setop_translation_list(Query *query, int newvarno,
							AppendRelInfo *appinfo)
{
	List	   *vars = NIL;
	AttrNumber *pcolnos;
	ListCell   *l;

	/* Initialize reverse-translation array with all entries zero */
	/* (entries for resjunk columns will stay that way) */
	appinfo->num_child_cols = list_length(query->targetList);
	appinfo->parent_colnos = pcolnos =
		(AttrNumber *) palloc0(appinfo->num_child_cols * sizeof(AttrNumber));

	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;

		vars = lappend(vars, makeVarFromTargetEntry(newvarno, tle));
		pcolnos[tle->resno - 1] = tle->resno;
	}

	appinfo->translated_vars = vars;
}

/*
 * is_simple_subquery
 *	  Check a subquery in the range table to see if it's simple enough
 *	  to pull up into the parent query.
 *
 * rte is the RTE_SUBQUERY RangeTblEntry that contained the subquery.
 * (Note subquery is not necessarily equal to rte->subquery; it could be a
 * processed copy of that.)
 * lowest_outer_join is the lowest outer join above the subquery, or NULL.
 */
static bool
is_simple_subquery(PlannerInfo *root, Query *subquery, RangeTblEntry *rte,
				   JoinExpr *lowest_outer_join)
{
	/*
	 * Let's just make sure it's a valid subselect ...
	 */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT)
		elog(ERROR, "subquery is bogus");

	/*
	 * Can't currently pull up a query with setops (unless it's simple UNION
	 * ALL, which is handled by a different code path). Maybe after querytree
	 * redesign...
	 */
	if (subquery->setOperations)
		return false;

	/*
	 * Can't pull up a subquery involving grouping, aggregation, SRFs,
	 * sorting, limiting, or WITH.  (XXX WITH could possibly be allowed later)
	 *
	 * We also don't pull up a subquery that has explicit FOR UPDATE/SHARE
	 * clauses, because pullup would cause the locking to occur semantically
	 * higher than it should.  Implicit FOR UPDATE/SHARE is okay because in
	 * that case the locking was originally declared in the upper query
	 * anyway.
	 */
	if (subquery->hasAggs ||
		subquery->hasWindowFuncs ||
		subquery->hasTargetSRFs ||
		subquery->groupClause ||
		subquery->groupingSets ||
		subquery->havingQual ||
		subquery->sortClause ||
		subquery->distinctClause ||
		subquery->limitOffset ||
		subquery->limitCount ||
		subquery->hasForUpdate ||
		subquery->cteList)
		return false;

	/*
	 * Don't pull up if the RTE represents a security-barrier view; we
	 * couldn't prevent information leakage once the RTE's Vars are scattered
	 * about in the upper query.
	 */
	if (rte->security_barrier)
		return false;

	/*
	 * If the subquery is LATERAL, check for pullup restrictions from that.
	 */
	if (rte->lateral)
	{
		bool		restricted;
		Relids		safe_upper_varnos;

		/*
		 * The subquery's WHERE and JOIN/ON quals mustn't contain any lateral
		 * references to rels outside a higher outer join (including the case
		 * where the outer join is within the subquery itself).  In such a
		 * case, pulling up would result in a situation where we need to
		 * postpone quals from below an outer join to above it, which is
		 * probably completely wrong and in any case is a complication that
		 * doesn't seem worth addressing at the moment.
		 */
		if (lowest_outer_join != NULL)
		{
			restricted = true;
			safe_upper_varnos = get_relids_in_jointree((Node *) lowest_outer_join,
													   true, true);
		}
		else
		{
			restricted = false;
			safe_upper_varnos = NULL;	/* doesn't matter */
		}

		if (jointree_contains_lateral_outer_refs(root,
												 (Node *) subquery->jointree,
												 restricted, safe_upper_varnos))
			return false;

		/*
		 * If there's an outer join above the LATERAL subquery, also disallow
		 * pullup if the subquery's targetlist has any references to rels
		 * outside the outer join, since these might get pulled into quals
		 * above the subquery (but in or below the outer join) and then lead
		 * to qual-postponement issues similar to the case checked for above.
		 * (We wouldn't need to prevent pullup if no such references appear in
		 * outer-query quals, but we don't have enough info here to check
		 * that.  Also, maybe this restriction could be removed if we forced
		 * such refs to be wrapped in PlaceHolderVars, even when they're below
		 * the nearest outer join?	But it's a pretty hokey usage, so not
		 * clear this is worth sweating over.)
		 */
		if (lowest_outer_join != NULL)
		{
			Relids		lvarnos = pull_varnos_of_level(root,
													   (Node *) subquery->targetList,
													   1);

			if (!bms_is_subset(lvarnos, safe_upper_varnos))
				return false;
		}
	}

	/*
	 * Don't pull up a subquery that has any volatile functions in its
	 * targetlist.  Otherwise we might introduce multiple evaluations of these
	 * functions, if they get copied to multiple places in the upper query,
	 * leading to surprising results.  (Note: the PlaceHolderVar mechanism
	 * doesn't quite guarantee single evaluation; else we could pull up anyway
	 * and just wrap such items in PlaceHolderVars ...)
	 */
	if (contain_volatile_functions((Node *) subquery->targetList))
		return false;

	return true;
}

/*
 * pull_up_simple_values
 *		Pull up a single simple VALUES RTE.
 *
 * jtnode is a RangeTblRef that has been identified as a simple VALUES RTE
 * by pull_up_subqueries.  We always return a RangeTblRef representing a
 * RESULT RTE to replace it (all failure cases should have been detected by
 * is_simple_values()).  Actually, what we return is just jtnode, because
 * we replace the VALUES RTE in the rangetable with the RESULT RTE.
 *
 * rte is the RangeTblEntry referenced by jtnode.  Because of the limited
 * possible usage of VALUES RTEs, we do not need the remaining parameters
 * of pull_up_subqueries_recurse.
 */
static Node *
pull_up_simple_values(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	List	   *values_list;
	List	   *tlist;
	AttrNumber	attrno;
	pullup_replace_vars_context rvcontext;
	ListCell   *lc;

	Assert(rte->rtekind == RTE_VALUES);
	Assert(list_length(rte->values_lists) == 1);

	/*
	 * Need a modifiable copy of the VALUES list to hack on, just in case it's
	 * multiply referenced.
	 */
	values_list = copyObject(linitial(rte->values_lists));

	/*
	 * The VALUES RTE can't contain any Vars of level zero, let alone any that
	 * are join aliases, so no need to flatten join alias Vars.
	 */
	Assert(!contain_vars_of_level((Node *) values_list, 0));

	/*
	 * Set up required context data for pullup_replace_vars.  In particular,
	 * we have to make the VALUES list look like a subquery targetlist.
	 */
	tlist = NIL;
	attrno = 1;
	foreach(lc, values_list)
	{
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) lfirst(lc),
										attrno,
										NULL,
										false));
		attrno++;
	}
	rvcontext.root = root;
	rvcontext.targetlist = tlist;
	rvcontext.target_rte = rte;
	rvcontext.relids = NULL;
	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = varno;
	rvcontext.wrap_non_vars = false;
	/* initialize cache array with indexes 0 .. length(tlist) */
	rvcontext.rv_cache = palloc0((list_length(tlist) + 1) *
								 sizeof(Node *));

	/*
	 * Replace all of the top query's references to the RTE's outputs with
	 * copies of the adjusted VALUES expressions, being careful not to replace
	 * any of the jointree structure.  We can assume there's no outer joins or
	 * appendrels in the dummy Query that surrounds a VALUES RTE.
	 */
	perform_pullup_replace_vars(root, &rvcontext, NULL);

	/*
	 * There should be no appendrels to fix, nor any outer joins and hence no
	 * PlaceHolderVars.
	 */
	Assert(root->append_rel_list == NIL);
	Assert(root->join_info_list == NIL);
	Assert(root->placeholder_list == NIL);

	/*
	 * Replace the VALUES RTE with a RESULT RTE.  The VALUES RTE is the only
	 * rtable entry in the current query level, so this is easy.
	 */
	Assert(list_length(parse->rtable) == 1);

	/* Create suitable RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);

	/* Replace rangetable */
	parse->rtable = list_make1(rte);

	/* We could manufacture a new RangeTblRef, but the one we have is fine */
	Assert(varno == 1);

	return jtnode;
}

/*
 * is_simple_values
 *	  Check a VALUES RTE in the range table to see if it's simple enough
 *	  to pull up into the parent query.
 *
 * rte is the RTE_VALUES RangeTblEntry to check.
 */
static bool
is_simple_values(PlannerInfo *root, RangeTblEntry *rte)
{
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * There must be exactly one VALUES list, else it's not semantically
	 * correct to replace the VALUES RTE with a RESULT RTE, nor would we have
	 * a unique set of expressions to substitute into the parent query.
	 */
	if (list_length(rte->values_lists) != 1)
		return false;

	/*
	 * Because VALUES can't appear under an outer join (or at least, we won't
	 * try to pull it up if it does), we need not worry about LATERAL, nor
	 * about validity of PHVs for the VALUES' outputs.
	 */

	/*
	 * Don't pull up a VALUES that contains any set-returning or volatile
	 * functions.  The considerations here are basically identical to the
	 * restrictions on a pull-able subquery's targetlist.
	 */
	if (expression_returns_set((Node *) rte->values_lists) ||
		contain_volatile_functions((Node *) rte->values_lists))
		return false;

	/*
	 * Do not pull up a VALUES that's not the only RTE in its parent query.
	 * This is actually the only case that the parser will generate at the
	 * moment, and assuming this is true greatly simplifies
	 * pull_up_simple_values().
	 */
	if (list_length(root->parse->rtable) != 1 ||
		rte != (RangeTblEntry *) linitial(root->parse->rtable))
		return false;

	return true;
}

/*
 * pull_up_constant_function
 *		Pull up an RTE_FUNCTION expression that was simplified to a constant.
 *
 * jtnode is a RangeTblRef that has been identified as a FUNCTION RTE by
 * pull_up_subqueries.  If its expression is just a Const, hoist that value
 * up into the parent query, and replace the RTE_FUNCTION with RTE_RESULT.
 *
 * In principle we could pull up any immutable expression, but we don't.
 * That might result in multiple evaluations of the expression, which could
 * be costly if it's not just a Const.  Also, the main value of this is
 * to let the constant participate in further const-folding, and of course
 * that won't happen for a non-Const.
 *
 * The pulled-up value might need to be wrapped in a PlaceHolderVar if the
 * RTE is below an outer join or is part of an appendrel; the extra
 * parameters show whether that's needed.
 */
static Node *
pull_up_constant_function(PlannerInfo *root, Node *jtnode,
						  RangeTblEntry *rte,
						  AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	RangeTblFunction *rtf;
	TypeFuncClass functypclass;
	Oid			funcrettype;
	TupleDesc	tupdesc;
	pullup_replace_vars_context rvcontext;

	/* Fail if the RTE has ORDINALITY - we don't implement that here. */
	if (rte->funcordinality)
		return jtnode;

	/* Fail if RTE isn't a single, simple Const expr */
	if (list_length(rte->functions) != 1)
		return jtnode;
	rtf = linitial_node(RangeTblFunction, rte->functions);
	if (!IsA(rtf->funcexpr, Const))
		return jtnode;

	/*
	 * If the function's result is not a scalar, we punt.  In principle we
	 * could break the composite constant value apart into per-column
	 * constants, but for now it seems not worth the work.
	 */
	if (rtf->funccolcount != 1)
		return jtnode;			/* definitely composite */

	/* If it has a coldeflist, it certainly returns RECORD */
	if (rtf->funccolnames != NIL)
		return jtnode;			/* must be a one-column RECORD type */

	functypclass = get_expr_result_type(rtf->funcexpr,
										&funcrettype,
										&tupdesc);
	if (functypclass != TYPEFUNC_SCALAR)
		return jtnode;			/* must be a one-column composite type */

	/* Create context for applying pullup_replace_vars */
	rvcontext.root = root;
	rvcontext.targetlist = list_make1(makeTargetEntry((Expr *) rtf->funcexpr,
													  1,	/* resno */
													  NULL, /* resname */
													  false));	/* resjunk */
	rvcontext.target_rte = rte;

	/*
	 * Since this function was reduced to a Const, it doesn't contain any
	 * lateral references, even if it's marked as LATERAL.  This means we
	 * don't need to fill relids.
	 */
	rvcontext.relids = NULL;

	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = ((RangeTblRef *) jtnode)->rtindex;
	/* this flag will be set below, if needed */
	rvcontext.wrap_non_vars = false;
	/* initialize cache array with indexes 0 .. length(tlist) */
	rvcontext.rv_cache = palloc0((list_length(rvcontext.targetlist) + 1) *
								 sizeof(Node *));

	/*
	 * If we are dealing with an appendrel member then anything that's not a
	 * simple Var has to be turned into a PlaceHolderVar.  (See comments in
	 * pull_up_simple_subquery().)
	 */
	if (containing_appendrel != NULL)
		rvcontext.wrap_non_vars = true;

	/*
	 * If the parent query uses grouping sets, we need a PlaceHolderVar for
	 * anything that's not a simple Var.
	 */
	if (parse->groupingSets)
		rvcontext.wrap_non_vars = true;

	/*
	 * Replace all of the top query's references to the RTE's output with
	 * copies of the funcexpr, being careful not to replace any of the
	 * jointree structure.
	 */
	perform_pullup_replace_vars(root, &rvcontext,
								containing_appendrel);

	/*
	 * We don't need to bother with changing PlaceHolderVars in the parent
	 * query.  Their references to the RT index are still good for now, and
	 * will get removed later if we're able to drop the RTE_RESULT.
	 */

	/*
	 * Convert the RTE to be RTE_RESULT type, signifying that we don't need to
	 * scan it anymore, and zero out RTE_FUNCTION-specific fields.  Also make
	 * sure the RTE is not marked LATERAL, since elsewhere we don't expect
	 * RTE_RESULTs to be LATERAL.
	 */
	rte->rtekind = RTE_RESULT;
	rte->functions = NIL;
	rte->lateral = false;

	/*
	 * We can reuse the RangeTblRef node.
	 */
	return jtnode;
}

/*
 * is_simple_union_all
 *	  Check a subquery to see if it's a simple UNION ALL.
 *
 * We require all the setops to be UNION ALL (no mixing) and there can't be
 * any datatype coercions involved, ie, all the leaf queries must emit the
 * same datatypes.
 */
static bool
is_simple_union_all(Query *subquery)
{
	SetOperationStmt *topop;

	/* Let's just make sure it's a valid subselect ... */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT)
		elog(ERROR, "subquery is bogus");

	/* Is it a set-operation query at all? */
	topop = castNode(SetOperationStmt, subquery->setOperations);
	if (!topop)
		return false;

	/* Can't handle ORDER BY, LIMIT/OFFSET, locking, or WITH */
	if (subquery->sortClause ||
		subquery->limitOffset ||
		subquery->limitCount ||
		subquery->rowMarks ||
		subquery->cteList)
		return false;

	/* Recursively check the tree of set operations */
	return is_simple_union_all_recurse((Node *) topop, subquery,
									   topop->colTypes);
}

static bool
is_simple_union_all_recurse(Node *setOp, Query *setOpQuery, List *colTypes)
{
	/* Since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, setOpQuery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);

		/* Leaf nodes are OK if they match the toplevel column types */
		/* We don't have to compare typmods or collations here */
		return tlist_same_datatypes(subquery->targetList, colTypes, true);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* Must be UNION ALL */
		if (op->op != SETOP_UNION || !op->all)
			return false;

		/* Recurse to check inputs */
		return is_simple_union_all_recurse(op->larg, setOpQuery, colTypes) &&
			is_simple_union_all_recurse(op->rarg, setOpQuery, colTypes);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		return false;			/* keep compiler quiet */
	}
}

/*
 * is_safe_append_member
 *	  Check a subquery that is a leaf of a UNION ALL appendrel to see if it's
 *	  safe to pull up.
 */
static bool
is_safe_append_member(Query *subquery)
{
	FromExpr   *jtnode;

	/*
	 * It's only safe to pull up the child if its jointree contains exactly
	 * one RTE, else the AppendRelInfo data structure breaks. The one base RTE
	 * could be buried in several levels of FromExpr, however.  Also, if the
	 * child's jointree is completely empty, we can pull up because
	 * pull_up_simple_subquery will insert a single RTE_RESULT RTE instead.
	 *
	 * Also, the child can't have any WHERE quals because there's no place to
	 * put them in an appendrel.  (This is a bit annoying...) If we didn't
	 * need to check this, we'd just test whether get_relids_in_jointree()
	 * yields a singleton set, to be more consistent with the coding of
	 * fix_append_rel_relids().
	 */
	jtnode = subquery->jointree;
	Assert(IsA(jtnode, FromExpr));
	/* Check the completely-empty case */
	if (jtnode->fromlist == NIL && jtnode->quals == NULL)
		return true;
	/* Check the more general case */
	while (IsA(jtnode, FromExpr))
	{
		if (jtnode->quals != NULL)
			return false;
		if (list_length(jtnode->fromlist) != 1)
			return false;
		jtnode = linitial(jtnode->fromlist);
	}
	if (!IsA(jtnode, RangeTblRef))
		return false;

	return true;
}

/*
 * jointree_contains_lateral_outer_refs
 *		Check for disallowed lateral references in a jointree's quals
 *
 * If restricted is false, all level-1 Vars are allowed (but we still must
 * search the jointree, since it might contain outer joins below which there
 * will be restrictions).  If restricted is true, return true when any qual
 * in the jointree contains level-1 Vars coming from outside the rels listed
 * in safe_upper_varnos.
 */
static bool
jointree_contains_lateral_outer_refs(PlannerInfo *root, Node *jtnode,
									 bool restricted,
									 Relids safe_upper_varnos)
{
	if (jtnode == NULL)
		return false;
	if (IsA(jtnode, RangeTblRef))
		return false;
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		/* First, recurse to check child joins */
		foreach(l, f->fromlist)
		{
			if (jointree_contains_lateral_outer_refs(root,
													 lfirst(l),
													 restricted,
													 safe_upper_varnos))
				return true;
		}

		/* Then check the top-level quals */
		if (restricted &&
			!bms_is_subset(pull_varnos_of_level(root, f->quals, 1),
						   safe_upper_varnos))
			return true;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/*
		 * If this is an outer join, we mustn't allow any upper lateral
		 * references in or below it.
		 */
		if (j->jointype != JOIN_INNER)
		{
			restricted = true;
			safe_upper_varnos = NULL;
		}

		/* Check the child joins */
		if (jointree_contains_lateral_outer_refs(root,
												 j->larg,
												 restricted,
												 safe_upper_varnos))
			return true;
		if (jointree_contains_lateral_outer_refs(root,
												 j->rarg,
												 restricted,
												 safe_upper_varnos))
			return true;

		/* Check the JOIN's qual clauses */
		if (restricted &&
			!bms_is_subset(pull_varnos_of_level(root, j->quals, 1),
						   safe_upper_varnos))
			return true;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return false;
}

/*
 * Perform pullup_replace_vars everyplace it's needed in the query tree.
 *
 * Caller has already filled *rvcontext with data describing what to
 * substitute for Vars referencing the target subquery.  In addition
 * we need the identity of the containing appendrel if any.
 */
static void
perform_pullup_replace_vars(PlannerInfo *root,
							pullup_replace_vars_context *rvcontext,
							AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	ListCell   *lc;

	/*
	 * If we are considering an appendrel child subquery (that is, a UNION ALL
	 * member query that we're pulling up), then the only part of the upper
	 * query that could reference the child yet is the translated_vars list of
	 * the associated AppendRelInfo.  Furthermore, we do not want to force use
	 * of PHVs in the AppendRelInfo --- there isn't any outer join between.
	 */
	if (containing_appendrel)
	{
		bool		save_wrap_non_vars = rvcontext->wrap_non_vars;

		rvcontext->wrap_non_vars = false;
		containing_appendrel->translated_vars = (List *)
			pullup_replace_vars((Node *) containing_appendrel->translated_vars,
								rvcontext);
		rvcontext->wrap_non_vars = save_wrap_non_vars;
		return;
	}

	/*
	 * Replace all of the top query's references to the subquery's outputs
	 * with copies of the adjusted subtlist items, being careful not to
	 * replace any of the jointree structure.  (This'd be a lot cleaner if we
	 * could use query_tree_mutator.)  We have to use PHVs in the targetList,
	 * returningList, and havingQual, since those are certainly above any
	 * outer join.  replace_vars_in_jointree tracks its location in the
	 * jointree and uses PHVs or not appropriately.
	 */
	parse->targetList = (List *)
		pullup_replace_vars((Node *) parse->targetList, rvcontext);
	parse->returningList = (List *)
		pullup_replace_vars((Node *) parse->returningList, rvcontext);

	if (parse->onConflict)
	{
		parse->onConflict->onConflictSet = (List *)
			pullup_replace_vars((Node *) parse->onConflict->onConflictSet,
								rvcontext);
		parse->onConflict->onConflictWhere =
			pullup_replace_vars(parse->onConflict->onConflictWhere,
								rvcontext);

		/*
		 * We assume ON CONFLICT's arbiterElems, arbiterWhere, exclRelTlist
		 * can't contain any references to a subquery.
		 */
	}
	if (parse->mergeActionList)
	{
		foreach(lc, parse->mergeActionList)
		{
			MergeAction *action = lfirst(lc);

			action->qual = pullup_replace_vars(action->qual, rvcontext);
			action->targetList = (List *)
				pullup_replace_vars((Node *) action->targetList, rvcontext);
		}
	}
	parse->mergeJoinCondition = pullup_replace_vars(parse->mergeJoinCondition,
													rvcontext);
	replace_vars_in_jointree((Node *) parse->jointree, rvcontext);
	Assert(parse->setOperations == NULL);
	parse->havingQual = pullup_replace_vars(parse->havingQual, rvcontext);

	/*
	 * Replace references in the translated_vars lists of appendrels.
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);

		appinfo->translated_vars = (List *)
			pullup_replace_vars((Node *) appinfo->translated_vars, rvcontext);
	}

	/*
	 * Replace references in the joinaliasvars lists of join RTEs.
	 */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *otherrte = (RangeTblEntry *) lfirst(lc);

		if (otherrte->rtekind == RTE_JOIN)
			otherrte->joinaliasvars = (List *)
				pullup_replace_vars((Node *) otherrte->joinaliasvars,
									rvcontext);
	}
}

/*
 * Helper routine for perform_pullup_replace_vars: do pullup_replace_vars on
 * every expression in the jointree, without changing the jointree structure
 * itself.  Ugly, but there's no other way...
 */
static void
replace_vars_in_jointree(Node *jtnode,
						 pullup_replace_vars_context *context)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/*
		 * If the RangeTblRef refers to a LATERAL subquery (that isn't the
		 * same subquery we're pulling up), it might contain references to the
		 * target subquery, which we must replace.  We drive this from the
		 * jointree scan, rather than a scan of the rtable, so that we can
		 * avoid processing no-longer-referenced RTEs.
		 */
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (varno != context->varno)	/* ignore target subquery itself */
		{
			RangeTblEntry *rte = rt_fetch(varno, context->root->parse->rtable);

			Assert(rte != context->target_rte);
			if (rte->lateral)
			{
				switch (rte->rtekind)
				{
					case RTE_RELATION:
						/* shouldn't be marked LATERAL unless tablesample */
						Assert(rte->tablesample);
						rte->tablesample = (TableSampleClause *)
							pullup_replace_vars((Node *) rte->tablesample,
												context);
						break;
					case RTE_SUBQUERY:
						rte->subquery =
							pullup_replace_vars_subquery(rte->subquery,
														 context);
						break;
					case RTE_FUNCTION:
						rte->functions = (List *)
							pullup_replace_vars((Node *) rte->functions,
												context);
						break;
					case RTE_TABLEFUNC:
						rte->tablefunc = (TableFunc *)
							pullup_replace_vars((Node *) rte->tablefunc,
												context);
						break;
					case RTE_VALUES:
						rte->values_lists = (List *)
							pullup_replace_vars((Node *) rte->values_lists,
												context);
						break;
					case RTE_JOIN:
					case RTE_CTE:
					case RTE_NAMEDTUPLESTORE:
					case RTE_RESULT:
						/* these shouldn't be marked LATERAL */
						Assert(false);
						break;
				}
			}
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			replace_vars_in_jointree(lfirst(l), context);
		f->quals = pullup_replace_vars(f->quals, context);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		bool		save_wrap_non_vars = context->wrap_non_vars;

		replace_vars_in_jointree(j->larg, context);
		replace_vars_in_jointree(j->rarg, context);

		/*
		 * Use PHVs within the join quals of a full join.  Otherwise, we
		 * cannot identify which side of the join a pulled-up var-free
		 * expression came from, which can lead to failure to make a plan at
		 * all because none of the quals appear to be mergeable or hashable
		 * conditions.
		 */
		if (j->jointype == JOIN_FULL)
			context->wrap_non_vars = true;

		j->quals = pullup_replace_vars(j->quals, context);

		context->wrap_non_vars = save_wrap_non_vars;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * Apply pullup variable replacement throughout an expression tree
 *
 * Returns a modified copy of the tree, so this can't be used where we
 * need to do in-place replacement.
 */
static Node *
pullup_replace_vars(Node *expr, pullup_replace_vars_context *context)
{
	return replace_rte_variables(expr,
								 context->varno, 0,
								 pullup_replace_vars_callback,
								 (void *) context,
								 context->outer_hasSubLinks);
}

static Node *
pullup_replace_vars_callback(Var *var,
							 replace_rte_variables_context *context)
{
	pullup_replace_vars_context *rcon = (pullup_replace_vars_context *) context->callback_arg;
	int			varattno = var->varattno;
	bool		need_phv;
	Node	   *newnode;

	/*
	 * We need a PlaceHolderVar if the Var-to-be-replaced has nonempty
	 * varnullingrels (unless we find below that the replacement expression is
	 * a Var or PlaceHolderVar that we can just add the nullingrels to).  We
	 * also need one if the caller has instructed us that all non-Var/PHV
	 * replacements need to be wrapped for identification purposes.
	 */
	need_phv = (var->varnullingrels != NULL) || rcon->wrap_non_vars;

	/*
	 * If PlaceHolderVars are needed, we cache the modified expressions in
	 * rcon->rv_cache[].  This is not in hopes of any material speed gain
	 * within this function, but to avoid generating identical PHVs with
	 * different IDs.  That would result in duplicate evaluations at runtime,
	 * and possibly prevent optimizations that rely on recognizing different
	 * references to the same subquery output as being equal().  So it's worth
	 * a bit of extra effort to avoid it.
	 *
	 * The cached items have phlevelsup = 0 and phnullingrels = NULL; we'll
	 * copy them and adjust those values for this reference site below.
	 */
	if (need_phv &&
		varattno >= InvalidAttrNumber &&
		varattno <= list_length(rcon->targetlist) &&
		rcon->rv_cache[varattno] != NULL)
	{
		/* Just copy the entry and fall through to adjust phlevelsup etc */
		newnode = copyObject(rcon->rv_cache[varattno]);
	}
	else if (varattno == InvalidAttrNumber)
	{
		/* Must expand whole-tuple reference into RowExpr */
		RowExpr    *rowexpr;
		List	   *colnames;
		List	   *fields;
		bool		save_wrap_non_vars = rcon->wrap_non_vars;
		int			save_sublevelsup = context->sublevels_up;

		/*
		 * If generating an expansion for a var of a named rowtype (ie, this
		 * is a plain relation RTE), then we must include dummy items for
		 * dropped columns.  If the var is RECORD (ie, this is a JOIN), then
		 * omit dropped columns.  In the latter case, attach column names to
		 * the RowExpr for use of the executor and ruleutils.c.
		 *
		 * In order to be able to cache the results, we always generate the
		 * expansion with varlevelsup = 0, and then adjust below if needed.
		 */
		expandRTE(rcon->target_rte,
				  var->varno, 0 /* not varlevelsup */ , var->location,
				  (var->vartype != RECORDOID),
				  &colnames, &fields);
		/* Expand the generated per-field Vars, but don't insert PHVs there */
		rcon->wrap_non_vars = false;
		context->sublevels_up = 0;	/* to match the expandRTE output */
		fields = (List *) replace_rte_variables_mutator((Node *) fields,
														context);
		rcon->wrap_non_vars = save_wrap_non_vars;
		context->sublevels_up = save_sublevelsup;

		rowexpr = makeNode(RowExpr);
		rowexpr->args = fields;
		rowexpr->row_typeid = var->vartype;
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		rowexpr->colnames = (var->vartype == RECORDOID) ? colnames : NIL;
		rowexpr->location = var->location;
		newnode = (Node *) rowexpr;

		/*
		 * Insert PlaceHolderVar if needed.  Notice that we are wrapping one
		 * PlaceHolderVar around the whole RowExpr, rather than putting one
		 * around each element of the row.  This is because we need the
		 * expression to yield NULL, not ROW(NULL,NULL,...) when it is forced
		 * to null by an outer join.
		 */
		if (need_phv)
		{
			newnode = (Node *)
				make_placeholder_expr(rcon->root,
									  (Expr *) newnode,
									  bms_make_singleton(rcon->varno));
			/* cache it with the PHV, and with phlevelsup etc not set yet */
			rcon->rv_cache[InvalidAttrNumber] = copyObject(newnode);
		}
	}
	else
	{
		/* Normal case referencing one targetlist element */
		TargetEntry *tle = get_tle_by_resno(rcon->targetlist, varattno);

		if (tle == NULL)		/* shouldn't happen */
			elog(ERROR, "could not find attribute %d in subquery targetlist",
				 varattno);

		/* Make a copy of the tlist item to return */
		newnode = (Node *) copyObject(tle->expr);

		/* Insert PlaceHolderVar if needed */
		if (need_phv)
		{
			bool		wrap;

			if (newnode && IsA(newnode, Var) &&
				((Var *) newnode)->varlevelsup == 0)
			{
				/*
				 * Simple Vars always escape being wrapped, unless they are
				 * lateral references to something outside the subquery being
				 * pulled up.  (Even then, we could omit the PlaceHolderVar if
				 * the referenced rel is under the same lowest outer join, but
				 * it doesn't seem worth the trouble to check that.)
				 */
				if (rcon->target_rte->lateral &&
					!bms_is_member(((Var *) newnode)->varno, rcon->relids))
					wrap = true;
				else
					wrap = false;
			}
			else if (newnode && IsA(newnode, PlaceHolderVar) &&
					 ((PlaceHolderVar *) newnode)->phlevelsup == 0)
			{
				/* The same rules apply for a PlaceHolderVar */
				if (rcon->target_rte->lateral &&
					!bms_is_subset(((PlaceHolderVar *) newnode)->phrels,
								   rcon->relids))
					wrap = true;
				else
					wrap = false;
			}
			else if (rcon->wrap_non_vars)
			{
				/* Caller told us to wrap all non-Vars in a PlaceHolderVar */
				wrap = true;
			}
			else
			{
				/*
				 * If the node contains Var(s) or PlaceHolderVar(s) of the
				 * subquery being pulled up, and does not contain any
				 * non-strict constructs, then instead of adding a PHV on top
				 * we can add the required nullingrels to those Vars/PHVs.
				 * (This is fundamentally a generalization of the above cases
				 * for bare Vars and PHVs.)
				 *
				 * This test is somewhat expensive, but it avoids pessimizing
				 * the plan in cases where the nullingrels get removed again
				 * later by outer join reduction.
				 *
				 * This analysis could be tighter: in particular, a non-strict
				 * construct hidden within a lower-level PlaceHolderVar is not
				 * reason to add another PHV.  But for now it doesn't seem
				 * worth the code to be more exact.
				 *
				 * For a LATERAL subquery, we have to check the actual var
				 * membership of the node, but if it's non-lateral then any
				 * level-zero var must belong to the subquery.
				 */
				if ((rcon->target_rte->lateral ?
					 bms_overlap(pull_varnos(rcon->root, newnode),
								 rcon->relids) :
					 contain_vars_of_level(newnode, 0)) &&
					!contain_nonstrict_functions(newnode))
				{
					/* No wrap needed */
					wrap = false;
				}
				else
				{
					/* Else wrap it in a PlaceHolderVar */
					wrap = true;
				}
			}

			if (wrap)
			{
				newnode = (Node *)
					make_placeholder_expr(rcon->root,
										  (Expr *) newnode,
										  bms_make_singleton(rcon->varno));

				/*
				 * Cache it if possible (ie, if the attno is in range, which
				 * it probably always should be).
				 */
				if (varattno > InvalidAttrNumber &&
					varattno <= list_length(rcon->targetlist))
					rcon->rv_cache[varattno] = copyObject(newnode);
			}
		}
	}

	/* Propagate any varnullingrels into the replacement expression */
	if (var->varnullingrels != NULL)
	{
		if (IsA(newnode, Var))
		{
			Var		   *newvar = (Var *) newnode;

			Assert(newvar->varlevelsup == 0);
			newvar->varnullingrels = bms_add_members(newvar->varnullingrels,
													 var->varnullingrels);
		}
		else if (IsA(newnode, PlaceHolderVar))
		{
			PlaceHolderVar *newphv = (PlaceHolderVar *) newnode;

			Assert(newphv->phlevelsup == 0);
			newphv->phnullingrels = bms_add_members(newphv->phnullingrels,
													var->varnullingrels);
		}
		else
		{
			/* There should be lower-level Vars/PHVs we can modify */
			newnode = add_nulling_relids(newnode,
										 NULL,	/* modify all Vars/PHVs */
										 var->varnullingrels);
			/* Assert we did put the varnullingrels into the expression */
			Assert(bms_is_subset(var->varnullingrels,
								 pull_varnos(rcon->root, newnode)));
		}
	}

	/* Must adjust varlevelsup if replaced Var is within a subquery */
	if (var->varlevelsup > 0)
		IncrementVarSublevelsUp(newnode, var->varlevelsup, 0);

	return newnode;
}

/*
 * Apply pullup variable replacement to a subquery
 *
 * This needs to be different from pullup_replace_vars() because
 * replace_rte_variables will think that it shouldn't increment sublevels_up
 * before entering the Query; so we need to call it with sublevels_up == 1.
 */
static Query *
pullup_replace_vars_subquery(Query *query,
							 pullup_replace_vars_context *context)
{
	Assert(IsA(query, Query));
	return (Query *) replace_rte_variables((Node *) query,
										   context->varno, 1,
										   pullup_replace_vars_callback,
										   (void *) context,
										   NULL);
}


/*
 * flatten_simple_union_all
 *		Try to optimize top-level UNION ALL structure into an appendrel
 *
 * If a query's setOperations tree consists entirely of simple UNION ALL
 * operations, flatten it into an append relation, which we can process more
 * intelligently than the general setops case.  Otherwise, do nothing.
 *
 * In most cases, this can succeed only for a top-level query, because for a
 * subquery in FROM, the parent query's invocation of pull_up_subqueries would
 * already have flattened the UNION via pull_up_simple_union_all.  But there
 * are a few cases we can support here but not in that code path, for example
 * when the subquery also contains ORDER BY.
 */
void
flatten_simple_union_all(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop;
	Node	   *leftmostjtnode;
	int			leftmostRTI;
	RangeTblEntry *leftmostRTE;
	int			childRTI;
	RangeTblEntry *childRTE;
	RangeTblRef *rtr;

	/* Shouldn't be called unless query has setops */
	topop = castNode(SetOperationStmt, parse->setOperations);
	Assert(topop);

	/* Can't optimize away a recursive UNION */
	if (root->hasRecursion)
		return;

	/*
	 * Recursively check the tree of set operations.  If not all UNION ALL
	 * with identical column types, punt.
	 */
	if (!is_simple_union_all_recurse((Node *) topop, parse, topop->colTypes))
		return;

	/*
	 * Locate the leftmost leaf query in the setops tree.  The upper query's
	 * Vars all refer to this RTE (see transformSetOperationStmt).
	 */
	leftmostjtnode = topop->larg;
	while (leftmostjtnode && IsA(leftmostjtnode, SetOperationStmt))
		leftmostjtnode = ((SetOperationStmt *) leftmostjtnode)->larg;
	Assert(leftmostjtnode && IsA(leftmostjtnode, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) leftmostjtnode)->rtindex;
	leftmostRTE = rt_fetch(leftmostRTI, parse->rtable);
	Assert(leftmostRTE->rtekind == RTE_SUBQUERY);

	/*
	 * Make a copy of the leftmost RTE and add it to the rtable.  This copy
	 * will represent the leftmost leaf query in its capacity as a member of
	 * the appendrel.  The original will represent the appendrel as a whole.
	 * (We must do things this way because the upper query's Vars have to be
	 * seen as referring to the whole appendrel.)
	 */
	childRTE = copyObject(leftmostRTE);
	parse->rtable = lappend(parse->rtable, childRTE);
	childRTI = list_length(parse->rtable);

	/* Modify the setops tree to reference the child copy */
	((RangeTblRef *) leftmostjtnode)->rtindex = childRTI;

	/* Modify the formerly-leftmost RTE to mark it as an appendrel parent */
	leftmostRTE->inh = true;

	/*
	 * Form a RangeTblRef for the appendrel, and insert it into FROM.  The top
	 * Query of a setops tree should have had an empty FromClause initially.
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = leftmostRTI;
	Assert(parse->jointree->fromlist == NIL);
	parse->jointree->fromlist = list_make1(rtr);

	/*
	 * Now pretend the query has no setops.  We must do this before trying to
	 * do subquery pullup, because of Assert in pull_up_simple_subquery.
	 */
	parse->setOperations = NULL;

	/*
	 * Build AppendRelInfo information, and apply pull_up_subqueries to the
	 * leaf queries of the UNION ALL.  (We must do that now because they
	 * weren't previously referenced by the jointree, and so were missed by
	 * the main invocation of pull_up_subqueries.)
	 */
	pull_up_union_leaf_queries((Node *) topop, root, leftmostRTI, parse, 0);
}


/*
 * reduce_outer_joins
 *		Attempt to reduce outer joins to plain inner joins.
 *
 * The idea here is that given a query like
 *		SELECT ... FROM a LEFT JOIN b ON (...) WHERE b.y = 42;
 * we can reduce the LEFT JOIN to a plain JOIN if the "=" operator in WHERE
 * is strict.  The strict operator will always return NULL, causing the outer
 * WHERE to fail, on any row where the LEFT JOIN filled in NULLs for b's
 * columns.  Therefore, there's no need for the join to produce null-extended
 * rows in the first place --- which makes it a plain join not an outer join.
 * (This scenario may not be very likely in a query written out by hand, but
 * it's reasonably likely when pushing quals down into complex views.)
 *
 * More generally, an outer join can be reduced in strength if there is a
 * strict qual above it in the qual tree that constrains a Var from the
 * nullable side of the join to be non-null.  (For FULL joins this applies
 * to each side separately.)
 *
 * Another transformation we apply here is to recognize cases like
 *		SELECT ... FROM a LEFT JOIN b ON (a.x = b.y) WHERE b.y IS NULL;
 * If the join clause is strict for b.y, then only null-extended rows could
 * pass the upper WHERE, and we can conclude that what the query is really
 * specifying is an anti-semijoin.  We change the join type from JOIN_LEFT
 * to JOIN_ANTI.  The IS NULL clause then becomes redundant, and must be
 * removed to prevent bogus selectivity calculations, but we leave it to
 * distribute_qual_to_rels to get rid of such clauses.
 *
 * Also, we get rid of JOIN_RIGHT cases by flipping them around to become
 * JOIN_LEFT.  This saves some code here and in some later planner routines;
 * the main benefit is to reduce the number of jointypes that can appear in
 * SpecialJoinInfo nodes.  Note that we can still generate Paths and Plans
 * that use JOIN_RIGHT (or JOIN_RIGHT_ANTI) by switching the inputs again.
 *
 * To ease recognition of strict qual clauses, we require this routine to be
 * run after expression preprocessing (i.e., qual canonicalization and JOIN
 * alias-var expansion).
 */
void
reduce_outer_joins(PlannerInfo *root)
{
	reduce_outer_joins_pass1_state *state1;
	reduce_outer_joins_pass2_state state2;
	ListCell   *lc;

	/*
	 * To avoid doing strictness checks on more quals than necessary, we want
	 * to stop descending the jointree as soon as there are no outer joins
	 * below our current point.  This consideration forces a two-pass process.
	 * The first pass gathers information about which base rels appear below
	 * each side of each join clause, and about whether there are outer
	 * join(s) below each side of each join clause. The second pass examines
	 * qual clauses and changes join types as it descends the tree.
	 */
	state1 = reduce_outer_joins_pass1((Node *) root->parse->jointree);

	/* planner.c shouldn't have called me if no outer joins */
	if (state1 == NULL || !state1->contains_outer)
		elog(ERROR, "so where are the outer joins?");

	state2.inner_reduced = NULL;
	state2.partial_reduced = NIL;

	reduce_outer_joins_pass2((Node *) root->parse->jointree,
							 state1, &state2,
							 root, NULL, NIL);

	/*
	 * If we successfully reduced the strength of any outer joins, we must
	 * remove references to those joins as nulling rels.  This is handled as
	 * an additional pass, for simplicity and because we can handle all
	 * fully-reduced joins in a single pass over the parse tree.
	 */
	if (!bms_is_empty(state2.inner_reduced))
	{
		root->parse = (Query *)
			remove_nulling_relids((Node *) root->parse,
								  state2.inner_reduced,
								  NULL);
		/* There could be references in the append_rel_list, too */
		root->append_rel_list = (List *)
			remove_nulling_relids((Node *) root->append_rel_list,
								  state2.inner_reduced,
								  NULL);
	}

	/*
	 * Partially-reduced full joins have to be done one at a time, since
	 * they'll each need a different setting of except_relids.
	 */
	foreach(lc, state2.partial_reduced)
	{
		reduce_outer_joins_partial_state *statep = lfirst(lc);
		Relids		full_join_relids = bms_make_singleton(statep->full_join_rti);

		root->parse = (Query *)
			remove_nulling_relids((Node *) root->parse,
								  full_join_relids,
								  statep->unreduced_side);
		root->append_rel_list = (List *)
			remove_nulling_relids((Node *) root->append_rel_list,
								  full_join_relids,
								  statep->unreduced_side);
	}
}

/*
 * reduce_outer_joins_pass1 - phase 1 data collection
 *
 * Returns a state node describing the given jointree node.
 */
static reduce_outer_joins_pass1_state *
reduce_outer_joins_pass1(Node *jtnode)
{
	reduce_outer_joins_pass1_state *result;

	result = (reduce_outer_joins_pass1_state *)
		palloc(sizeof(reduce_outer_joins_pass1_state));
	result->relids = NULL;
	result->contains_outer = false;
	result->sub_states = NIL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result->relids = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			reduce_outer_joins_pass1_state *sub_state;

			sub_state = reduce_outer_joins_pass1(lfirst(l));
			result->relids = bms_add_members(result->relids,
											 sub_state->relids);
			result->contains_outer |= sub_state->contains_outer;
			result->sub_states = lappend(result->sub_states, sub_state);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		reduce_outer_joins_pass1_state *sub_state;

		/* join's own RT index is not wanted in result->relids */
		if (IS_OUTER_JOIN(j->jointype))
			result->contains_outer = true;

		sub_state = reduce_outer_joins_pass1(j->larg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);

		sub_state = reduce_outer_joins_pass1(j->rarg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * reduce_outer_joins_pass2 - phase 2 processing
 *
 *	jtnode: current jointree node
 *	state1: state data collected by phase 1 for this node
 *	state2: where to accumulate info about successfully-reduced joins
 *	root: toplevel planner state
 *	nonnullable_rels: set of base relids forced non-null by upper quals
 *	forced_null_vars: multibitmapset of Vars forced null by upper quals
 *
 * Returns info in state2 about outer joins that were successfully simplified.
 * Joins that were fully reduced to inner joins are all added to
 * state2->inner_reduced.  If a full join is reduced to a left join,
 * it needs its own entry in state2->partial_reduced, since that will
 * require custom processing to remove only the correct nullingrel markers.
 */
static void
reduce_outer_joins_pass2(Node *jtnode,
						 reduce_outer_joins_pass1_state *state1,
						 reduce_outer_joins_pass2_state *state2,
						 PlannerInfo *root,
						 Relids nonnullable_rels,
						 List *forced_null_vars)
{
	/*
	 * pass 2 should never descend as far as an empty subnode or base rel,
	 * because it's only called on subtrees marked as contains_outer.
	 */
	if (jtnode == NULL)
		elog(ERROR, "reached empty jointree");
	if (IsA(jtnode, RangeTblRef))
		elog(ERROR, "reached base rel");
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;
		ListCell   *s;
		Relids		pass_nonnullable_rels;
		List	   *pass_forced_null_vars;

		/* Scan quals to see if we can add any constraints */
		pass_nonnullable_rels = find_nonnullable_rels(f->quals);
		pass_nonnullable_rels = bms_add_members(pass_nonnullable_rels,
												nonnullable_rels);
		pass_forced_null_vars = find_forced_null_vars(f->quals);
		pass_forced_null_vars = mbms_add_members(pass_forced_null_vars,
												 forced_null_vars);
		/* And recurse --- but only into interesting subtrees */
		Assert(list_length(f->fromlist) == list_length(state1->sub_states));
		forboth(l, f->fromlist, s, state1->sub_states)
		{
			reduce_outer_joins_pass1_state *sub_state = lfirst(s);

			if (sub_state->contains_outer)
				reduce_outer_joins_pass2(lfirst(l), sub_state,
										 state2, root,
										 pass_nonnullable_rels,
										 pass_forced_null_vars);
		}
		bms_free(pass_nonnullable_rels);
		/* can't so easily clean up var lists, unfortunately */
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			rtindex = j->rtindex;
		JoinType	jointype = j->jointype;
		reduce_outer_joins_pass1_state *left_state = linitial(state1->sub_states);
		reduce_outer_joins_pass1_state *right_state = lsecond(state1->sub_states);

		/* Can we simplify this join? */
		switch (jointype)
		{
			case JOIN_INNER:
				break;
			case JOIN_LEFT:
				if (bms_overlap(nonnullable_rels, right_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_RIGHT:
				if (bms_overlap(nonnullable_rels, left_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_FULL:
				if (bms_overlap(nonnullable_rels, left_state->relids))
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
						jointype = JOIN_INNER;
					else
					{
						jointype = JOIN_LEFT;
						/* Also report partial reduction in state2 */
						report_reduced_full_join(state2, rtindex,
												 right_state->relids);
					}
				}
				else
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
					{
						jointype = JOIN_RIGHT;
						/* Also report partial reduction in state2 */
						report_reduced_full_join(state2, rtindex,
												 left_state->relids);
					}
				}
				break;
			case JOIN_SEMI:
			case JOIN_ANTI:

				/*
				 * These could only have been introduced by pull_up_sublinks,
				 * so there's no way that upper quals could refer to their
				 * righthand sides, and no point in checking.  We don't expect
				 * to see JOIN_RIGHT_SEMI or JOIN_RIGHT_ANTI yet.
				 */
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) jointype);
				break;
		}

		/*
		 * Convert JOIN_RIGHT to JOIN_LEFT.  Note that in the case where we
		 * reduced JOIN_FULL to JOIN_RIGHT, this will mean the JoinExpr no
		 * longer matches the internal ordering of any CoalesceExpr's built to
		 * represent merged join variables.  We don't care about that at
		 * present, but be wary of it ...
		 */
		if (jointype == JOIN_RIGHT)
		{
			Node	   *tmparg;

			tmparg = j->larg;
			j->larg = j->rarg;
			j->rarg = tmparg;
			jointype = JOIN_LEFT;
			right_state = linitial(state1->sub_states);
			left_state = lsecond(state1->sub_states);
		}

		/*
		 * See if we can reduce JOIN_LEFT to JOIN_ANTI.  This is the case if
		 * the join's own quals are strict for any var that was forced null by
		 * higher qual levels.  NOTE: there are other ways that we could
		 * detect an anti-join, in particular if we were to check whether Vars
		 * coming from the RHS must be non-null because of table constraints.
		 * That seems complicated and expensive though (in particular, one
		 * would have to be wary of lower outer joins). For the moment this
		 * seems sufficient.
		 */
		if (jointype == JOIN_LEFT)
		{
			List	   *nonnullable_vars;
			Bitmapset  *overlap;

			/* Find Vars in j->quals that must be non-null in joined rows */
			nonnullable_vars = find_nonnullable_vars(j->quals);

			/*
			 * It's not sufficient to check whether nonnullable_vars and
			 * forced_null_vars overlap: we need to know if the overlap
			 * includes any RHS variables.
			 */
			overlap = mbms_overlap_sets(nonnullable_vars, forced_null_vars);
			if (bms_overlap(overlap, right_state->relids))
				jointype = JOIN_ANTI;
		}

		/*
		 * Apply the jointype change, if any, to both jointree node and RTE.
		 * Also, if we changed an RTE to INNER, add its RTI to inner_reduced.
		 */
		if (rtindex && jointype != j->jointype)
		{
			RangeTblEntry *rte = rt_fetch(rtindex, root->parse->rtable);

			Assert(rte->rtekind == RTE_JOIN);
			Assert(rte->jointype == j->jointype);
			rte->jointype = jointype;
			if (jointype == JOIN_INNER)
				state2->inner_reduced = bms_add_member(state2->inner_reduced,
													   rtindex);
		}
		j->jointype = jointype;

		/* Only recurse if there's more to do below here */
		if (left_state->contains_outer || right_state->contains_outer)
		{
			Relids		local_nonnullable_rels;
			List	   *local_forced_null_vars;
			Relids		pass_nonnullable_rels;
			List	   *pass_forced_null_vars;

			/*
			 * If this join is (now) inner, we can add any constraints its
			 * quals provide to those we got from above.  But if it is outer,
			 * we can pass down the local constraints only into the nullable
			 * side, because an outer join never eliminates any rows from its
			 * non-nullable side.  Also, there is no point in passing upper
			 * constraints into the nullable side, since if there were any
			 * we'd have been able to reduce the join.  (In the case of upper
			 * forced-null constraints, we *must not* pass them into the
			 * nullable side --- they either applied here, or not.) The upshot
			 * is that we pass either the local or the upper constraints,
			 * never both, to the children of an outer join.
			 *
			 * Note that a SEMI join works like an inner join here: it's okay
			 * to pass down both local and upper constraints.  (There can't be
			 * any upper constraints affecting its inner side, but it's not
			 * worth having a separate code path to avoid passing them.)
			 *
			 * At a FULL join we just punt and pass nothing down --- is it
			 * possible to be smarter?
			 */
			if (jointype != JOIN_FULL)
			{
				local_nonnullable_rels = find_nonnullable_rels(j->quals);
				local_forced_null_vars = find_forced_null_vars(j->quals);
				if (jointype == JOIN_INNER || jointype == JOIN_SEMI)
				{
					/* OK to merge upper and local constraints */
					local_nonnullable_rels = bms_add_members(local_nonnullable_rels,
															 nonnullable_rels);
					local_forced_null_vars = mbms_add_members(local_forced_null_vars,
															  forced_null_vars);
				}
			}
			else
			{
				/* no use in calculating these */
				local_nonnullable_rels = NULL;
				local_forced_null_vars = NIL;
			}

			if (left_state->contains_outer)
			{
				if (jointype == JOIN_INNER || jointype == JOIN_SEMI)
				{
					/* pass union of local and upper constraints */
					pass_nonnullable_rels = local_nonnullable_rels;
					pass_forced_null_vars = local_forced_null_vars;
				}
				else if (jointype != JOIN_FULL) /* ie, LEFT or ANTI */
				{
					/* can't pass local constraints to non-nullable side */
					pass_nonnullable_rels = nonnullable_rels;
					pass_forced_null_vars = forced_null_vars;
				}
				else
				{
					/* no constraints pass through JOIN_FULL */
					pass_nonnullable_rels = NULL;
					pass_forced_null_vars = NIL;
				}
				reduce_outer_joins_pass2(j->larg, left_state,
										 state2, root,
										 pass_nonnullable_rels,
										 pass_forced_null_vars);
			}

			if (right_state->contains_outer)
			{
				if (jointype != JOIN_FULL)	/* ie, INNER/LEFT/SEMI/ANTI */
				{
					/* pass appropriate constraints, per comment above */
					pass_nonnullable_rels = local_nonnullable_rels;
					pass_forced_null_vars = local_forced_null_vars;
				}
				else
				{
					/* no constraints pass through JOIN_FULL */
					pass_nonnullable_rels = NULL;
					pass_forced_null_vars = NIL;
				}
				reduce_outer_joins_pass2(j->rarg, right_state,
										 state2, root,
										 pass_nonnullable_rels,
										 pass_forced_null_vars);
			}
			bms_free(local_nonnullable_rels);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/* Helper for reduce_outer_joins_pass2 */
static void
report_reduced_full_join(reduce_outer_joins_pass2_state *state2,
						 int rtindex, Relids relids)
{
	reduce_outer_joins_partial_state *statep;

	statep = palloc(sizeof(reduce_outer_joins_partial_state));
	statep->full_join_rti = rtindex;
	statep->unreduced_side = relids;
	state2->partial_reduced = lappend(state2->partial_reduced, statep);
}


/*
 * remove_useless_result_rtes
 *		Attempt to remove RTE_RESULT RTEs from the join tree.
 *		Also, elide single-child FromExprs where possible.
 *
 * We can remove RTE_RESULT entries from the join tree using the knowledge
 * that RTE_RESULT returns exactly one row and has no output columns.  Hence,
 * if one is inner-joined to anything else, we can delete it.  Optimizations
 * are also possible for some outer-join cases, as detailed below.
 *
 * This pass also replaces single-child FromExprs with their child node
 * where possible.  It's appropriate to do that here and not earlier because
 * RTE_RESULT removal might reduce a multiple-child FromExpr to have only one
 * child.  We can remove such a FromExpr if its quals are empty, or if it's
 * semantically valid to merge the quals into those of the parent node.
 * While removing unnecessary join tree nodes has some micro-efficiency value,
 * the real reason to do this is to eliminate cases where the nullable side of
 * an outer join node is a FromExpr whose single child is another outer join.
 * To correctly determine whether the two outer joins can commute,
 * deconstruct_jointree() must treat any quals of such a FromExpr as being
 * degenerate quals of the upper outer join.  The best way to do that is to
 * make them actually *be* quals of the upper join, by dropping the FromExpr
 * and hoisting the quals up into the upper join's quals.  (Note that there is
 * no hazard when the intermediate FromExpr has multiple children, since then
 * it represents an inner join that cannot commute with the upper outer join.)
 * As long as we have to do that, we might as well elide such FromExprs
 * everywhere.
 *
 * Some of these optimizations depend on recognizing empty (constant-true)
 * quals for FromExprs and JoinExprs.  That makes it useful to apply this
 * optimization pass after expression preprocessing, since that will have
 * eliminated constant-true quals, allowing more cases to be recognized as
 * optimizable.  What's more, the usual reason for an RTE_RESULT to be present
 * is that we pulled up a subquery or VALUES clause, thus very possibly
 * replacing Vars with constants, making it more likely that a qual can be
 * reduced to constant true.  Also, because some optimizations depend on
 * the outer-join type, it's best to have done reduce_outer_joins() first.
 *
 * A PlaceHolderVar referencing an RTE_RESULT RTE poses an obstacle to this
 * process: we must remove the RTE_RESULT's relid from the PHV's phrels, but
 * we must not reduce the phrels set to empty.  If that would happen, and
 * the RTE_RESULT is an immediate child of an outer join, we have to give up
 * and not remove the RTE_RESULT: there is noplace else to evaluate the
 * PlaceHolderVar.  (That is, in such cases the RTE_RESULT *does* have output
 * columns.)  But if the RTE_RESULT is an immediate child of an inner join,
 * we can usually change the PlaceHolderVar's phrels so as to evaluate it at
 * the inner join instead.  This is OK because we really only care that PHVs
 * are evaluated above or below the correct outer joins.  We can't, however,
 * postpone the evaluation of a PHV to above where it is used; so there are
 * some checks below on whether output PHVs are laterally referenced in the
 * other join input rel(s).
 *
 * We used to try to do this work as part of pull_up_subqueries() where the
 * potentially-optimizable cases get introduced; but it's way simpler, and
 * more effective, to do it separately.
 */
void
remove_useless_result_rtes(PlannerInfo *root)
{
	Relids		dropped_outer_joins = NULL;
	ListCell   *cell;

	/* Top level of jointree must always be a FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
	/* Recurse ... */
	root->parse->jointree = (FromExpr *)
		remove_useless_results_recurse(root,
									   (Node *) root->parse->jointree,
									   NULL,
									   &dropped_outer_joins);
	/* We should still have a FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));

	/*
	 * If we removed any outer-join nodes from the jointree, run around and
	 * remove references to those joins as nulling rels.  (There could be such
	 * references in PHVs that we pulled up out of the original subquery that
	 * the RESULT rel replaced.  This is kosher on the grounds that we now
	 * know that such an outer join wouldn't really have nulled anything.)  We
	 * don't do this during the main recursion, for simplicity and because we
	 * can handle all such joins in a single pass over the parse tree.
	 */
	if (!bms_is_empty(dropped_outer_joins))
	{
		root->parse = (Query *)
			remove_nulling_relids((Node *) root->parse,
								  dropped_outer_joins,
								  NULL);
		/* There could be references in the append_rel_list, too */
		root->append_rel_list = (List *)
			remove_nulling_relids((Node *) root->append_rel_list,
								  dropped_outer_joins,
								  NULL);
	}

	/*
	 * Remove any PlanRowMark referencing an RTE_RESULT RTE.  We obviously
	 * must do that for any RTE_RESULT that we just removed.  But one for a
	 * RTE that we did not remove can be dropped anyway: since the RTE has
	 * only one possible output row, there is no need for EPQ to mark and
	 * restore that row.
	 *
	 * It's necessary, not optional, to remove the PlanRowMark for a surviving
	 * RTE_RESULT RTE; otherwise we'll generate a whole-row Var for the
	 * RTE_RESULT, which the executor has no support for.
	 */
	foreach(cell, root->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(cell);

		if (rt_fetch(rc->rti, root->parse->rtable)->rtekind == RTE_RESULT)
			root->rowMarks = foreach_delete_current(root->rowMarks, cell);
	}
}

/*
 * remove_useless_results_recurse
 *		Recursive guts of remove_useless_result_rtes.
 *
 * This recursively processes the jointree and returns a modified jointree.
 * In addition, the RT indexes of any removed outer-join nodes are added to
 * *dropped_outer_joins.
 *
 * jtnode is the current jointree node.  If it could be valid to merge
 * its quals into those of the parent node, parent_quals should point to
 * the parent's quals list; otherwise, pass NULL for parent_quals.
 * (Note that in some cases, parent_quals points to the quals of a parent
 * more than one level up in the tree.)
 */
static Node *
remove_useless_results_recurse(PlannerInfo *root, Node *jtnode,
							   Node **parent_quals,
							   Relids *dropped_outer_joins)
{
	Assert(jtnode != NULL);
	if (IsA(jtnode, RangeTblRef))
	{
		/* Can't immediately do anything with a RangeTblRef */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		Relids		result_relids = NULL;
		ListCell   *cell;

		/*
		 * We can drop RTE_RESULT rels from the fromlist so long as at least
		 * one child remains, since joining to a one-row table changes
		 * nothing.  (But we can't drop a RTE_RESULT that computes PHV(s) that
		 * are needed by some sibling.  The cleanup transformation below would
		 * reassign the PHVs to be computed at the join, which is too late for
		 * the sibling's use.)  The easiest way to mechanize this rule is to
		 * modify the list in-place.
		 */
		foreach(cell, f->fromlist)
		{
			Node	   *child = (Node *) lfirst(cell);
			int			varno;

			/* Recursively transform child, allowing it to push up quals ... */
			child = remove_useless_results_recurse(root, child,
												   &f->quals,
												   dropped_outer_joins);
			/* ... and stick it back into the tree */
			lfirst(cell) = child;

			/*
			 * If it's an RTE_RESULT with at least one sibling, and no sibling
			 * references dependent PHVs, we can drop it.  We don't yet know
			 * what the inner join's final relid set will be, so postpone
			 * cleanup of PHVs etc till after this loop.
			 */
			if (list_length(f->fromlist) > 1 &&
				(varno = get_result_relid(root, child)) != 0 &&
				!find_dependent_phvs_in_jointree(root, (Node *) f, varno))
			{
				f->fromlist = foreach_delete_current(f->fromlist, cell);
				result_relids = bms_add_member(result_relids, varno);
			}
		}

		/*
		 * Clean up if we dropped any RTE_RESULT RTEs.  This is a bit
		 * inefficient if there's more than one, but it seems better to
		 * optimize the support code for the single-relid case.
		 */
		if (result_relids)
		{
			int			varno = -1;

			while ((varno = bms_next_member(result_relids, varno)) >= 0)
				remove_result_refs(root, varno, (Node *) f);
		}

		/*
		 * If the FromExpr now has only one child, see if we can elide it.
		 * This is always valid if there are no quals, except at the top of
		 * the jointree (since Query.jointree is required to point to a
		 * FromExpr).  Otherwise, we can do it if we can push the quals up to
		 * the parent node.
		 *
		 * Note: while it would not be terribly hard to generalize this
		 * transformation to merge multi-child FromExprs into their parent
		 * FromExpr, that risks making the parent join too expensive to plan.
		 * We leave it to later processing to decide heuristically whether
		 * that's a good idea.  Pulling up a single child is always OK,
		 * however.
		 */
		if (list_length(f->fromlist) == 1 &&
			f != root->parse->jointree &&
			(f->quals == NULL || parent_quals != NULL))
		{
			/*
			 * Merge any quals up to parent.  They should be in implicit-AND
			 * format by now, so we just need to concatenate lists.  Put the
			 * child quals at the front, on the grounds that they should
			 * nominally be evaluated earlier.
			 */
			if (f->quals != NULL)
				*parent_quals = (Node *)
					list_concat(castNode(List, f->quals),
								castNode(List, *parent_quals));
			return (Node *) linitial(f->fromlist);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			varno;

		/*
		 * First, recurse.  We can absorb pushed-up FromExpr quals from either
		 * child into this node if the jointype is INNER, since then this is
		 * equivalent to a FromExpr.  When the jointype is LEFT, we can absorb
		 * quals from the RHS child into the current node, as they're
		 * essentially degenerate quals of the outer join.  Moreover, if we've
		 * been passed down a parent_quals pointer then we can allow quals of
		 * the LHS child to be absorbed into the parent.  (This is important
		 * to ensure we remove single-child FromExprs immediately below
		 * commutable left joins.)  For other jointypes, we can't move child
		 * quals up, or at least there's no particular reason to.
		 */
		j->larg = remove_useless_results_recurse(root, j->larg,
												 (j->jointype == JOIN_INNER) ?
												 &j->quals :
												 (j->jointype == JOIN_LEFT) ?
												 parent_quals : NULL,
												 dropped_outer_joins);
		j->rarg = remove_useless_results_recurse(root, j->rarg,
												 (j->jointype == JOIN_INNER ||
												  j->jointype == JOIN_LEFT) ?
												 &j->quals : NULL,
												 dropped_outer_joins);

		/* Apply join-type-specific optimization rules */
		switch (j->jointype)
		{
			case JOIN_INNER:

				/*
				 * An inner join is equivalent to a FromExpr, so if either
				 * side was simplified to an RTE_RESULT rel, we can replace
				 * the join with a FromExpr with just the other side.
				 * Furthermore, we can elide that FromExpr according to the
				 * same rules as above.
				 *
				 * Just as in the FromExpr case, we can't simplify if the
				 * other input rel references any PHVs that are marked as to
				 * be evaluated at the RTE_RESULT rel, because we can't
				 * postpone their evaluation in that case.  But we only have
				 * to check this in cases where it's syntactically legal for
				 * the other input to have a LATERAL reference to the
				 * RTE_RESULT rel.  Only RHSes of inner and left joins are
				 * allowed to have such refs.
				 */
				if ((varno = get_result_relid(root, j->larg)) != 0 &&
					!find_dependent_phvs_in_jointree(root, j->rarg, varno))
				{
					remove_result_refs(root, varno, j->rarg);
					if (j->quals != NULL && parent_quals == NULL)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->rarg), j->quals);
					else
					{
						/* Merge any quals up to parent */
						if (j->quals != NULL)
							*parent_quals = (Node *)
								list_concat(castNode(List, j->quals),
											castNode(List, *parent_quals));
						jtnode = j->rarg;
					}
				}
				else if ((varno = get_result_relid(root, j->rarg)) != 0)
				{
					remove_result_refs(root, varno, j->larg);
					if (j->quals != NULL && parent_quals == NULL)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->larg), j->quals);
					else
					{
						/* Merge any quals up to parent */
						if (j->quals != NULL)
							*parent_quals = (Node *)
								list_concat(castNode(List, j->quals),
											castNode(List, *parent_quals));
						jtnode = j->larg;
					}
				}
				break;
			case JOIN_LEFT:

				/*
				 * We can simplify this case if the RHS is an RTE_RESULT, with
				 * two different possibilities:
				 *
				 * If the qual is empty (JOIN ON TRUE), then the join can be
				 * strength-reduced to a plain inner join, since each LHS row
				 * necessarily has exactly one join partner.  So we can always
				 * discard the RHS, much as in the JOIN_INNER case above.
				 * (Again, the LHS could not contain a lateral reference to
				 * the RHS.)
				 *
				 * Otherwise, it's still true that each LHS row should be
				 * returned exactly once, and since the RHS returns no columns
				 * (unless there are PHVs that have to be evaluated there), we
				 * don't much care if it's null-extended or not.  So in this
				 * case also, we can just ignore the qual and discard the left
				 * join.
				 */
				if ((varno = get_result_relid(root, j->rarg)) != 0 &&
					(j->quals == NULL ||
					 !find_dependent_phvs(root, varno)))
				{
					remove_result_refs(root, varno, j->larg);
					*dropped_outer_joins = bms_add_member(*dropped_outer_joins,
														  j->rtindex);
					jtnode = j->larg;
				}
				break;
			case JOIN_SEMI:

				/*
				 * We may simplify this case if the RHS is an RTE_RESULT; the
				 * join qual becomes effectively just a filter qual for the
				 * LHS, since we should either return the LHS row or not.  The
				 * filter clause must go into a new FromExpr if we can't push
				 * it up to the parent.
				 *
				 * There is a fine point about PHVs that are supposed to be
				 * evaluated at the RHS.  Such PHVs could only appear in the
				 * semijoin's qual, since the rest of the query cannot
				 * reference any outputs of the semijoin's RHS.  Therefore,
				 * they can't actually go to null before being examined, and
				 * it'd be OK to just remove the PHV wrapping.  We don't have
				 * infrastructure for that, but remove_result_refs() will
				 * relabel them as to be evaluated at the LHS, which is fine.
				 *
				 * Also, we don't need to worry about removing traces of the
				 * join's rtindex, since it hasn't got one.
				 */
				if ((varno = get_result_relid(root, j->rarg)) != 0)
				{
					Assert(j->rtindex == 0);
					remove_result_refs(root, varno, j->larg);
					if (j->quals != NULL && parent_quals == NULL)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->larg), j->quals);
					else
					{
						/* Merge any quals up to parent */
						if (j->quals != NULL)
							*parent_quals = (Node *)
								list_concat(castNode(List, j->quals),
											castNode(List, *parent_quals));
						jtnode = j->larg;
					}
				}
				break;
			case JOIN_FULL:
			case JOIN_ANTI:
				/* We have no special smarts for these cases */
				break;
			default:
				/* Note: JOIN_RIGHT should be gone at this point */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * get_result_relid
 *		If jtnode is a RangeTblRef for an RTE_RESULT RTE, return its relid;
 *		otherwise return 0.
 */
static int
get_result_relid(PlannerInfo *root, Node *jtnode)
{
	int			varno;

	if (!IsA(jtnode, RangeTblRef))
		return 0;
	varno = ((RangeTblRef *) jtnode)->rtindex;
	if (rt_fetch(varno, root->parse->rtable)->rtekind != RTE_RESULT)
		return 0;
	return varno;
}

/*
 * remove_result_refs
 *		Helper routine for dropping an unneeded RTE_RESULT RTE.
 *
 * This doesn't physically remove the RTE from the jointree, because that's
 * more easily handled in remove_useless_results_recurse.  What it does do
 * is the necessary cleanup in the rest of the tree: we must adjust any PHVs
 * that may reference the RTE.  Be sure to call this at a point where the
 * jointree is valid (no disconnected nodes).
 *
 * Note that we don't need to process the append_rel_list, since RTEs
 * referenced directly in the jointree won't be appendrel members.
 *
 * varno is the RTE_RESULT's relid.
 * newjtloc is the jointree location at which any PHVs referencing the
 * RTE_RESULT should be evaluated instead.
 */
static void
remove_result_refs(PlannerInfo *root, int varno, Node *newjtloc)
{
	/* Fix up PlaceHolderVars as needed */
	/* If there are no PHVs anywhere, we can skip this bit */
	if (root->glob->lastPHId != 0)
	{
		Relids		subrelids;

		subrelids = get_relids_in_jointree(newjtloc, true, false);
		Assert(!bms_is_empty(subrelids));
		substitute_phv_relids((Node *) root->parse, varno, subrelids);
		fix_append_rel_relids(root, varno, subrelids);
	}

	/*
	 * We also need to remove any PlanRowMark referencing the RTE, but we
	 * postpone that work until we return to remove_useless_result_rtes.
	 */
}


/*
 * find_dependent_phvs - are there any PlaceHolderVars whose relids are
 * exactly the given varno?
 *
 * find_dependent_phvs should be used when we want to see if there are
 * any such PHVs anywhere in the Query.  Another use-case is to see if
 * a subtree of the join tree contains such PHVs; but for that, we have
 * to look not only at the join tree nodes themselves but at the
 * referenced RTEs.  For that, use find_dependent_phvs_in_jointree.
 */

typedef struct
{
	Relids		relids;
	int			sublevels_up;
} find_dependent_phvs_context;

static bool
find_dependent_phvs_walker(Node *node,
						   find_dependent_phvs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			bms_equal(context->relids, phv->phrels))
			return true;
		/* fall through to examine children */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   find_dependent_phvs_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	/* Shouldn't need to handle most planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_walker(node, find_dependent_phvs_walker,
								  (void *) context);
}

static bool
find_dependent_phvs(PlannerInfo *root, int varno)
{
	find_dependent_phvs_context context;

	/* If there are no PHVs anywhere, we needn't work hard */
	if (root->glob->lastPHId == 0)
		return false;

	context.relids = bms_make_singleton(varno);
	context.sublevels_up = 0;

	if (query_tree_walker(root->parse,
						  find_dependent_phvs_walker,
						  (void *) &context,
						  0))
		return true;
	/* The append_rel_list could be populated already, so check it too */
	if (expression_tree_walker((Node *) root->append_rel_list,
							   find_dependent_phvs_walker,
							   (void *) &context))
		return true;
	return false;
}

static bool
find_dependent_phvs_in_jointree(PlannerInfo *root, Node *node, int varno)
{
	find_dependent_phvs_context context;
	Relids		subrelids;
	int			relid;

	/* If there are no PHVs anywhere, we needn't work hard */
	if (root->glob->lastPHId == 0)
		return false;

	context.relids = bms_make_singleton(varno);
	context.sublevels_up = 0;

	/*
	 * See if the jointree fragment itself contains references (in join quals)
	 */
	if (find_dependent_phvs_walker(node, &context))
		return true;

	/*
	 * Otherwise, identify the set of referenced RTEs (we can ignore joins,
	 * since they should be flattened already, so their join alias lists no
	 * longer matter), and tediously check each RTE.  We can ignore RTEs that
	 * are not marked LATERAL, though, since they couldn't possibly contain
	 * any cross-references to other RTEs.
	 */
	subrelids = get_relids_in_jointree(node, false, false);
	relid = -1;
	while ((relid = bms_next_member(subrelids, relid)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(relid, root->parse->rtable);

		if (rte->lateral &&
			range_table_entry_walker(rte,
									 find_dependent_phvs_walker,
									 (void *) &context,
									 0))
			return true;
	}

	return false;
}

/*
 * substitute_phv_relids - adjust PlaceHolderVar relid sets after pulling up
 * a subquery or removing an RTE_RESULT jointree item
 *
 * Find any PlaceHolderVar nodes in the given tree that reference the
 * pulled-up relid, and change them to reference the replacement relid(s).
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.  This should be OK since the tree was copied by
 * pullup_replace_vars earlier.  Avoid scribbling on the original values of
 * the bitmapsets, though, because expression_tree_mutator doesn't copy those.
 */

typedef struct
{
	int			varno;
	int			sublevels_up;
	Relids		subrelids;
} substitute_phv_relids_context;

static bool
substitute_phv_relids_walker(Node *node,
							 substitute_phv_relids_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			bms_is_member(context->varno, phv->phrels))
		{
			phv->phrels = bms_union(phv->phrels,
									context->subrelids);
			phv->phrels = bms_del_member(phv->phrels,
										 context->varno);
			/* Assert we haven't broken the PHV */
			Assert(!bms_is_empty(phv->phrels));
		}
		/* fall through to examine children */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   substitute_phv_relids_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_walker(node, substitute_phv_relids_walker,
								  (void *) context);
}

static void
substitute_phv_relids(Node *node, int varno, Relids subrelids)
{
	substitute_phv_relids_context context;

	context.varno = varno;
	context.sublevels_up = 0;
	context.subrelids = subrelids;

	/*
	 * Must be prepared to start with a Query or a bare expression tree.
	 */
	query_or_expression_tree_walker(node,
									substitute_phv_relids_walker,
									(void *) &context,
									0);
}

/*
 * fix_append_rel_relids: update RT-index fields of AppendRelInfo nodes
 *
 * When we pull up a subquery, any AppendRelInfo references to the subquery's
 * RT index have to be replaced by the substituted relid (and there had better
 * be only one).  We also need to apply substitute_phv_relids to their
 * translated_vars lists, since those might contain PlaceHolderVars.
 *
 * We assume we may modify the AppendRelInfo nodes in-place.
 */
static void
fix_append_rel_relids(PlannerInfo *root, int varno, Relids subrelids)
{
	ListCell   *l;
	int			subvarno = -1;

	/*
	 * We only want to extract the member relid once, but we mustn't fail
	 * immediately if there are multiple members; it could be that none of the
	 * AppendRelInfo nodes refer to it.  So compute it on first use. Note that
	 * bms_singleton_member will complain if set is not singleton.
	 */
	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);

		/* The parent_relid shouldn't ever be a pullup target */
		Assert(appinfo->parent_relid != varno);

		if (appinfo->child_relid == varno)
		{
			if (subvarno < 0)
				subvarno = bms_singleton_member(subrelids);
			appinfo->child_relid = subvarno;
		}

		/* Also fix up any PHVs in its translated vars */
		if (root->glob->lastPHId != 0)
			substitute_phv_relids((Node *) appinfo->translated_vars,
								  varno, subrelids);
	}
}

/*
 * get_relids_in_jointree: get set of RT indexes present in a jointree
 *
 * Base-relation relids are always included in the result.
 * If include_outer_joins is true, outer-join RT indexes are included.
 * If include_inner_joins is true, inner-join RT indexes are included.
 *
 * Note that for most purposes in the planner, outer joins are included
 * in standard relid sets.  Setting include_inner_joins true is only
 * appropriate for special purposes during subquery flattening.
 */
Relids
get_relids_in_jointree(Node *jtnode, bool include_outer_joins,
					   bool include_inner_joins)
{
	Relids		result = NULL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			result = bms_join(result,
							  get_relids_in_jointree(lfirst(l),
													 include_outer_joins,
													 include_inner_joins));
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		result = get_relids_in_jointree(j->larg,
										include_outer_joins,
										include_inner_joins);
		result = bms_join(result,
						  get_relids_in_jointree(j->rarg,
												 include_outer_joins,
												 include_inner_joins));
		if (j->rtindex)
		{
			if (j->jointype == JOIN_INNER)
			{
				if (include_inner_joins)
					result = bms_add_member(result, j->rtindex);
			}
			else
			{
				if (include_outer_joins)
					result = bms_add_member(result, j->rtindex);
			}
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * get_relids_for_join: get set of base+OJ RT indexes making up a join
 */
Relids
get_relids_for_join(Query *query, int joinrelid)
{
	Node	   *jtnode;

	jtnode = find_jointree_node_for_rel((Node *) query->jointree,
										joinrelid);
	if (!jtnode)
		elog(ERROR, "could not find join node %d", joinrelid);
	return get_relids_in_jointree(jtnode, true, false);
}

/*
 * find_jointree_node_for_rel: locate jointree node for a base or join RT index
 *
 * Returns NULL if not found
 */
static Node *
find_jointree_node_for_rel(Node *jtnode, int relid)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (relid == varno)
			return jtnode;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			jtnode = find_jointree_node_for_rel(lfirst(l), relid);
			if (jtnode)
				return jtnode;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (relid == j->rtindex)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->larg, relid);
		if (jtnode)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->rarg, relid);
		if (jtnode)
			return jtnode;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return NULL;
}
