/*-------------------------------------------------------------------------
 *
 * equivclass.c
 *	  Routines for managing EquivalenceClasses
 *
 * See src/backend/optimizer/README for discussion of EquivalenceClasses.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/equivclass.c,v 1.9 2008/01/09 20:42:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/skey.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "utils/lsyscache.h"


static EquivalenceMember *add_eq_member(EquivalenceClass *ec,
			  Expr *expr, Relids relids,
			  bool is_child, Oid datatype);
static void generate_base_implied_equalities_const(PlannerInfo *root,
									   EquivalenceClass *ec);
static void generate_base_implied_equalities_no_const(PlannerInfo *root,
										  EquivalenceClass *ec);
static void generate_base_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec);
static List *generate_join_implied_equalities_normal(PlannerInfo *root,
										EquivalenceClass *ec,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel);
static List *generate_join_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel);
static Oid select_equality_operator(EquivalenceClass *ec,
						 Oid lefttype, Oid righttype);
static RestrictInfo *create_join_clause(PlannerInfo *root,
				   EquivalenceClass *ec, Oid opno,
				   EquivalenceMember *leftem,
				   EquivalenceMember *rightem,
				   EquivalenceClass *parent_ec);
static bool reconsider_outer_join_clause(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 bool outer_on_left);
static bool reconsider_full_join_clause(PlannerInfo *root,
							RestrictInfo *rinfo);


/*
 * process_equivalence
 *	  The given clause has a mergejoinable operator and can be applied without
 *	  any delay by an outer join, so its two sides can be considered equal
 *	  anywhere they are both computable; moreover that equality can be
 *	  extended transitively.  Record this knowledge in the EquivalenceClass
 *	  data structure.  Returns TRUE if successful, FALSE if not (in which
 *	  case caller should treat the clause as ordinary, not an equivalence).
 *
 * If below_outer_join is true, then the clause was found below the nullable
 * side of an outer join, so its sides might validly be both NULL rather than
 * strictly equal.	We can still deduce equalities in such cases, but we take
 * care to mark an EquivalenceClass if it came from any such clauses.  Also,
 * we have to check that both sides are either pseudo-constants or strict
 * functions of Vars, else they might not both go to NULL above the outer
 * join.  (This is the reason why we need a failure return.  It's more
 * convenient to check this case here than at the call sites...)
 *
 * Note: constructing merged EquivalenceClasses is a standard UNION-FIND
 * problem, for which there exist better data structures than simple lists.
 * If this code ever proves to be a bottleneck then it could be sped up ---
 * but for now, simple is beautiful.
 *
 * Note: this is only called during planner startup, not during GEQO
 * exploration, so we need not worry about whether we're in the right
 * memory context.
 */
bool
process_equivalence(PlannerInfo *root, RestrictInfo *restrictinfo,
					bool below_outer_join)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno,
				item1_type,
				item2_type;
	Expr	   *item1;
	Expr	   *item2;
	Relids		item1_relids,
				item2_relids;
	List	   *opfamilies;
	EquivalenceClass *ec1,
			   *ec2;
	EquivalenceMember *em1,
			   *em2;
	ListCell   *lc1;

	/* Extract info from given clause */
	Assert(is_opclause(clause));
	opno = ((OpExpr *) clause)->opno;
	item1 = (Expr *) get_leftop(clause);
	item2 = (Expr *) get_rightop(clause);
	item1_relids = restrictinfo->left_relids;
	item2_relids = restrictinfo->right_relids;

	/*
	 * If below outer join, check for strictness, else reject.
	 */
	if (below_outer_join)
	{
		if (!bms_is_empty(item1_relids) &&
			contain_nonstrict_functions((Node *) item1))
			return false;		/* LHS is non-strict but not constant */
		if (!bms_is_empty(item2_relids) &&
			contain_nonstrict_functions((Node *) item2))
			return false;		/* RHS is non-strict but not constant */
	}

	/*
	 * We use the declared input types of the operator, not exprType() of the
	 * inputs, as the nominal datatypes for opfamily lookup.  This presumes
	 * that btree operators are always registered with amoplefttype and
	 * amoprighttype equal to their declared input types.  We will need this
	 * info anyway to build EquivalenceMember nodes, and by extracting it now
	 * we can use type comparisons to short-circuit some equal() tests.
	 */
	op_input_types(opno, &item1_type, &item2_type);

	opfamilies = restrictinfo->mergeopfamilies;

	/*
	 * Sweep through the existing EquivalenceClasses looking for matches to
	 * item1 and item2.  These are the possible outcomes:
	 *
	 * 1. We find both in the same EC.	The equivalence is already known, so
	 * there's nothing to do.
	 *
	 * 2. We find both in different ECs.  Merge the two ECs together.
	 *
	 * 3. We find just one.  Add the other to its EC.
	 *
	 * 4. We find neither.	Make a new, two-entry EC.
	 *
	 * Note: since all ECs are built through this process, it's impossible
	 * that we'd match an item in more than one existing EC.  It is possible
	 * to match more than once within an EC, if someone fed us something silly
	 * like "WHERE X=X".  (However, we can't simply discard such clauses,
	 * since they should fail when X is null; so we will build a 2-member EC
	 * to ensure the correct restriction clause gets generated.  Hence there
	 * is no shortcut here for item1 and item2 equal.)
	 */
	ec1 = ec2 = NULL;
	em1 = em2 = NULL;
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/* Never match to a volatile EC */
		if (cur_ec->ec_has_volatile)
			continue;

		/*
		 * A "match" requires matching sets of btree opfamilies.  Use of
		 * equal() for this test has implications discussed in the comments
		 * for get_mergejoin_opfamilies().
		 */
		if (!equal(opfamilies, cur_ec->ec_opfamilies))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			Assert(!cur_em->em_is_child);		/* no children yet */

			/*
			 * If below an outer join, don't match constants: they're not as
			 * constant as they look.
			 */
			if ((below_outer_join || cur_ec->ec_below_outer_join) &&
				cur_em->em_is_const)
				continue;

			if (!ec1 &&
				item1_type == cur_em->em_datatype &&
				equal(item1, cur_em->em_expr))
			{
				ec1 = cur_ec;
				em1 = cur_em;
				if (ec2)
					break;
			}

			if (!ec2 &&
				item2_type == cur_em->em_datatype &&
				equal(item2, cur_em->em_expr))
			{
				ec2 = cur_ec;
				em2 = cur_em;
				if (ec1)
					break;
			}
		}

		if (ec1 && ec2)
			break;
	}

	/* Sweep finished, what did we find? */

	if (ec1 && ec2)
	{
		/* If case 1, nothing to do, except add to sources */
		if (ec1 == ec2)
		{
			ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
			ec1->ec_below_outer_join |= below_outer_join;
			/* mark the RI as usable with this pair of EMs */
			/* NB: can't set left_ec/right_ec until merging is finished */
			restrictinfo->left_em = em1;
			restrictinfo->right_em = em2;
			return true;
		}

		/*
		 * Case 2: need to merge ec1 and ec2.  We add ec2's items to ec1, then
		 * set ec2's ec_merged link to point to ec1 and remove ec2 from the
		 * eq_classes list.  We cannot simply delete ec2 because that could
		 * leave dangling pointers in existing PathKeys.  We leave it behind
		 * with a link so that the merged EC can be found.
		 */
		ec1->ec_members = list_concat(ec1->ec_members, ec2->ec_members);
		ec1->ec_sources = list_concat(ec1->ec_sources, ec2->ec_sources);
		ec1->ec_derives = list_concat(ec1->ec_derives, ec2->ec_derives);
		ec1->ec_relids = bms_join(ec1->ec_relids, ec2->ec_relids);
		ec1->ec_has_const |= ec2->ec_has_const;
		/* can't need to set has_volatile */
		ec1->ec_below_outer_join |= ec2->ec_below_outer_join;
		ec2->ec_merged = ec1;
		root->eq_classes = list_delete_ptr(root->eq_classes, ec2);
		/* just to avoid debugging confusion w/ dangling pointers: */
		ec2->ec_members = NIL;
		ec2->ec_sources = NIL;
		ec2->ec_derives = NIL;
		ec2->ec_relids = NULL;
		ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
		ec1->ec_below_outer_join |= below_outer_join;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else if (ec1)
	{
		/* Case 3: add item2 to ec1 */
		em2 = add_eq_member(ec1, item2, item2_relids, false, item2_type);
		ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
		ec1->ec_below_outer_join |= below_outer_join;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else if (ec2)
	{
		/* Case 3: add item1 to ec2 */
		em1 = add_eq_member(ec2, item1, item1_relids, false, item1_type);
		ec2->ec_sources = lappend(ec2->ec_sources, restrictinfo);
		ec2->ec_below_outer_join |= below_outer_join;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else
	{
		/* Case 4: make a new, two-entry EC */
		EquivalenceClass *ec = makeNode(EquivalenceClass);

		ec->ec_opfamilies = opfamilies;
		ec->ec_members = NIL;
		ec->ec_sources = list_make1(restrictinfo);
		ec->ec_derives = NIL;
		ec->ec_relids = NULL;
		ec->ec_has_const = false;
		ec->ec_has_volatile = false;
		ec->ec_below_outer_join = below_outer_join;
		ec->ec_broken = false;
		ec->ec_sortref = 0;
		ec->ec_merged = NULL;
		em1 = add_eq_member(ec, item1, item1_relids, false, item1_type);
		em2 = add_eq_member(ec, item2, item2_relids, false, item2_type);

		root->eq_classes = lappend(root->eq_classes, ec);

		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}

	return true;
}

/*
 * add_eq_member - build a new EquivalenceMember and add it to an EC
 */
static EquivalenceMember *
add_eq_member(EquivalenceClass *ec, Expr *expr, Relids relids,
			  bool is_child, Oid datatype)
{
	EquivalenceMember *em = makeNode(EquivalenceMember);

	em->em_expr = expr;
	em->em_relids = relids;
	em->em_is_const = false;
	em->em_is_child = is_child;
	em->em_datatype = datatype;

	if (bms_is_empty(relids))
	{
		/*
		 * No Vars, assume it's a pseudoconstant.  This is correct for entries
		 * generated from process_equivalence(), because a WHERE clause can't
		 * contain aggregates or SRFs, and non-volatility was checked before
		 * process_equivalence() ever got called.  But
		 * get_eclass_for_sort_expr() has to work harder.  We put the tests
		 * there not here to save cycles in the equivalence case.
		 */
		Assert(!is_child);
		em->em_is_const = true;
		ec->ec_has_const = true;
		/* it can't affect ec_relids */
	}
	else if (!is_child)			/* child members don't add to ec_relids */
	{
		ec->ec_relids = bms_add_members(ec->ec_relids, relids);
	}
	ec->ec_members = lappend(ec->ec_members, em);

	return em;
}


/*
 * get_eclass_for_sort_expr
 *	  Given an expression and opfamily info, find an existing equivalence
 *	  class it is a member of; if none, build a new single-member
 *	  EquivalenceClass for it.
 *
 * sortref is the SortGroupRef of the originating SortClause, if any,
 * or zero if not.
 *
 * This can be used safely both before and after EquivalenceClass merging;
 * since it never causes merging it does not invalidate any existing ECs
 * or PathKeys.
 *
 * Note: opfamilies must be chosen consistently with the way
 * process_equivalence() would do; that is, generated from a mergejoinable
 * equality operator.  Else we might fail to detect valid equivalences,
 * generating poor (but not incorrect) plans.
 */
EquivalenceClass *
get_eclass_for_sort_expr(PlannerInfo *root,
						 Expr *expr,
						 Oid expr_datatype,
						 List *opfamilies,
						 Index sortref)
{
	EquivalenceClass *newec;
	EquivalenceMember *newem;
	ListCell   *lc1;
	MemoryContext oldcontext;

	/*
	 * Scan through the existing EquivalenceClasses for a match
	 */
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/* Never match to a volatile EC */
		if (cur_ec->ec_has_volatile)
			continue;

		if (!equal(opfamilies, cur_ec->ec_opfamilies))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			/*
			 * If below an outer join, don't match constants: they're not as
			 * constant as they look.
			 */
			if (cur_ec->ec_below_outer_join &&
				cur_em->em_is_const)
				continue;

			if (expr_datatype == cur_em->em_datatype &&
				equal(expr, cur_em->em_expr))
				return cur_ec;	/* Match! */
		}
	}

	/*
	 * No match, so build a new single-member EC
	 *
	 * Here, we must be sure that we construct the EC in the right context. We
	 * can assume, however, that the passed expr is long-lived.
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	newec = makeNode(EquivalenceClass);
	newec->ec_opfamilies = list_copy(opfamilies);
	newec->ec_members = NIL;
	newec->ec_sources = NIL;
	newec->ec_derives = NIL;
	newec->ec_relids = NULL;
	newec->ec_has_const = false;
	newec->ec_has_volatile = contain_volatile_functions((Node *) expr);
	newec->ec_below_outer_join = false;
	newec->ec_broken = false;
	newec->ec_sortref = sortref;
	newec->ec_merged = NULL;
	newem = add_eq_member(newec, expr, pull_varnos((Node *) expr),
						  false, expr_datatype);

	/*
	 * add_eq_member doesn't check for volatile functions, set-returning
	 * functions, or aggregates, but such could appear in sort expressions; so
	 * we have to check whether its const-marking was correct.
	 */
	if (newec->ec_has_const)
	{
		if (newec->ec_has_volatile ||
			expression_returns_set((Node *) expr) ||
			contain_agg_clause((Node *) expr))
		{
			newec->ec_has_const = false;
			newem->em_is_const = false;
		}
	}

	root->eq_classes = lappend(root->eq_classes, newec);

	MemoryContextSwitchTo(oldcontext);

	return newec;
}


/*
 * generate_base_implied_equalities
 *	  Generate any restriction clauses that we can deduce from equivalence
 *	  classes.
 *
 * When an EC contains pseudoconstants, our strategy is to generate
 * "member = const1" clauses where const1 is the first constant member, for
 * every other member (including other constants).	If we are able to do this
 * then we don't need any "var = var" comparisons because we've successfully
 * constrained all the vars at their points of creation.  If we fail to
 * generate any of these clauses due to lack of cross-type operators, we fall
 * back to the "ec_broken" strategy described below.  (XXX if there are
 * multiple constants of different types, it's possible that we might succeed
 * in forming all the required clauses if we started from a different const
 * member; but this seems a sufficiently hokey corner case to not be worth
 * spending lots of cycles on.)
 *
 * For ECs that contain no pseudoconstants, we generate derived clauses
 * "member1 = member2" for each pair of members belonging to the same base
 * relation (actually, if there are more than two for the same base relation,
 * we only need enough clauses to link each to each other).  This provides
 * the base case for the recursion: each row emitted by a base relation scan
 * will constrain all computable members of the EC to be equal.  As each
 * join path is formed, we'll add additional derived clauses on-the-fly
 * to maintain this invariant (see generate_join_implied_equalities).
 *
 * If the opfamilies used by the EC do not provide complete sets of cross-type
 * equality operators, it is possible that we will fail to generate a clause
 * that must be generated to maintain the invariant.  (An example: given
 * "WHERE a.x = b.y AND b.y = a.z", the scheme breaks down if we cannot
 * generate "a.x = a.z" as a restriction clause for A.)  In this case we mark
 * the EC "ec_broken" and fall back to regurgitating its original source
 * RestrictInfos at appropriate times.	We do not try to retract any derived
 * clauses already generated from the broken EC, so the resulting plan could
 * be poor due to bad selectivity estimates caused by redundant clauses.  But
 * the correct solution to that is to fix the opfamilies ...
 *
 * Equality clauses derived by this function are passed off to
 * process_implied_equality (in plan/initsplan.c) to be inserted into the
 * restrictinfo datastructures.  Note that this must be called after initial
 * scanning of the quals and before Path construction begins.
 *
 * We make no attempt to avoid generating duplicate RestrictInfos here: we
 * don't search ec_sources for matches, nor put the created RestrictInfos
 * into ec_derives.  Doing so would require some slightly ugly changes in
 * initsplan.c's API, and there's no real advantage, because the clauses
 * generated here can't duplicate anything we will generate for joins anyway.
 */
void
generate_base_implied_equalities(PlannerInfo *root)
{
	ListCell   *lc;
	Index		rti;

	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);

		Assert(ec->ec_merged == NULL);	/* else shouldn't be in list */
		Assert(!ec->ec_broken); /* not yet anyway... */

		/* Single-member ECs won't generate any deductions */
		if (list_length(ec->ec_members) <= 1)
			continue;

		if (ec->ec_has_const)
			generate_base_implied_equalities_const(root, ec);
		else
			generate_base_implied_equalities_no_const(root, ec);

		/* Recover if we failed to generate required derived clauses */
		if (ec->ec_broken)
			generate_base_implied_equalities_broken(root, ec);
	}

	/*
	 * This is also a handy place to mark base rels (which should all exist by
	 * now) with flags showing whether they have pending eclass joins.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL)
			continue;

		brel->has_eclass_joins = has_relevant_eclass_joinclause(root, brel);
	}
}

/*
 * generate_base_implied_equalities when EC contains pseudoconstant(s)
 */
static void
generate_base_implied_equalities_const(PlannerInfo *root,
									   EquivalenceClass *ec)
{
	EquivalenceMember *const_em = NULL;
	ListCell   *lc;

	/*
	 * In the trivial case where we just had one "var = const" clause,
	 * push the original clause back into the main planner machinery.  There
	 * is nothing to be gained by doing it differently, and we save the
	 * effort to re-build and re-analyze an equality clause that will be
	 * exactly equivalent to the old one.
	 */
	if (list_length(ec->ec_members) == 2 &&
		list_length(ec->ec_sources) == 1)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) linitial(ec->ec_sources);

		if (bms_membership(restrictinfo->required_relids) != BMS_MULTIPLE)
		{
			distribute_restrictinfo_to_rels(root, restrictinfo);
			return;
		}
	}

	/* Find the constant member to use */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (cur_em->em_is_const)
		{
			const_em = cur_em;
			break;
		}
	}
	Assert(const_em != NULL);

	/* Generate a derived equality against each other member */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		Oid			eq_op;

		Assert(!cur_em->em_is_child);	/* no children yet */
		if (cur_em == const_em)
			continue;
		eq_op = select_equality_operator(ec,
										 cur_em->em_datatype,
										 const_em->em_datatype);
		if (!OidIsValid(eq_op))
		{
			/* failed... */
			ec->ec_broken = true;
			break;
		}
		process_implied_equality(root, eq_op,
								 cur_em->em_expr, const_em->em_expr,
								 ec->ec_relids,
								 ec->ec_below_outer_join,
								 cur_em->em_is_const);
	}
}

/*
 * generate_base_implied_equalities when EC contains no pseudoconstants
 */
static void
generate_base_implied_equalities_no_const(PlannerInfo *root,
										  EquivalenceClass *ec)
{
	EquivalenceMember **prev_ems;
	ListCell   *lc;

	/*
	 * We scan the EC members once and track the last-seen member for each
	 * base relation.  When we see another member of the same base relation,
	 * we generate "prev_mem = cur_mem".  This results in the minimum number
	 * of derived clauses, but it's possible that it will fail when a
	 * different ordering would succeed.  XXX FIXME: use a UNION-FIND
	 * algorithm similar to the way we build merged ECs.  (Use a list-of-lists
	 * for each rel.)
	 */
	prev_ems = (EquivalenceMember **)
		palloc0(root->simple_rel_array_size * sizeof(EquivalenceMember *));

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		int			relid;

		Assert(!cur_em->em_is_child);	/* no children yet */
		if (bms_membership(cur_em->em_relids) != BMS_SINGLETON)
			continue;
		relid = bms_singleton_member(cur_em->em_relids);
		Assert(relid < root->simple_rel_array_size);

		if (prev_ems[relid] != NULL)
		{
			EquivalenceMember *prev_em = prev_ems[relid];
			Oid			eq_op;

			eq_op = select_equality_operator(ec,
											 prev_em->em_datatype,
											 cur_em->em_datatype);
			if (!OidIsValid(eq_op))
			{
				/* failed... */
				ec->ec_broken = true;
				break;
			}
			process_implied_equality(root, eq_op,
									 prev_em->em_expr, cur_em->em_expr,
									 ec->ec_relids,
									 ec->ec_below_outer_join,
									 false);
		}
		prev_ems[relid] = cur_em;
	}

	pfree(prev_ems);

	/*
	 * We also have to make sure that all the Vars used in the member clauses
	 * will be available at any join node we might try to reference them at.
	 * For the moment we force all the Vars to be available at all join nodes
	 * for this eclass.  Perhaps this could be improved by doing some
	 * pre-analysis of which members we prefer to join, but it's no worse than
	 * what happened in the pre-8.3 code.
	 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		List	   *vars = pull_var_clause((Node *) cur_em->em_expr, false);

		add_vars_to_targetlist(root, vars, ec->ec_relids);
		list_free(vars);
	}
}

/*
 * generate_base_implied_equalities cleanup after failure
 *
 * What we must do here is push any zero- or one-relation source RestrictInfos
 * of the EC back into the main restrictinfo datastructures.  Multi-relation
 * clauses will be regurgitated later by generate_join_implied_equalities().
 * (We do it this way to maintain continuity with the case that ec_broken
 * becomes set only after we've gone up a join level or two.)
 */
static void
generate_base_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec)
{
	ListCell   *lc;

	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		if (bms_membership(restrictinfo->required_relids) != BMS_MULTIPLE)
			distribute_restrictinfo_to_rels(root, restrictinfo);
	}
}


/*
 * generate_join_implied_equalities
 *	  Generate any join clauses that we can deduce from equivalence classes.
 *
 * At a join node, we must enforce restriction clauses sufficient to ensure
 * that all equivalence-class members computable at that node are equal.
 * Since the set of clauses to enforce can vary depending on which subset
 * relations are the inputs, we have to compute this afresh for each join
 * path pair.  Hence a fresh List of RestrictInfo nodes is built and passed
 * back on each call.
 *
 * The results are sufficient for use in merge, hash, and plain nestloop join
 * methods.  We do not worry here about selecting clauses that are optimal
 * for use in a nestloop-with-inner-indexscan join, however.  indxpath.c makes
 * its own selections of clauses to use, and if the ones we pick here are
 * redundant with those, the extras will be eliminated in createplan.c.
 *
 * Because the same join clauses are likely to be needed multiple times as
 * we consider different join paths, we avoid generating multiple copies:
 * whenever we select a particular pair of EquivalenceMembers to join,
 * we check to see if the pair matches any original clause (in ec_sources)
 * or previously-built clause (in ec_derives).	This saves memory and allows
 * re-use of information cached in RestrictInfos.
 */
List *
generate_join_implied_equalities(PlannerInfo *root,
								 RelOptInfo *joinrel,
								 RelOptInfo *outer_rel,
								 RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		List	   *sublist = NIL;

		/* ECs containing consts do not need any further enforcement */
		if (ec->ec_has_const)
			continue;

		/* Single-member ECs won't generate any deductions */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/* We can quickly ignore any that don't overlap the join, too */
		if (!bms_overlap(ec->ec_relids, joinrel->relids))
			continue;

		if (!ec->ec_broken)
			sublist = generate_join_implied_equalities_normal(root,
															  ec,
															  joinrel,
															  outer_rel,
															  inner_rel);

		/* Recover if we failed to generate required derived clauses */
		if (ec->ec_broken)
			sublist = generate_join_implied_equalities_broken(root,
															  ec,
															  joinrel,
															  outer_rel,
															  inner_rel);

		result = list_concat(result, sublist);
	}

	return result;
}

/*
 * generate_join_implied_equalities for a still-valid EC
 */
static List *
generate_join_implied_equalities_normal(PlannerInfo *root,
										EquivalenceClass *ec,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	List	   *new_members = NIL;
	List	   *outer_members = NIL;
	List	   *inner_members = NIL;
	ListCell   *lc1;

	/*
	 * First, scan the EC to identify member values that are computable at the
	 * outer rel, at the inner rel, or at this relation but not in either
	 * input rel.  The outer-rel members should already be enforced equal,
	 * likewise for the inner-rel members.	We'll need to create clauses to
	 * enforce that any newly computable members are all equal to each other
	 * as well as to at least one input member, plus enforce at least one
	 * outer-rel member equal to at least one inner-rel member.
	 */
	foreach(lc1, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc1);

		if (cur_em->em_is_child)
			continue;			/* ignore children here */
		if (!bms_is_subset(cur_em->em_relids, joinrel->relids))
			continue;			/* ignore --- not computable yet */

		if (bms_is_subset(cur_em->em_relids, outer_rel->relids))
			outer_members = lappend(outer_members, cur_em);
		else if (bms_is_subset(cur_em->em_relids, inner_rel->relids))
			inner_members = lappend(inner_members, cur_em);
		else
			new_members = lappend(new_members, cur_em);
	}

	/*
	 * First, select the joinclause if needed.	We can equate any one outer
	 * member to any one inner member, but we have to find a datatype
	 * combination for which an opfamily member operator exists.  If we have
	 * choices, we prefer simple Var members (possibly with RelabelType) since
	 * these are (a) cheapest to compute at runtime and (b) most likely to
	 * have useful statistics.	Also, if enable_hashjoin is on, we prefer
	 * operators that are also hashjoinable.
	 */
	if (outer_members && inner_members)
	{
		EquivalenceMember *best_outer_em = NULL;
		EquivalenceMember *best_inner_em = NULL;
		Oid			best_eq_op = InvalidOid;
		int			best_score = -1;
		RestrictInfo *rinfo;

		foreach(lc1, outer_members)
		{
			EquivalenceMember *outer_em = (EquivalenceMember *) lfirst(lc1);
			ListCell   *lc2;

			foreach(lc2, inner_members)
			{
				EquivalenceMember *inner_em = (EquivalenceMember *) lfirst(lc2);
				Oid			eq_op;
				int			score;

				eq_op = select_equality_operator(ec,
												 outer_em->em_datatype,
												 inner_em->em_datatype);
				if (!OidIsValid(eq_op))
					continue;
				score = 0;
				if (IsA(outer_em->em_expr, Var) ||
					(IsA(outer_em->em_expr, RelabelType) &&
					 IsA(((RelabelType *) outer_em->em_expr)->arg, Var)))
					score++;
				if (IsA(inner_em->em_expr, Var) ||
					(IsA(inner_em->em_expr, RelabelType) &&
					 IsA(((RelabelType *) inner_em->em_expr)->arg, Var)))
					score++;
				if (!enable_hashjoin || op_hashjoinable(eq_op))
					score++;
				if (score > best_score)
				{
					best_outer_em = outer_em;
					best_inner_em = inner_em;
					best_eq_op = eq_op;
					best_score = score;
					if (best_score == 3)
						break;	/* no need to look further */
				}
			}
			if (best_score == 3)
				break;			/* no need to look further */
		}
		if (best_score < 0)
		{
			/* failed... */
			ec->ec_broken = true;
			return NIL;
		}

		/*
		 * Create clause, setting parent_ec to mark it as redundant with other
		 * joinclauses
		 */
		rinfo = create_join_clause(root, ec, best_eq_op,
								   best_outer_em, best_inner_em,
								   ec);

		result = lappend(result, rinfo);
	}

	/*
	 * Now deal with building restrictions for any expressions that involve
	 * Vars from both sides of the join.  We have to equate all of these to
	 * each other as well as to at least one old member (if any).
	 *
	 * XXX as in generate_base_implied_equalities_no_const, we could be a lot
	 * smarter here to avoid unnecessary failures in cross-type situations.
	 * For now, use the same left-to-right method used there.
	 */
	if (new_members)
	{
		List	   *old_members = list_concat(outer_members, inner_members);
		EquivalenceMember *prev_em = NULL;
		RestrictInfo *rinfo;

		/* For now, arbitrarily take the first old_member as the one to use */
		if (old_members)
			new_members = lappend(new_members, linitial(old_members));

		foreach(lc1, new_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc1);

			if (prev_em != NULL)
			{
				Oid			eq_op;

				eq_op = select_equality_operator(ec,
												 prev_em->em_datatype,
												 cur_em->em_datatype);
				if (!OidIsValid(eq_op))
				{
					/* failed... */
					ec->ec_broken = true;
					return NIL;
				}
				/* do NOT set parent_ec, this qual is not redundant! */
				rinfo = create_join_clause(root, ec, eq_op,
										   prev_em, cur_em,
										   NULL);

				result = lappend(result, rinfo);
			}
			prev_em = cur_em;
		}
	}

	return result;
}

/*
 * generate_join_implied_equalities cleanup after failure
 *
 * Return any original RestrictInfos that are enforceable at this join.
 */
static List *
generate_join_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(restrictinfo->required_relids, joinrel->relids) &&
		  !bms_is_subset(restrictinfo->required_relids, outer_rel->relids) &&
			!bms_is_subset(restrictinfo->required_relids, inner_rel->relids))
			result = lappend(result, restrictinfo);
	}

	return result;
}


/*
 * select_equality_operator
 *	  Select a suitable equality operator for comparing two EC members
 *
 * Returns InvalidOid if no operator can be found for this datatype combination
 */
static Oid
select_equality_operator(EquivalenceClass *ec, Oid lefttype, Oid righttype)
{
	ListCell   *lc;

	foreach(lc, ec->ec_opfamilies)
	{
		Oid			opfamily = lfirst_oid(lc);
		Oid			opno;

		opno = get_opfamily_member(opfamily, lefttype, righttype,
								   BTEqualStrategyNumber);
		if (OidIsValid(opno))
			return opno;
	}
	return InvalidOid;
}


/*
 * create_join_clause
 *	  Find or make a RestrictInfo comparing the two given EC members
 *	  with the given operator.
 *
 * parent_ec is either equal to ec (if the clause is a potentially-redundant
 * join clause) or NULL (if not).  We have to treat this as part of the
 * match requirements --- it's possible that a clause comparing the same two
 * EMs is a join clause in one join path and a restriction clause in another.
 */
static RestrictInfo *
create_join_clause(PlannerInfo *root,
				   EquivalenceClass *ec, Oid opno,
				   EquivalenceMember *leftem,
				   EquivalenceMember *rightem,
				   EquivalenceClass *parent_ec)
{
	RestrictInfo *rinfo;
	ListCell   *lc;
	MemoryContext oldcontext;

	/*
	 * Search to see if we already built a RestrictInfo for this pair of
	 * EquivalenceMembers.	We can use either original source clauses or
	 * previously-derived clauses.	The check on opno is probably redundant,
	 * but be safe ...
	 */
	foreach(lc, ec->ec_sources)
	{
		rinfo = (RestrictInfo *) lfirst(lc);
		if (rinfo->left_em == leftem &&
			rinfo->right_em == rightem &&
			rinfo->parent_ec == parent_ec &&
			opno == ((OpExpr *) rinfo->clause)->opno)
			return rinfo;
	}

	foreach(lc, ec->ec_derives)
	{
		rinfo = (RestrictInfo *) lfirst(lc);
		if (rinfo->left_em == leftem &&
			rinfo->right_em == rightem &&
			rinfo->parent_ec == parent_ec &&
			opno == ((OpExpr *) rinfo->clause)->opno)
			return rinfo;
	}

	/*
	 * Not there, so build it, in planner context so we can re-use it. (Not
	 * important in normal planning, but definitely so in GEQO.)
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	rinfo = build_implied_join_equality(opno,
										leftem->em_expr,
										rightem->em_expr,
										bms_union(leftem->em_relids,
												  rightem->em_relids));

	/* Mark the clause as redundant, or not */
	rinfo->parent_ec = parent_ec;

	/*
	 * We can set these now, rather than letting them be looked up later,
	 * since this is only used after EC merging is complete.
	 */
	rinfo->left_ec = ec;
	rinfo->right_ec = ec;

	/* Mark it as usable with these EMs */
	rinfo->left_em = leftem;
	rinfo->right_em = rightem;
	/* and save it for possible re-use */
	ec->ec_derives = lappend(ec->ec_derives, rinfo);

	MemoryContextSwitchTo(oldcontext);

	return rinfo;
}


/*
 * reconsider_outer_join_clauses
 *	  Re-examine any outer-join clauses that were set aside by
 *	  distribute_qual_to_rels(), and see if we can derive any
 *	  EquivalenceClasses from them.  Then, if they were not made
 *	  redundant, push them out into the regular join-clause lists.
 *
 * When we have mergejoinable clauses A = B that are outer-join clauses,
 * we can't blindly combine them with other clauses A = C to deduce B = C,
 * since in fact the "equality" A = B won't necessarily hold above the
 * outer join (one of the variables might be NULL instead).  Nonetheless
 * there are cases where we can add qual clauses using transitivity.
 *
 * One case that we look for here is an outer-join clause OUTERVAR = INNERVAR
 * for which there is also an equivalence clause OUTERVAR = CONSTANT.
 * It is safe and useful to push a clause INNERVAR = CONSTANT into the
 * evaluation of the inner (nullable) relation, because any inner rows not
 * meeting this condition will not contribute to the outer-join result anyway.
 * (Any outer rows they could join to will be eliminated by the pushed-down
 * equivalence clause.)
 *
 * Note that the above rule does not work for full outer joins; nor is it
 * very interesting to consider cases where the generated equivalence clause
 * would involve relations outside the outer join, since such clauses couldn't
 * be pushed into the inner side's scan anyway.  So the restriction to
 * outervar = pseudoconstant is not really giving up anything.
 *
 * For full-join cases, we can only do something useful if it's a FULL JOIN
 * USING and a merged column has an equivalence MERGEDVAR = CONSTANT.
 * By the time it gets here, the merged column will look like
 *		COALESCE(LEFTVAR, RIGHTVAR)
 * and we will have a full-join clause LEFTVAR = RIGHTVAR that we can match
 * the COALESCE expression to. In this situation we can push LEFTVAR = CONSTANT
 * and RIGHTVAR = CONSTANT into the input relations, since any rows not
 * meeting these conditions cannot contribute to the join result.
 *
 * Again, there isn't any traction to be gained by trying to deal with
 * clauses comparing a mergedvar to a non-pseudoconstant.  So we can make
 * use of the EquivalenceClasses to search for matching variables that were
 * equivalenced to constants.  The interesting outer-join clauses were
 * accumulated for us by distribute_qual_to_rels.
 *
 * When we find one of these cases, we implement the changes we want by
 * generating a new equivalence clause INNERVAR = CONSTANT (or LEFTVAR, etc)
 * and pushing it into the EquivalenceClass structures.  This is because we
 * may already know that INNERVAR is equivalenced to some other var(s), and
 * we'd like the constant to propagate to them too.  Note that it would be
 * unsafe to merge any existing EC for INNERVAR with the OUTERVAR's EC ---
 * that could result in propagating constant restrictions from
 * INNERVAR to OUTERVAR, which would be very wrong.
 *
 * It's possible that the INNERVAR is also an OUTERVAR for some other
 * outer-join clause, in which case the process can be repeated.  So we repeat
 * looping over the lists of clauses until no further deductions can be made.
 * Whenever we do make a deduction, we remove the generating clause from the
 * lists, since we don't want to make the same deduction twice.
 *
 * If we don't find any match for a set-aside outer join clause, we must
 * throw it back into the regular joinclause processing by passing it to
 * distribute_restrictinfo_to_rels().  If we do generate a derived clause,
 * however, the outer-join clause is redundant.  We still throw it back,
 * because otherwise the join will be seen as a clauseless join and avoided
 * during join order searching; but we mark it as redundant to keep from
 * messing up the joinrel's size estimate.  (This behavior means that the
 * API for this routine is uselessly complex: we could have just put all
 * the clauses into the regular processing initially.  We keep it because
 * someday we might want to do something else, such as inserting "dummy"
 * joinclauses instead of real ones.)
 *
 * Outer join clauses that are marked outerjoin_delayed are special: this
 * condition means that one or both VARs might go to null due to a lower
 * outer join.  We can still push a constant through the clause, but only
 * if its operator is strict; and we *have to* throw the clause back into
 * regular joinclause processing.  By keeping the strict join clause,
 * we ensure that any null-extended rows that are mistakenly generated due
 * to suppressing rows not matching the constant will be rejected at the
 * upper outer join.  (This doesn't work for full-join clauses.)
 */
void
reconsider_outer_join_clauses(PlannerInfo *root)
{
	bool		found;
	ListCell   *cell;
	ListCell   *prev;
	ListCell   *next;

	/* Outer loop repeats until we find no more deductions */
	do
	{
		found = false;

		/* Process the LEFT JOIN clauses */
		prev = NULL;
		for (cell = list_head(root->left_join_clauses); cell; cell = next)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);

			next = lnext(cell);
			if (reconsider_outer_join_clause(root, rinfo, true))
			{
				found = true;
				/* remove it from the list */
				root->left_join_clauses =
					list_delete_cell(root->left_join_clauses, cell, prev);
				/* we throw it back anyway (see notes above) */
				/* but the thrown-back clause has no extra selectivity */
				rinfo->this_selec = 1.0;
				distribute_restrictinfo_to_rels(root, rinfo);
			}
			else
				prev = cell;
		}

		/* Process the RIGHT JOIN clauses */
		prev = NULL;
		for (cell = list_head(root->right_join_clauses); cell; cell = next)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);

			next = lnext(cell);
			if (reconsider_outer_join_clause(root, rinfo, false))
			{
				found = true;
				/* remove it from the list */
				root->right_join_clauses =
					list_delete_cell(root->right_join_clauses, cell, prev);
				/* we throw it back anyway (see notes above) */
				/* but the thrown-back clause has no extra selectivity */
				rinfo->this_selec = 1.0;
				distribute_restrictinfo_to_rels(root, rinfo);
			}
			else
				prev = cell;
		}

		/* Process the FULL JOIN clauses */
		prev = NULL;
		for (cell = list_head(root->full_join_clauses); cell; cell = next)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);

			next = lnext(cell);
			if (reconsider_full_join_clause(root, rinfo))
			{
				found = true;
				/* remove it from the list */
				root->full_join_clauses =
					list_delete_cell(root->full_join_clauses, cell, prev);
				/* we throw it back anyway (see notes above) */
				/* but the thrown-back clause has no extra selectivity */
				rinfo->this_selec = 1.0;
				distribute_restrictinfo_to_rels(root, rinfo);
			}
			else
				prev = cell;
		}
	} while (found);

	/* Now, any remaining clauses have to be thrown back */
	foreach(cell, root->left_join_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);

		distribute_restrictinfo_to_rels(root, rinfo);
	}
	foreach(cell, root->right_join_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);

		distribute_restrictinfo_to_rels(root, rinfo);
	}
	foreach(cell, root->full_join_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);

		distribute_restrictinfo_to_rels(root, rinfo);
	}
}

/*
 * reconsider_outer_join_clauses for a single LEFT/RIGHT JOIN clause
 *
 * Returns TRUE if we were able to propagate a constant through the clause.
 */
static bool
reconsider_outer_join_clause(PlannerInfo *root, RestrictInfo *rinfo,
							 bool outer_on_left)
{
	Expr	   *outervar,
			   *innervar;
	Oid			opno,
				left_type,
				right_type,
				inner_datatype;
	Relids		inner_relids;
	ListCell   *lc1;

	Assert(is_opclause(rinfo->clause));
	opno = ((OpExpr *) rinfo->clause)->opno;

	/* If clause is outerjoin_delayed, operator must be strict */
	if (rinfo->outerjoin_delayed && !op_strict(opno))
		return false;

	/* Extract needed info from the clause */
	op_input_types(opno, &left_type, &right_type);
	if (outer_on_left)
	{
		outervar = (Expr *) get_leftop(rinfo->clause);
		innervar = (Expr *) get_rightop(rinfo->clause);
		inner_datatype = right_type;
		inner_relids = rinfo->right_relids;
	}
	else
	{
		outervar = (Expr *) get_rightop(rinfo->clause);
		innervar = (Expr *) get_leftop(rinfo->clause);
		inner_datatype = left_type;
		inner_relids = rinfo->left_relids;
	}

	/* Scan EquivalenceClasses for a match to outervar */
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		bool		match;
		ListCell   *lc2;

		/* Ignore EC unless it contains pseudoconstants */
		if (!cur_ec->ec_has_const)
			continue;
		/* Never match to a volatile EC */
		if (cur_ec->ec_has_volatile)
			continue;
		/* It has to match the outer-join clause as to opfamilies, too */
		if (!equal(rinfo->mergeopfamilies, cur_ec->ec_opfamilies))
			continue;
		/* Does it contain a match to outervar? */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			if (equal(outervar, cur_em->em_expr))
			{
				match = true;
				break;
			}
		}
		if (!match)
			continue;			/* no match, so ignore this EC */

		/*
		 * Yes it does!  Try to generate a clause INNERVAR = CONSTANT for each
		 * CONSTANT in the EC.	Note that we must succeed with at least one
		 * constant before we can decide to throw away the outer-join clause.
		 */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);
			Oid			eq_op;
			RestrictInfo *newrinfo;

			if (!cur_em->em_is_const)
				continue;		/* ignore non-const members */
			eq_op = select_equality_operator(cur_ec,
											 inner_datatype,
											 cur_em->em_datatype);
			if (!OidIsValid(eq_op))
				continue;		/* can't generate equality */
			newrinfo = build_implied_join_equality(eq_op,
												   innervar,
												   cur_em->em_expr,
												   inner_relids);
			if (process_equivalence(root, newrinfo, true))
				match = true;
		}

		/*
		 * If we were able to equate INNERVAR to any constant, report success.
		 * Otherwise, fall out of the search loop, since we know the OUTERVAR
		 * appears in at most one EC.
		 */
		if (match)
			return true;
		else
			break;
	}

	return false;				/* failed to make any deduction */
}

/*
 * reconsider_outer_join_clauses for a single FULL JOIN clause
 *
 * Returns TRUE if we were able to propagate a constant through the clause.
 */
static bool
reconsider_full_join_clause(PlannerInfo *root, RestrictInfo *rinfo)
{
	Expr	   *leftvar;
	Expr	   *rightvar;
	Oid			opno,
				left_type,
				right_type;
	Relids		left_relids,
				right_relids;
	ListCell   *lc1;

	/* Can't use an outerjoin_delayed clause here */
	if (rinfo->outerjoin_delayed)
		return false;

	/* Extract needed info from the clause */
	Assert(is_opclause(rinfo->clause));
	opno = ((OpExpr *) rinfo->clause)->opno;
	op_input_types(opno, &left_type, &right_type);
	leftvar = (Expr *) get_leftop(rinfo->clause);
	rightvar = (Expr *) get_rightop(rinfo->clause);
	left_relids = rinfo->left_relids;
	right_relids = rinfo->right_relids;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		EquivalenceMember *coal_em = NULL;
		bool		match;
		bool		matchleft;
		bool		matchright;
		ListCell   *lc2;

		/* Ignore EC unless it contains pseudoconstants */
		if (!cur_ec->ec_has_const)
			continue;
		/* Never match to a volatile EC */
		if (cur_ec->ec_has_volatile)
			continue;
		/* It has to match the outer-join clause as to opfamilies, too */
		if (!equal(rinfo->mergeopfamilies, cur_ec->ec_opfamilies))
			continue;

		/*
		 * Does it contain a COALESCE(leftvar, rightvar) construct?
		 *
		 * We can assume the COALESCE() inputs are in the same order as the
		 * join clause, since both were automatically generated in the cases
		 * we care about.
		 *
		 * XXX currently this may fail to match in cross-type cases because
		 * the COALESCE will contain typecast operations while the join clause
		 * may not (if there is a cross-type mergejoin operator available for
		 * the two column types). Is it OK to strip implicit coercions from
		 * the COALESCE arguments?
		 */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			coal_em = (EquivalenceMember *) lfirst(lc2);
			if (IsA(coal_em->em_expr, CoalesceExpr))
			{
				CoalesceExpr *cexpr = (CoalesceExpr *) coal_em->em_expr;
				Node	   *cfirst;
				Node	   *csecond;

				if (list_length(cexpr->args) != 2)
					continue;
				cfirst = (Node *) linitial(cexpr->args);
				csecond = (Node *) lsecond(cexpr->args);

				if (equal(leftvar, cfirst) && equal(rightvar, csecond))
				{
					match = true;
					break;
				}
			}
		}
		if (!match)
			continue;			/* no match, so ignore this EC */

		/*
		 * Yes it does!  Try to generate clauses LEFTVAR = CONSTANT and
		 * RIGHTVAR = CONSTANT for each CONSTANT in the EC.  Note that we must
		 * succeed with at least one constant for each var before we can
		 * decide to throw away the outer-join clause.
		 */
		matchleft = matchright = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);
			Oid			eq_op;
			RestrictInfo *newrinfo;

			if (!cur_em->em_is_const)
				continue;		/* ignore non-const members */
			eq_op = select_equality_operator(cur_ec,
											 left_type,
											 cur_em->em_datatype);
			if (OidIsValid(eq_op))
			{
				newrinfo = build_implied_join_equality(eq_op,
													   leftvar,
													   cur_em->em_expr,
													   left_relids);
				if (process_equivalence(root, newrinfo, true))
					matchleft = true;
			}
			eq_op = select_equality_operator(cur_ec,
											 right_type,
											 cur_em->em_datatype);
			if (OidIsValid(eq_op))
			{
				newrinfo = build_implied_join_equality(eq_op,
													   rightvar,
													   cur_em->em_expr,
													   right_relids);
				if (process_equivalence(root, newrinfo, true))
					matchright = true;
			}
		}

		/*
		 * If we were able to equate both vars to constants, we're done, and
		 * we can throw away the full-join clause as redundant.  Moreover, we
		 * can remove the COALESCE entry from the EC, since the added
		 * restrictions ensure it will always have the expected value. (We
		 * don't bother trying to update ec_relids or ec_sources.)
		 */
		if (matchleft && matchright)
		{
			cur_ec->ec_members = list_delete_ptr(cur_ec->ec_members, coal_em);
			return true;
		}

		/*
		 * Otherwise, fall out of the search loop, since we know the COALESCE
		 * appears in at most one EC (XXX might stop being true if we allow
		 * stripping of coercions above?)
		 */
		break;
	}

	return false;				/* failed to make any deduction */
}


/*
 * exprs_known_equal
 *	  Detect whether two expressions are known equal due to equivalence
 *	  relationships.
 *
 * Actually, this only shows that the expressions are equal according
 * to some opfamily's notion of equality --- but we only use it for
 * selectivity estimation, so a fuzzy idea of equality is OK.
 *
 * Note: does not bother to check for "equal(item1, item2)"; caller must
 * check that case if it's possible to pass identical items.
 */
bool
exprs_known_equal(PlannerInfo *root, Node *item1, Node *item2)
{
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc1);
		bool		item1member = false;
		bool		item2member = false;
		ListCell   *lc2;

		/* Never match to a volatile EC */
		if (ec->ec_has_volatile)
			continue;

		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);

			if (equal(item1, em->em_expr))
				item1member = true;
			else if (equal(item2, em->em_expr))
				item2member = true;
			/* Exit as soon as equality is proven */
			if (item1member && item2member)
				return true;
		}
	}
	return false;
}


/*
 * add_child_rel_equivalences
 *	  Search for EC members that reference (only) the parent_rel, and
 *	  add transformed members referencing the child_rel.
 *
 * We only need to do this for ECs that could generate join conditions,
 * since the child members are only used for creating inner-indexscan paths.
 *
 * parent_rel and child_rel could be derived from appinfo, but since the
 * caller has already computed them, we might as well just pass them in.
 */
void
add_child_rel_equivalences(PlannerInfo *root,
						   AppendRelInfo *appinfo,
						   RelOptInfo *parent_rel,
						   RelOptInfo *child_rel)
{
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/*
		 * Won't generate joinclauses if const or single-member (the latter
		 * test covers the volatile case too)
		 */
		if (cur_ec->ec_has_const || list_length(cur_ec->ec_members) <= 1)
			continue;

		/* No point in searching if parent rel not mentioned in eclass */
		if (!bms_is_subset(parent_rel->relids, cur_ec->ec_relids))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			/* Does it reference (only) parent_rel? */
			if (bms_equal(cur_em->em_relids, parent_rel->relids))
			{
				/* Yes, generate transformed child version */
				Expr	   *child_expr;

				child_expr = (Expr *)
					adjust_appendrel_attrs((Node *) cur_em->em_expr,
										   appinfo);
				(void) add_eq_member(cur_ec, child_expr, child_rel->relids,
									 true, cur_em->em_datatype);
			}
		}
	}
}


/*
 * find_eclass_clauses_for_index_join
 *	  Create joinclauses usable for a nestloop-with-inner-indexscan
 *	  scanning the given inner rel with the specified set of outer rels.
 */
List *
find_eclass_clauses_for_index_join(PlannerInfo *root, RelOptInfo *rel,
								   Relids outer_relids)
{
	List	   *result = NIL;
	bool		is_child_rel = (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/*
		 * Won't generate joinclauses if const or single-member (the latter
		 * test covers the volatile case too)
		 */
		if (cur_ec->ec_has_const || list_length(cur_ec->ec_members) <= 1)
			continue;

		/*
		 * No point in searching if rel not mentioned in eclass (but we can't
		 * tell that for a child rel).
		 */
		if (!is_child_rel &&
			!bms_is_subset(rel->relids, cur_ec->ec_relids))
			continue;
		/* ... nor if no overlap with outer_relids */
		if (!bms_overlap(outer_relids, cur_ec->ec_relids))
			continue;

		/* Scan members, looking for indexable columns */
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);
			EquivalenceMember *best_outer_em = NULL;
			Oid			best_eq_op = InvalidOid;
			ListCell   *lc3;

			if (!bms_equal(cur_em->em_relids, rel->relids) ||
				!eclass_matches_any_index(cur_ec, cur_em, rel))
				continue;

			/*
			 * Found one, so try to generate a join clause.  This is like
			 * generate_join_implied_equalities_normal, except simpler since
			 * our only preference item is to pick a Var on the outer side. We
			 * only need one join clause per index col.
			 */
			foreach(lc3, cur_ec->ec_members)
			{
				EquivalenceMember *outer_em = (EquivalenceMember *) lfirst(lc3);
				Oid			eq_op;

				if (!bms_is_subset(outer_em->em_relids, outer_relids))
					continue;
				eq_op = select_equality_operator(cur_ec,
												 cur_em->em_datatype,
												 outer_em->em_datatype);
				if (!OidIsValid(eq_op))
					continue;
				best_outer_em = outer_em;
				best_eq_op = eq_op;
				if (IsA(outer_em->em_expr, Var) ||
					(IsA(outer_em->em_expr, RelabelType) &&
					 IsA(((RelabelType *) outer_em->em_expr)->arg, Var)))
					break;		/* no need to look further */
			}

			if (best_outer_em)
			{
				/* Found a suitable joinclause */
				RestrictInfo *rinfo;

				/* set parent_ec to mark as redundant with other joinclauses */
				rinfo = create_join_clause(root, cur_ec, best_eq_op,
										   cur_em, best_outer_em,
										   cur_ec);

				result = lappend(result, rinfo);

				/*
				 * Note: we keep scanning here because we want to provide a
				 * clause for every possible indexcol.
				 */
			}
		}
	}

	return result;
}


/*
 * have_relevant_eclass_joinclause
 *		Detect whether there is an EquivalenceClass that could produce
 *		a joinclause between the two given relations.
 *
 * This is essentially a very cut-down version of
 * generate_join_implied_equalities().	Note it's OK to occasionally say "yes"
 * incorrectly.  Hence we don't bother with details like whether the lack of a
 * cross-type operator might prevent the clause from actually being generated.
 */
bool
have_relevant_eclass_joinclause(PlannerInfo *root,
								RelOptInfo *rel1, RelOptInfo *rel2)
{
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc1);
		bool		has_rel1;
		bool		has_rel2;
		ListCell   *lc2;

		/*
		 * Won't generate joinclauses if single-member (this test covers the
		 * volatile case too)
		 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/*
		 * Note we don't test ec_broken; if we did, we'd need a separate code
		 * path to look through ec_sources.  Checking the members anyway is OK
		 * as a possibly-overoptimistic heuristic.
		 *
		 * We don't test ec_has_const either, even though a const eclass
		 * won't generate real join clauses.  This is because if we had
		 * "WHERE a.x = b.y and a.x = 42", it is worth considering a join
		 * between a and b, since the join result is likely to be small even
		 * though it'll end up being an unqualified nestloop.
		 */

		/* Needn't scan if it couldn't contain members from each rel */
		if (!bms_overlap(rel1->relids, ec->ec_relids) ||
			!bms_overlap(rel2->relids, ec->ec_relids))
			continue;

		/* Scan the EC to see if it has member(s) in each rel */
		has_rel1 = has_rel2 = false;
		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			if (cur_em->em_is_const || cur_em->em_is_child)
				continue;		/* ignore consts and children here */
			if (bms_is_subset(cur_em->em_relids, rel1->relids))
			{
				has_rel1 = true;
				if (has_rel2)
					break;
			}
			if (bms_is_subset(cur_em->em_relids, rel2->relids))
			{
				has_rel2 = true;
				if (has_rel1)
					break;
			}
		}

		if (has_rel1 && has_rel2)
			return true;
	}

	return false;
}


/*
 * has_relevant_eclass_joinclause
 *		Detect whether there is an EquivalenceClass that could produce
 *		a joinclause between the given relation and anything else.
 *
 * This is the same as have_relevant_eclass_joinclause with the other rel
 * implicitly defined as "everything else in the query".
 */
bool
has_relevant_eclass_joinclause(PlannerInfo *root, RelOptInfo *rel1)
{
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc1);
		bool		has_rel1;
		bool		has_rel2;
		ListCell   *lc2;

		/*
		 * Won't generate joinclauses if single-member (this test covers the
		 * volatile case too)
		 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/*
		 * Note we don't test ec_broken; if we did, we'd need a separate code
		 * path to look through ec_sources.  Checking the members anyway is OK
		 * as a possibly-overoptimistic heuristic.
		 *
		 * We don't test ec_has_const either, even though a const eclass
		 * won't generate real join clauses.  This is because if we had
		 * "WHERE a.x = b.y and a.x = 42", it is worth considering a join
		 * between a and b, since the join result is likely to be small even
		 * though it'll end up being an unqualified nestloop.
		 */

		/* Needn't scan if it couldn't contain members from each rel */
		if (!bms_overlap(rel1->relids, ec->ec_relids) ||
			bms_is_subset(ec->ec_relids, rel1->relids))
			continue;

		/* Scan the EC to see if it has member(s) in each rel */
		has_rel1 = has_rel2 = false;
		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			if (cur_em->em_is_const || cur_em->em_is_child)
				continue;		/* ignore consts and children here */
			if (bms_is_subset(cur_em->em_relids, rel1->relids))
			{
				has_rel1 = true;
				if (has_rel2)
					break;
			}
			if (!bms_overlap(cur_em->em_relids, rel1->relids))
			{
				has_rel2 = true;
				if (has_rel1)
					break;
			}
		}

		if (has_rel1 && has_rel2)
			return true;
	}

	return false;
}


/*
 * eclass_useful_for_merging
 *	  Detect whether the EC could produce any mergejoinable join clauses
 *	  against the specified relation.
 *
 * This is just a heuristic test and doesn't have to be exact; it's better
 * to say "yes" incorrectly than "no".	Hence we don't bother with details
 * like whether the lack of a cross-type operator might prevent the clause
 * from actually being generated.
 */
bool
eclass_useful_for_merging(EquivalenceClass *eclass,
						  RelOptInfo *rel)
{
	ListCell   *lc;

	Assert(!eclass->ec_merged);

	/*
	 * Won't generate joinclauses if const or single-member (the latter test
	 * covers the volatile case too)
	 */
	if (eclass->ec_has_const || list_length(eclass->ec_members) <= 1)
		return false;

	/*
	 * Note we don't test ec_broken; if we did, we'd need a separate code path
	 * to look through ec_sources.	Checking the members anyway is OK as a
	 * possibly-overoptimistic heuristic.
	 */

	/* If rel already includes all members of eclass, no point in searching */
	if (bms_is_subset(eclass->ec_relids, rel->relids))
		return false;

	/* To join, we need a member not in the given rel */
	foreach(lc, eclass->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (!cur_em->em_is_child &&
			!bms_overlap(cur_em->em_relids, rel->relids))
			return true;
	}

	return false;
}
