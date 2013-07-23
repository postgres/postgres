/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  parser support routines dealing with relations
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_relation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static RangeTblEntry *scanNameSpaceForRefname(ParseState *pstate,
						const char *refname, int location);
static RangeTblEntry *scanNameSpaceForRelid(ParseState *pstate, Oid relid,
					  int location);
static void markRTEForSelectPriv(ParseState *pstate, RangeTblEntry *rte,
					 int rtindex, AttrNumber col);
static void expandRelation(Oid relid, Alias *eref,
			   int rtindex, int sublevels_up,
			   int location, bool include_dropped,
			   List **colnames, List **colvars);
static void expandTupleDesc(TupleDesc tupdesc, Alias *eref,
				int rtindex, int sublevels_up,
				int location, bool include_dropped,
				List **colnames, List **colvars);
static int	specialAttNum(const char *attname);
static bool isQueryUsingTempRelation_walker(Node *node, void *context);


/*
 * refnameRangeTblEntry
 *	  Given a possibly-qualified refname, look to see if it matches any RTE.
 *	  If so, return a pointer to the RangeTblEntry; else return NULL.
 *
 *	  Optionally get RTE's nesting depth (0 = current) into *sublevels_up.
 *	  If sublevels_up is NULL, only consider items at the current nesting
 *	  level.
 *
 * An unqualified refname (schemaname == NULL) can match any RTE with matching
 * alias, or matching unqualified relname in the case of alias-less relation
 * RTEs.  It is possible that such a refname matches multiple RTEs in the
 * nearest nesting level that has a match; if so, we report an error via
 * ereport().
 *
 * A qualified refname (schemaname != NULL) can only match a relation RTE
 * that (a) has no alias and (b) is for the same relation identified by
 * schemaname.refname.	In this case we convert schemaname.refname to a
 * relation OID and search by relid, rather than by alias name.  This is
 * peculiar, but it's what SQL says to do.
 */
RangeTblEntry *
refnameRangeTblEntry(ParseState *pstate,
					 const char *schemaname,
					 const char *refname,
					 int location,
					 int *sublevels_up)
{
	Oid			relId = InvalidOid;

	if (sublevels_up)
		*sublevels_up = 0;

	if (schemaname != NULL)
	{
		Oid			namespaceId;

		/*
		 * We can use LookupNamespaceNoError() here because we are only
		 * interested in finding existing RTEs.  Checking USAGE permission on
		 * the schema is unnecessary since it would have already been checked
		 * when the RTE was made.  Furthermore, we want to report "RTE not
		 * found", not "no permissions for schema", if the name happens to
		 * match a schema name the user hasn't got access to.
		 */
		namespaceId = LookupNamespaceNoError(schemaname);
		if (!OidIsValid(namespaceId))
			return NULL;
		relId = get_relname_relid(refname, namespaceId);
		if (!OidIsValid(relId))
			return NULL;
	}

	while (pstate != NULL)
	{
		RangeTblEntry *result;

		if (OidIsValid(relId))
			result = scanNameSpaceForRelid(pstate, relId, location);
		else
			result = scanNameSpaceForRefname(pstate, refname, location);

		if (result)
			return result;

		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;

		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * Search the query's table namespace for an RTE matching the
 * given unqualified refname.  Return the RTE if a unique match, or NULL
 * if no match.  Raise error if multiple matches.
 */
static RangeTblEntry *
scanNameSpaceForRefname(ParseState *pstate, const char *refname, int location)
{
	RangeTblEntry *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
		RangeTblEntry *rte = nsitem->p_rte;

		/* Ignore columns-only items */
		if (!nsitem->p_rel_visible)
			continue;
		/* If not inside LATERAL, ignore lateral-only items */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;

		if (strcmp(rte->eref->aliasname, refname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference \"%s\" is ambiguous",
								refname),
						 parser_errposition(pstate, location)));
			/* SQL:2008 demands this be an error, not an invisible item */
			if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
								refname),
						 errdetail("The combining JOIN type must be INNER or LEFT for a LATERAL reference."),
						 parser_errposition(pstate, location)));
			result = rte;
		}
	}
	return result;
}

/*
 * Search the query's table namespace for a relation RTE matching the
 * given relation OID.	Return the RTE if a unique match, or NULL
 * if no match.  Raise error if multiple matches (which shouldn't
 * happen if the namespace was checked correctly when it was created).
 *
 * See the comments for refnameRangeTblEntry to understand why this
 * acts the way it does.
 */
static RangeTblEntry *
scanNameSpaceForRelid(ParseState *pstate, Oid relid, int location)
{
	RangeTblEntry *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
		RangeTblEntry *rte = nsitem->p_rte;

		/* Ignore columns-only items */
		if (!nsitem->p_rel_visible)
			continue;
		/* If not inside LATERAL, ignore lateral-only items */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;

		/* yes, the test for alias == NULL should be there... */
		if (rte->rtekind == RTE_RELATION &&
			rte->relid == relid &&
			rte->alias == NULL)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference %u is ambiguous",
								relid),
						 parser_errposition(pstate, location)));
			/* SQL:2008 demands this be an error, not an invisible item */
			if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
								rte->eref->aliasname),
						 errdetail("The combining JOIN type must be INNER or LEFT for a LATERAL reference."),
						 parser_errposition(pstate, location)));
			result = rte;
		}
	}
	return result;
}

/*
 * Search the query's CTE namespace for a CTE matching the given unqualified
 * refname.  Return the CTE (and its levelsup count) if a match, or NULL
 * if no match.  We need not worry about multiple matches, since parse_cte.c
 * rejects WITH lists containing duplicate CTE names.
 */
CommonTableExpr *
scanNameSpaceForCTE(ParseState *pstate, const char *refname,
					Index *ctelevelsup)
{
	Index		levelsup;

	for (levelsup = 0;
		 pstate != NULL;
		 pstate = pstate->parentParseState, levelsup++)
	{
		ListCell   *lc;

		foreach(lc, pstate->p_ctenamespace)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			if (strcmp(cte->ctename, refname) == 0)
			{
				*ctelevelsup = levelsup;
				return cte;
			}
		}
	}
	return NULL;
}

/*
 * Search for a possible "future CTE", that is one that is not yet in scope
 * according to the WITH scoping rules.  This has nothing to do with valid
 * SQL semantics, but it's important for error reporting purposes.
 */
static bool
isFutureCTE(ParseState *pstate, const char *refname)
{
	for (; pstate != NULL; pstate = pstate->parentParseState)
	{
		ListCell   *lc;

		foreach(lc, pstate->p_future_ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			if (strcmp(cte->ctename, refname) == 0)
				return true;
		}
	}
	return false;
}

/*
 * searchRangeTableForRel
 *	  See if any RangeTblEntry could possibly match the RangeVar.
 *	  If so, return a pointer to the RangeTblEntry; else return NULL.
 *
 * This is different from refnameRangeTblEntry in that it considers every
 * entry in the ParseState's rangetable(s), not only those that are currently
 * visible in the p_namespace list(s).	This behavior is invalid per the SQL
 * spec, and it may give ambiguous results (there might be multiple equally
 * valid matches, but only one will be returned).  This must be used ONLY
 * as a heuristic in giving suitable error messages.  See errorMissingRTE.
 *
 * Notice that we consider both matches on actual relation (or CTE) name
 * and matches on alias.
 */
static RangeTblEntry *
searchRangeTableForRel(ParseState *pstate, RangeVar *relation)
{
	const char *refname = relation->relname;
	Oid			relId = InvalidOid;
	CommonTableExpr *cte = NULL;
	Index		ctelevelsup = 0;
	Index		levelsup;

	/*
	 * If it's an unqualified name, check for possible CTE matches. A CTE
	 * hides any real relation matches.  If no CTE, look for a matching
	 * relation.
	 *
	 * NB: It's not critical that RangeVarGetRelid return the correct answer
	 * here in the face of concurrent DDL.	If it doesn't, the worst case
	 * scenario is a less-clear error message.	Also, the tables involved in
	 * the query are already locked, which reduces the number of cases in
	 * which surprising behavior can occur.  So we do the name lookup
	 * unlocked.
	 */
	if (!relation->schemaname)
		cte = scanNameSpaceForCTE(pstate, refname, &ctelevelsup);
	if (!cte)
		relId = RangeVarGetRelid(relation, NoLock, true);

	/* Now look for RTEs matching either the relation/CTE or the alias */
	for (levelsup = 0;
		 pstate != NULL;
		 pstate = pstate->parentParseState, levelsup++)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

			if (rte->rtekind == RTE_RELATION &&
				OidIsValid(relId) &&
				rte->relid == relId)
				return rte;
			if (rte->rtekind == RTE_CTE &&
				cte != NULL &&
				rte->ctelevelsup + levelsup == ctelevelsup &&
				strcmp(rte->ctename, refname) == 0)
				return rte;
			if (strcmp(rte->eref->aliasname, refname) == 0)
				return rte;
		}
	}
	return NULL;
}

/*
 * Check for relation-name conflicts between two namespace lists.
 * Raise an error if any is found.
 *
 * Note: we assume that each given argument does not contain conflicts
 * itself; we just want to know if the two can be merged together.
 *
 * Per SQL, two alias-less plain relation RTEs do not conflict even if
 * they have the same eref->aliasname (ie, same relation name), if they
 * are for different relation OIDs (implying they are in different schemas).
 *
 * We ignore the lateral-only flags in the namespace items: the lists must
 * not conflict, even when all items are considered visible.  However,
 * columns-only items should be ignored.
 */
void
checkNameSpaceConflicts(ParseState *pstate, List *namespace1,
						List *namespace2)
{
	ListCell   *l1;

	foreach(l1, namespace1)
	{
		ParseNamespaceItem *nsitem1 = (ParseNamespaceItem *) lfirst(l1);
		RangeTblEntry *rte1 = nsitem1->p_rte;
		const char *aliasname1 = rte1->eref->aliasname;
		ListCell   *l2;

		if (!nsitem1->p_rel_visible)
			continue;

		foreach(l2, namespace2)
		{
			ParseNamespaceItem *nsitem2 = (ParseNamespaceItem *) lfirst(l2);
			RangeTblEntry *rte2 = nsitem2->p_rte;

			if (!nsitem2->p_rel_visible)
				continue;
			if (strcmp(rte2->eref->aliasname, aliasname1) != 0)
				continue;		/* definitely no conflict */
			if (rte1->rtekind == RTE_RELATION && rte1->alias == NULL &&
				rte2->rtekind == RTE_RELATION && rte2->alias == NULL &&
				rte1->relid != rte2->relid)
				continue;		/* no conflict per SQL rule */
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("table name \"%s\" specified more than once",
							aliasname1)));
		}
	}
}

/*
 * given an RTE, return RT index (starting with 1) of the entry,
 * and optionally get its nesting depth (0 = current).	If sublevels_up
 * is NULL, only consider rels at the current nesting level.
 * Raises error if RTE not found.
 */
int
RTERangeTablePosn(ParseState *pstate, RangeTblEntry *rte, int *sublevels_up)
{
	int			index;
	ListCell   *l;

	if (sublevels_up)
		*sublevels_up = 0;

	while (pstate != NULL)
	{
		index = 1;
		foreach(l, pstate->p_rtable)
		{
			if (rte == (RangeTblEntry *) lfirst(l))
				return index;
			index++;
		}
		pstate = pstate->parentParseState;
		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;
	}

	elog(ERROR, "RTE not found (internal error)");
	return 0;					/* keep compiler quiet */
}

/*
 * Given an RT index and nesting depth, find the corresponding RTE.
 * This is the inverse of RTERangeTablePosn.
 */
RangeTblEntry *
GetRTEByRangeTablePosn(ParseState *pstate,
					   int varno,
					   int sublevels_up)
{
	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	Assert(varno > 0 && varno <= list_length(pstate->p_rtable));
	return rt_fetch(varno, pstate->p_rtable);
}

/*
 * Fetch the CTE for a CTE-reference RTE.
 *
 * rtelevelsup is the number of query levels above the given pstate that the
 * RTE came from.  Callers that don't have this information readily available
 * may pass -1 instead.
 */
CommonTableExpr *
GetCTEForRTE(ParseState *pstate, RangeTblEntry *rte, int rtelevelsup)
{
	Index		levelsup;
	ListCell   *lc;

	/* Determine RTE's levelsup if caller didn't know it */
	if (rtelevelsup < 0)
		(void) RTERangeTablePosn(pstate, rte, &rtelevelsup);

	Assert(rte->rtekind == RTE_CTE);
	levelsup = rte->ctelevelsup + rtelevelsup;
	while (levelsup-- > 0)
	{
		pstate = pstate->parentParseState;
		if (!pstate)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	foreach(lc, pstate->p_ctenamespace)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			return cte;
	}
	/* shouldn't happen */
	elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	return NULL;				/* keep compiler quiet */
}

/*
 * scanRTEForColumn
 *	  Search the column names of a single RTE for the given name.
 *	  If found, return an appropriate Var node, else return NULL.
 *	  If the name proves ambiguous within this RTE, raise error.
 *
 * Side effect: if we find a match, mark the RTE as requiring read access
 * for the column.
 */
Node *
scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte, char *colname,
				 int location)
{
	Node	   *result = NULL;
	int			attnum = 0;
	Var		   *var;
	ListCell   *c;

	/*
	 * Scan the user column names (or aliases) for a match. Complain if
	 * multiple matches.
	 *
	 * Note: eref->colnames may include entries for dropped columns, but those
	 * will be empty strings that cannot match any legal SQL identifier, so we
	 * don't bother to test for that case here.
	 *
	 * Should this somehow go wrong and we try to access a dropped column,
	 * we'll still catch it by virtue of the checks in
	 * get_rte_attribute_type(), which is called by make_var().  That routine
	 * has to do a cache lookup anyway, so the check there is cheap.
	 */
	foreach(c, rte->eref->colnames)
	{
		attnum++;
		if (strcmp(strVal(lfirst(c)), colname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous",
								colname),
						 parser_errposition(pstate, location)));
			var = make_var(pstate, rte, attnum, location);
			/* Require read access to the column */
			markVarForSelectPriv(pstate, var, rte);
			result = (Node *) var;
		}
	}

	/*
	 * If we have a unique match, return it.  Note that this allows a user
	 * alias to override a system column name (such as OID) without error.
	 */
	if (result)
		return result;

	/*
	 * If the RTE represents a real table, consider system column names.
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		/* quick check to see if name could be a system column */
		attnum = specialAttNum(colname);
		if (attnum != InvalidAttrNumber)
		{
			/* now check to see if column actually is defined */
			if (SearchSysCacheExists2(ATTNUM,
									  ObjectIdGetDatum(rte->relid),
									  Int16GetDatum(attnum)))
			{
				var = make_var(pstate, rte, attnum, location);
				/* Require read access to the column */
				markVarForSelectPriv(pstate, var, rte);
				result = (Node *) var;
			}
		}
	}

	return result;
}

/*
 * colNameToVar
 *	  Search for an unqualified column name.
 *	  If found, return the appropriate Var node (or expression).
 *	  If not found, return NULL.  If the name proves ambiguous, raise error.
 *	  If localonly is true, only names in the innermost query are considered.
 */
Node *
colNameToVar(ParseState *pstate, char *colname, bool localonly,
			 int location)
{
	Node	   *result = NULL;
	ParseState *orig_pstate = pstate;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
			RangeTblEntry *rte = nsitem->p_rte;
			Node	   *newresult;

			/* Ignore table-only items */
			if (!nsitem->p_cols_visible)
				continue;
			/* If not inside LATERAL, ignore lateral-only items */
			if (nsitem->p_lateral_only && !pstate->p_lateral_active)
				continue;

			/* use orig_pstate here to get the right sublevels_up */
			newresult = scanRTEForColumn(orig_pstate, rte, colname, location);

			if (newresult)
			{
				if (result)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("column reference \"%s\" is ambiguous",
									colname),
							 parser_errposition(orig_pstate, location)));
				/* SQL:2008 demands this be an error, not an invisible item */
				if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
							 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
									rte->eref->aliasname),
							 errdetail("The combining JOIN type must be INNER or LEFT for a LATERAL reference."),
							 parser_errposition(orig_pstate, location)));
				result = newresult;
			}
		}

		if (result != NULL || localonly)
			break;				/* found, or don't want to look at parent */

		pstate = pstate->parentParseState;
	}

	return result;
}

/*
 * searchRangeTableForCol
 *	  See if any RangeTblEntry could possibly provide the given column name.
 *	  If so, return a pointer to the RangeTblEntry; else return NULL.
 *
 * This is different from colNameToVar in that it considers every entry in
 * the ParseState's rangetable(s), not only those that are currently visible
 * in the p_namespace list(s).	This behavior is invalid per the SQL spec,
 * and it may give ambiguous results (there might be multiple equally valid
 * matches, but only one will be returned).  This must be used ONLY as a
 * heuristic in giving suitable error messages.  See errorMissingColumn.
 */
static RangeTblEntry *
searchRangeTableForCol(ParseState *pstate, char *colname, int location)
{
	ParseState *orig_pstate = pstate;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

			if (scanRTEForColumn(orig_pstate, rte, colname, location))
				return rte;
		}

		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * markRTEForSelectPriv
 *	   Mark the specified column of an RTE as requiring SELECT privilege
 *
 * col == InvalidAttrNumber means a "whole row" reference
 *
 * The caller should pass the actual RTE if it has it handy; otherwise pass
 * NULL, and we'll look it up here.  (This uglification of the API is
 * worthwhile because nearly all external callers have the RTE at hand.)
 */
static void
markRTEForSelectPriv(ParseState *pstate, RangeTblEntry *rte,
					 int rtindex, AttrNumber col)
{
	if (rte == NULL)
		rte = rt_fetch(rtindex, pstate->p_rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		/* Make sure the rel as a whole is marked for SELECT access */
		rte->requiredPerms |= ACL_SELECT;
		/* Must offset the attnum to fit in a bitmapset */
		rte->selectedCols = bms_add_member(rte->selectedCols,
								   col - FirstLowInvalidHeapAttributeNumber);
	}
	else if (rte->rtekind == RTE_JOIN)
	{
		if (col == InvalidAttrNumber)
		{
			/*
			 * A whole-row reference to a join has to be treated as whole-row
			 * references to the two inputs.
			 */
			JoinExpr   *j;

			if (rtindex > 0 && rtindex <= list_length(pstate->p_joinexprs))
				j = (JoinExpr *) list_nth(pstate->p_joinexprs, rtindex - 1);
			else
				j = NULL;
			if (j == NULL)
				elog(ERROR, "could not find JoinExpr for whole-row reference");
			Assert(IsA(j, JoinExpr));

			/* Note: we can't see FromExpr here */
			if (IsA(j->larg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, NULL, varno, InvalidAttrNumber);
			}
			else if (IsA(j->larg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, NULL, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->larg));
			if (IsA(j->rarg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, NULL, varno, InvalidAttrNumber);
			}
			else if (IsA(j->rarg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, NULL, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->rarg));
		}
		else
		{
			/*
			 * Regular join attribute, look at the alias-variable list.
			 *
			 * The aliasvar could be either a Var or a COALESCE expression,
			 * but in the latter case we should already have marked the two
			 * referent variables as being selected, due to their use in the
			 * JOIN clause.  So we need only be concerned with the Var case.
			 * But we do need to drill down through implicit coercions.
			 */
			Var		   *aliasvar;

			Assert(col > 0 && col <= list_length(rte->joinaliasvars));
			aliasvar = (Var *) list_nth(rte->joinaliasvars, col - 1);
			aliasvar = (Var *) strip_implicit_coercions((Node *) aliasvar);
			if (aliasvar && IsA(aliasvar, Var))
				markVarForSelectPriv(pstate, aliasvar, NULL);
		}
	}
	/* other RTE types don't require privilege marking */
}

/*
 * markVarForSelectPriv
 *	   Mark the RTE referenced by a Var as requiring SELECT privilege
 *
 * The caller should pass the Var's referenced RTE if it has it handy
 * (nearly all do); otherwise pass NULL.
 */
void
markVarForSelectPriv(ParseState *pstate, Var *var, RangeTblEntry *rte)
{
	Index		lv;

	Assert(IsA(var, Var));
	/* Find the appropriate pstate if it's an uplevel Var */
	for (lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;
	markRTEForSelectPriv(pstate, rte, var->varno, var->varattno);
}

/*
 * buildRelationAliases
 *		Construct the eref column name list for a relation RTE.
 *		This code is also used for the case of a function RTE returning
 *		a named composite type.
 *
 * tupdesc: the physical column information
 * alias: the user-supplied alias, or NULL if none
 * eref: the eref Alias to store column names in
 *
 * eref->colnames is filled in.  Also, alias->colnames is rebuilt to insert
 * empty strings for any dropped columns, so that it will be one-to-one with
 * physical column numbers.
 */
static void
buildRelationAliases(TupleDesc tupdesc, Alias *alias, Alias *eref)
{
	int			maxattrs = tupdesc->natts;
	ListCell   *aliaslc;
	int			numaliases;
	int			varattno;
	int			numdropped = 0;

	Assert(eref->colnames == NIL);

	if (alias)
	{
		aliaslc = list_head(alias->colnames);
		numaliases = list_length(alias->colnames);
		/* We'll rebuild the alias colname list */
		alias->colnames = NIL;
	}
	else
	{
		aliaslc = NULL;
		numaliases = 0;
	}

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = tupdesc->attrs[varattno];
		Value	   *attrname;

		if (attr->attisdropped)
		{
			/* Always insert an empty string for a dropped column */
			attrname = makeString(pstrdup(""));
			if (aliaslc)
				alias->colnames = lappend(alias->colnames, attrname);
			numdropped++;
		}
		else if (aliaslc)
		{
			/* Use the next user-supplied alias */
			attrname = (Value *) lfirst(aliaslc);
			aliaslc = lnext(aliaslc);
			alias->colnames = lappend(alias->colnames, attrname);
		}
		else
		{
			attrname = makeString(pstrdup(NameStr(attr->attname)));
			/* we're done with the alias if any */
		}

		eref->colnames = lappend(eref->colnames, attrname);
	}

	/* Too many user-supplied aliases? */
	if (aliaslc)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, maxattrs - numdropped, numaliases)));
}

/*
 * buildScalarFunctionAlias
 *		Construct the eref column name list for a function RTE,
 *		when the function returns a scalar type (not composite or RECORD).
 *
 * funcexpr: transformed expression tree for the function call
 * funcname: function name (used only for error message)
 * alias: the user-supplied alias, or NULL if none
 * eref: the eref Alias to store column names in
 *
 * eref->colnames is filled in.
 */
static void
buildScalarFunctionAlias(Node *funcexpr, char *funcname,
						 Alias *alias, Alias *eref)
{
	char	   *pname;

	Assert(eref->colnames == NIL);

	/* Use user-specified column alias if there is one. */
	if (alias && alias->colnames != NIL)
	{
		if (list_length(alias->colnames) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				  errmsg("too many column aliases specified for function %s",
						 funcname)));
		eref->colnames = copyObject(alias->colnames);
		return;
	}

	/*
	 * If the expression is a simple function call, and the function has a
	 * single OUT parameter that is named, use the parameter's name.
	 */
	if (funcexpr && IsA(funcexpr, FuncExpr))
	{
		pname = get_func_result_name(((FuncExpr *) funcexpr)->funcid);
		if (pname)
		{
			eref->colnames = list_make1(makeString(pname));
			return;
		}
	}

	/*
	 * Otherwise use the previously-determined alias (not necessarily the
	 * function name!)
	 */
	eref->colnames = list_make1(makeString(eref->aliasname));
}

/*
 * Open a table during parse analysis
 *
 * This is essentially just the same as heap_openrv(), except that it caters
 * to some parser-specific error reporting needs, notably that it arranges
 * to include the RangeVar's parse location in any resulting error.
 *
 * Note: properly, lockmode should be declared LOCKMODE not int, but that
 * would require importing storage/lock.h into parse_relation.h.  Since
 * LOCKMODE is typedef'd as int anyway, that seems like overkill.
 */
Relation
parserOpenTable(ParseState *pstate, const RangeVar *relation, int lockmode)
{
	Relation	rel;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, pstate, relation->location);
	rel = heap_openrv_extended(relation, lockmode, true);
	if (rel == NULL)
	{
		if (relation->schemaname)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
		{
			/*
			 * An unqualified name might have been meant as a reference to
			 * some not-yet-in-scope CTE.  The bare "does not exist" message
			 * has proven remarkably unhelpful for figuring out such problems,
			 * so we take pains to offer a specific hint.
			 */
			if (isFutureCTE(pstate, relation->relname))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								relation->relname),
						 errdetail("There is a WITH item named \"%s\", but it cannot be referenced from this part of the query.",
								   relation->relname),
						 errhint("Use WITH RECURSIVE, or re-order the WITH items to remove forward references.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								relation->relname)));
		}
	}
	cancel_parser_errposition_callback(&pcbstate);
	return rel;
}

/*
 * Add an entry for a relation to the pstate's range table (p_rtable).
 *
 * If pstate is NULL, we just build an RTE and return it without adding it
 * to an rtable list.
 *
 * Note: formerly this checked for refname conflicts, but that's wrong.
 * Caller is responsible for checking for conflicts in the appropriate scope.
 */
RangeTblEntry *
addRangeTableEntry(ParseState *pstate,
				   RangeVar *relation,
				   Alias *alias,
				   bool inh,
				   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->aliasname : relation->relname;
	LOCKMODE	lockmode;
	Relation	rel;

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;

	/*
	 * Get the rel's OID.  This access also ensures that we have an up-to-date
	 * relcache entry for the rel.	Since this is typically the first access
	 * to a rel in a statement, be careful to get the right access level
	 * depending on whether we're doing SELECT FOR UPDATE/SHARE.
	 */
	lockmode = isLockedRefname(pstate, refname) ? RowShareLock : AccessShareLock;
	rel = parserOpenTable(pstate, relation, lockmode);
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;

	/*
	 * Build the list of effective column names using user-supplied aliases
	 * and/or actual column names.
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * Drop the rel refcount, but keep the access lock till end of transaction
	 * so that the table can't be deleted or have its schema modified
	 * underneath us.
	 */
	heap_close(rel, NoLock);

	/*
	 * Set flags and access permissions.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 */
	rte->lateral = false;
	rte->inh = inh;
	rte->inFromCl = inFromCl;

	rte->requiredPerms = ACL_SELECT;
	rte->checkAsUser = InvalidOid;		/* not set-uid by default, either */
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add an entry for a relation to the pstate's range table (p_rtable).
 *
 * This is just like addRangeTableEntry() except that it makes an RTE
 * given an already-open relation instead of a RangeVar reference.
 */
RangeTblEntry *
addRangeTableEntryForRelation(ParseState *pstate,
							  Relation rel,
							  Alias *alias,
							  bool inh,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->aliasname : RelationGetRelationName(rel);

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;

	/*
	 * Build the list of effective column names using user-supplied aliases
	 * and/or actual column names.
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * Set flags and access permissions.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 */
	rte->lateral = false;
	rte->inh = inh;
	rte->inFromCl = inFromCl;

	rte->requiredPerms = ACL_SELECT;
	rte->checkAsUser = InvalidOid;		/* not set-uid by default, either */
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add an entry for a subquery to the pstate's range table (p_rtable).
 *
 * This is just like addRangeTableEntry() except that it makes a subquery RTE.
 * Note that an alias clause *must* be supplied.
 */
RangeTblEntry *
addRangeTableEntryForSubquery(ParseState *pstate,
							  Query *subquery,
							  Alias *alias,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias->aliasname;
	Alias	   *eref;
	int			numaliases;
	int			varattno;
	ListCell   *tlistitem;

	rte->rtekind = RTE_SUBQUERY;
	rte->relid = InvalidOid;
	rte->subquery = subquery;
	rte->alias = alias;

	eref = copyObject(alias);
	numaliases = list_length(eref->colnames);

	/* fill in any unspecified alias columns */
	varattno = 0;
	foreach(tlistitem, subquery->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

		if (te->resjunk)
			continue;
		varattno++;
		Assert(varattno == te->resno);
		if (varattno > numaliases)
		{
			char	   *attrname;

			attrname = pstrdup(te->resname);
			eref->colnames = lappend(eref->colnames, makeString(attrname));
		}
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						refname, varattno, numaliases)));

	rte->eref = eref;

	/*
	 * Set flags and access permissions.
	 *
	 * Subqueries are never checked for access rights.
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* never true for subqueries */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add an entry for a function to the pstate's range table (p_rtable).
 *
 * This is just like addRangeTableEntry() except that it makes a function RTE.
 */
RangeTblEntry *
addRangeTableEntryForFunction(ParseState *pstate,
							  char *funcname,
							  Node *funcexpr,
							  RangeFunction *rangefunc,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	TypeFuncClass functypclass;
	Oid			funcrettype;
	TupleDesc	tupdesc;
	Alias	   *alias = rangefunc->alias;
	List	   *coldeflist = rangefunc->coldeflist;
	Alias	   *eref;

	rte->rtekind = RTE_FUNCTION;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->funcexpr = funcexpr;
	rte->funccoltypes = NIL;
	rte->funccoltypmods = NIL;
	rte->funccolcollations = NIL;
	rte->alias = alias;

	eref = makeAlias(alias ? alias->aliasname : funcname, NIL);
	rte->eref = eref;

	/*
	 * Now determine if the function returns a simple or composite type.
	 */
	functypclass = get_expr_result_type(funcexpr,
										&funcrettype,
										&tupdesc);

	/*
	 * A coldeflist is required if the function returns RECORD and hasn't got
	 * a predetermined record type, and is prohibited otherwise.
	 */
	if (coldeflist != NIL)
	{
		if (functypclass != TYPEFUNC_RECORD)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("a column definition list is only allowed for functions returning \"record\""),
					 parser_errposition(pstate, exprLocation(funcexpr))));
	}
	else
	{
		if (functypclass == TYPEFUNC_RECORD)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("a column definition list is required for functions returning \"record\""),
					 parser_errposition(pstate, exprLocation(funcexpr))));
	}

	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* Composite data type, e.g. a table's row type */
		Assert(tupdesc);
		/* Build the column alias list */
		buildRelationAliases(tupdesc, alias, eref);
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* Base data type, i.e. scalar */
		buildScalarFunctionAlias(funcexpr, funcname, alias, eref);
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		ListCell   *col;

		/*
		 * Use the column definition list to form the alias list and
		 * funccoltypes/funccoltypmods/funccolcollations lists.
		 */
		foreach(col, coldeflist)
		{
			ColumnDef  *n = (ColumnDef *) lfirst(col);
			char	   *attrname;
			Oid			attrtype;
			int32		attrtypmod;
			Oid			attrcollation;

			attrname = pstrdup(n->colname);
			if (n->typeName->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("column \"%s\" cannot be declared SETOF",
								attrname),
						 parser_errposition(pstate, n->typeName->location)));
			typenameTypeIdAndMod(pstate, n->typeName, &attrtype, &attrtypmod);
			attrcollation = GetColumnDefCollation(pstate, n, attrtype);
			eref->colnames = lappend(eref->colnames, makeString(attrname));
			rte->funccoltypes = lappend_oid(rte->funccoltypes, attrtype);
			rte->funccoltypmods = lappend_int(rte->funccoltypmods, attrtypmod);
			rte->funccolcollations = lappend_oid(rte->funccolcollations,
												 attrcollation);
		}
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("function \"%s\" in FROM has unsupported return type %s",
					funcname, format_type_be(funcrettype)),
				 parser_errposition(pstate, exprLocation(funcexpr))));

	/*
	 * Set flags and access permissions.
	 *
	 * Functions are never checked for access rights (at least, not by the RTE
	 * permissions mechanism).
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* never true for functions */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add an entry for a VALUES list to the pstate's range table (p_rtable).
 *
 * This is much like addRangeTableEntry() except that it makes a values RTE.
 */
RangeTblEntry *
addRangeTableEntryForValues(ParseState *pstate,
							List *exprs,
							List *collations,
							Alias *alias,
							bool lateral,
							bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->aliasname : pstrdup("*VALUES*");
	Alias	   *eref;
	int			numaliases;
	int			numcolumns;

	rte->rtekind = RTE_VALUES;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->values_lists = exprs;
	rte->values_collations = collations;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias(refname, NIL);

	/* fill in any unspecified alias columns */
	numcolumns = list_length((List *) linitial(exprs));
	numaliases = list_length(eref->colnames);
	while (numaliases < numcolumns)
	{
		char		attrname[64];

		numaliases++;
		snprintf(attrname, sizeof(attrname), "column%d", numaliases);
		eref->colnames = lappend(eref->colnames,
								 makeString(pstrdup(attrname)));
	}
	if (numcolumns < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("VALUES lists \"%s\" have %d columns available but %d columns specified",
						refname, numcolumns, numaliases)));

	rte->eref = eref;

	/*
	 * Set flags and access permissions.
	 *
	 * Subqueries are never checked for access rights.
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* never true for values RTEs */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add an entry for a join to the pstate's range table (p_rtable).
 *
 * This is much like addRangeTableEntry() except that it makes a join RTE.
 */
RangeTblEntry *
addRangeTableEntryForJoin(ParseState *pstate,
						  List *colnames,
						  JoinType jointype,
						  List *aliasvars,
						  Alias *alias,
						  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;

	/*
	 * Fail if join has too many columns --- we must be able to reference any
	 * of the columns with an AttrNumber.
	 */
	if (list_length(aliasvars) > MaxAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("joins can have at most %d columns",
						MaxAttrNumber)));

	rte->rtekind = RTE_JOIN;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->jointype = jointype;
	rte->joinaliasvars = aliasvars;
	rte->alias = alias;

	eref = alias ? (Alias *) copyObject(alias) : makeAlias("unnamed_join", NIL);
	numaliases = list_length(eref->colnames);

	/* fill in any unspecified alias columns */
	if (numaliases < list_length(colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(colnames, numaliases));

	rte->eref = eref;

	/*
	 * Set flags and access permissions.
	 *
	 * Joins are never checked for access rights.
	 */
	rte->lateral = false;
	rte->inh = false;			/* never true for joins */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add an entry for a CTE reference to the pstate's range table (p_rtable).
 *
 * This is much like addRangeTableEntry() except that it makes a CTE RTE.
 */
RangeTblEntry *
addRangeTableEntryForCTE(ParseState *pstate,
						 CommonTableExpr *cte,
						 Index levelsup,
						 RangeVar *rv,
						 bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rv->alias;
	char	   *refname = alias ? alias->aliasname : cte->ctename;
	Alias	   *eref;
	int			numaliases;
	int			varattno;
	ListCell   *lc;

	rte->rtekind = RTE_CTE;
	rte->ctename = cte->ctename;
	rte->ctelevelsup = levelsup;

	/* Self-reference if and only if CTE's parse analysis isn't completed */
	rte->self_reference = !IsA(cte->ctequery, Query);
	Assert(cte->cterecursive || !rte->self_reference);
	/* Bump the CTE's refcount if this isn't a self-reference */
	if (!rte->self_reference)
		cte->cterefcount++;

	/*
	 * We throw error if the CTE is INSERT/UPDATE/DELETE without RETURNING.
	 * This won't get checked in case of a self-reference, but that's OK
	 * because data-modifying CTEs aren't allowed to be recursive anyhow.
	 */
	if (IsA(cte->ctequery, Query))
	{
		Query	   *ctequery = (Query *) cte->ctequery;

		if (ctequery->commandType != CMD_SELECT &&
			ctequery->returningList == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("WITH query \"%s\" does not have a RETURNING clause",
						cte->ctename),
					 parser_errposition(pstate, rv->location)));
	}

	rte->ctecoltypes = cte->ctecoltypes;
	rte->ctecoltypmods = cte->ctecoltypmods;
	rte->ctecolcollations = cte->ctecolcollations;

	rte->alias = alias;
	if (alias)
		eref = copyObject(alias);
	else
		eref = makeAlias(refname, NIL);
	numaliases = list_length(eref->colnames);

	/* fill in any unspecified alias columns */
	varattno = 0;
	foreach(lc, cte->ctecolnames)
	{
		varattno++;
		if (varattno > numaliases)
			eref->colnames = lappend(eref->colnames, lfirst(lc));
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						refname, varattno, numaliases)));

	rte->eref = eref;

	/*
	 * Set flags and access permissions.
	 *
	 * Subqueries are never checked for access rights.
	 */
	rte->lateral = false;
	rte->inh = false;			/* never true for subqueries */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * Add completed RTE to pstate's range table list, but not to join list
	 * nor namespace --- caller must do that if appropriate.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}


/*
 * Has the specified refname been selected FOR UPDATE/FOR SHARE?
 *
 * This is used when we have not yet done transformLockingClause, but need
 * to know the correct lock to take during initial opening of relations.
 *
 * Note: we pay no attention to whether it's FOR UPDATE vs FOR SHARE,
 * since the table-level lock is the same either way.
 */
bool
isLockedRefname(ParseState *pstate, const char *refname)
{
	ListCell   *l;

	/*
	 * If we are in a subquery specified as locked FOR UPDATE/SHARE from
	 * parent level, then act as though there's a generic FOR UPDATE here.
	 */
	if (pstate->p_locked_from_parent)
		return true;

	foreach(l, pstate->p_locking_clause)
	{
		LockingClause *lc = (LockingClause *) lfirst(l);

		if (lc->lockedRels == NIL)
		{
			/* all tables used in query */
			return true;
		}
		else
		{
			/* just the named tables */
			ListCell   *l2;

			foreach(l2, lc->lockedRels)
			{
				RangeVar   *thisrel = (RangeVar *) lfirst(l2);

				if (strcmp(refname, thisrel->relname) == 0)
					return true;
			}
		}
	}
	return false;
}

/*
 * Add the given RTE as a top-level entry in the pstate's join list
 * and/or namespace list.  (We assume caller has checked for any
 * namespace conflicts.)  The RTE is always marked as unconditionally
 * visible, that is, not LATERAL-only.
 */
void
addRTEtoQuery(ParseState *pstate, RangeTblEntry *rte,
			  bool addToJoinList,
			  bool addToRelNameSpace, bool addToVarNameSpace)
{
	if (addToJoinList)
	{
		int			rtindex = RTERangeTablePosn(pstate, rte, NULL);
		RangeTblRef *rtr = makeNode(RangeTblRef);

		rtr->rtindex = rtindex;
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	}
	if (addToRelNameSpace || addToVarNameSpace)
	{
		ParseNamespaceItem *nsitem;

		nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
		nsitem->p_rte = rte;
		nsitem->p_rel_visible = addToRelNameSpace;
		nsitem->p_cols_visible = addToVarNameSpace;
		nsitem->p_lateral_only = false;
		nsitem->p_lateral_ok = true;
		pstate->p_namespace = lappend(pstate->p_namespace, nsitem);
	}
}

/*
 * expandRTE -- expand the columns of a rangetable entry
 *
 * This creates lists of an RTE's column names (aliases if provided, else
 * real names) and Vars for each column.  Only user columns are considered.
 * If include_dropped is FALSE then dropped columns are omitted from the
 * results.  If include_dropped is TRUE then empty strings and NULL constants
 * (not Vars!) are returned for dropped columns.
 *
 * rtindex, sublevels_up, and location are the varno, varlevelsup, and location
 * values to use in the created Vars.  Ordinarily rtindex should match the
 * actual position of the RTE in its rangetable.
 *
 * The output lists go into *colnames and *colvars.
 * If only one of the two kinds of output list is needed, pass NULL for the
 * output pointer for the unwanted one.
 */
void
expandRTE(RangeTblEntry *rte, int rtindex, int sublevels_up,
		  int location, bool include_dropped,
		  List **colnames, List **colvars)
{
	int			varattno;

	if (colnames)
		*colnames = NIL;
	if (colvars)
		*colvars = NIL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* Ordinary relation RTE */
			expandRelation(rte->relid, rte->eref,
						   rtindex, sublevels_up, location,
						   include_dropped, colnames, colvars);
			break;
		case RTE_SUBQUERY:
			{
				/* Subquery RTE */
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *tlistitem;

				varattno = 0;
				foreach(tlistitem, rte->subquery->targetList)
				{
					TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

					if (te->resjunk)
						continue;
					varattno++;
					Assert(varattno == te->resno);

					if (colnames)
					{
						/* Assume there is one alias per target item */
						char	   *label = strVal(lfirst(aliasp_item));

						*colnames = lappend(*colnames, makeString(pstrdup(label)));
						aliasp_item = lnext(aliasp_item);
					}

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, varattno,
										  exprType((Node *) te->expr),
										  exprTypmod((Node *) te->expr),
										  exprCollation((Node *) te->expr),
										  sublevels_up);
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_FUNCTION:
			{
				/* Function RTE */
				TypeFuncClass functypclass;
				Oid			funcrettype;
				TupleDesc	tupdesc;

				functypclass = get_expr_result_type(rte->funcexpr,
													&funcrettype,
													&tupdesc);
				if (functypclass == TYPEFUNC_COMPOSITE)
				{
					/* Composite data type, e.g. a table's row type */
					Assert(tupdesc);
					expandTupleDesc(tupdesc, rte->eref,
									rtindex, sublevels_up, location,
									include_dropped, colnames, colvars);
				}
				else if (functypclass == TYPEFUNC_SCALAR)
				{
					/* Base data type, i.e. scalar */
					if (colnames)
						*colnames = lappend(*colnames,
											linitial(rte->eref->colnames));

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, 1,
										  funcrettype, -1,
										  exprCollation(rte->funcexpr),
										  sublevels_up);
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}
				}
				else if (functypclass == TYPEFUNC_RECORD)
				{
					if (colnames)
						*colnames = copyObject(rte->eref->colnames);
					if (colvars)
					{
						ListCell   *l1;
						ListCell   *l2;
						ListCell   *l3;
						int			attnum = 0;

						forthree(l1, rte->funccoltypes,
								 l2, rte->funccoltypmods,
								 l3, rte->funccolcollations)
						{
							Oid			attrtype = lfirst_oid(l1);
							int32		attrtypmod = lfirst_int(l2);
							Oid			attrcollation = lfirst_oid(l3);
							Var		   *varnode;

							attnum++;
							varnode = makeVar(rtindex,
											  attnum,
											  attrtype,
											  attrtypmod,
											  attrcollation,
											  sublevels_up);
							varnode->location = location;
							*colvars = lappend(*colvars, varnode);
						}
					}
				}
				else
				{
					/* addRangeTableEntryForFunction should've caught this */
					elog(ERROR, "function in FROM has unsupported return type");
				}
			}
			break;
		case RTE_VALUES:
			{
				/* Values RTE */
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *lcv;
				ListCell   *lcc;

				varattno = 0;
				forboth(lcv, (List *) linitial(rte->values_lists),
						lcc, rte->values_collations)
				{
					Node	   *col = (Node *) lfirst(lcv);
					Oid			colcollation = lfirst_oid(lcc);

					varattno++;
					if (colnames)
					{
						/* Assume there is one alias per column */
						char	   *label = strVal(lfirst(aliasp_item));

						*colnames = lappend(*colnames,
											makeString(pstrdup(label)));
						aliasp_item = lnext(aliasp_item);
					}

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, varattno,
										  exprType(col),
										  exprTypmod(col),
										  colcollation,
										  sublevels_up);
						varnode->location = location;
						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_JOIN:
			{
				/* Join RTE */
				ListCell   *colname;
				ListCell   *aliasvar;

				Assert(list_length(rte->eref->colnames) == list_length(rte->joinaliasvars));

				varattno = 0;
				forboth(colname, rte->eref->colnames, aliasvar, rte->joinaliasvars)
				{
					Node	   *avar = (Node *) lfirst(aliasvar);

					varattno++;

					/*
					 * During ordinary parsing, there will never be any
					 * deleted columns in the join; but we have to check since
					 * this routine is also used by the rewriter, and joins
					 * found in stored rules might have join columns for
					 * since-deleted columns.  This will be signaled by a null
					 * pointer in the alias-vars list.
					 */
					if (avar == NULL)
					{
						if (include_dropped)
						{
							if (colnames)
								*colnames = lappend(*colnames,
													makeString(pstrdup("")));
							if (colvars)
							{
								/*
								 * Can't use join's column type here (it might
								 * be dropped!); but it doesn't really matter
								 * what type the Const claims to be.
								 */
								*colvars = lappend(*colvars,
												   makeNullConst(INT4OID, -1,
																 InvalidOid));
							}
						}
						continue;
					}

					if (colnames)
					{
						char	   *label = strVal(lfirst(colname));

						*colnames = lappend(*colnames,
											makeString(pstrdup(label)));
					}

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, varattno,
										  exprType(avar),
										  exprTypmod(avar),
										  exprCollation(avar),
										  sublevels_up);
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_CTE:
			{
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *lct;
				ListCell   *lcm;
				ListCell   *lcc;

				varattno = 0;
				forthree(lct, rte->ctecoltypes,
						 lcm, rte->ctecoltypmods,
						 lcc, rte->ctecolcollations)
				{
					Oid			coltype = lfirst_oid(lct);
					int32		coltypmod = lfirst_int(lcm);
					Oid			colcoll = lfirst_oid(lcc);

					varattno++;

					if (colnames)
					{
						/* Assume there is one alias per output column */
						char	   *label = strVal(lfirst(aliasp_item));

						*colnames = lappend(*colnames, makeString(pstrdup(label)));
						aliasp_item = lnext(aliasp_item);
					}

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, varattno,
										  coltype, coltypmod, colcoll,
										  sublevels_up);
						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
	}
}

/*
 * expandRelation -- expandRTE subroutine
 */
static void
expandRelation(Oid relid, Alias *eref, int rtindex, int sublevels_up,
			   int location, bool include_dropped,
			   List **colnames, List **colvars)
{
	Relation	rel;

	/* Get the tupledesc and turn it over to expandTupleDesc */
	rel = relation_open(relid, AccessShareLock);
	expandTupleDesc(rel->rd_att, eref, rtindex, sublevels_up,
					location, include_dropped,
					colnames, colvars);
	relation_close(rel, AccessShareLock);
}

/*
 * expandTupleDesc -- expandRTE subroutine
 */
static void
expandTupleDesc(TupleDesc tupdesc, Alias *eref,
				int rtindex, int sublevels_up,
				int location, bool include_dropped,
				List **colnames, List **colvars)
{
	int			maxattrs = tupdesc->natts;
	int			numaliases = list_length(eref->colnames);
	int			varattno;

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = tupdesc->attrs[varattno];

		if (attr->attisdropped)
		{
			if (include_dropped)
			{
				if (colnames)
					*colnames = lappend(*colnames, makeString(pstrdup("")));
				if (colvars)
				{
					/*
					 * can't use atttypid here, but it doesn't really matter
					 * what type the Const claims to be.
					 */
					*colvars = lappend(*colvars,
									 makeNullConst(INT4OID, -1, InvalidOid));
				}
			}
			continue;
		}

		if (colnames)
		{
			char	   *label;

			if (varattno < numaliases)
				label = strVal(list_nth(eref->colnames, varattno));
			else
				label = NameStr(attr->attname);
			*colnames = lappend(*colnames, makeString(pstrdup(label)));
		}

		if (colvars)
		{
			Var		   *varnode;

			varnode = makeVar(rtindex, attr->attnum,
							  attr->atttypid, attr->atttypmod,
							  attr->attcollation,
							  sublevels_up);
			varnode->location = location;

			*colvars = lappend(*colvars, varnode);
		}
	}
}

/*
 * expandRelAttrs -
 *	  Workhorse for "*" expansion: produce a list of targetentries
 *	  for the attributes of the RTE
 *
 * As with expandRTE, rtindex/sublevels_up determine the varno/varlevelsup
 * fields of the Vars produced, and location sets their location.
 * pstate->p_next_resno determines the resnos assigned to the TLEs.
 * The referenced columns are marked as requiring SELECT access.
 */
List *
expandRelAttrs(ParseState *pstate, RangeTblEntry *rte,
			   int rtindex, int sublevels_up, int location)
{
	List	   *names,
			   *vars;
	ListCell   *name,
			   *var;
	List	   *te_list = NIL;

	expandRTE(rte, rtindex, sublevels_up, location, false,
			  &names, &vars);

	/*
	 * Require read access to the table.  This is normally redundant with the
	 * markVarForSelectPriv calls below, but not if the table has zero
	 * columns.
	 */
	rte->requiredPerms |= ACL_SELECT;

	forboth(name, names, var, vars)
	{
		char	   *label = strVal(lfirst(name));
		Var		   *varnode = (Var *) lfirst(var);
		TargetEntry *te;

		te = makeTargetEntry((Expr *) varnode,
							 (AttrNumber) pstate->p_next_resno++,
							 label,
							 false);
		te_list = lappend(te_list, te);

		/* Require read access to each column */
		markVarForSelectPriv(pstate, varnode, rte);
	}

	Assert(name == NULL && var == NULL);		/* lists not the same length? */

	return te_list;
}

/*
 * get_rte_attribute_name
 *		Get an attribute name from a RangeTblEntry
 *
 * This is unlike get_attname() because we use aliases if available.
 * In particular, it will work on an RTE for a subselect or join, whereas
 * get_attname() only works on real relations.
 *
 * "*" is returned if the given attnum is InvalidAttrNumber --- this case
 * occurs when a Var represents a whole tuple of a relation.
 */
char *
get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum)
{
	if (attnum == InvalidAttrNumber)
		return "*";

	/*
	 * If there is a user-written column alias, use it.
	 */
	if (rte->alias &&
		attnum > 0 && attnum <= list_length(rte->alias->colnames))
		return strVal(list_nth(rte->alias->colnames, attnum - 1));

	/*
	 * If the RTE is a relation, go to the system catalogs not the
	 * eref->colnames list.  This is a little slower but it will give the
	 * right answer if the column has been renamed since the eref list was
	 * built (which can easily happen for rules).
	 */
	if (rte->rtekind == RTE_RELATION)
		return get_relid_attribute_name(rte->relid, attnum);

	/*
	 * Otherwise use the column name from eref.  There should always be one.
	 */
	if (attnum > 0 && attnum <= list_length(rte->eref->colnames))
		return strVal(list_nth(rte->eref->colnames, attnum - 1));

	/* else caller gave us a bogus attnum */
	elog(ERROR, "invalid attnum %d for rangetable entry %s",
		 attnum, rte->eref->aliasname);
	return NULL;				/* keep compiler quiet */
}

/*
 * get_rte_attribute_type
 *		Get attribute type/typmod/collation information from a RangeTblEntry
 */
void
get_rte_attribute_type(RangeTblEntry *rte, AttrNumber attnum,
					   Oid *vartype, int32 *vartypmod, Oid *varcollid)
{
	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				/* Plain relation RTE --- get the attribute's type info */
				HeapTuple	tp;
				Form_pg_attribute att_tup;

				tp = SearchSysCache2(ATTNUM,
									 ObjectIdGetDatum(rte->relid),
									 Int16GetDatum(attnum));
				if (!HeapTupleIsValid(tp))		/* shouldn't happen */
					elog(ERROR, "cache lookup failed for attribute %d of relation %u",
						 attnum, rte->relid);
				att_tup = (Form_pg_attribute) GETSTRUCT(tp);

				/*
				 * If dropped column, pretend it ain't there.  See notes in
				 * scanRTEForColumn.
				 */
				if (att_tup->attisdropped)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   NameStr(att_tup->attname),
						   get_rel_name(rte->relid))));
				*vartype = att_tup->atttypid;
				*vartypmod = att_tup->atttypmod;
				*varcollid = att_tup->attcollation;
				ReleaseSysCache(tp);
			}
			break;
		case RTE_SUBQUERY:
			{
				/* Subselect RTE --- get type info from subselect's tlist */
				TargetEntry *te = get_tle_by_resno(rte->subquery->targetList,
												   attnum);

				if (te == NULL || te->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				*vartype = exprType((Node *) te->expr);
				*vartypmod = exprTypmod((Node *) te->expr);
				*varcollid = exprCollation((Node *) te->expr);
			}
			break;
		case RTE_FUNCTION:
			{
				/* Function RTE */
				TypeFuncClass functypclass;
				Oid			funcrettype;
				TupleDesc	tupdesc;

				functypclass = get_expr_result_type(rte->funcexpr,
													&funcrettype,
													&tupdesc);

				if (functypclass == TYPEFUNC_COMPOSITE)
				{
					/* Composite data type, e.g. a table's row type */
					Form_pg_attribute att_tup;

					Assert(tupdesc);
					/* this is probably a can't-happen case */
					if (attnum < 1 || attnum > tupdesc->natts)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("column %d of relation \"%s\" does not exist",
							   attnum,
							   rte->eref->aliasname)));

					att_tup = tupdesc->attrs[attnum - 1];

					/*
					 * If dropped column, pretend it ain't there.  See notes
					 * in scanRTEForColumn.
					 */
					if (att_tup->attisdropped)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										NameStr(att_tup->attname),
										rte->eref->aliasname)));
					*vartype = att_tup->atttypid;
					*vartypmod = att_tup->atttypmod;
					*varcollid = att_tup->attcollation;
				}
				else if (functypclass == TYPEFUNC_SCALAR)
				{
					/* Base data type, i.e. scalar */
					*vartype = funcrettype;
					*vartypmod = -1;
					*varcollid = exprCollation(rte->funcexpr);
				}
				else if (functypclass == TYPEFUNC_RECORD)
				{
					*vartype = list_nth_oid(rte->funccoltypes, attnum - 1);
					*vartypmod = list_nth_int(rte->funccoltypmods, attnum - 1);
					*varcollid = list_nth_oid(rte->funccolcollations, attnum - 1);
				}
				else
				{
					/* addRangeTableEntryForFunction should've caught this */
					elog(ERROR, "function in FROM has unsupported return type");
				}
			}
			break;
		case RTE_VALUES:
			{
				/* Values RTE --- get type info from first sublist */
				/* collation is stored separately, though */
				List	   *collist = (List *) linitial(rte->values_lists);
				Node	   *col;

				if (attnum < 1 || attnum > list_length(collist))
					elog(ERROR, "values list %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				col = (Node *) list_nth(collist, attnum - 1);
				*vartype = exprType(col);
				*vartypmod = exprTypmod(col);
				*varcollid = list_nth_oid(rte->values_collations, attnum - 1);
			}
			break;
		case RTE_JOIN:
			{
				/*
				 * Join RTE --- get type info from join RTE's alias variable
				 */
				Node	   *aliasvar;

				Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
				aliasvar = (Node *) list_nth(rte->joinaliasvars, attnum - 1);
				Assert(aliasvar != NULL);
				*vartype = exprType(aliasvar);
				*vartypmod = exprTypmod(aliasvar);
				*varcollid = exprCollation(aliasvar);
			}
			break;
		case RTE_CTE:
			{
				/* CTE RTE --- get type info from lists in the RTE */
				Assert(attnum > 0 && attnum <= list_length(rte->ctecoltypes));
				*vartype = list_nth_oid(rte->ctecoltypes, attnum - 1);
				*vartypmod = list_nth_int(rte->ctecoltypmods, attnum - 1);
				*varcollid = list_nth_oid(rte->ctecolcollations, attnum - 1);
			}
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
	}
}

/*
 * get_rte_attribute_is_dropped
 *		Check whether attempted attribute ref is to a dropped column
 */
bool
get_rte_attribute_is_dropped(RangeTblEntry *rte, AttrNumber attnum)
{
	bool		result;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				/*
				 * Plain relation RTE --- get the attribute's catalog entry
				 */
				HeapTuple	tp;
				Form_pg_attribute att_tup;

				tp = SearchSysCache2(ATTNUM,
									 ObjectIdGetDatum(rte->relid),
									 Int16GetDatum(attnum));
				if (!HeapTupleIsValid(tp))		/* shouldn't happen */
					elog(ERROR, "cache lookup failed for attribute %d of relation %u",
						 attnum, rte->relid);
				att_tup = (Form_pg_attribute) GETSTRUCT(tp);
				result = att_tup->attisdropped;
				ReleaseSysCache(tp);
			}
			break;
		case RTE_SUBQUERY:
		case RTE_VALUES:
		case RTE_CTE:
			/* Subselect, Values, CTE RTEs never have dropped columns */
			result = false;
			break;
		case RTE_JOIN:
			{
				/*
				 * A join RTE would not have dropped columns when constructed,
				 * but one in a stored rule might contain columns that were
				 * dropped from the underlying tables, if said columns are
				 * nowhere explicitly referenced in the rule.  This will be
				 * signaled to us by a null pointer in the joinaliasvars list.
				 */
				Var		   *aliasvar;

				if (attnum <= 0 ||
					attnum > list_length(rte->joinaliasvars))
					elog(ERROR, "invalid varattno %d", attnum);
				aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);

				result = (aliasvar == NULL);
			}
			break;
		case RTE_FUNCTION:
			{
				/* Function RTE */
				Oid			funcrettype = exprType(rte->funcexpr);
				Oid			funcrelid = typeidTypeRelid(funcrettype);

				if (OidIsValid(funcrelid))
				{
					/*
					 * Composite data type, i.e. a table's row type
					 *
					 * Same as ordinary relation RTE
					 */
					HeapTuple	tp;
					Form_pg_attribute att_tup;

					tp = SearchSysCache2(ATTNUM,
										 ObjectIdGetDatum(funcrelid),
										 Int16GetDatum(attnum));
					if (!HeapTupleIsValid(tp))	/* shouldn't happen */
						elog(ERROR, "cache lookup failed for attribute %d of relation %u",
							 attnum, funcrelid);
					att_tup = (Form_pg_attribute) GETSTRUCT(tp);
					result = att_tup->attisdropped;
					ReleaseSysCache(tp);
				}
				else
				{
					/*
					 * Must be a base data type, i.e. scalar
					 */
					result = false;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
			result = false;		/* keep compiler quiet */
	}

	return result;
}

/*
 * Given a targetlist and a resno, return the matching TargetEntry
 *
 * Returns NULL if resno is not present in list.
 *
 * Note: we need to search, rather than just indexing with list_nth(),
 * because not all tlists are sorted by resno.
 */
TargetEntry *
get_tle_by_resno(List *tlist, AttrNumber resno)
{
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resno == resno)
			return tle;
	}
	return NULL;
}

/*
 * Given a Query and rangetable index, return relation's RowMarkClause if any
 *
 * Returns NULL if relation is not selected FOR UPDATE/SHARE
 */
RowMarkClause *
get_parse_rowmark(Query *qry, Index rtindex)
{
	ListCell   *l;

	foreach(l, qry->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (rc->rti == rtindex)
			return rc;
	}
	return NULL;
}

/*
 *	given relation and att name, return attnum of variable
 *
 *	Returns InvalidAttrNumber if the attr doesn't exist (or is dropped).
 *
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version get_attnum()
 *	for access to non-opened relations.
 */
int
attnameAttNum(Relation rd, const char *attname, bool sysColOK)
{
	int			i;

	for (i = 0; i < rd->rd_rel->relnatts; i++)
	{
		Form_pg_attribute att = rd->rd_att->attrs[i];

		if (namestrcmp(&(att->attname), attname) == 0 && !att->attisdropped)
			return i + 1;
	}

	if (sysColOK)
	{
		if ((i = specialAttNum(attname)) != InvalidAttrNumber)
		{
			if (i != ObjectIdAttributeNumber || rd->rd_rel->relhasoids)
				return i;
		}
	}

	/* on failure */
	return InvalidAttrNumber;
}

/* specialAttNum()
 *
 * Check attribute name to see if it is "special", e.g. "oid".
 * - thomas 2000-02-07
 *
 * Note: this only discovers whether the name could be a system attribute.
 * Caller needs to verify that it really is an attribute of the rel,
 * at least in the case of "oid", which is now optional.
 */
static int
specialAttNum(const char *attname)
{
	Form_pg_attribute sysatt;

	sysatt = SystemAttributeByName(attname,
								   true /* "oid" will be accepted */ );
	if (sysatt != NULL)
		return sysatt->attnum;
	return InvalidAttrNumber;
}


/*
 * given attribute id, return name of that attribute
 *
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version get_atttype()
 *	for access to non-opened relations.
 */
Name
attnumAttName(Relation rd, int attid)
{
	if (attid <= 0)
	{
		Form_pg_attribute sysatt;

		sysatt = SystemAttributeDefinition(attid, rd->rd_rel->relhasoids);
		return &sysatt->attname;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return &rd->rd_att->attrs[attid - 1]->attname;
}

/*
 * given attribute id, return type of that attribute
 *
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version get_atttype()
 *	for access to non-opened relations.
 */
Oid
attnumTypeId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		Form_pg_attribute sysatt;

		sysatt = SystemAttributeDefinition(attid, rd->rd_rel->relhasoids);
		return sysatt->atttypid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return rd->rd_att->attrs[attid - 1]->atttypid;
}

/*
 * given attribute id, return collation of that attribute
 *
 *	This should only be used if the relation is already heap_open()'ed.
 */
Oid
attnumCollationId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		/* All system attributes are of noncollatable types. */
		return InvalidOid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return rd->rd_att->attrs[attid - 1]->attcollation;
}

/*
 * Generate a suitable error about a missing RTE.
 *
 * Since this is a very common type of error, we work rather hard to
 * produce a helpful message.
 */
void
errorMissingRTE(ParseState *pstate, RangeVar *relation)
{
	RangeTblEntry *rte;
	int			sublevels_up;
	const char *badAlias = NULL;

	/*
	 * Check to see if there are any potential matches in the query's
	 * rangetable.	(Note: cases involving a bad schema name in the RangeVar
	 * will throw error immediately here.  That seems OK.)
	 */
	rte = searchRangeTableForRel(pstate, relation);

	/*
	 * If we found a match that has an alias and the alias is visible in the
	 * namespace, then the problem is probably use of the relation's real name
	 * instead of its alias, ie "SELECT foo.* FROM foo f". This mistake is
	 * common enough to justify a specific hint.
	 *
	 * If we found a match that doesn't meet those criteria, assume the
	 * problem is illegal use of a relation outside its scope, as in the
	 * MySQL-ism "SELECT ... FROM a, b LEFT JOIN c ON (a.x = c.y)".
	 */
	if (rte && rte->alias &&
		strcmp(rte->eref->aliasname, relation->relname) != 0 &&
		refnameRangeTblEntry(pstate, NULL, rte->eref->aliasname,
							 relation->location,
							 &sublevels_up) == rte)
		badAlias = rte->eref->aliasname;

	if (rte)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
			errmsg("invalid reference to FROM-clause entry for table \"%s\"",
				   relation->relname),
				 (badAlias ?
			errhint("Perhaps you meant to reference the table alias \"%s\".",
					badAlias) :
				  errhint("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						  rte->eref->aliasname)),
				 parser_errposition(pstate, relation->location)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("missing FROM-clause entry for table \"%s\"",
						relation->relname),
				 parser_errposition(pstate, relation->location)));
}

/*
 * Generate a suitable error about a missing column.
 *
 * Since this is a very common type of error, we work rather hard to
 * produce a helpful message.
 */
void
errorMissingColumn(ParseState *pstate,
				   char *relname, char *colname, int location)
{
	RangeTblEntry *rte;

	/*
	 * If relname was given, just play dumb and report it.	(In practice, a
	 * bad qualification name should end up at errorMissingRTE, not here, so
	 * no need to work hard on this case.)
	 */
	if (relname)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %s.%s does not exist", relname, colname),
				 parser_errposition(pstate, location)));

	/*
	 * Otherwise, search the entire rtable looking for possible matches.  If
	 * we find one, emit a hint about it.
	 *
	 * TODO: improve this code (and also errorMissingRTE) to mention using
	 * LATERAL if appropriate.
	 */
	rte = searchRangeTableForCol(pstate, colname, location);

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_COLUMN),
			 errmsg("column \"%s\" does not exist", colname),
			 rte ? errhint("There is a column named \"%s\" in table \"%s\", but it cannot be referenced from this part of the query.",
						   colname, rte->eref->aliasname) : 0,
			 parser_errposition(pstate, location)));
}


/*
 * Examine a fully-parsed query, and return TRUE iff any relation underlying
 * the query is a temporary relation (table, view, or materialized view).
 */
bool
isQueryUsingTempRelation(Query *query)
{
	return isQueryUsingTempRelation_walker((Node *) query, NULL);
}

static bool
isQueryUsingTempRelation_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);

			if (rte->rtekind == RTE_RELATION)
			{
				Relation	rel = heap_open(rte->relid, AccessShareLock);
				char		relpersistence = rel->rd_rel->relpersistence;

				heap_close(rel, AccessShareLock);
				if (relpersistence == RELPERSISTENCE_TEMP)
					return true;
			}
		}

		return query_tree_walker(query,
								 isQueryUsingTempRelation_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  isQueryUsingTempRelation_walker,
								  context);
}
