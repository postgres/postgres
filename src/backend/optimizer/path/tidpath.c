/*-------------------------------------------------------------------------
 *
 * tidpath.c
 *	  Routines to determine which TID conditions are usable for scanning
 *	  a given relation, and create TidPaths accordingly.
 *
 * What we are looking for here is WHERE conditions of the form
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
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"

/* source-code-compatibility hacks for pull_varnos() API change */
#define pull_varnos(a,b) pull_varnos_new(a,b)


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
		var->varlevelsup == 0)
		return true;
	return false;
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
	OpExpr	   *node;
	Node	   *arg1,
			   *arg2,
			   *other;
	Relids		other_relids;

	/* Must be an OpExpr */
	if (!is_opclause(rinfo->clause))
		return false;
	node = (OpExpr *) rinfo->clause;

	/* Operator must be tideq */
	if (node->opno != TIDEqualOperator)
		return false;
	Assert(list_length(node->args) == 2);
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
 * Extract a set of CTID conditions from the given RestrictInfo
 *
 * Returns a List of CTID qual RestrictInfos for the specified rel (with
 * implicit OR semantics across the list), or NIL if there are no usable
 * conditions.
 *
 * This function considers only base cases; AND/OR combination is handled
 * below.  Therefore the returned List never has more than one element.
 * (Using a List may seem a bit weird, but it simplifies the caller.)
 */
static List *
TidQualFromRestrictInfo(PlannerInfo *root, RestrictInfo *rinfo, RelOptInfo *rel)
{
	/*
	 * We may ignore pseudoconstant clauses (they can't contain Vars, so could
	 * not match anyway).
	 */
	if (rinfo->pseudoconstant)
		return NIL;

	/*
	 * If clause must wait till after some lower-security-level restriction
	 * clause, reject it.
	 */
	if (!restriction_is_securely_promotable(rinfo, rel))
		return NIL;

	/*
	 * Check all base cases.  If we get a match, return the clause.
	 */
	if (IsTidEqualClause(rinfo, rel) ||
		IsTidEqualAnyClause(root, rinfo, rel) ||
		IsCurrentOfClause(rinfo, rel))
		return list_make1(rinfo);

	return NIL;
}

/*
 * Extract a set of CTID conditions from implicit-AND List of RestrictInfos
 *
 * Returns a List of CTID qual RestrictInfos for the specified rel (with
 * implicit OR semantics across the list), or NIL if there are no usable
 * conditions.
 *
 * This function is just concerned with handling AND/OR recursion.
 */
static List *
TidQualFromRestrictInfoList(PlannerInfo *root, List *rlist, RelOptInfo *rel)
{
	List	   *rlst = NIL;
	ListCell   *l;

	foreach(l, rlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (restriction_is_or_clause(rinfo))
		{
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

					/* Recurse in case there are sub-ORs */
					sublist = TidQualFromRestrictInfoList(root, andargs, rel);
				}
				else
				{
					RestrictInfo *rinfo = castNode(RestrictInfo, orarg);

					Assert(!restriction_is_or_clause(rinfo));
					sublist = TidQualFromRestrictInfo(root, rinfo, rel);
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
		}
		else
		{
			/* Not an OR clause, so handle base cases */
			rlst = TidQualFromRestrictInfo(root, rinfo, rel);
		}

		/*
		 * Stop as soon as we find any usable CTID condition.  In theory we
		 * could get CTID equality conditions from different AND'ed clauses,
		 * in which case we could try to pick the most efficient one.  In
		 * practice, such usage seems very unlikely, so we don't bother; we
		 * just exit as soon as we find the first candidate.
		 */
		if (rlst)
			break;
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
		 * use TidQualFromRestrictInfo; but this must match that function
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
void
create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tidquals;

	/*
	 * If any suitable quals exist in the rel's baserestrict list, generate a
	 * plain (unparameterized) TidPath with them.
	 */
	tidquals = TidQualFromRestrictInfoList(root, rel->baserestrictinfo, rel);

	if (tidquals)
	{
		/*
		 * This path uses no join clauses, but it could still have required
		 * parameterization due to LATERAL refs in its tlist.
		 */
		Relids		required_outer = rel->lateral_relids;

		add_path(rel, (Path *) create_tidscan_path(root, rel, tidquals,
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
}
