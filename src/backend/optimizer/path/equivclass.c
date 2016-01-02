/*-------------------------------------------------------------------------
 *
 * equivclass.c
 *	  Routines for managing EquivalenceClasses
 *
 * See src/backend/optimizer/README for discussion of EquivalenceClasses.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/equivclass.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "utils/lsyscache.h"


static EquivalenceMember *add_eq_member(EquivalenceClass *ec,
			  Expr *expr, Relids relids, Relids nullable_relids,
			  bool is_child, Oid datatype);
static void generate_base_implied_equalities_const(PlannerInfo *root,
									   EquivalenceClass *ec);
static void generate_base_implied_equalities_no_const(PlannerInfo *root,
										  EquivalenceClass *ec);
static void generate_base_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec);
static List *generate_join_implied_equalities_normal(PlannerInfo *root,
										EquivalenceClass *ec,
										Relids join_relids,
										Relids outer_relids,
										Relids inner_relids);
static List *generate_join_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec,
										Relids nominal_join_relids,
										Relids outer_relids,
										Relids nominal_inner_relids,
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
 * strictly equal.  We can still deduce equalities in such cases, but we take
 * care to mark an EquivalenceClass if it came from any such clauses.  Also,
 * we have to check that both sides are either pseudo-constants or strict
 * functions of Vars, else they might not both go to NULL above the outer
 * join.  (This is the reason why we need a failure return.  It's more
 * convenient to check this case here than at the call sites...)
 *
 * On success return, we have also initialized the clause's left_ec/right_ec
 * fields to point to the EquivalenceClass representing it.  This saves lookup
 * effort later.
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
				collation,
				item1_type,
				item2_type;
	Expr	   *item1;
	Expr	   *item2;
	Relids		item1_relids,
				item2_relids,
				item1_nullable_relids,
				item2_nullable_relids;
	List	   *opfamilies;
	EquivalenceClass *ec1,
			   *ec2;
	EquivalenceMember *em1,
			   *em2;
	ListCell   *lc1;

	/* Should not already be marked as having generated an eclass */
	Assert(restrictinfo->left_ec == NULL);
	Assert(restrictinfo->right_ec == NULL);

	/* Extract info from given clause */
	Assert(is_opclause(clause));
	opno = ((OpExpr *) clause)->opno;
	collation = ((OpExpr *) clause)->inputcollid;
	item1 = (Expr *) get_leftop(clause);
	item2 = (Expr *) get_rightop(clause);
	item1_relids = restrictinfo->left_relids;
	item2_relids = restrictinfo->right_relids;

	/*
	 * Ensure both input expressions expose the desired collation (their types
	 * should be OK already); see comments for canonicalize_ec_expression.
	 */
	item1 = canonicalize_ec_expression(item1,
									   exprType((Node *) item1),
									   collation);
	item2 = canonicalize_ec_expression(item2,
									   exprType((Node *) item2),
									   collation);

	/*
	 * Reject clauses of the form X=X.  These are not as redundant as they
	 * might seem at first glance: assuming the operator is strict, this is
	 * really an expensive way to write X IS NOT NULL.  So we must not risk
	 * just losing the clause, which would be possible if there is already a
	 * single-element EquivalenceClass containing X.  The case is not common
	 * enough to be worth contorting the EC machinery for, so just reject the
	 * clause and let it be processed as a normal restriction clause.
	 */
	if (equal(item1, item2))
		return false;			/* X=X is not a useful equivalence */

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

	/* Calculate nullable-relid sets for each side of the clause */
	item1_nullable_relids = bms_intersect(item1_relids,
										  restrictinfo->nullable_relids);
	item2_nullable_relids = bms_intersect(item2_relids,
										  restrictinfo->nullable_relids);

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
	 * 1. We find both in the same EC.  The equivalence is already known, so
	 * there's nothing to do.
	 *
	 * 2. We find both in different ECs.  Merge the two ECs together.
	 *
	 * 3. We find just one.  Add the other to its EC.
	 *
	 * 4. We find neither.  Make a new, two-entry EC.
	 *
	 * Note: since all ECs are built through this process or the similar
	 * search in get_eclass_for_sort_expr(), it's impossible that we'd match
	 * an item in more than one existing nonvolatile EC.  So it's okay to stop
	 * at the first match.
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
		 * The collation has to match; check this first since it's cheaper
		 * than the opfamily comparison.
		 */
		if (collation != cur_ec->ec_collation)
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
			/* mark the RI as associated with this eclass */
			restrictinfo->left_ec = ec1;
			restrictinfo->right_ec = ec1;
			/* mark the RI as usable with this pair of EMs */
			restrictinfo->left_em = em1;
			restrictinfo->right_em = em2;
			return true;
		}

		/*
		 * Case 2: need to merge ec1 and ec2.  This should never happen after
		 * we've built any canonical pathkeys; if it did, those pathkeys might
		 * be rendered non-canonical by the merge.
		 */
		if (root->canon_pathkeys != NIL)
			elog(ERROR, "too late to merge equivalence classes");

		/*
		 * We add ec2's items to ec1, then set ec2's ec_merged link to point
		 * to ec1 and remove ec2 from the eq_classes list.  We cannot simply
		 * delete ec2 because that could leave dangling pointers in existing
		 * PathKeys.  We leave it behind with a link so that the merged EC can
		 * be found.
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
		/* mark the RI as associated with this eclass */
		restrictinfo->left_ec = ec1;
		restrictinfo->right_ec = ec1;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else if (ec1)
	{
		/* Case 3: add item2 to ec1 */
		em2 = add_eq_member(ec1, item2, item2_relids, item2_nullable_relids,
							false, item2_type);
		ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
		ec1->ec_below_outer_join |= below_outer_join;
		/* mark the RI as associated with this eclass */
		restrictinfo->left_ec = ec1;
		restrictinfo->right_ec = ec1;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else if (ec2)
	{
		/* Case 3: add item1 to ec2 */
		em1 = add_eq_member(ec2, item1, item1_relids, item1_nullable_relids,
							false, item1_type);
		ec2->ec_sources = lappend(ec2->ec_sources, restrictinfo);
		ec2->ec_below_outer_join |= below_outer_join;
		/* mark the RI as associated with this eclass */
		restrictinfo->left_ec = ec2;
		restrictinfo->right_ec = ec2;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else
	{
		/* Case 4: make a new, two-entry EC */
		EquivalenceClass *ec = makeNode(EquivalenceClass);

		ec->ec_opfamilies = opfamilies;
		ec->ec_collation = collation;
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
		em1 = add_eq_member(ec, item1, item1_relids, item1_nullable_relids,
							false, item1_type);
		em2 = add_eq_member(ec, item2, item2_relids, item2_nullable_relids,
							false, item2_type);

		root->eq_classes = lappend(root->eq_classes, ec);

		/* mark the RI as associated with this eclass */
		restrictinfo->left_ec = ec;
		restrictinfo->right_ec = ec;
		/* mark the RI as usable with this pair of EMs */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}

	return true;
}

/*
 * canonicalize_ec_expression
 *
 * This function ensures that the expression exposes the expected type and
 * collation, so that it will be equal() to other equivalence-class expressions
 * that it ought to be equal() to.
 *
 * The rule for datatypes is that the exposed type should match what it would
 * be for an input to an operator of the EC's opfamilies; which is usually
 * the declared input type of the operator, but in the case of polymorphic
 * operators no relabeling is wanted (compare the behavior of parse_coerce.c).
 * Expressions coming in from quals will generally have the right type
 * already, but expressions coming from indexkeys may not (because they are
 * represented without any explicit relabel in pg_index), and the same problem
 * occurs for sort expressions (because the parser is likewise cavalier about
 * putting relabels on them).  Such cases will be binary-compatible with the
 * real operators, so adding a RelabelType is sufficient.
 *
 * Also, the expression's exposed collation must match the EC's collation.
 * This is important because in comparisons like "foo < bar COLLATE baz",
 * only one of the expressions has the correct exposed collation as we receive
 * it from the parser.  Forcing both of them to have it ensures that all
 * variant spellings of such a construct behave the same.  Again, we can
 * stick on a RelabelType to force the right exposed collation.  (It might
 * work to not label the collation at all in EC members, but this is risky
 * since some parts of the system expect exprCollation() to deliver the
 * right answer for a sort key.)
 *
 * Note this code assumes that the expression has already been through
 * eval_const_expressions, so there are no CollateExprs and no redundant
 * RelabelTypes.
 */
Expr *
canonicalize_ec_expression(Expr *expr, Oid req_type, Oid req_collation)
{
	Oid			expr_type = exprType((Node *) expr);

	/*
	 * For a polymorphic-input-type opclass, just keep the same exposed type.
	 */
	if (IsPolymorphicType(req_type))
		req_type = expr_type;

	/*
	 * No work if the expression exposes the right type/collation already.
	 */
	if (expr_type != req_type ||
		exprCollation((Node *) expr) != req_collation)
	{
		/*
		 * Strip any existing RelabelType, then add a new one if needed. This
		 * is to preserve the invariant of no redundant RelabelTypes.
		 *
		 * If we have to change the exposed type of the stripped expression,
		 * set typmod to -1 (since the new type may not have the same typmod
		 * interpretation).  If we only have to change collation, preserve the
		 * exposed typmod.
		 */
		while (expr && IsA(expr, RelabelType))
			expr = (Expr *) ((RelabelType *) expr)->arg;

		if (exprType((Node *) expr) != req_type)
			expr = (Expr *) makeRelabelType(expr,
											req_type,
											-1,
											req_collation,
											COERCE_IMPLICIT_CAST);
		else if (exprCollation((Node *) expr) != req_collation)
			expr = (Expr *) makeRelabelType(expr,
											req_type,
											exprTypmod((Node *) expr),
											req_collation,
											COERCE_IMPLICIT_CAST);
	}

	return expr;
}

/*
 * add_eq_member - build a new EquivalenceMember and add it to an EC
 */
static EquivalenceMember *
add_eq_member(EquivalenceClass *ec, Expr *expr, Relids relids,
			  Relids nullable_relids, bool is_child, Oid datatype)
{
	EquivalenceMember *em = makeNode(EquivalenceMember);

	em->em_expr = expr;
	em->em_relids = relids;
	em->em_nullable_relids = nullable_relids;
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
 *	  Given an expression and opfamily/collation info, find an existing
 *	  equivalence class it is a member of; if none, optionally build a new
 *	  single-member EquivalenceClass for it.
 *
 * expr is the expression, and nullable_relids is the set of base relids
 * that are potentially nullable below it.  We actually only care about
 * the set of such relids that are used in the expression; but for caller
 * convenience, we perform that intersection step here.  The caller need
 * only be sure that nullable_relids doesn't omit any nullable rels that
 * might appear in the expr.
 *
 * sortref is the SortGroupRef of the originating SortGroupClause, if any,
 * or zero if not.  (It should never be zero if the expression is volatile!)
 *
 * If rel is not NULL, it identifies a specific relation we're considering
 * a path for, and indicates that child EC members for that relation can be
 * considered.  Otherwise child members are ignored.  (Note: since child EC
 * members aren't guaranteed unique, a non-NULL value means that there could
 * be more than one EC that matches the expression; if so it's order-dependent
 * which one you get.  This is annoying but it only happens in corner cases,
 * so for now we live with just reporting the first match.  See also
 * generate_implied_equalities_for_column and match_pathkeys_to_index.)
 *
 * If create_it is TRUE, we'll build a new EquivalenceClass when there is no
 * match.  If create_it is FALSE, we just return NULL when no match.
 *
 * This can be used safely both before and after EquivalenceClass merging;
 * since it never causes merging it does not invalidate any existing ECs
 * or PathKeys.  However, ECs added after path generation has begun are
 * of limited usefulness, so usually it's best to create them beforehand.
 *
 * Note: opfamilies must be chosen consistently with the way
 * process_equivalence() would do; that is, generated from a mergejoinable
 * equality operator.  Else we might fail to detect valid equivalences,
 * generating poor (but not incorrect) plans.
 */
EquivalenceClass *
get_eclass_for_sort_expr(PlannerInfo *root,
						 Expr *expr,
						 Relids nullable_relids,
						 List *opfamilies,
						 Oid opcintype,
						 Oid collation,
						 Index sortref,
						 Relids rel,
						 bool create_it)
{
	Relids		expr_relids;
	EquivalenceClass *newec;
	EquivalenceMember *newem;
	ListCell   *lc1;
	MemoryContext oldcontext;

	/*
	 * Ensure the expression exposes the correct type and collation.
	 */
	expr = canonicalize_ec_expression(expr, opcintype, collation);

	/*
	 * Get the precise set of nullable relids appearing in the expression.
	 */
	expr_relids = pull_varnos((Node *) expr);
	nullable_relids = bms_intersect(nullable_relids, expr_relids);

	/*
	 * Scan through the existing EquivalenceClasses for a match
	 */
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/*
		 * Never match to a volatile EC, except when we are looking at another
		 * reference to the same volatile SortGroupClause.
		 */
		if (cur_ec->ec_has_volatile &&
			(sortref == 0 || sortref != cur_ec->ec_sortref))
			continue;

		if (collation != cur_ec->ec_collation)
			continue;
		if (!equal(opfamilies, cur_ec->ec_opfamilies))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			/*
			 * Ignore child members unless they match the request.
			 */
			if (cur_em->em_is_child &&
				!bms_equal(cur_em->em_relids, rel))
				continue;

			/*
			 * If below an outer join, don't match constants: they're not as
			 * constant as they look.
			 */
			if (cur_ec->ec_below_outer_join &&
				cur_em->em_is_const)
				continue;

			if (opcintype == cur_em->em_datatype &&
				equal(expr, cur_em->em_expr))
				return cur_ec;	/* Match! */
		}
	}

	/* No match; does caller want a NULL result? */
	if (!create_it)
		return NULL;

	/*
	 * OK, build a new single-member EC
	 *
	 * Here, we must be sure that we construct the EC in the right context.
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	newec = makeNode(EquivalenceClass);
	newec->ec_opfamilies = list_copy(opfamilies);
	newec->ec_collation = collation;
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

	if (newec->ec_has_volatile && sortref == 0) /* should not happen */
		elog(ERROR, "volatile EquivalenceClass has no sortref");

	newem = add_eq_member(newec, copyObject(expr), expr_relids,
						  nullable_relids, false, opcintype);

	/*
	 * add_eq_member doesn't check for volatile functions, set-returning
	 * functions, aggregates, or window functions, but such could appear in
	 * sort expressions; so we have to check whether its const-marking was
	 * correct.
	 */
	if (newec->ec_has_const)
	{
		if (newec->ec_has_volatile ||
			expression_returns_set((Node *) expr) ||
			contain_agg_clause((Node *) expr) ||
			contain_window_function((Node *) expr))
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
 * every other member (including other constants).  If we are able to do this
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
 * RestrictInfos at appropriate times.  We do not try to retract any derived
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
	 * In the trivial case where we just had one "var = const" clause, push
	 * the original clause back into the main planner machinery.  There is
	 * nothing to be gained by doing it differently, and we save the effort to
	 * re-build and re-analyze an equality clause that will be exactly
	 * equivalent to the old one.
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

	/*
	 * Find the constant member to use.  We prefer an actual constant to
	 * pseudo-constants (such as Params), because the constraint exclusion
	 * machinery might be able to exclude relations on the basis of generated
	 * "var = const" equalities, but "var = param" won't work for that.
	 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (cur_em->em_is_const)
		{
			const_em = cur_em;
			if (IsA(cur_em->em_expr, Const))
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
		process_implied_equality(root, eq_op, ec->ec_collation,
								 cur_em->em_expr, const_em->em_expr,
								 bms_copy(ec->ec_relids),
								 bms_union(cur_em->em_nullable_relids,
										   const_em->em_nullable_relids),
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
		if (!bms_get_singleton_member(cur_em->em_relids, &relid))
			continue;
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
			process_implied_equality(root, eq_op, ec->ec_collation,
									 prev_em->em_expr, cur_em->em_expr,
									 bms_copy(ec->ec_relids),
									 bms_union(prev_em->em_nullable_relids,
											   cur_em->em_nullable_relids),
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
		List	   *vars = pull_var_clause((Node *) cur_em->em_expr,
										   PVC_RECURSE_AGGREGATES,
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, ec->ec_relids, false);
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
 * becomes set only after we've gone up a join level or two.)  However, for
 * an EC that contains constants, we can adopt a simpler strategy and just
 * throw back all the source RestrictInfos immediately; that works because
 * we know that such an EC can't become broken later.  (This rule justifies
 * ignoring ec_has_const ECs in generate_join_implied_equalities, even when
 * they are broken.)
 */
static void
generate_base_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec)
{
	ListCell   *lc;

	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		if (ec->ec_has_const ||
			bms_membership(restrictinfo->required_relids) != BMS_MULTIPLE)
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
 * relation pair.  Hence a fresh List of RestrictInfo nodes is built and
 * passed back on each call.
 *
 * In addition to its use at join nodes, this can be applied to generate
 * eclass-based join clauses for use in a parameterized scan of a base rel.
 * The reason for the asymmetry of specifying the inner rel as a RelOptInfo
 * and the outer rel by Relids is that this usage occurs before we have
 * built any join RelOptInfos.
 *
 * An annoying special case for parameterized scans is that the inner rel can
 * be an appendrel child (an "other rel").  In this case we must generate
 * appropriate clauses using child EC members.  add_child_rel_equivalences
 * must already have been done for the child rel.
 *
 * The results are sufficient for use in merge, hash, and plain nestloop join
 * methods.  We do not worry here about selecting clauses that are optimal
 * for use in a parameterized indexscan.  indxpath.c makes its own selections
 * of clauses to use, and if the ones we pick here are redundant with those,
 * the extras will be eliminated at createplan time, using the parent_ec
 * markers that we provide (see is_redundant_derived_clause()).
 *
 * Because the same join clauses are likely to be needed multiple times as
 * we consider different join paths, we avoid generating multiple copies:
 * whenever we select a particular pair of EquivalenceMembers to join,
 * we check to see if the pair matches any original clause (in ec_sources)
 * or previously-built clause (in ec_derives).  This saves memory and allows
 * re-use of information cached in RestrictInfos.
 *
 * join_relids should always equal bms_union(outer_relids, inner_rel->relids).
 * We could simplify this function's API by computing it internally, but in
 * all current uses, the caller has the value at hand anyway.
 */
List *
generate_join_implied_equalities(PlannerInfo *root,
								 Relids join_relids,
								 Relids outer_relids,
								 RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	Relids		inner_relids = inner_rel->relids;
	Relids		nominal_inner_relids;
	Relids		nominal_join_relids;
	ListCell   *lc;

	/* If inner rel is a child, extra setup work is needed */
	if (inner_rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
	{
		/* Fetch relid set for the topmost parent rel */
		nominal_inner_relids = find_childrel_top_parent(root, inner_rel)->relids;
		/* ECs will be marked with the parent's relid, not the child's */
		nominal_join_relids = bms_union(outer_relids, nominal_inner_relids);
	}
	else
	{
		nominal_inner_relids = inner_relids;
		nominal_join_relids = join_relids;
	}

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
		if (!bms_overlap(ec->ec_relids, nominal_join_relids))
			continue;

		if (!ec->ec_broken)
			sublist = generate_join_implied_equalities_normal(root,
															  ec,
															  join_relids,
															  outer_relids,
															  inner_relids);

		/* Recover if we failed to generate required derived clauses */
		if (ec->ec_broken)
			sublist = generate_join_implied_equalities_broken(root,
															  ec,
														 nominal_join_relids,
															  outer_relids,
														nominal_inner_relids,
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
										Relids join_relids,
										Relids outer_relids,
										Relids inner_relids)
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
	 * likewise for the inner-rel members.  We'll need to create clauses to
	 * enforce that any newly computable members are all equal to each other
	 * as well as to at least one input member, plus enforce at least one
	 * outer-rel member equal to at least one inner-rel member.
	 */
	foreach(lc1, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc1);

		/*
		 * We don't need to check explicitly for child EC members.  This test
		 * against join_relids will cause them to be ignored except when
		 * considering a child inner rel, which is what we want.
		 */
		if (!bms_is_subset(cur_em->em_relids, join_relids))
			continue;			/* not computable yet, or wrong child */

		if (bms_is_subset(cur_em->em_relids, outer_relids))
			outer_members = lappend(outer_members, cur_em);
		else if (bms_is_subset(cur_em->em_relids, inner_relids))
			inner_members = lappend(inner_members, cur_em);
		else
			new_members = lappend(new_members, cur_em);
	}

	/*
	 * First, select the joinclause if needed.  We can equate any one outer
	 * member to any one inner member, but we have to find a datatype
	 * combination for which an opfamily member operator exists.  If we have
	 * choices, we prefer simple Var members (possibly with RelabelType) since
	 * these are (a) cheapest to compute at runtime and (b) most likely to
	 * have useful statistics. Also, prefer operators that are also
	 * hashjoinable.
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
				if (op_hashjoinable(eq_op,
									exprType((Node *) outer_em->em_expr)))
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
 *
 * In the case of a child inner relation, we have to translate the
 * original RestrictInfos from parent to child Vars.
 */
static List *
generate_join_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec,
										Relids nominal_join_relids,
										Relids outer_relids,
										Relids nominal_inner_relids,
										RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);
		Relids		clause_relids = restrictinfo->required_relids;

		if (bms_is_subset(clause_relids, nominal_join_relids) &&
			!bms_is_subset(clause_relids, outer_relids) &&
			!bms_is_subset(clause_relids, nominal_inner_relids))
			result = lappend(result, restrictinfo);
	}

	/*
	 * If we have to translate, just brute-force apply adjust_appendrel_attrs
	 * to all the RestrictInfos at once.  This will result in returning
	 * RestrictInfos that are not listed in ec_derives, but there shouldn't be
	 * any duplication, and it's a sufficiently narrow corner case that we
	 * shouldn't sweat too much over it anyway.
	 *
	 * Since inner_rel might be an indirect descendant of the baserel
	 * mentioned in the ec_sources clauses, we have to be prepared to apply
	 * multiple levels of Var translation.
	 */
	if (inner_rel->reloptkind == RELOPT_OTHER_MEMBER_REL &&
		result != NIL)
		result = (List *) adjust_appendrel_attrs_multilevel(root,
															(Node *) result,
															inner_rel);

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
	 * EquivalenceMembers.  We can use either original source clauses or
	 * previously-derived clauses.  The check on opno is probably redundant,
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
										ec->ec_collation,
										leftem->em_expr,
										rightem->em_expr,
										bms_union(leftem->em_relids,
												  rightem->em_relids),
										bms_union(leftem->em_nullable_relids,
											   rightem->em_nullable_relids));

	/* Mark the clause as redundant, or not */
	rinfo->parent_ec = parent_ec;

	/*
	 * We know the correct values for left_ec/right_ec, ie this particular EC,
	 * so we can just set them directly instead of forcing another lookup.
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
				rinfo->norm_selec = 2.0;
				rinfo->outer_selec = 1.0;
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
				rinfo->norm_selec = 2.0;
				rinfo->outer_selec = 1.0;
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
				rinfo->norm_selec = 2.0;
				rinfo->outer_selec = 1.0;
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
				collation,
				left_type,
				right_type,
				inner_datatype;
	Relids		inner_relids,
				inner_nullable_relids;
	ListCell   *lc1;

	Assert(is_opclause(rinfo->clause));
	opno = ((OpExpr *) rinfo->clause)->opno;
	collation = ((OpExpr *) rinfo->clause)->inputcollid;

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
	inner_nullable_relids = bms_intersect(inner_relids,
										  rinfo->nullable_relids);

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
		/* It has to match the outer-join clause as to semantics, too */
		if (collation != cur_ec->ec_collation)
			continue;
		if (!equal(rinfo->mergeopfamilies, cur_ec->ec_opfamilies))
			continue;
		/* Does it contain a match to outervar? */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			Assert(!cur_em->em_is_child);		/* no children yet */
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
		 * CONSTANT in the EC.  Note that we must succeed with at least one
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
												   cur_ec->ec_collation,
												   innervar,
												   cur_em->em_expr,
												   bms_copy(inner_relids),
											bms_copy(inner_nullable_relids));
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
				collation,
				left_type,
				right_type;
	Relids		left_relids,
				right_relids,
				left_nullable_relids,
				right_nullable_relids;
	ListCell   *lc1;

	/* Can't use an outerjoin_delayed clause here */
	if (rinfo->outerjoin_delayed)
		return false;

	/* Extract needed info from the clause */
	Assert(is_opclause(rinfo->clause));
	opno = ((OpExpr *) rinfo->clause)->opno;
	collation = ((OpExpr *) rinfo->clause)->inputcollid;
	op_input_types(opno, &left_type, &right_type);
	leftvar = (Expr *) get_leftop(rinfo->clause);
	rightvar = (Expr *) get_rightop(rinfo->clause);
	left_relids = rinfo->left_relids;
	right_relids = rinfo->right_relids;
	left_nullable_relids = bms_intersect(left_relids,
										 rinfo->nullable_relids);
	right_nullable_relids = bms_intersect(right_relids,
										  rinfo->nullable_relids);

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
		/* It has to match the outer-join clause as to semantics, too */
		if (collation != cur_ec->ec_collation)
			continue;
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
			Assert(!coal_em->em_is_child);		/* no children yet */
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
													   cur_ec->ec_collation,
													   leftvar,
													   cur_em->em_expr,
													   bms_copy(left_relids),
											 bms_copy(left_nullable_relids));
				if (process_equivalence(root, newrinfo, true))
					matchleft = true;
			}
			eq_op = select_equality_operator(cur_ec,
											 right_type,
											 cur_em->em_datatype);
			if (OidIsValid(eq_op))
			{
				newrinfo = build_implied_join_equality(eq_op,
													   cur_ec->ec_collation,
													   rightvar,
													   cur_em->em_expr,
													   bms_copy(right_relids),
											bms_copy(right_nullable_relids));
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

			if (em->em_is_child)
				continue;		/* ignore children here */
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
 *	  Search for EC members that reference the parent_rel, and
 *	  add transformed members referencing the child_rel.
 *
 * Note that this function won't be called at all unless we have at least some
 * reason to believe that the EC members it generates will be useful.
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
		 * If this EC contains a volatile expression, then generating child
		 * EMs would be downright dangerous, so skip it.  We rely on a
		 * volatile EC having only one EM.
		 */
		if (cur_ec->ec_has_volatile)
			continue;

		/*
		 * No point in searching if parent rel not mentioned in eclass; but we
		 * can't tell that for sure if parent rel is itself a child.
		 */
		if (parent_rel->reloptkind == RELOPT_BASEREL &&
			!bms_is_subset(parent_rel->relids, cur_ec->ec_relids))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			if (cur_em->em_is_const)
				continue;		/* ignore consts here */

			/* Does it reference parent_rel? */
			if (bms_overlap(cur_em->em_relids, parent_rel->relids))
			{
				/* Yes, generate transformed child version */
				Expr	   *child_expr;
				Relids		new_relids;
				Relids		new_nullable_relids;

				child_expr = (Expr *)
					adjust_appendrel_attrs(root,
										   (Node *) cur_em->em_expr,
										   appinfo);

				/*
				 * Transform em_relids to match.  Note we do *not* do
				 * pull_varnos(child_expr) here, as for example the
				 * transformation might have substituted a constant, but we
				 * don't want the child member to be marked as constant.
				 */
				new_relids = bms_difference(cur_em->em_relids,
											parent_rel->relids);
				new_relids = bms_add_members(new_relids, child_rel->relids);

				/*
				 * And likewise for nullable_relids.  Note this code assumes
				 * parent and child relids are singletons.
				 */
				new_nullable_relids = cur_em->em_nullable_relids;
				if (bms_overlap(new_nullable_relids, parent_rel->relids))
				{
					new_nullable_relids = bms_difference(new_nullable_relids,
														 parent_rel->relids);
					new_nullable_relids = bms_add_members(new_nullable_relids,
														  child_rel->relids);
				}

				(void) add_eq_member(cur_ec, child_expr,
									 new_relids, new_nullable_relids,
									 true, cur_em->em_datatype);
			}
		}
	}
}


/*
 * mutate_eclass_expressions
 *	  Apply an expression tree mutator to all expressions stored in
 *	  equivalence classes (but ignore child exprs unless include_child_exprs).
 *
 * This is a bit of a hack ... it's currently needed only by planagg.c,
 * which needs to do a global search-and-replace of MIN/MAX Aggrefs
 * after eclasses are already set up.  Without changing the eclasses too,
 * subsequent matching of ORDER BY and DISTINCT clauses would fail.
 *
 * Note that we assume the mutation won't affect relation membership or any
 * other properties we keep track of (which is a bit bogus, but by the time
 * planagg.c runs, it no longer matters).  Also we must be called in the
 * main planner memory context.
 */
void
mutate_eclass_expressions(PlannerInfo *root,
						  Node *(*mutator) (),
						  void *context,
						  bool include_child_exprs)
{
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			if (cur_em->em_is_child && !include_child_exprs)
				continue;		/* ignore children unless requested */

			cur_em->em_expr = (Expr *)
				mutator((Node *) cur_em->em_expr, context);
		}
	}
}


/*
 * generate_implied_equalities_for_column
 *	  Create EC-derived joinclauses usable with a specific column.
 *
 * This is used by indxpath.c to extract potentially indexable joinclauses
 * from ECs, and can be used by foreign data wrappers for similar purposes.
 * We assume that only expressions in Vars of a single table are of interest,
 * but the caller provides a callback function to identify exactly which
 * such expressions it would like to know about.
 *
 * We assume that any given table/index column could appear in only one EC.
 * (This should be true in all but the most pathological cases, and if it
 * isn't, we stop on the first match anyway.)  Therefore, what we return
 * is a redundant list of clauses equating the table/index column to each of
 * the other-relation values it is known to be equal to.  Any one of
 * these clauses can be used to create a parameterized path, and there
 * is no value in using more than one.  (But it *is* worthwhile to create
 * a separate parameterized path for each one, since that leads to different
 * join orders.)
 *
 * The caller can pass a Relids set of rels we aren't interested in joining
 * to, so as to save the work of creating useless clauses.
 */
List *
generate_implied_equalities_for_column(PlannerInfo *root,
									   RelOptInfo *rel,
									   ec_matches_callback_type callback,
									   void *callback_arg,
									   Relids prohibited_rels)
{
	List	   *result = NIL;
	bool		is_child_rel = (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);
	Relids		parent_relids;
	ListCell   *lc1;

	/* If it's a child rel, we'll need to know what its parent(s) are */
	if (is_child_rel)
		parent_relids = find_childrel_parents(root, rel);
	else
		parent_relids = NULL;	/* not used, but keep compiler quiet */

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		EquivalenceMember *cur_em;
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

		/*
		 * Scan members, looking for a match to the target column.  Note that
		 * child EC members are considered, but only when they belong to the
		 * target relation.  (Unlike regular members, the same expression
		 * could be a child member of more than one EC.  Therefore, it's
		 * potentially order-dependent which EC a child relation's target
		 * column gets matched to.  This is annoying but it only happens in
		 * corner cases, so for now we live with just reporting the first
		 * match.  See also get_eclass_for_sort_expr.)
		 */
		cur_em = NULL;
		foreach(lc2, cur_ec->ec_members)
		{
			cur_em = (EquivalenceMember *) lfirst(lc2);
			if (bms_equal(cur_em->em_relids, rel->relids) &&
				callback(root, rel, cur_ec, cur_em, callback_arg))
				break;
			cur_em = NULL;
		}

		if (!cur_em)
			continue;

		/*
		 * Found our match.  Scan the other EC members and attempt to generate
		 * joinclauses.
		 */
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *other_em = (EquivalenceMember *) lfirst(lc2);
			Oid			eq_op;
			RestrictInfo *rinfo;

			if (other_em->em_is_child)
				continue;		/* ignore children here */

			/* Make sure it'll be a join to a different rel */
			if (other_em == cur_em ||
				bms_overlap(other_em->em_relids, rel->relids))
				continue;

			/* Forget it if caller doesn't want joins to this rel */
			if (bms_overlap(other_em->em_relids, prohibited_rels))
				continue;

			/*
			 * Also, if this is a child rel, avoid generating a useless join
			 * to its parent rel(s).
			 */
			if (is_child_rel &&
				bms_overlap(parent_relids, other_em->em_relids))
				continue;

			eq_op = select_equality_operator(cur_ec,
											 cur_em->em_datatype,
											 other_em->em_datatype);
			if (!OidIsValid(eq_op))
				continue;

			/* set parent_ec to mark as redundant with other joinclauses */
			rinfo = create_join_clause(root, cur_ec, eq_op,
									   cur_em, other_em,
									   cur_ec);

			result = lappend(result, rinfo);
		}

		/*
		 * If somehow we failed to create any join clauses, we might as well
		 * keep scanning the ECs for another match.  But if we did make any,
		 * we're done, because we don't want to return non-redundant clauses.
		 */
		if (result)
			break;
	}

	return result;
}

/*
 * have_relevant_eclass_joinclause
 *		Detect whether there is an EquivalenceClass that could produce
 *		a joinclause involving the two given relations.
 *
 * This is essentially a very cut-down version of
 * generate_join_implied_equalities().  Note it's OK to occasionally say "yes"
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

		/*
		 * Won't generate joinclauses if single-member (this test covers the
		 * volatile case too)
		 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/*
		 * We do not need to examine the individual members of the EC, because
		 * all that we care about is whether each rel overlaps the relids of
		 * at least one member, and a test on ec_relids is sufficient to prove
		 * that.  (As with have_relevant_joinclause(), it is not necessary
		 * that the EC be able to form a joinclause relating exactly the two
		 * given rels, only that it be able to form a joinclause mentioning
		 * both, and this will surely be true if both of them overlap
		 * ec_relids.)
		 *
		 * Note we don't test ec_broken; if we did, we'd need a separate code
		 * path to look through ec_sources.  Checking the membership anyway is
		 * OK as a possibly-overoptimistic heuristic.
		 *
		 * We don't test ec_has_const either, even though a const eclass won't
		 * generate real join clauses.  This is because if we had "WHERE a.x =
		 * b.y and a.x = 42", it is worth considering a join between a and b,
		 * since the join result is likely to be small even though it'll end
		 * up being an unqualified nestloop.
		 */
		if (bms_overlap(rel1->relids, ec->ec_relids) &&
			bms_overlap(rel2->relids, ec->ec_relids))
			return true;
	}

	return false;
}


/*
 * has_relevant_eclass_joinclause
 *		Detect whether there is an EquivalenceClass that could produce
 *		a joinclause involving the given relation and anything else.
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

		/*
		 * Won't generate joinclauses if single-member (this test covers the
		 * volatile case too)
		 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/*
		 * Per the comment in have_relevant_eclass_joinclause, it's sufficient
		 * to find an EC that mentions both this rel and some other rel.
		 */
		if (bms_overlap(rel1->relids, ec->ec_relids) &&
			!bms_is_subset(ec->ec_relids, rel1->relids))
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
 * to say "yes" incorrectly than "no".  Hence we don't bother with details
 * like whether the lack of a cross-type operator might prevent the clause
 * from actually being generated.
 */
bool
eclass_useful_for_merging(PlannerInfo *root,
						  EquivalenceClass *eclass,
						  RelOptInfo *rel)
{
	Relids		relids;
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
	 * to look through ec_sources.  Checking the members anyway is OK as a
	 * possibly-overoptimistic heuristic.
	 */

	/* If specified rel is a child, we must consider the topmost parent rel */
	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		relids = find_childrel_top_parent(root, rel)->relids;
	else
		relids = rel->relids;

	/* If rel already includes all members of eclass, no point in searching */
	if (bms_is_subset(eclass->ec_relids, relids))
		return false;

	/* To join, we need a member not in the given rel */
	foreach(lc, eclass->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (cur_em->em_is_child)
			continue;			/* ignore children here */

		if (!bms_overlap(cur_em->em_relids, relids))
			return true;
	}

	return false;
}


/*
 * is_redundant_derived_clause
 *		Test whether rinfo is derived from same EC as any clause in clauselist;
 *		if so, it can be presumed to represent a condition that's redundant
 *		with that member of the list.
 */
bool
is_redundant_derived_clause(RestrictInfo *rinfo, List *clauselist)
{
	EquivalenceClass *parent_ec = rinfo->parent_ec;
	ListCell   *lc;

	/* Fail if it's not a potentially-redundant clause from some EC */
	if (parent_ec == NULL)
		return false;

	foreach(lc, clauselist)
	{
		RestrictInfo *otherrinfo = (RestrictInfo *) lfirst(lc);

		if (otherrinfo->parent_ec == parent_ec)
			return true;
	}

	return false;
}
