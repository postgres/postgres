/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/restrictinfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"


static Expr *make_sub_restrictinfos(PlannerInfo *root,
									Expr *clause,
									bool is_pushed_down,
									bool has_clone,
									bool is_clone,
									bool pseudoconstant,
									Index security_level,
									Relids required_relids,
									Relids incompatible_relids,
									Relids outer_relids);


/*
 * make_restrictinfo
 *
 * Build a RestrictInfo node containing the given subexpression.
 *
 * The is_pushed_down, has_clone, is_clone, and pseudoconstant flags for the
 * RestrictInfo must be supplied by the caller, as well as the correct values
 * for security_level, incompatible_relids, and outer_relids.
 * required_relids can be NULL, in which case it defaults to the actual clause
 * contents (i.e., clause_relids).
 *
 * We initialize fields that depend only on the given subexpression, leaving
 * others that depend on context (or may never be needed at all) to be filled
 * later.
 */
RestrictInfo *
make_restrictinfo(PlannerInfo *root,
				  Expr *clause,
				  bool is_pushed_down,
				  bool has_clone,
				  bool is_clone,
				  bool pseudoconstant,
				  Index security_level,
				  Relids required_relids,
				  Relids incompatible_relids,
				  Relids outer_relids)
{
	/*
	 * If it's an OR clause, build a modified copy with RestrictInfos inserted
	 * above each subclause of the top-level AND/OR structure.
	 */
	if (is_orclause(clause))
		return (RestrictInfo *) make_sub_restrictinfos(root,
													   clause,
													   is_pushed_down,
													   has_clone,
													   is_clone,
													   pseudoconstant,
													   security_level,
													   required_relids,
													   incompatible_relids,
													   outer_relids);

	/* Shouldn't be an AND clause, else AND/OR flattening messed up */
	Assert(!is_andclause(clause));

	return make_plain_restrictinfo(root,
								   clause,
								   NULL,
								   is_pushed_down,
								   has_clone,
								   is_clone,
								   pseudoconstant,
								   security_level,
								   required_relids,
								   incompatible_relids,
								   outer_relids);
}

/*
 * make_plain_restrictinfo
 *
 * Common code for the main entry points and the recursive cases.  Also,
 * useful while contrucitng RestrictInfos above OR clause, which already has
 * RestrictInfos above its subclauses.
 */
RestrictInfo *
make_plain_restrictinfo(PlannerInfo *root,
						Expr *clause,
						Expr *orclause,
						bool is_pushed_down,
						bool has_clone,
						bool is_clone,
						bool pseudoconstant,
						Index security_level,
						Relids required_relids,
						Relids incompatible_relids,
						Relids outer_relids)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		baserels;

	restrictinfo->clause = clause;
	restrictinfo->orclause = orclause;
	restrictinfo->is_pushed_down = is_pushed_down;
	restrictinfo->pseudoconstant = pseudoconstant;
	restrictinfo->has_clone = has_clone;
	restrictinfo->is_clone = is_clone;
	restrictinfo->can_join = false; /* may get set below */
	restrictinfo->security_level = security_level;
	restrictinfo->incompatible_relids = incompatible_relids;
	restrictinfo->outer_relids = outer_relids;

	/*
	 * If it's potentially delayable by lower-level security quals, figure out
	 * whether it's leakproof.  We can skip testing this for level-zero quals,
	 * since they would never get delayed on security grounds anyway.
	 */
	if (security_level > 0)
		restrictinfo->leakproof = !contain_leaked_vars((Node *) clause);
	else
		restrictinfo->leakproof = false;	/* really, "don't know" */

	/*
	 * Mark volatility as unknown.  The contain_volatile_functions function
	 * will determine if there are any volatile functions when called for the
	 * first time with this RestrictInfo.
	 */
	restrictinfo->has_volatile = VOLATILITY_UNKNOWN;

	/*
	 * If it's a binary opclause, set up left/right relids info. In any case
	 * set up the total clause relids info.
	 */
	if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
	{
		restrictinfo->left_relids = pull_varnos(root, get_leftop(clause));
		restrictinfo->right_relids = pull_varnos(root, get_rightop(clause));

		restrictinfo->clause_relids = bms_union(restrictinfo->left_relids,
												restrictinfo->right_relids);

		/*
		 * Does it look like a normal join clause, i.e., a binary operator
		 * relating expressions that come from distinct relations? If so we
		 * might be able to use it in a join algorithm.  Note that this is a
		 * purely syntactic test that is made regardless of context.
		 */
		if (!bms_is_empty(restrictinfo->left_relids) &&
			!bms_is_empty(restrictinfo->right_relids) &&
			!bms_overlap(restrictinfo->left_relids,
						 restrictinfo->right_relids))
		{
			restrictinfo->can_join = true;
			/* pseudoconstant should certainly not be true */
			Assert(!restrictinfo->pseudoconstant);
		}
	}
	else
	{
		/* Not a binary opclause, so mark left/right relid sets as empty */
		restrictinfo->left_relids = NULL;
		restrictinfo->right_relids = NULL;
		/* and get the total relid set the hard way */
		restrictinfo->clause_relids = pull_varnos(root, (Node *) clause);
	}

	/* required_relids defaults to clause_relids */
	if (required_relids != NULL)
		restrictinfo->required_relids = required_relids;
	else
		restrictinfo->required_relids = restrictinfo->clause_relids;

	/*
	 * Count the number of base rels appearing in clause_relids.  To do this,
	 * we just delete rels mentioned in root->outer_join_rels and count the
	 * survivors.  Because we are called during deconstruct_jointree which is
	 * the same tree walk that populates outer_join_rels, this is a little bit
	 * unsafe-looking; but it should be fine because the recursion in
	 * deconstruct_jointree should already have visited any outer join that
	 * could be mentioned in this clause.
	 */
	baserels = bms_difference(restrictinfo->clause_relids,
							  root->outer_join_rels);
	restrictinfo->num_base_rels = bms_num_members(baserels);
	bms_free(baserels);

	/*
	 * Label this RestrictInfo with a fresh serial number.
	 */
	restrictinfo->rinfo_serial = ++(root->last_rinfo_serial);

	/*
	 * Fill in all the cacheable fields with "not yet set" markers. None of
	 * these will be computed until/unless needed.  Note in particular that we
	 * don't mark a binary opclause as mergejoinable or hashjoinable here;
	 * that happens only if it appears in the right context (top level of a
	 * joinclause list).
	 */
	restrictinfo->parent_ec = NULL;

	restrictinfo->eval_cost.startup = -1;
	restrictinfo->norm_selec = -1;
	restrictinfo->outer_selec = -1;

	restrictinfo->mergeopfamilies = NIL;

	restrictinfo->left_ec = NULL;
	restrictinfo->right_ec = NULL;
	restrictinfo->left_em = NULL;
	restrictinfo->right_em = NULL;
	restrictinfo->scansel_cache = NIL;

	restrictinfo->outer_is_left = false;

	restrictinfo->hashjoinoperator = InvalidOid;

	restrictinfo->left_bucketsize = -1;
	restrictinfo->right_bucketsize = -1;
	restrictinfo->left_mcvfreq = -1;
	restrictinfo->right_mcvfreq = -1;

	restrictinfo->left_hasheqoperator = InvalidOid;
	restrictinfo->right_hasheqoperator = InvalidOid;

	return restrictinfo;
}

/*
 * Recursively insert sub-RestrictInfo nodes into a boolean expression.
 *
 * We put RestrictInfos above simple (non-AND/OR) clauses and above
 * sub-OR clauses, but not above sub-AND clauses, because there's no need.
 * This may seem odd but it is closely related to the fact that we use
 * implicit-AND lists at top level of RestrictInfo lists.  Only ORs and
 * simple clauses are valid RestrictInfos.
 *
 * The same is_pushed_down, has_clone, is_clone, and pseudoconstant flag
 * values can be applied to all RestrictInfo nodes in the result.  Likewise
 * for security_level, incompatible_relids, and outer_relids.
 *
 * The given required_relids are attached to our top-level output,
 * but any OR-clause constituents are allowed to default to just the
 * contained rels.
 */
static Expr *
make_sub_restrictinfos(PlannerInfo *root,
					   Expr *clause,
					   bool is_pushed_down,
					   bool has_clone,
					   bool is_clone,
					   bool pseudoconstant,
					   Index security_level,
					   Relids required_relids,
					   Relids incompatible_relids,
					   Relids outer_relids)
{
	if (is_orclause(clause))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			orlist = lappend(orlist,
							 make_sub_restrictinfos(root,
													lfirst(temp),
													is_pushed_down,
													has_clone,
													is_clone,
													pseudoconstant,
													security_level,
													NULL,
													incompatible_relids,
													outer_relids));
		return (Expr *) make_plain_restrictinfo(root,
												clause,
												make_orclause(orlist),
												is_pushed_down,
												has_clone,
												is_clone,
												pseudoconstant,
												security_level,
												required_relids,
												incompatible_relids,
												outer_relids);
	}
	else if (is_andclause(clause))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			andlist = lappend(andlist,
							  make_sub_restrictinfos(root,
													 lfirst(temp),
													 is_pushed_down,
													 has_clone,
													 is_clone,
													 pseudoconstant,
													 security_level,
													 required_relids,
													 incompatible_relids,
													 outer_relids));
		return make_andclause(andlist);
	}
	else
		return (Expr *) make_plain_restrictinfo(root,
												clause,
												NULL,
												is_pushed_down,
												has_clone,
												is_clone,
												pseudoconstant,
												security_level,
												required_relids,
												incompatible_relids,
												outer_relids);
}

/*
 * commute_restrictinfo
 *
 * Given a RestrictInfo containing a binary opclause, produce a RestrictInfo
 * representing the commutation of that clause.  The caller must pass the
 * OID of the commutator operator (which it's presumably looked up, else
 * it would not know this is valid).
 *
 * Beware that the result shares sub-structure with the given RestrictInfo.
 * That's okay for the intended usage with derived index quals, but might
 * be hazardous if the source is subject to change.  Also notice that we
 * assume without checking that the commutator op is a member of the same
 * btree and hash opclasses as the original op.
 */
RestrictInfo *
commute_restrictinfo(RestrictInfo *rinfo, Oid comm_op)
{
	RestrictInfo *result;
	OpExpr	   *newclause;
	OpExpr	   *clause = castNode(OpExpr, rinfo->clause);

	Assert(list_length(clause->args) == 2);

	/* flat-copy all the fields of clause ... */
	newclause = makeNode(OpExpr);
	memcpy(newclause, clause, sizeof(OpExpr));

	/* ... and adjust those we need to change to commute it */
	newclause->opno = comm_op;
	newclause->opfuncid = InvalidOid;
	newclause->args = list_make2(lsecond(clause->args),
								 linitial(clause->args));

	/* likewise, flat-copy all the fields of rinfo ... */
	result = makeNode(RestrictInfo);
	memcpy(result, rinfo, sizeof(RestrictInfo));

	/*
	 * ... and adjust those we need to change.  Note in particular that we can
	 * preserve any cached selectivity or cost estimates, since those ought to
	 * be the same for the new clause.  Likewise we can keep the source's
	 * parent_ec.  It's also important that we keep the same rinfo_serial.
	 */
	result->clause = (Expr *) newclause;
	result->left_relids = rinfo->right_relids;
	result->right_relids = rinfo->left_relids;
	Assert(result->orclause == NULL);
	result->left_ec = rinfo->right_ec;
	result->right_ec = rinfo->left_ec;
	result->left_em = rinfo->right_em;
	result->right_em = rinfo->left_em;
	result->scansel_cache = NIL;	/* not worth updating this */
	if (rinfo->hashjoinoperator == clause->opno)
		result->hashjoinoperator = comm_op;
	else
		result->hashjoinoperator = InvalidOid;
	result->left_bucketsize = rinfo->right_bucketsize;
	result->right_bucketsize = rinfo->left_bucketsize;
	result->left_mcvfreq = rinfo->right_mcvfreq;
	result->right_mcvfreq = rinfo->left_mcvfreq;
	result->left_hasheqoperator = InvalidOid;
	result->right_hasheqoperator = InvalidOid;

	return result;
}

/*
 * restriction_is_or_clause
 *
 * Returns t iff the restrictinfo node contains an 'or' clause.
 */
bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
	if (restrictinfo->orclause != NULL)
		return true;
	else
		return false;
}

/*
 * restriction_is_securely_promotable
 *
 * Returns true if it's okay to evaluate this clause "early", that is before
 * other restriction clauses attached to the specified relation.
 */
bool
restriction_is_securely_promotable(RestrictInfo *restrictinfo,
								   RelOptInfo *rel)
{
	/*
	 * It's okay if there are no baserestrictinfo clauses for the rel that
	 * would need to go before this one, *or* if this one is leakproof.
	 */
	if (restrictinfo->security_level <= rel->baserestrict_min_security ||
		restrictinfo->leakproof)
		return true;
	else
		return false;
}

/*
 * Detect whether a RestrictInfo's clause is constant TRUE (note that it's
 * surely of type boolean).  No such WHERE clause could survive qual
 * canonicalization, but equivclass.c may generate such RestrictInfos for
 * reasons discussed therein.  We should drop them again when creating
 * the finished plan, which is handled by the next few functions.
 */
static inline bool
rinfo_is_constant_true(RestrictInfo *rinfo)
{
	return IsA(rinfo->clause, Const) &&
		!((Const *) rinfo->clause)->constisnull &&
		DatumGetBool(((Const *) rinfo->clause)->constvalue);
}

/*
 * get_actual_clauses
 *
 * Returns a list containing the bare clauses from 'restrictinfo_list'.
 *
 * This is only to be used in cases where none of the RestrictInfos can
 * be pseudoconstant clauses (for instance, it's OK on indexqual lists).
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		Assert(!rinfo->pseudoconstant);
		Assert(!rinfo_is_constant_true(rinfo));

		result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_clauses
 *
 * Extract bare clauses from 'restrictinfo_list', returning either the
 * regular ones or the pseudoconstant ones per 'pseudoconstant'.
 * Constant-TRUE clauses are dropped in any case.
 */
List *
extract_actual_clauses(List *restrictinfo_list,
					   bool pseudoconstant)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (rinfo->pseudoconstant == pseudoconstant &&
			!rinfo_is_constant_true(rinfo))
			result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_join_clauses
 *
 * Extract bare clauses from 'restrictinfo_list', separating those that
 * semantically match the join level from those that were pushed down.
 * Pseudoconstant and constant-TRUE clauses are excluded from the results.
 *
 * This is only used at outer joins, since for plain joins we don't care
 * about pushed-down-ness.
 */
void
extract_actual_join_clauses(List *restrictinfo_list,
							Relids joinrelids,
							List **joinquals,
							List **otherquals)
{
	ListCell   *l;

	*joinquals = NIL;
	*otherquals = NIL;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (RINFO_IS_PUSHED_DOWN(rinfo, joinrelids))
		{
			if (!rinfo->pseudoconstant &&
				!rinfo_is_constant_true(rinfo))
				*otherquals = lappend(*otherquals, rinfo->clause);
		}
		else
		{
			/* joinquals shouldn't have been marked pseudoconstant */
			Assert(!rinfo->pseudoconstant);
			if (!rinfo_is_constant_true(rinfo))
				*joinquals = lappend(*joinquals, rinfo->clause);
		}
	}
}

/*
 * join_clause_is_movable_to
 *		Test whether a join clause is a safe candidate for parameterization
 *		of a scan on the specified base relation.
 *
 * A movable join clause is one that can safely be evaluated at a rel below
 * its normal semantic level (ie, its required_relids), if the values of
 * variables that it would need from other rels are provided.
 *
 * We insist that the clause actually reference the target relation; this
 * prevents undesirable movement of degenerate join clauses, and ensures
 * that there is a unique place that a clause can be moved down to.
 *
 * We cannot move an outer-join clause into the non-nullable side of its
 * outer join, as that would change the results (rows would be suppressed
 * rather than being null-extended).
 *
 * Also there must not be an outer join below the clause that would null the
 * Vars coming from the target relation.  Otherwise the clause might give
 * results different from what it would give at its normal semantic level.
 *
 * Also, the join clause must not use any relations that have LATERAL
 * references to the target relation, since we could not put such rels on
 * the outer side of a nestloop with the target relation.
 *
 * Also, we reject is_clone versions of outer-join clauses.  This has the
 * effect of preventing us from generating variant parameterized paths
 * that differ only in which outer joins null the parameterization rel(s).
 * Generating one path from the minimally-parameterized has_clone version
 * is sufficient.
 */
bool
join_clause_is_movable_to(RestrictInfo *rinfo, RelOptInfo *baserel)
{
	/* Clause must physically reference target rel */
	if (!bms_is_member(baserel->relid, rinfo->clause_relids))
		return false;

	/* Cannot move an outer-join clause into the join's outer side */
	if (bms_is_member(baserel->relid, rinfo->outer_relids))
		return false;

	/*
	 * Target rel's Vars must not be nulled by any outer join.  We can check
	 * this without groveling through the individual Vars by seeing whether
	 * clause_relids (which includes all such Vars' varnullingrels) includes
	 * any outer join that can null the target rel.  You might object that
	 * this could reject the clause on the basis of an OJ relid that came from
	 * some other rel's Var.  However, that would still mean that the clause
	 * came from above that outer join and shouldn't be pushed down; so there
	 * should be no false positives.
	 */
	if (bms_overlap(rinfo->clause_relids, baserel->nulling_relids))
		return false;

	/* Clause must not use any rels with LATERAL references to this rel */
	if (bms_overlap(baserel->lateral_referencers, rinfo->clause_relids))
		return false;

	/* Ignore clones, too */
	if (rinfo->is_clone)
		return false;

	return true;
}

/*
 * join_clause_is_movable_into
 *		Test whether a join clause is movable and can be evaluated within
 *		the current join context.
 *
 * currentrelids: the relids of the proposed evaluation location
 * current_and_outer: the union of currentrelids and the required_outer
 *		relids (parameterization's outer relations)
 *
 * The API would be a bit clearer if we passed the current relids and the
 * outer relids separately and did bms_union internally; but since most
 * callers need to apply this function to multiple clauses, we make the
 * caller perform the union.
 *
 * Obviously, the clause must only refer to Vars available from the current
 * relation plus the outer rels.  We also check that it does reference at
 * least one current Var, ensuring that the clause will be pushed down to
 * a unique place in a parameterized join tree.  And we check that we're
 * not pushing the clause into its outer-join outer side.
 *
 * We used to need to check that we're not pushing the clause into a lower
 * outer join's inner side.  However, now that clause_relids includes
 * references to potentially-nulling outer joins, the other tests handle that
 * concern.  If the clause references any Var coming from the inside of a
 * lower outer join, its clause_relids will mention that outer join, causing
 * the evaluability check to fail; while if it references no such Vars, the
 * references-a-target-rel check will fail.
 *
 * There's no check here equivalent to join_clause_is_movable_to's test on
 * lateral_referencers.  We assume the caller wouldn't be inquiring unless
 * it'd verified that the proposed outer rels don't have lateral references
 * to the current rel(s).  (If we are considering join paths with the outer
 * rels on the outside and the current rels on the inside, then this should
 * have been checked at the outset of such consideration; see join_is_legal
 * and the path parameterization checks in joinpath.c.)  On the other hand,
 * in join_clause_is_movable_to we are asking whether the clause could be
 * moved for some valid set of outer rels, so we don't have the benefit of
 * relying on prior checks for lateral-reference validity.
 *
 * Likewise, we don't check is_clone here: rejecting the inappropriate
 * variants of a cloned clause must be handled upstream.
 *
 * Note: if this returns true, it means that the clause could be moved to
 * this join relation, but that doesn't mean that this is the lowest join
 * it could be moved to.  Caller may need to make additional calls to verify
 * that this doesn't succeed on either of the inputs of a proposed join.
 *
 * Note: get_joinrel_parampathinfo depends on the fact that if
 * current_and_outer is NULL, this function will always return false
 * (since one or the other of the first two tests must fail).
 */
bool
join_clause_is_movable_into(RestrictInfo *rinfo,
							Relids currentrelids,
							Relids current_and_outer)
{
	/* Clause must be evaluable given available context */
	if (!bms_is_subset(rinfo->clause_relids, current_and_outer))
		return false;

	/* Clause must physically reference at least one target rel */
	if (!bms_overlap(currentrelids, rinfo->clause_relids))
		return false;

	/* Cannot move an outer-join clause into the join's outer side */
	if (bms_overlap(currentrelids, rinfo->outer_relids))
		return false;

	return true;
}
