/*-------------------------------------------------------------------------
 *
 * tidpath.c
 *	  Routines to determine which TID conditions are usable for scanning
 *	  a given relation, and create TidPaths and TidRangePaths accordingly.
 *
 * For TidPaths, we look for WHERE conditions of the form
 * "CTID = pseudoconstant", which can be implemented by just fetching
 * the tuple directly via heap_fetch().  We can also handle OR'd conditions
 * such as (CTID = const1) OR (CTID = const2), as well as ScalarArrayOpExpr
 * conditions of the form CTID = ANY(pseudoconstant_array).  In particular
 * this allows
 *		WHERE ctid IN (tid1, tid2, ...)
 *
 * As with indexscans, our definition of "pseudoconstant" is pretty liberal:
 * we allow anything that doesn't involve a volatile function or a Var of
 * the relation under consideration.  Vars belonging to other relations of
 * the query are allowed, giving rise to parameterized TID scans.
 *
 * We also support "WHERE CURRENT OF cursor" conditions (CurrentOfExpr),
 * which amount to "CTID = run-time-determined-TID".  These could in
 * theory be translated to a simple comparison of CTID to the result of
 * a function, but in practice it works better to keep the special node
 * representation all the way through to execution.
 *
 * Additionally, TidRangePaths may be created for conditions of the form
 * "CTID relop pseudoconstant", where relop is one of >,>=,<,<=, and
 * AND-clauses composed of such conditions.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/tidpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


/*
 * Does this Var represent the CTID column of the specified baserel?
 */
static inline bool
IsCTIDVar(Var *var, RelOptInfo *rel)
{
	/* The vartype check is strictly paranoia */
	if (var->varattno == SelfItemPointerAttributeNumber &&
		var->vartype == TIDOID &&
		var->varno == rel->relid &&
		var->varnullingrels == NULL &&
		var->varlevelsup == 0)
		return true;
	return false;
}

/*
 * Check to see if a RestrictInfo is of the form
 *		CTID OP pseudoconstant
 * or
 *		pseudoconstant OP CTID
 * where OP is a binary operation, the CTID Var belongs to relation "rel",
 * and nothing on the other side of the clause does.
 */
static bool
IsBinaryTidClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	OpExpr	   *node;
	Node	   *arg1,
			   *arg2,
			   *other;
	Relids		other_relids;

	/* Must be an OpExpr */
	if (!is_opclause(rinfo->clause))
		return false;
	node = (OpExpr *) rinfo->clause;

	/* OpExpr must have two arguments */
	if (list_length(node->args) != 2)
		return false;
	arg1 = linitial(node->args);
	arg2 = lsecond(node->args);

	/* Look for CTID as either argument */
	other = NULL;
	other_relids = NULL;
	if (arg1 && IsA(arg1, Var) &&
		IsCTIDVar((Var *) arg1, rel))
	{
		other = arg2;
		other_relids = rinfo->right_relids;
	}
	if (!other && arg2 && IsA(arg2, Var) &&
		IsCTIDVar((Var *) arg2, rel))
	{
		other = arg1;
		other_relids = rinfo->left_relids;
	}
	if (!other)
		return false;

	/* The other argument must be a pseudoconstant */
	if (bms_is_member(rel->relid, other_relids) ||
		contain_volatile_functions(other))
		return false;

	return true;				/* success */
}

/*
 * Check to see if a RestrictInfo is of the form
 *		CTID = pseudoconstant
 * or
 *		pseudoconstant = CTID
 * where the CTID Var belongs to relation "rel", and nothing on the
 * other side of the clause does.
 */
static bool
IsTidEqualClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	if (!IsBinaryTidClause(rinfo, rel))
		return false;

	if (((OpExpr *) rinfo->clause)->opno == TIDEqualOperator)
		return true;

	return false;
}

/*
 * Check to see if a RestrictInfo is of the form
 *		CTID OP pseudoconstant
 * or
 *		pseudoconstant OP CTID
 * where OP is a range operator such as <, <=, >, or >=, the CTID Var belongs
 * to relation "rel", and nothing on the other side of the clause does.
 */
static bool
IsTidRangeClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	Oid			opno;

	if (!IsBinaryTidClause(rinfo, rel))
		return false;
	opno = ((OpExpr *) rinfo->clause)->opno;

	if (opno == TIDLessOperator || opno == TIDLessEqOperator ||
		opno == TIDGreaterOperator || opno == TIDGreaterEqOperator)
		return true;

	return false;
}

/*
 * Check to see if a RestrictInfo is of the form
 *		CTID = ANY (pseudoconstant_array)
 * where the CTID Var belongs to relation "rel", and nothing on the
 * other side of the clause does.
 */
static bool
IsTidEqualAnyClause(PlannerInfo *root, RestrictInfo *rinfo, RelOptInfo *rel)
{
	ScalarArrayOpExpr *node;
	Node	   *arg1,
			   *arg2;

	/* Must be a ScalarArrayOpExpr */
	if (!(rinfo->clause && IsA(rinfo->clause, ScalarArrayOpExpr)))
		return false;
	node = (ScalarArrayOpExpr *) rinfo->clause;

	/* Operator must be tideq */
	if (node->opno != TIDEqualOperator)
		return false;
	if (!node->useOr)
		return false;
	Assert(list_length(node->args) == 2);
	arg1 = linitial(node->args);
	arg2 = lsecond(node->args);

	/* CTID must be first argument */
	if (arg1 && IsA(arg1, Var) &&
		IsCTIDVar((Var *) arg1, rel))
	{
		/* The other argument must be a pseudoconstant */
		if (bms_is_member(rel->relid, pull_varnos(root, arg2)) ||
			contain_volatile_functions(arg2))
			return false;

		return true;			/* success */
	}

	return false;
}

/*
 * Check to see if a RestrictInfo is a CurrentOfExpr referencing "rel".
 */
static bool
IsCurrentOfClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	CurrentOfExpr *node;

	/* Must be a CurrentOfExpr */
	if (!(rinfo->clause && IsA(rinfo->clause, CurrentOfExpr)))
		return false;
	node = (CurrentOfExpr *) rinfo->clause;

	/* If it references this rel, we're good */
	if (node->cvarno == rel->relid)
		return true;

	return false;
}

/*
 * Is the RestrictInfo usable as a CTID qual for the specified rel?
 *
 * This function considers only base cases; AND/OR combination is handled
 * below.
 */
static bool
RestrictInfoIsTidQual(PlannerInfo *root, RestrictInfo *rinfo, RelOptInfo *rel)
{
	/*
	 * We may ignore pseudoconstant clauses (they can't contain Vars, so could
	 * not match anyway).
	 */
	if (rinfo->pseudoconstant)
		return false;

	/*
	 * If clause must wait till after some lower-security-level restriction
	 * clause, reject it.
	 */
	if (!restriction_is_securely_promotable(rinfo, rel))
		return false;

	/*
	 * Check all base cases.
	 */
	if (IsTidEqualClause(rinfo, rel) ||
		IsTidEqualAnyClause(root, rinfo, rel) ||
		IsCurrentOfClause(rinfo, rel))
		return true;

	return false;
}

/*
 * Extract a set of CTID conditions from implicit-AND List of RestrictInfos
 *
 * Returns a List of CTID qual RestrictInfos for the specified rel (with
 * implicit OR semantics across the list), or NIL if there are no usable
 * equality conditions.
 *
 * This function is mainly concerned with handling AND/OR recursion.
 * However, we do have a special rule to enforce: if there is a CurrentOfExpr
 * qual, we *must* return that and only that, else the executor may fail.
 * Ordinarily a CurrentOfExpr would be all alone anyway because of grammar
 * restrictions, but it is possible for RLS quals to appear AND'ed with it.
 * It's even possible (if fairly useless) for the RLS quals to be CTID quals.
 * So we must scan the whole rlist to see if there's a CurrentOfExpr.  Since
 * we have to do that, we also apply some very-trivial preference rules about
 * which of the other possibilities should be chosen, in the unlikely event
 * that there's more than one choice.
 */
static List *
TidQualFromRestrictInfoList(PlannerInfo *root, List *rlist, RelOptInfo *rel,
							bool *isCurrentOf)
{
	RestrictInfo *tidclause = NULL; /* best simple CTID qual so far */
	List	   *orlist = NIL;	/* best OR'ed CTID qual so far */
	ListCell   *l;

	*isCurrentOf = false;

	foreach(l, rlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (restriction_is_or_clause(rinfo))
		{
			List	   *rlst = NIL;
			ListCell   *j;

			/*
			 * We must be able to extract a CTID condition from every
			 * sub-clause of an OR, or we can't use it.
			 */
			foreach(j, ((BoolExpr *) rinfo->orclause)->args)
			{
				Node	   *orarg = (Node *) lfirst(j);
				List	   *sublist;

				/* OR arguments should be ANDs or sub-RestrictInfos */
				if (is_andclause(orarg))
				{
					List	   *andargs = ((BoolExpr *) orarg)->args;
					bool		sublistIsCurrentOf;

					/* Recurse in case there are sub-ORs */
					sublist = TidQualFromRestrictInfoList(root, andargs, rel,
														  &sublistIsCurrentOf);
					if (sublistIsCurrentOf)
						elog(ERROR, "IS CURRENT OF within OR clause");
				}
				else
				{
					RestrictInfo *ri = castNode(RestrictInfo, orarg);

					Assert(!restriction_is_or_clause(ri));
					if (RestrictInfoIsTidQual(root, ri, rel))
						sublist = list_make1(ri);
					else
						sublist = NIL;
				}

				/*
				 * If nothing found in this arm, we can't do anything with
				 * this OR clause.
				 */
				if (sublist == NIL)
				{
					rlst = NIL; /* forget anything we had */
					break;		/* out of loop over OR args */
				}

				/*
				 * OK, continue constructing implicitly-OR'ed result list.
				 */
				rlst = list_concat(rlst, sublist);
			}

			if (rlst)
			{
				/*
				 * Accept the OR'ed list if it's the first one, or if it's
				 * shorter than the previous one.
				 */
				if (orlist == NIL || list_length(rlst) < list_length(orlist))
					orlist = rlst;
			}
		}
		else
		{
			/* Not an OR clause, so handle base cases */
			if (RestrictInfoIsTidQual(root, rinfo, rel))
			{
				/* We can stop immediately if it's a CurrentOfExpr */
				if (IsCurrentOfClause(rinfo, rel))
				{
					*isCurrentOf = true;
					return list_make1(rinfo);
				}

				/*
				 * Otherwise, remember the first non-OR CTID qual.  We could
				 * try to apply some preference order if there's more than
				 * one, but such usage seems very unlikely, so don't bother.
				 */
				if (tidclause == NULL)
					tidclause = rinfo;
			}
		}
	}

	/*
	 * Prefer any singleton CTID qual to an OR'ed list.  Again, it seems
	 * unlikely to be worth thinking harder than that.
	 */
	if (tidclause)
		return list_make1(tidclause);
	return orlist;
}

/*
 * Extract a set of CTID range conditions from implicit-AND List of RestrictInfos
 *
 * Returns a List of CTID range qual RestrictInfos for the specified rel
 * (with implicit AND semantics across the list), or NIL if there are no
 * usable range conditions or if the rel's table AM does not support TID range
 * scans.
 */
static List *
TidRangeQualFromRestrictInfoList(List *rlist, RelOptInfo *rel)
{
	List	   *rlst = NIL;
	ListCell   *l;

	if ((rel->amflags & AMFLAG_HAS_TID_RANGE) == 0)
		return NIL;

	foreach(l, rlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (IsTidRangeClause(rinfo, rel))
			rlst = lappend(rlst, rinfo);
	}

	return rlst;
}

/*
 * Given a list of join clauses involving our rel, create a parameterized
 * TidPath for each one that is a suitable TidEqual clause.
 *
 * In principle we could combine clauses that reference the same outer rels,
 * but it doesn't seem like such cases would arise often enough to be worth
 * troubling over.
 */
static void
BuildParameterizedTidPaths(PlannerInfo *root, RelOptInfo *rel, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		List	   *tidquals;
		Relids		required_outer;

		/*
		 * Validate whether each clause is actually usable; we must check this
		 * even when examining clauses generated from an EquivalenceClass,
		 * since they might not satisfy the restriction on not having Vars of
		 * our rel on the other side, or somebody might've built an operator
		 * class that accepts type "tid" but has other operators in it.
		 *
		 * We currently consider only TidEqual join clauses.  In principle we
		 * might find a suitable ScalarArrayOpExpr in the rel's joininfo list,
		 * but it seems unlikely to be worth expending the cycles to check.
		 * And we definitely won't find a CurrentOfExpr here.  Hence, we don't
		 * use RestrictInfoIsTidQual; but this must match that function
		 * otherwise.
		 */
		if (rinfo->pseudoconstant ||
			!restriction_is_securely_promotable(rinfo, rel) ||
			!IsTidEqualClause(rinfo, rel))
			continue;

		/*
		 * Check if clause can be moved to this rel; this is probably
		 * redundant when considering EC-derived clauses, but we must check it
		 * for "loose" join clauses.
		 */
		if (!join_clause_is_movable_to(rinfo, rel))
			continue;

		/* OK, make list of clauses for this path */
		tidquals = list_make1(rinfo);

		/* Compute required outer rels for this path */
		required_outer = bms_union(rinfo->required_relids, rel->lateral_relids);
		required_outer = bms_del_member(required_outer, rel->relid);

		add_path(rel, (Path *) create_tidscan_path(root, rel, tidquals,
												   required_outer));
	}
}

/*
 * Test whether an EquivalenceClass member matches our rel's CTID Var.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ec_member_matches_ctid(PlannerInfo *root, RelOptInfo *rel,
					   EquivalenceClass *ec, EquivalenceMember *em,
					   void *arg)
{
	if (em->em_expr && IsA(em->em_expr, Var) &&
		IsCTIDVar((Var *) em->em_expr, rel))
		return true;
	return false;
}

/*
 * create_tidscan_paths
 *	  Create paths corresponding to direct TID scans of the given rel.
 *
 *	  Candidate paths are added to the rel's pathlist (using add_path).
 */
bool
create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tidquals;
	List	   *tidrangequals;
	bool		isCurrentOf;

	/*
	 * If any suitable quals exist in the rel's baserestrict list, generate a
	 * plain (unparameterized) TidPath with them.
	 *
	 * We skip this when enable_tidscan = false, except when the qual is
	 * CurrentOfExpr. In that case, a TID scan is the only correct path.
	 */
	tidquals = TidQualFromRestrictInfoList(root, rel->baserestrictinfo, rel,
										   &isCurrentOf);

	if (tidquals != NIL && (enable_tidscan || isCurrentOf))
	{
		/*
		 * This path uses no join clauses, but it could still have required
		 * parameterization due to LATERAL refs in its tlist.
		 */
		Relids		required_outer = rel->lateral_relids;

		add_path(rel, (Path *) create_tidscan_path(root, rel, tidquals,
												   required_outer));

		/*
		 * When the qual is CurrentOfExpr, the path that we just added is the
		 * only one the executor can handle, so we should return before adding
		 * any others. Returning true lets the caller know not to add any
		 * others, either.
		 */
		if (isCurrentOf)
			return true;
	}

	/* Skip the rest if TID scans are disabled. */
	if (!enable_tidscan)
		return false;

	/*
	 * If there are range quals in the baserestrict list, generate a
	 * TidRangePath.
	 */
	tidrangequals = TidRangeQualFromRestrictInfoList(rel->baserestrictinfo,
													 rel);

	if (tidrangequals != NIL)
	{
		/*
		 * This path uses no join clauses, but it could still have required
		 * parameterization due to LATERAL refs in its tlist.
		 */
		Relids		required_outer = rel->lateral_relids;

		add_path(rel, (Path *) create_tidrangescan_path(root, rel,
														tidrangequals,
														required_outer));
	}

	/*
	 * Try to generate parameterized TidPaths using equality clauses extracted
	 * from EquivalenceClasses.  (This is important since simple "t1.ctid =
	 * t2.ctid" clauses will turn into ECs.)
	 */
	if (rel->has_eclass_joins)
	{
		List	   *clauses;

		/* Generate clauses, skipping any that join to lateral_referencers */
		clauses = generate_implied_equalities_for_column(root,
														 rel,
														 ec_member_matches_ctid,
														 NULL,
														 rel->lateral_referencers);

		/* Generate a path for each usable join clause */
		BuildParameterizedTidPaths(root, rel, clauses);
	}

	/*
	 * Also consider parameterized TidPaths using "loose" join quals.  Quals
	 * of the form "t1.ctid = t2.ctid" would turn into these if they are outer
	 * join quals, for example.
	 */
	BuildParameterizedTidPaths(root, rel, rel->joininfo);

	return false;
}
