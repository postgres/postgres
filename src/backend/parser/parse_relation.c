/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  parser support routines dealing with relations
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_enr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * Support for fuzzily matching columns.
 *
 * This is for building diagnostic messages, where multiple or non-exact
 * matching attributes are of interest.
 *
 * "distance" is the current best fuzzy-match distance if rfirst isn't NULL,
 * otherwise it is the maximum acceptable distance plus 1.
 *
 * rfirst/first record the closest non-exact match so far, and distance
 * is its distance from the target name.  If we have found a second non-exact
 * match of exactly the same distance, rsecond/second record that.  (If
 * we find three of the same distance, we conclude that "distance" is not
 * a tight enough bound for a useful hint and clear rfirst/rsecond again.
 * Only if we later find something closer will we re-populate rfirst.)
 *
 * rexact1/exact1 record the location of the first exactly-matching column,
 * if any.  If we find multiple exact matches then rexact2/exact2 record
 * another one (we don't especially care which).  Currently, these get
 * populated independently of the fuzzy-match fields.
 */
typedef struct
{
	int			distance;		/* Current or limit distance */
	RangeTblEntry *rfirst;		/* RTE of closest non-exact match, or NULL */
	AttrNumber	first;			/* Col index in rfirst */
	RangeTblEntry *rsecond;		/* RTE of another non-exact match w/same dist */
	AttrNumber	second;			/* Col index in rsecond */
	RangeTblEntry *rexact1;		/* RTE of first exact match, or NULL */
	AttrNumber	exact1;			/* Col index in rexact1 */
	RangeTblEntry *rexact2;		/* RTE of second exact match, or NULL */
	AttrNumber	exact2;			/* Col index in rexact2 */
} FuzzyAttrMatchState;

#define MAX_FUZZY_DISTANCE				3


static ParseNamespaceItem *scanNameSpaceForRefname(ParseState *pstate,
												   const char *refname,
												   int location);
static ParseNamespaceItem *scanNameSpaceForRelid(ParseState *pstate, Oid relid,
												 int location);
static void check_lateral_ref_ok(ParseState *pstate, ParseNamespaceItem *nsitem,
								 int location);
static int	scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
							 Alias *eref,
							 const char *colname, int location,
							 int fuzzy_rte_penalty,
							 FuzzyAttrMatchState *fuzzystate);
static void markRTEForSelectPriv(ParseState *pstate,
								 int rtindex, AttrNumber col);
static void expandRelation(Oid relid, Alias *eref,
						   int rtindex, int sublevels_up,
						   int location, bool include_dropped,
						   List **colnames, List **colvars);
static void expandTupleDesc(TupleDesc tupdesc, Alias *eref,
							int count, int offset,
							int rtindex, int sublevels_up,
							int location, bool include_dropped,
							List **colnames, List **colvars);
static int	specialAttNum(const char *attname);
static bool rte_visible_if_lateral(ParseState *pstate, RangeTblEntry *rte);
static bool rte_visible_if_qualified(ParseState *pstate, RangeTblEntry *rte);
static bool isQueryUsingTempRelation_walker(Node *node, void *context);


/*
 * refnameNamespaceItem
 *	  Given a possibly-qualified refname, look to see if it matches any visible
 *	  namespace item.  If so, return a pointer to the nsitem; else return NULL.
 *
 *	  Optionally get nsitem's nesting depth (0 = current) into *sublevels_up.
 *	  If sublevels_up is NULL, only consider items at the current nesting
 *	  level.
 *
 * An unqualified refname (schemaname == NULL) can match any item with matching
 * alias, or matching unqualified relname in the case of alias-less relation
 * items.  It is possible that such a refname matches multiple items in the
 * nearest nesting level that has a match; if so, we report an error via
 * ereport().
 *
 * A qualified refname (schemaname != NULL) can only match a relation item
 * that (a) has no alias and (b) is for the same relation identified by
 * schemaname.refname.  In this case we convert schemaname.refname to a
 * relation OID and search by relid, rather than by alias name.  This is
 * peculiar, but it's what SQL says to do.
 */
ParseNamespaceItem *
refnameNamespaceItem(ParseState *pstate,
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
		ParseNamespaceItem *result;

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
 * Search the query's table namespace for an item matching the
 * given unqualified refname.  Return the nsitem if a unique match, or NULL
 * if no match.  Raise error if multiple matches.
 *
 * Note: it might seem that we shouldn't have to worry about the possibility
 * of multiple matches; after all, the SQL standard disallows duplicate table
 * aliases within a given SELECT level.  Historically, however, Postgres has
 * been laxer than that.  For example, we allow
 *		SELECT ... FROM tab1 x CROSS JOIN (tab2 x CROSS JOIN tab3 y) z
 * on the grounds that the aliased join (z) hides the aliases within it,
 * therefore there is no conflict between the two RTEs named "x".  However,
 * if tab3 is a LATERAL subquery, then from within the subquery both "x"es
 * are visible.  Rather than rejecting queries that used to work, we allow
 * this situation, and complain only if there's actually an ambiguous
 * reference to "x".
 */
static ParseNamespaceItem *
scanNameSpaceForRefname(ParseState *pstate, const char *refname, int location)
{
	ParseNamespaceItem *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

		/* Ignore columns-only items */
		if (!nsitem->p_rel_visible)
			continue;
		/* If not inside LATERAL, ignore lateral-only items */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;

		if (strcmp(nsitem->p_names->aliasname, refname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference \"%s\" is ambiguous",
								refname),
						 parser_errposition(pstate, location)));
			check_lateral_ref_ok(pstate, nsitem, location);
			result = nsitem;
		}
	}
	return result;
}

/*
 * Search the query's table namespace for a relation item matching the
 * given relation OID.  Return the nsitem if a unique match, or NULL
 * if no match.  Raise error if multiple matches.
 *
 * See the comments for refnameNamespaceItem to understand why this
 * acts the way it does.
 */
static ParseNamespaceItem *
scanNameSpaceForRelid(ParseState *pstate, Oid relid, int location)
{
	ParseNamespaceItem *result = NULL;
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
			check_lateral_ref_ok(pstate, nsitem, location);
			result = nsitem;
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
 * Search the query's ephemeral named relation namespace for a relation
 * matching the given unqualified refname.
 */
bool
scanNameSpaceForENR(ParseState *pstate, const char *refname)
{
	return name_matches_visible_ENR(pstate, refname);
}

/*
 * searchRangeTableForRel
 *	  See if any RangeTblEntry could possibly match the RangeVar.
 *	  If so, return a pointer to the RangeTblEntry; else return NULL.
 *
 * This is different from refnameNamespaceItem in that it considers every
 * entry in the ParseState's rangetable(s), not only those that are currently
 * visible in the p_namespace list(s).  This behavior is invalid per the SQL
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
	bool		isenr = false;
	Index		ctelevelsup = 0;
	Index		levelsup;

	/*
	 * If it's an unqualified name, check for possible CTE matches. A CTE
	 * hides any real relation matches.  If no CTE, look for a matching
	 * relation.
	 *
	 * NB: It's not critical that RangeVarGetRelid return the correct answer
	 * here in the face of concurrent DDL.  If it doesn't, the worst case
	 * scenario is a less-clear error message.  Also, the tables involved in
	 * the query are already locked, which reduces the number of cases in
	 * which surprising behavior can occur.  So we do the name lookup
	 * unlocked.
	 */
	if (!relation->schemaname)
	{
		cte = scanNameSpaceForCTE(pstate, refname, &ctelevelsup);
		if (!cte)
			isenr = scanNameSpaceForENR(pstate, refname);
	}

	if (!cte && !isenr)
		relId = RangeVarGetRelid(relation, NoLock, true);

	/* Now look for RTEs matching either the relation/CTE/ENR or the alias */
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
			if (rte->rtekind == RTE_NAMEDTUPLESTORE &&
				isenr &&
				strcmp(rte->enrname, refname) == 0)
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
		const char *aliasname1 = nsitem1->p_names->aliasname;
		ListCell   *l2;

		if (!nsitem1->p_rel_visible)
			continue;

		foreach(l2, namespace2)
		{
			ParseNamespaceItem *nsitem2 = (ParseNamespaceItem *) lfirst(l2);
			RangeTblEntry *rte2 = nsitem2->p_rte;
			const char *aliasname2 = nsitem2->p_names->aliasname;

			if (!nsitem2->p_rel_visible)
				continue;
			if (strcmp(aliasname2, aliasname1) != 0)
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
 * Complain if a namespace item is currently disallowed as a LATERAL reference.
 * This enforces both SQL:2008's rather odd idea of what to do with a LATERAL
 * reference to the wrong side of an outer join, and our own prohibition on
 * referencing the target table of an UPDATE or DELETE as a lateral reference
 * in a FROM/USING clause.
 *
 * Note: the pstate should be the same query level the nsitem was found in.
 *
 * Convenience subroutine to avoid multiple copies of a rather ugly ereport.
 */
static void
check_lateral_ref_ok(ParseState *pstate, ParseNamespaceItem *nsitem,
					 int location)
{
	if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
	{
		/* SQL:2008 demands this be an error, not an invisible item */
		RangeTblEntry *rte = nsitem->p_rte;
		char	   *refname = nsitem->p_names->aliasname;

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						refname),
				 (pstate->p_target_nsitem != NULL &&
				  rte == pstate->p_target_nsitem->p_rte) ?
				 errhint("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						 refname) :
				 errdetail("The combining JOIN type must be INNER or LEFT for a LATERAL reference."),
				 parser_errposition(pstate, location)));
	}
}

/*
 * Given an RT index and nesting depth, find the corresponding
 * ParseNamespaceItem (there must be one).
 */
ParseNamespaceItem *
GetNSItemByRangeTablePosn(ParseState *pstate,
						  int varno,
						  int sublevels_up)
{
	ListCell   *lc;

	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	foreach(lc, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rtindex == varno)
			return nsitem;
	}
	elog(ERROR, "nsitem not found (internal error)");
	return NULL;				/* keep compiler quiet */
}

/*
 * Given an RT index and nesting depth, find the corresponding RTE.
 * (Note that the RTE need not be in the query's namespace.)
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
 * RTE came from.
 */
CommonTableExpr *
GetCTEForRTE(ParseState *pstate, RangeTblEntry *rte, int rtelevelsup)
{
	Index		levelsup;
	ListCell   *lc;

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
 * updateFuzzyAttrMatchState
 *	  Using Levenshtein distance, consider if column is best fuzzy match.
 */
static void
updateFuzzyAttrMatchState(int fuzzy_rte_penalty,
						  FuzzyAttrMatchState *fuzzystate, RangeTblEntry *rte,
						  const char *actual, const char *match, int attnum)
{
	int			columndistance;
	int			matchlen;

	/* Bail before computing the Levenshtein distance if there's no hope. */
	if (fuzzy_rte_penalty > fuzzystate->distance)
		return;

	/*
	 * Outright reject dropped columns, which can appear here with apparent
	 * empty actual names, per remarks within scanRTEForColumn().
	 */
	if (actual[0] == '\0')
		return;

	/* Use Levenshtein to compute match distance. */
	matchlen = strlen(match);
	columndistance =
		varstr_levenshtein_less_equal(actual, strlen(actual), match, matchlen,
									  1, 1, 1,
									  fuzzystate->distance + 1
									  - fuzzy_rte_penalty,
									  true);

	/*
	 * If more than half the characters are different, don't treat it as a
	 * match, to avoid making ridiculous suggestions.
	 */
	if (columndistance > matchlen / 2)
		return;

	/*
	 * From this point on, we can ignore the distinction between the RTE-name
	 * distance and the column-name distance.
	 */
	columndistance += fuzzy_rte_penalty;

	/*
	 * If the new distance is less than or equal to that of the best match
	 * found so far, update fuzzystate.
	 */
	if (columndistance < fuzzystate->distance)
	{
		/* Store new lowest observed distance as first/only match */
		fuzzystate->distance = columndistance;
		fuzzystate->rfirst = rte;
		fuzzystate->first = attnum;
		fuzzystate->rsecond = NULL;
	}
	else if (columndistance == fuzzystate->distance)
	{
		/* If we already have a match of this distance, update state */
		if (fuzzystate->rsecond != NULL)
		{
			/*
			 * Too many matches at same distance.  Clearly, this value of
			 * distance is too low a bar, so drop these entries while keeping
			 * the current distance value, so that only smaller distances will
			 * be considered interesting.  Only if we find something of lower
			 * distance will we re-populate rfirst (via the stanza above).
			 */
			fuzzystate->rfirst = NULL;
			fuzzystate->rsecond = NULL;
		}
		else if (fuzzystate->rfirst != NULL)
		{
			/* Record as provisional second match */
			fuzzystate->rsecond = rte;
			fuzzystate->second = attnum;
		}
		else
		{
			/*
			 * Do nothing.  When rfirst is NULL, distance is more than what we
			 * want to consider acceptable, so we should ignore this match.
			 */
		}
	}
}

/*
 * scanNSItemForColumn
 *	  Search the column names of a single namespace item for the given name.
 *	  If found, return an appropriate Var node, else return NULL.
 *	  If the name proves ambiguous within this nsitem, raise error.
 *
 * Side effect: if we find a match, mark the corresponding RTE as requiring
 * read access for the column.
 */
Node *
scanNSItemForColumn(ParseState *pstate, ParseNamespaceItem *nsitem,
					int sublevels_up, const char *colname, int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	int			attnum;
	Var		   *var;

	/*
	 * Scan the nsitem's column names (or aliases) for a match.  Complain if
	 * multiple matches.
	 */
	attnum = scanRTEForColumn(pstate, rte, nsitem->p_names,
							  colname, location,
							  0, NULL);

	if (attnum == InvalidAttrNumber)
		return NULL;			/* Return NULL if no match */

	/* In constraint check, no system column is allowed except tableOid */
	if (pstate->p_expr_kind == EXPR_KIND_CHECK_CONSTRAINT &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("system column \"%s\" reference in check constraint is invalid",
						colname),
				 parser_errposition(pstate, location)));

	/* In generated column, no system column is allowed except tableOid */
	if (pstate->p_expr_kind == EXPR_KIND_GENERATED_COLUMN &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use system column \"%s\" in column generation expression",
						colname),
				 parser_errposition(pstate, location)));

	/*
	 * In a MERGE WHEN condition, no system column is allowed except tableOid
	 */
	if (pstate->p_expr_kind == EXPR_KIND_MERGE_WHEN &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use system column \"%s\" in MERGE WHEN condition",
						colname),
				 parser_errposition(pstate, location)));

	/* Found a valid match, so build a Var */
	if (attnum > InvalidAttrNumber)
	{
		/* Get attribute data from the ParseNamespaceColumn array */
		ParseNamespaceColumn *nscol = &nsitem->p_nscolumns[attnum - 1];

		/* Complain if dropped column.  See notes in scanRTEForColumn. */
		if (nscol->p_varno == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colname,
							nsitem->p_names->aliasname)));

		var = makeVar(nscol->p_varno,
					  nscol->p_varattno,
					  nscol->p_vartype,
					  nscol->p_vartypmod,
					  nscol->p_varcollid,
					  sublevels_up);
		/* makeVar doesn't offer parameters for these, so set them by hand: */
		var->varnosyn = nscol->p_varnosyn;
		var->varattnosyn = nscol->p_varattnosyn;
	}
	else
	{
		/* System column, so use predetermined type data */
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attnum);
		var = makeVar(nsitem->p_rtindex,
					  attnum,
					  sysatt->atttypid,
					  sysatt->atttypmod,
					  sysatt->attcollation,
					  sublevels_up);
	}
	var->location = location;

	/* Mark Var if it's nulled by any outer joins */
	markNullableIfNeeded(pstate, var);

	/* Require read access to the column */
	markVarForSelectPriv(pstate, var);

	return (Node *) var;
}

/*
 * scanRTEForColumn
 *	  Search the column names of a single RTE for the given name.
 *	  If found, return the attnum (possibly negative, for a system column);
 *	  else return InvalidAttrNumber.
 *	  If the name proves ambiguous within this RTE, raise error.
 *
 * Actually, we only search the names listed in "eref".  This can be either
 * rte->eref, in which case we are indeed searching all the column names,
 * or for a join it can be rte->join_using_alias, in which case we are only
 * considering the common column names (which are the first N columns of the
 * join, so everything works).
 *
 * pstate and location are passed only for error-reporting purposes.
 *
 * Side effect: if fuzzystate is non-NULL, check non-system columns
 * for an approximate match and update fuzzystate accordingly.
 *
 * Note: this is factored out of scanNSItemForColumn because error message
 * creation may want to check RTEs that are not in the namespace.  To support
 * that usage, minimize the number of validity checks performed here.  It's
 * okay to complain about ambiguous-name cases, though, since if we are
 * working to complain about an invalid name, we've already eliminated that.
 */
static int
scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
				 Alias *eref,
				 const char *colname, int location,
				 int fuzzy_rte_penalty,
				 FuzzyAttrMatchState *fuzzystate)
{
	int			result = InvalidAttrNumber;
	int			attnum = 0;
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
	 * we'll still catch it by virtue of the check in scanNSItemForColumn().
	 * Callers interested in finding match with shortest distance need to
	 * defend against this directly, though.
	 */
	foreach(c, eref->colnames)
	{
		const char *attcolname = strVal(lfirst(c));

		attnum++;
		if (strcmp(attcolname, colname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous",
								colname),
						 parser_errposition(pstate, location)));
			result = attnum;
		}

		/* Update fuzzy match state, if provided. */
		if (fuzzystate != NULL)
			updateFuzzyAttrMatchState(fuzzy_rte_penalty, fuzzystate,
									  rte, attcolname, colname, attnum);
	}

	/*
	 * If we have a unique match, return it.  Note that this allows a user
	 * alias to override a system column name (such as OID) without error.
	 */
	if (result)
		return result;

	/*
	 * If the RTE represents a real relation, consider system column names.
	 * Composites are only used for pseudo-relations like ON CONFLICT's
	 * excluded.
	 */
	if (rte->rtekind == RTE_RELATION &&
		rte->relkind != RELKIND_COMPOSITE_TYPE)
	{
		/* quick check to see if name could be a system column */
		attnum = specialAttNum(colname);
		if (attnum != InvalidAttrNumber)
		{
			/* now check to see if column actually is defined */
			if (SearchSysCacheExists2(ATTNUM,
									  ObjectIdGetDatum(rte->relid),
									  Int16GetDatum(attnum)))
				result = attnum;
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
colNameToVar(ParseState *pstate, const char *colname, bool localonly,
			 int location)
{
	Node	   *result = NULL;
	int			sublevels_up = 0;
	ParseState *orig_pstate = pstate;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
			Node	   *newresult;

			/* Ignore table-only items */
			if (!nsitem->p_cols_visible)
				continue;
			/* If not inside LATERAL, ignore lateral-only items */
			if (nsitem->p_lateral_only && !pstate->p_lateral_active)
				continue;

			/* use orig_pstate here for consistency with other callers */
			newresult = scanNSItemForColumn(orig_pstate, nsitem, sublevels_up,
											colname, location);

			if (newresult)
			{
				if (result)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("column reference \"%s\" is ambiguous",
									colname),
							 parser_errposition(pstate, location)));
				check_lateral_ref_ok(pstate, nsitem, location);
				result = newresult;
			}
		}

		if (result != NULL || localonly)
			break;				/* found, or don't want to look at parent */

		pstate = pstate->parentParseState;
		sublevels_up++;
	}

	return result;
}

/*
 * searchRangeTableForCol
 *	  See if any RangeTblEntry could possibly provide the given column name (or
 *	  find the best match available).  Returns state with relevant details.
 *
 * This is different from colNameToVar in that it considers every entry in
 * the ParseState's rangetable(s), not only those that are currently visible
 * in the p_namespace list(s).  This behavior is invalid per the SQL spec,
 * and it may give ambiguous results (since there might be multiple equally
 * valid matches).  This must be used ONLY as a heuristic in giving suitable
 * error messages.  See errorMissingColumn.
 *
 * This function is also different in that it will consider approximate
 * matches -- if the user entered an alias/column pair that is only slightly
 * different from a valid pair, we may be able to infer what they meant to
 * type and provide a reasonable hint.  We return a FuzzyAttrMatchState
 * struct providing information about both exact and approximate matches.
 */
static FuzzyAttrMatchState *
searchRangeTableForCol(ParseState *pstate, const char *alias, const char *colname,
					   int location)
{
	ParseState *orig_pstate = pstate;
	FuzzyAttrMatchState *fuzzystate = palloc(sizeof(FuzzyAttrMatchState));

	fuzzystate->distance = MAX_FUZZY_DISTANCE + 1;
	fuzzystate->rfirst = NULL;
	fuzzystate->rsecond = NULL;
	fuzzystate->rexact1 = NULL;
	fuzzystate->rexact2 = NULL;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
			int			fuzzy_rte_penalty = 0;
			int			attnum;

			/*
			 * Typically, it is not useful to look for matches within join
			 * RTEs; they effectively duplicate other RTEs for our purposes,
			 * and if a match is chosen from a join RTE, an unhelpful alias is
			 * displayed in the final diagnostic message.
			 */
			if (rte->rtekind == RTE_JOIN)
				continue;

			/*
			 * If the user didn't specify an alias, then matches against one
			 * RTE are as good as another.  But if the user did specify an
			 * alias, then we want at least a fuzzy - and preferably an exact
			 * - match for the range table entry.
			 */
			if (alias != NULL)
				fuzzy_rte_penalty =
					varstr_levenshtein_less_equal(alias, strlen(alias),
												  rte->eref->aliasname,
												  strlen(rte->eref->aliasname),
												  1, 1, 1,
												  MAX_FUZZY_DISTANCE + 1,
												  true);

			/*
			 * Scan for a matching column, and update fuzzystate.  Non-exact
			 * matches are dealt with inside scanRTEForColumn, but exact
			 * matches are handled here.  (There won't be more than one exact
			 * match in the same RTE, else we'd have thrown error earlier.)
			 */
			attnum = scanRTEForColumn(orig_pstate, rte, rte->eref,
									  colname, location,
									  fuzzy_rte_penalty, fuzzystate);
			if (attnum != InvalidAttrNumber && fuzzy_rte_penalty == 0)
			{
				if (fuzzystate->rexact1 == NULL)
				{
					fuzzystate->rexact1 = rte;
					fuzzystate->exact1 = attnum;
				}
				else
				{
					/* Needn't worry about overwriting previous rexact2 */
					fuzzystate->rexact2 = rte;
					fuzzystate->exact2 = attnum;
				}
			}
		}

		pstate = pstate->parentParseState;
	}

	return fuzzystate;
}

/*
 * markNullableIfNeeded
 *		If the RTE referenced by the Var is nullable by outer join(s)
 *		at this point in the query, set var->varnullingrels to show that.
 */
void
markNullableIfNeeded(ParseState *pstate, Var *var)
{
	int			rtindex = var->varno;
	Bitmapset  *relids;

	/* Find the appropriate pstate */
	for (int lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;

	/* Find currently-relevant join relids for the Var's rel */
	if (rtindex > 0 && rtindex <= list_length(pstate->p_nullingrels))
		relids = (Bitmapset *) list_nth(pstate->p_nullingrels, rtindex - 1);
	else
		relids = NULL;

	/*
	 * Merge with any already-declared nulling rels.  (Typically there won't
	 * be any, but let's get it right if there are.)
	 */
	if (relids != NULL)
		var->varnullingrels = bms_union(var->varnullingrels, relids);
}

/*
 * markRTEForSelectPriv
 *	   Mark the specified column of the RTE with index rtindex
 *	   as requiring SELECT privilege
 *
 * col == InvalidAttrNumber means a "whole row" reference
 */
static void
markRTEForSelectPriv(ParseState *pstate, int rtindex, AttrNumber col)
{
	RangeTblEntry *rte = rt_fetch(rtindex, pstate->p_rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		RTEPermissionInfo *perminfo;

		/* Make sure the rel as a whole is marked for SELECT access */
		perminfo = getRTEPermissionInfo(pstate->p_rteperminfos, rte);
		perminfo->requiredPerms |= ACL_SELECT;
		/* Must offset the attnum to fit in a bitmapset */
		perminfo->selectedCols =
			bms_add_member(perminfo->selectedCols,
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
				j = list_nth_node(JoinExpr, pstate->p_joinexprs, rtindex - 1);
			else
				j = NULL;
			if (j == NULL)
				elog(ERROR, "could not find JoinExpr for whole-row reference");

			/* Note: we can't see FromExpr here */
			if (IsA(j->larg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else if (IsA(j->larg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->larg));
			if (IsA(j->rarg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else if (IsA(j->rarg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->rarg));
		}
		else
		{
			/*
			 * Join alias Vars for ordinary columns must refer to merged JOIN
			 * USING columns.  We don't need to do anything here, because the
			 * join input columns will also be referenced in the join's qual
			 * clause, and will get marked for select privilege there.
			 */
		}
	}
	/* other RTE types don't require privilege marking */
}

/*
 * markVarForSelectPriv
 *	   Mark the RTE referenced by the Var as requiring SELECT privilege
 *	   for the Var's column (the Var could be a whole-row Var, too)
 */
void
markVarForSelectPriv(ParseState *pstate, Var *var)
{
	Index		lv;

	Assert(IsA(var, Var));
	/* Find the appropriate pstate if it's an uplevel Var */
	for (lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;
	markRTEForSelectPriv(pstate, var->varno, var->varattno);
}

/*
 * buildRelationAliases
 *		Construct the eref column name list for a relation RTE.
 *		This code is also used for function RTEs.
 *
 * tupdesc: the physical column information
 * alias: the user-supplied alias, or NULL if none
 * eref: the eref Alias to store column names in
 *
 * eref->colnames is filled in.  Also, alias->colnames is rebuilt to insert
 * empty strings for any dropped columns, so that it will be one-to-one with
 * physical column numbers.
 *
 * It is an error for there to be more aliases present than required.
 */
static void
buildRelationAliases(TupleDesc tupdesc, Alias *alias, Alias *eref)
{
	int			maxattrs = tupdesc->natts;
	List	   *aliaslist;
	ListCell   *aliaslc;
	int			numaliases;
	int			varattno;
	int			numdropped = 0;

	Assert(eref->colnames == NIL);

	if (alias)
	{
		aliaslist = alias->colnames;
		aliaslc = list_head(aliaslist);
		numaliases = list_length(aliaslist);
		/* We'll rebuild the alias colname list */
		alias->colnames = NIL;
	}
	else
	{
		aliaslist = NIL;
		aliaslc = NULL;
		numaliases = 0;
	}

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);
		String	   *attrname;

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
			attrname = lfirst_node(String, aliaslc);
			aliaslc = lnext(aliaslist, aliaslc);
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
 * chooseScalarFunctionAlias
 *		Select the column alias for a function in a function RTE,
 *		when the function returns a scalar type (not composite or RECORD).
 *
 * funcexpr: transformed expression tree for the function call
 * funcname: function name (as determined by FigureColname)
 * alias: the user-supplied alias for the RTE, or NULL if none
 * nfuncs: the number of functions appearing in the function RTE
 *
 * Note that the name we choose might be overridden later, if the user-given
 * alias includes column alias names.  That's of no concern here.
 */
static char *
chooseScalarFunctionAlias(Node *funcexpr, char *funcname,
						  Alias *alias, int nfuncs)
{
	char	   *pname;

	/*
	 * If the expression is a simple function call, and the function has a
	 * single OUT parameter that is named, use the parameter's name.
	 */
	if (funcexpr && IsA(funcexpr, FuncExpr))
	{
		pname = get_func_result_name(((FuncExpr *) funcexpr)->funcid);
		if (pname)
			return pname;
	}

	/*
	 * If there's just one function in the RTE, and the user gave an RTE alias
	 * name, use that name.  (This makes FROM func() AS foo use "foo" as the
	 * column name as well as the table alias.)
	 */
	if (nfuncs == 1 && alias)
		return alias->aliasname;

	/*
	 * Otherwise use the function name.
	 */
	return funcname;
}

/*
 * buildNSItemFromTupleDesc
 *		Build a ParseNamespaceItem, given a tupdesc describing the columns.
 *
 * rte: the new RangeTblEntry for the rel
 * rtindex: its index in the rangetable list
 * perminfo: permission list entry for the rel
 * tupdesc: the physical column information
 */
static ParseNamespaceItem *
buildNSItemFromTupleDesc(RangeTblEntry *rte, Index rtindex,
						 RTEPermissionInfo *perminfo,
						 TupleDesc tupdesc)
{
	ParseNamespaceItem *nsitem;
	ParseNamespaceColumn *nscolumns;
	int			maxattrs = tupdesc->natts;
	int			varattno;

	/* colnames must have the same number of entries as the nsitem */
	Assert(maxattrs == list_length(rte->eref->colnames));

	/* extract per-column data from the tupdesc */
	nscolumns = (ParseNamespaceColumn *)
		palloc0(maxattrs * sizeof(ParseNamespaceColumn));

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

		/* For a dropped column, just leave the entry as zeroes */
		if (attr->attisdropped)
			continue;

		nscolumns[varattno].p_varno = rtindex;
		nscolumns[varattno].p_varattno = varattno + 1;
		nscolumns[varattno].p_vartype = attr->atttypid;
		nscolumns[varattno].p_vartypmod = attr->atttypmod;
		nscolumns[varattno].p_varcollid = attr->attcollation;
		nscolumns[varattno].p_varnosyn = rtindex;
		nscolumns[varattno].p_varattnosyn = varattno + 1;
	}

	/* ... and build the nsitem */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_rtindex = rtindex;
	nsitem->p_perminfo = perminfo;
	nsitem->p_nscolumns = nscolumns;
	/* set default visibility flags; might get changed later */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	return nsitem;
}

/*
 * buildNSItemFromLists
 *		Build a ParseNamespaceItem, given column type information in lists.
 *
 * rte: the new RangeTblEntry for the rel
 * rtindex: its index in the rangetable list
 * coltypes: per-column datatype OIDs
 * coltypmods: per-column type modifiers
 * colcollation: per-column collation OIDs
 */
static ParseNamespaceItem *
buildNSItemFromLists(RangeTblEntry *rte, Index rtindex,
					 List *coltypes, List *coltypmods, List *colcollations)
{
	ParseNamespaceItem *nsitem;
	ParseNamespaceColumn *nscolumns;
	int			maxattrs = list_length(coltypes);
	int			varattno;
	ListCell   *lct;
	ListCell   *lcm;
	ListCell   *lcc;

	/* colnames must have the same number of entries as the nsitem */
	Assert(maxattrs == list_length(rte->eref->colnames));

	Assert(maxattrs == list_length(coltypmods));
	Assert(maxattrs == list_length(colcollations));

	/* extract per-column data from the lists */
	nscolumns = (ParseNamespaceColumn *)
		palloc0(maxattrs * sizeof(ParseNamespaceColumn));

	varattno = 0;
	forthree(lct, coltypes,
			 lcm, coltypmods,
			 lcc, colcollations)
	{
		nscolumns[varattno].p_varno = rtindex;
		nscolumns[varattno].p_varattno = varattno + 1;
		nscolumns[varattno].p_vartype = lfirst_oid(lct);
		nscolumns[varattno].p_vartypmod = lfirst_int(lcm);
		nscolumns[varattno].p_varcollid = lfirst_oid(lcc);
		nscolumns[varattno].p_varnosyn = rtindex;
		nscolumns[varattno].p_varattnosyn = varattno + 1;
		varattno++;
	}

	/* ... and build the nsitem */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_rtindex = rtindex;
	nsitem->p_perminfo = NULL;
	nsitem->p_nscolumns = nscolumns;
	/* set default visibility flags; might get changed later */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	return nsitem;
}

/*
 * Open a table during parse analysis
 *
 * This is essentially just the same as table_openrv(), except that it caters
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
	rel = table_openrv_extended(relation, lockmode, true);
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
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * We do not link the ParseNamespaceItem into the pstate here; it's the
 * caller's job to do that in the appropriate way.
 *
 * Note: formerly this checked for refname conflicts, but that's wrong.
 * Caller is responsible for checking for conflicts in the appropriate scope.
 */
ParseNamespaceItem *
addRangeTableEntry(ParseState *pstate,
				   RangeVar *relation,
				   Alias *alias,
				   bool inh,
				   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo;
	char	   *refname = alias ? alias->aliasname : relation->relname;
	LOCKMODE	lockmode;
	Relation	rel;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;

	/*
	 * Identify the type of lock we'll need on this relation.  It's not the
	 * query's target table (that case is handled elsewhere), so we need
	 * either RowShareLock if it's locked by FOR UPDATE/SHARE, or plain
	 * AccessShareLock otherwise.
	 */
	lockmode = isLockedRefname(pstate, refname) ? RowShareLock : AccessShareLock;

	/*
	 * Get the rel's OID.  This access also ensures that we have an up-to-date
	 * relcache entry for the rel.  Since this is typically the first access
	 * to a rel in a statement, we must open the rel with the proper lockmode.
	 */
	rel = parserOpenTable(pstate, relation, lockmode);
	rte->relid = RelationGetRelid(rel);
	rte->inh = inh;
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = lockmode;

	/*
	 * Build the list of effective column names using user-supplied aliases
	 * and/or actual column names.
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * Set flags and initialize access permissions.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	perminfo = addRTEPermissionInfo(&pstate->p_rteperminfos, rte);
	perminfo->requiredPerms = ACL_SELECT;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	nsitem = buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable),
									  perminfo, rel->rd_att);

	/*
	 * Drop the rel refcount, but keep the access lock till end of transaction
	 * so that the table can't be deleted or have its schema modified
	 * underneath us.
	 */
	table_close(rel, NoLock);

	return nsitem;
}

/*
 * Add an entry for a relation to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is just like addRangeTableEntry() except that it makes an RTE
 * given an already-open relation instead of a RangeVar reference.
 *
 * lockmode is the lock type required for query execution; it must be one
 * of AccessShareLock, RowShareLock, or RowExclusiveLock depending on the
 * RTE's role within the query.  The caller must hold that lock mode
 * or a stronger one.
 *
 * Note: properly, lockmode should be declared LOCKMODE not int, but that
 * would require importing storage/lock.h into parse_relation.h.  Since
 * LOCKMODE is typedef'd as int anyway, that seems like overkill.
 */
ParseNamespaceItem *
addRangeTableEntryForRelation(ParseState *pstate,
							  Relation rel,
							  int lockmode,
							  Alias *alias,
							  bool inh,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo;
	char	   *refname = alias ? alias->aliasname : RelationGetRelationName(rel);

	Assert(pstate != NULL);

	Assert(lockmode == AccessShareLock ||
		   lockmode == RowShareLock ||
		   lockmode == RowExclusiveLock);
	Assert(CheckRelationLockedByMe(rel, lockmode, true));

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;
	rte->relid = RelationGetRelid(rel);
	rte->inh = inh;
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = lockmode;

	/*
	 * Build the list of effective column names using user-supplied aliases
	 * and/or actual column names.
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * Set flags and initialize access permissions.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	perminfo = addRTEPermissionInfo(&pstate->p_rteperminfos, rte);
	perminfo->requiredPerms = ACL_SELECT;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable),
									perminfo, rel->rd_att);
}

/*
 * Add an entry for a subquery to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is much like addRangeTableEntry() except that it makes a subquery RTE.
 *
 * If the subquery does not have an alias, the auto-generated relation name in
 * the returned ParseNamespaceItem will be marked as not visible, and so only
 * unqualified references to the subquery columns will be allowed, and the
 * relation name will not conflict with others in the pstate's namespace list.
 */
ParseNamespaceItem *
addRangeTableEntryForSubquery(ParseState *pstate,
							  Query *subquery,
							  Alias *alias,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;
	List	   *coltypes,
			   *coltypmods,
			   *colcollations;
	int			varattno;
	ListCell   *tlistitem;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = subquery;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias("unnamed_subquery", NIL);
	numaliases = list_length(eref->colnames);

	/* fill in any unspecified alias columns, and extract column type info */
	coltypes = coltypmods = colcollations = NIL;
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
		coltypes = lappend_oid(coltypes,
							   exprType((Node *) te->expr));
		coltypmods = lappend_int(coltypmods,
								 exprTypmod((Node *) te->expr));
		colcollations = lappend_oid(colcollations,
									exprCollation((Node *) te->expr));
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, varattno, numaliases)));

	rte->eref = eref;

	/*
	 * Set flags.
	 *
	 * Subqueries are never checked for access rights, so no need to perform
	 * addRTEPermissionInfo().
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	nsitem = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								  coltypes, coltypmods, colcollations);

	/*
	 * Mark it visible as a relation name only if it had a user-written alias.
	 */
	nsitem->p_rel_visible = (alias != NULL);

	return nsitem;
}

/*
 * Add an entry for a function (or functions) to the pstate's range table
 * (p_rtable).  Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is much like addRangeTableEntry() except that it makes a function RTE.
 */
ParseNamespaceItem *
addRangeTableEntryForFunction(ParseState *pstate,
							  List *funcnames,
							  List *funcexprs,
							  List *coldeflists,
							  RangeFunction *rangefunc,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rangefunc->alias;
	Alias	   *eref;
	char	   *aliasname;
	int			nfuncs = list_length(funcexprs);
	TupleDesc  *functupdescs;
	TupleDesc	tupdesc;
	ListCell   *lc1,
			   *lc2,
			   *lc3;
	int			i;
	int			j;
	int			funcno;
	int			natts,
				totalatts;

	Assert(pstate != NULL);

	rte->rtekind = RTE_FUNCTION;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->functions = NIL;		/* we'll fill this list below */
	rte->funcordinality = rangefunc->ordinality;
	rte->alias = alias;

	/*
	 * Choose the RTE alias name.  We default to using the first function's
	 * name even when there's more than one; which is maybe arguable but beats
	 * using something constant like "table".
	 */
	if (alias)
		aliasname = alias->aliasname;
	else
		aliasname = linitial(funcnames);

	eref = makeAlias(aliasname, NIL);
	rte->eref = eref;

	/* Process each function ... */
	functupdescs = (TupleDesc *) palloc(nfuncs * sizeof(TupleDesc));

	totalatts = 0;
	funcno = 0;
	forthree(lc1, funcexprs, lc2, funcnames, lc3, coldeflists)
	{
		Node	   *funcexpr = (Node *) lfirst(lc1);
		char	   *funcname = (char *) lfirst(lc2);
		List	   *coldeflist = (List *) lfirst(lc3);
		RangeTblFunction *rtfunc = makeNode(RangeTblFunction);
		TypeFuncClass functypclass;
		Oid			funcrettype;

		/* Initialize RangeTblFunction node */
		rtfunc->funcexpr = funcexpr;
		rtfunc->funccolnames = NIL;
		rtfunc->funccoltypes = NIL;
		rtfunc->funccoltypmods = NIL;
		rtfunc->funccolcollations = NIL;
		rtfunc->funcparams = NULL;	/* not set until planning */

		/*
		 * Now determine if the function returns a simple or composite type.
		 */
		functypclass = get_expr_result_type(funcexpr,
											&funcrettype,
											&tupdesc);

		/*
		 * A coldeflist is required if the function returns RECORD and hasn't
		 * got a predetermined record type, and is prohibited otherwise.  This
		 * can be a bit confusing, so we expend some effort on delivering a
		 * relevant error message.
		 */
		if (coldeflist != NIL)
		{
			switch (functypclass)
			{
				case TYPEFUNC_RECORD:
					/* ok */
					break;
				case TYPEFUNC_COMPOSITE:
				case TYPEFUNC_COMPOSITE_DOMAIN:

					/*
					 * If the function's raw result type is RECORD, we must
					 * have resolved it using its OUT parameters.  Otherwise,
					 * it must have a named composite type.
					 */
					if (exprType(funcexpr) == RECORDOID)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("a column definition list is redundant for a function with OUT parameters"),
								 parser_errposition(pstate,
													exprLocation((Node *) coldeflist))));
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("a column definition list is redundant for a function returning a named composite type"),
								 parser_errposition(pstate,
													exprLocation((Node *) coldeflist))));
					break;
				default:
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("a column definition list is only allowed for functions returning \"record\""),
							 parser_errposition(pstate,
												exprLocation((Node *) coldeflist))));
					break;
			}
		}
		else
		{
			if (functypclass == TYPEFUNC_RECORD)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("a column definition list is required for functions returning \"record\""),
						 parser_errposition(pstate, exprLocation(funcexpr))));
		}

		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			/* Composite data type, e.g. a table's row type */
			Assert(tupdesc);
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* Base data type, i.e. scalar */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   chooseScalarFunctionAlias(funcexpr, funcname,
														 alias, nfuncs),
							   funcrettype,
							   exprTypmod(funcexpr),
							   0);
			TupleDescInitEntryCollation(tupdesc,
										(AttrNumber) 1,
										exprCollation(funcexpr));
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			ListCell   *col;

			/*
			 * Use the column definition list to construct a tupdesc and fill
			 * in the RangeTblFunction's lists.  Limit number of columns to
			 * MaxHeapAttributeNumber, because CheckAttributeNamesTypes will.
			 */
			if (list_length(coldeflist) > MaxHeapAttributeNumber)
				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_COLUMNS),
						 errmsg("column definition lists can have at most %d entries",
								MaxHeapAttributeNumber),
						 parser_errposition(pstate,
											exprLocation((Node *) coldeflist))));
			tupdesc = CreateTemplateTupleDesc(list_length(coldeflist));
			i = 1;
			foreach(col, coldeflist)
			{
				ColumnDef  *n = (ColumnDef *) lfirst(col);
				char	   *attrname;
				Oid			attrtype;
				int32		attrtypmod;
				Oid			attrcollation;

				attrname = n->colname;
				if (n->typeName->setof)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("column \"%s\" cannot be declared SETOF",
									attrname),
							 parser_errposition(pstate, n->location)));
				typenameTypeIdAndMod(pstate, n->typeName,
									 &attrtype, &attrtypmod);
				attrcollation = GetColumnDefCollation(pstate, n, attrtype);
				TupleDescInitEntry(tupdesc,
								   (AttrNumber) i,
								   attrname,
								   attrtype,
								   attrtypmod,
								   0);
				TupleDescInitEntryCollation(tupdesc,
											(AttrNumber) i,
											attrcollation);
				rtfunc->funccolnames = lappend(rtfunc->funccolnames,
											   makeString(pstrdup(attrname)));
				rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes,
												   attrtype);
				rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods,
													 attrtypmod);
				rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
														attrcollation);

				i++;
			}

			/*
			 * Ensure that the coldeflist defines a legal set of names (no
			 * duplicates, but we needn't worry about system column names) and
			 * datatypes.  Although we mostly can't allow pseudo-types, it
			 * seems safe to allow RECORD and RECORD[], since values within
			 * those type classes are self-identifying at runtime, and the
			 * coldeflist doesn't represent anything that will be visible to
			 * other sessions.
			 */
			CheckAttributeNamesTypes(tupdesc, RELKIND_COMPOSITE_TYPE,
									 CHKATYPE_ANYRECORD);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function \"%s\" in FROM has unsupported return type %s",
							funcname, format_type_be(funcrettype)),
					 parser_errposition(pstate, exprLocation(funcexpr))));

		/* Finish off the RangeTblFunction and add it to the RTE's list */
		rtfunc->funccolcount = tupdesc->natts;
		rte->functions = lappend(rte->functions, rtfunc);

		/* Save the tupdesc for use below */
		functupdescs[funcno] = tupdesc;
		totalatts += tupdesc->natts;
		funcno++;
	}

	/*
	 * If there's more than one function, or we want an ordinality column, we
	 * have to produce a merged tupdesc.
	 */
	if (nfuncs > 1 || rangefunc->ordinality)
	{
		if (rangefunc->ordinality)
			totalatts++;

		/* Disallow more columns than will fit in a tuple */
		if (totalatts > MaxTupleAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("functions in FROM can return at most %d columns",
							MaxTupleAttributeNumber),
					 parser_errposition(pstate,
										exprLocation((Node *) funcexprs))));

		/* Merge the tuple descs of each function into a composite one */
		tupdesc = CreateTemplateTupleDesc(totalatts);
		natts = 0;
		for (i = 0; i < nfuncs; i++)
		{
			for (j = 1; j <= functupdescs[i]->natts; j++)
				TupleDescCopyEntry(tupdesc, ++natts, functupdescs[i], j);
		}

		/* Add the ordinality column if needed */
		if (rangefunc->ordinality)
		{
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) ++natts,
							   "ordinality",
							   INT8OID,
							   -1,
							   0);
			/* no need to set collation */
		}

		Assert(natts == totalatts);
	}
	else
	{
		/* We can just use the single function's tupdesc as-is */
		tupdesc = functupdescs[0];
	}

	/* Use the tupdesc while assigning column aliases for the RTE */
	buildRelationAliases(tupdesc, alias, eref);

	/*
	 * Set flags and access permissions.
	 *
	 * Functions are never checked for access rights (at least, not by
	 * ExecCheckPermissions()), so no need to perform addRTEPermissionInfo().
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable), NULL,
									tupdesc);
}

/*
 * Add an entry for a table function to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is much like addRangeTableEntry() except that it makes a tablefunc RTE.
 */
ParseNamespaceItem *
addRangeTableEntryForTableFunc(ParseState *pstate,
							   TableFunc *tf,
							   Alias *alias,
							   bool lateral,
							   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname;
	Alias	   *eref;
	int			numaliases;

	Assert(pstate != NULL);

	/* Disallow more columns than will fit in a tuple */
	if (list_length(tf->colnames) > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("functions in FROM can return at most %d columns",
						MaxTupleAttributeNumber),
				 parser_errposition(pstate,
									exprLocation((Node *) tf))));
	Assert(list_length(tf->coltypes) == list_length(tf->colnames));
	Assert(list_length(tf->coltypmods) == list_length(tf->colnames));
	Assert(list_length(tf->colcollations) == list_length(tf->colnames));

	rte->rtekind = RTE_TABLEFUNC;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->tablefunc = tf;
	rte->coltypes = tf->coltypes;
	rte->coltypmods = tf->coltypmods;
	rte->colcollations = tf->colcollations;
	rte->alias = alias;

	refname = alias ? alias->aliasname :
		pstrdup(tf->functype == TFT_XMLTABLE ? "xmltable" : "json_table");
	eref = alias ? copyObject(alias) : makeAlias(refname, NIL);
	numaliases = list_length(eref->colnames);

	/* fill in any unspecified alias columns */
	if (numaliases < list_length(tf->colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(tf->colnames, numaliases));

	if (numaliases > list_length(tf->colnames))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("%s function has %d columns available but %d columns specified",
						tf->functype == TFT_XMLTABLE ? "XMLTABLE" : "JSON_TABLE",
						list_length(tf->colnames), numaliases)));

	rte->eref = eref;

	/*
	 * Set flags and access permissions.
	 *
	 * Tablefuncs are never checked for access rights (at least, not by
	 * ExecCheckPermissions()), so no need to perform addRTEPermissionInfo().
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	return buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								rte->coltypes, rte->coltypmods,
								rte->colcollations);
}

/*
 * Add an entry for a VALUES list to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is much like addRangeTableEntry() except that it makes a values RTE.
 */
ParseNamespaceItem *
addRangeTableEntryForValues(ParseState *pstate,
							List *exprs,
							List *coltypes,
							List *coltypmods,
							List *colcollations,
							Alias *alias,
							bool lateral,
							bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->aliasname : pstrdup("*VALUES*");
	Alias	   *eref;
	int			numaliases;
	int			numcolumns;

	Assert(pstate != NULL);

	rte->rtekind = RTE_VALUES;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->values_lists = exprs;
	rte->coltypes = coltypes;
	rte->coltypmods = coltypmods;
	rte->colcollations = colcollations;
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
	 * Subqueries are never checked for access rights, so no need to perform
	 * addRTEPermissionInfo().
	 */
	rte->lateral = lateral;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	return buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								rte->coltypes, rte->coltypmods,
								rte->colcollations);
}

/*
 * Add an entry for a join to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is much like addRangeTableEntry() except that it makes a join RTE.
 * Also, it's more convenient for the caller to construct the
 * ParseNamespaceColumn array, so we pass that in.
 */
ParseNamespaceItem *
addRangeTableEntryForJoin(ParseState *pstate,
						  List *colnames,
						  ParseNamespaceColumn *nscolumns,
						  JoinType jointype,
						  int nummergedcols,
						  List *aliasvars,
						  List *leftcols,
						  List *rightcols,
						  Alias *join_using_alias,
						  Alias *alias,
						  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

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
	rte->joinmergedcols = nummergedcols;
	rte->joinaliasvars = aliasvars;
	rte->joinleftcols = leftcols;
	rte->joinrightcols = rightcols;
	rte->join_using_alias = join_using_alias;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias("unnamed_join", NIL);
	numaliases = list_length(eref->colnames);

	/* fill in any unspecified alias columns */
	if (numaliases < list_length(colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(colnames, numaliases));

	if (numaliases > list_length(colnames))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("join expression \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, list_length(colnames), numaliases)));

	rte->eref = eref;

	/*
	 * Set flags and access permissions.
	 *
	 * Joins are never checked for access rights, so no need to perform
	 * addRTEPermissionInfo().
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_perminfo = NULL;
	nsitem->p_rtindex = list_length(pstate->p_rtable);
	nsitem->p_nscolumns = nscolumns;
	/* set default visibility flags; might get changed later */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	return nsitem;
}

/*
 * Add an entry for a CTE reference to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * This is much like addRangeTableEntry() except that it makes a CTE RTE.
 */
ParseNamespaceItem *
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
	int			n_dontexpand_columns = 0;
	ParseNamespaceItem *psi;

	Assert(pstate != NULL);

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
	 * We throw error if the CTE is INSERT/UPDATE/DELETE/MERGE without
	 * RETURNING.  This won't get checked in case of a self-reference, but
	 * that's OK because data-modifying CTEs aren't allowed to be recursive
	 * anyhow.
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

	rte->coltypes = list_copy(cte->ctecoltypes);
	rte->coltypmods = list_copy(cte->ctecoltypmods);
	rte->colcollations = list_copy(cte->ctecolcollations);

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

	if (cte->search_clause)
	{
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->search_clause->search_seq_column));
		if (cte->search_clause->search_breadth_first)
			rte->coltypes = lappend_oid(rte->coltypes, RECORDOID);
		else
			rte->coltypes = lappend_oid(rte->coltypes, RECORDARRAYOID);
		rte->coltypmods = lappend_int(rte->coltypmods, -1);
		rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);

		n_dontexpand_columns += 1;
	}

	if (cte->cycle_clause)
	{
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->cycle_clause->cycle_mark_column));
		rte->coltypes = lappend_oid(rte->coltypes, cte->cycle_clause->cycle_mark_type);
		rte->coltypmods = lappend_int(rte->coltypmods, cte->cycle_clause->cycle_mark_typmod);
		rte->colcollations = lappend_oid(rte->colcollations, cte->cycle_clause->cycle_mark_collation);

		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->cycle_clause->cycle_path_column));
		rte->coltypes = lappend_oid(rte->coltypes, RECORDARRAYOID);
		rte->coltypmods = lappend_int(rte->coltypmods, -1);
		rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);

		n_dontexpand_columns += 2;
	}

	/*
	 * Set flags and access permissions.
	 *
	 * Subqueries are never checked for access rights, so no need to perform
	 * addRTEPermissionInfo().
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	psi = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
							   rte->coltypes, rte->coltypmods,
							   rte->colcollations);

	/*
	 * The columns added by search and cycle clauses are not included in star
	 * expansion in queries contained in the CTE.
	 */
	if (rte->ctelevelsup > 0)
		for (int i = 0; i < n_dontexpand_columns; i++)
			psi->p_nscolumns[list_length(psi->p_names->colnames) - 1 - i].p_dontexpand = true;

	return psi;
}

/*
 * Add an entry for an ephemeral named relation reference to the pstate's
 * range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 *
 * It is expected that the RangeVar, which up until now is only known to be an
 * ephemeral named relation, will (in conjunction with the QueryEnvironment in
 * the ParseState), create a RangeTblEntry for a specific *kind* of ephemeral
 * named relation, based on enrtype.
 *
 * This is much like addRangeTableEntry() except that it makes an RTE for an
 * ephemeral named relation.
 */
ParseNamespaceItem *
addRangeTableEntryForENR(ParseState *pstate,
						 RangeVar *rv,
						 bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rv->alias;
	char	   *refname = alias ? alias->aliasname : rv->relname;
	EphemeralNamedRelationMetadata enrmd;
	TupleDesc	tupdesc;
	int			attno;

	Assert(pstate != NULL);
	enrmd = get_visible_ENR(pstate, rv->relname);
	Assert(enrmd != NULL);

	switch (enrmd->enrtype)
	{
		case ENR_NAMED_TUPLESTORE:
			rte->rtekind = RTE_NAMEDTUPLESTORE;
			break;

		default:
			elog(ERROR, "unexpected enrtype: %d", enrmd->enrtype);
			return NULL;		/* for fussy compilers */
	}

	/*
	 * Record dependency on a relation.  This allows plans to be invalidated
	 * if they access transition tables linked to a table that is altered.
	 */
	rte->relid = enrmd->reliddesc;

	/*
	 * Build the list of effective column names using user-supplied aliases
	 * and/or actual column names.
	 */
	tupdesc = ENRMetadataGetTupDesc(enrmd);
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(tupdesc, alias, rte->eref);

	/* Record additional data for ENR, including column type info */
	rte->enrname = enrmd->name;
	rte->enrtuples = enrmd->enrtuples;
	rte->coltypes = NIL;
	rte->coltypmods = NIL;
	rte->colcollations = NIL;
	for (attno = 1; attno <= tupdesc->natts; ++attno)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, attno - 1);

		if (att->attisdropped)
		{
			/* Record zeroes for a dropped column */
			rte->coltypes = lappend_oid(rte->coltypes, InvalidOid);
			rte->coltypmods = lappend_int(rte->coltypmods, 0);
			rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);
		}
		else
		{
			/* Let's just make sure we can tell this isn't dropped */
			if (att->atttypid == InvalidOid)
				elog(ERROR, "atttypid is invalid for non-dropped column in \"%s\"",
					 rv->relname);
			rte->coltypes = lappend_oid(rte->coltypes, att->atttypid);
			rte->coltypmods = lappend_int(rte->coltypmods, att->atttypmod);
			rte->colcollations = lappend_oid(rte->colcollations,
											 att->attcollation);
		}
	}

	/*
	 * Set flags and access permissions.
	 *
	 * ENRs are never checked for access rights, so no need to perform
	 * addRTEPermissionInfo().
	 */
	rte->lateral = false;
	rte->inFromCl = inFromCl;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable), NULL,
									tupdesc);
}

/*
 * Add an entry for grouping step to the pstate's range table (p_rtable).
 * Then, construct and return a ParseNamespaceItem for the new RTE.
 */
ParseNamespaceItem *
addRangeTableEntryForGroup(ParseState *pstate,
						   List *groupClauses)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	List	   *groupexprs;
	List	   *coltypes,
			   *coltypmods,
			   *colcollations;
	ListCell   *lc;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_GROUP;
	rte->alias = NULL;

	eref = makeAlias("*GROUP*", NIL);

	/* fill in any unspecified alias columns, and extract column type info */
	groupexprs = NIL;
	coltypes = coltypmods = colcollations = NIL;
	foreach(lc, groupClauses)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		char	   *colname = te->resname ? pstrdup(te->resname) : "?column?";

		eref->colnames = lappend(eref->colnames, makeString(colname));

		groupexprs = lappend(groupexprs, copyObject(te->expr));

		coltypes = lappend_oid(coltypes,
							   exprType((Node *) te->expr));
		coltypmods = lappend_int(coltypmods,
								 exprTypmod((Node *) te->expr));
		colcollations = lappend_oid(colcollations,
									exprCollation((Node *) te->expr));
	}

	rte->eref = eref;
	rte->groupexprs = groupexprs;

	/*
	 * Set flags.
	 *
	 * The grouping step is never checked for access rights, so no need to
	 * perform addRTEPermissionInfo().
	 */
	rte->lateral = false;
	rte->inFromCl = false;

	/*
	 * Add completed RTE to pstate's range table list, so that we know its
	 * index.  But we don't add it to the join list --- caller must do that if
	 * appropriate.
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * Build a ParseNamespaceItem, but don't add it to the pstate's namespace
	 * list --- caller must do that if appropriate.
	 */
	nsitem = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								  coltypes, coltypmods, colcollations);

	return nsitem;
}


/*
 * Has the specified refname been selected FOR UPDATE/FOR SHARE?
 *
 * This is used when we have not yet done transformLockingClause, but need
 * to know the correct lock to take during initial opening of relations.
 *
 * Note that refname may be NULL (for a subquery without an alias), in which
 * case the relation can't be locked by name, but it might still be locked if
 * a locking clause requests that all tables be locked.
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
		else if (refname != NULL)
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
 * Add the given nsitem/RTE as a top-level entry in the pstate's join list
 * and/or namespace list.  (We assume caller has checked for any
 * namespace conflicts.)  The nsitem is always marked as unconditionally
 * visible, that is, not LATERAL-only.
 */
void
addNSItemToQuery(ParseState *pstate, ParseNamespaceItem *nsitem,
				 bool addToJoinList,
				 bool addToRelNameSpace, bool addToVarNameSpace)
{
	if (addToJoinList)
	{
		RangeTblRef *rtr = makeNode(RangeTblRef);

		rtr->rtindex = nsitem->p_rtindex;
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	}
	if (addToRelNameSpace || addToVarNameSpace)
	{
		/* Set the new nsitem's visibility flags correctly */
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
 * If include_dropped is false then dropped columns are omitted from the
 * results.  If include_dropped is true then empty strings and NULL constants
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

					/*
					 * Formerly it was possible for the subquery tlist to have
					 * more non-junk entries than the colnames list does (if
					 * this RTE has been expanded from a view that has more
					 * columns than it did when the current query was parsed).
					 * Now that ApplyRetrieveRule cleans up such cases, we
					 * shouldn't see that anymore, but let's just check.
					 */
					if (!aliasp_item)
						elog(ERROR, "too few column names for subquery %s",
							 rte->eref->aliasname);

					if (colnames)
					{
						char	   *label = strVal(lfirst(aliasp_item));

						*colnames = lappend(*colnames, makeString(pstrdup(label)));
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

					aliasp_item = lnext(rte->eref->colnames, aliasp_item);
				}
			}
			break;
		case RTE_FUNCTION:
			{
				/* Function RTE */
				int			atts_done = 0;
				ListCell   *lc;

				foreach(lc, rte->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
					TypeFuncClass functypclass;
					Oid			funcrettype = InvalidOid;
					TupleDesc	tupdesc = NULL;

					/* If it has a coldeflist, it returns RECORD */
					if (rtfunc->funccolnames != NIL)
						functypclass = TYPEFUNC_RECORD;
					else
						functypclass = get_expr_result_type(rtfunc->funcexpr,
															&funcrettype,
															&tupdesc);

					if (functypclass == TYPEFUNC_COMPOSITE ||
						functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
					{
						/* Composite data type, e.g. a table's row type */
						Assert(tupdesc);
						expandTupleDesc(tupdesc, rte->eref,
										rtfunc->funccolcount, atts_done,
										rtindex, sublevels_up, location,
										include_dropped, colnames, colvars);
					}
					else if (functypclass == TYPEFUNC_SCALAR)
					{
						/* Base data type, i.e. scalar */
						if (colnames)
							*colnames = lappend(*colnames,
												list_nth(rte->eref->colnames,
														 atts_done));

						if (colvars)
						{
							Var		   *varnode;

							varnode = makeVar(rtindex, atts_done + 1,
											  funcrettype,
											  exprTypmod(rtfunc->funcexpr),
											  exprCollation(rtfunc->funcexpr),
											  sublevels_up);
							varnode->location = location;

							*colvars = lappend(*colvars, varnode);
						}
					}
					else if (functypclass == TYPEFUNC_RECORD)
					{
						if (colnames)
						{
							List	   *namelist;

							/* extract appropriate subset of column list */
							namelist = list_copy_tail(rte->eref->colnames,
													  atts_done);
							namelist = list_truncate(namelist,
													 rtfunc->funccolcount);
							*colnames = list_concat(*colnames, namelist);
						}

						if (colvars)
						{
							ListCell   *l1;
							ListCell   *l2;
							ListCell   *l3;
							int			attnum = atts_done;

							forthree(l1, rtfunc->funccoltypes,
									 l2, rtfunc->funccoltypmods,
									 l3, rtfunc->funccolcollations)
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
					atts_done += rtfunc->funccolcount;
				}

				/* Append the ordinality column if any */
				if (rte->funcordinality)
				{
					if (colnames)
						*colnames = lappend(*colnames,
											llast(rte->eref->colnames));

					if (colvars)
					{
						Var		   *varnode = makeVar(rtindex,
													  atts_done + 1,
													  INT8OID,
													  -1,
													  InvalidOid,
													  sublevels_up);

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
					 * deleted columns in the join.  While this function is
					 * also used by the rewriter and planner, they do not
					 * currently call it on any JOIN RTEs.  Therefore, this
					 * next bit is dead code, but it seems prudent to handle
					 * the case correctly anyway.
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

						/*
						 * If the joinaliasvars entry is a simple Var, just
						 * copy it (with adjustment of varlevelsup and
						 * location); otherwise it is a JOIN USING column and
						 * we must generate a join alias Var.  This matches
						 * the results that expansion of "join.*" by
						 * expandNSItemVars would have produced, if we had
						 * access to the ParseNamespaceItem for the join.
						 */
						if (IsA(avar, Var))
						{
							varnode = copyObject((Var *) avar);
							varnode->varlevelsup = sublevels_up;
						}
						else
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
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
			{
				/* Tablefunc, Values, CTE, or ENR RTE */
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *lct;
				ListCell   *lcm;
				ListCell   *lcc;

				varattno = 0;
				forthree(lct, rte->coltypes,
						 lcm, rte->coltypmods,
						 lcc, rte->colcollations)
				{
					Oid			coltype = lfirst_oid(lct);
					int32		coltypmod = lfirst_int(lcm);
					Oid			colcoll = lfirst_oid(lcc);

					varattno++;

					if (colnames)
					{
						/* Assume there is one alias per output column */
						if (OidIsValid(coltype))
						{
							char	   *label = strVal(lfirst(aliasp_item));

							*colnames = lappend(*colnames,
												makeString(pstrdup(label)));
						}
						else if (include_dropped)
							*colnames = lappend(*colnames,
												makeString(pstrdup("")));

						aliasp_item = lnext(rte->eref->colnames, aliasp_item);
					}

					if (colvars)
					{
						if (OidIsValid(coltype))
						{
							Var		   *varnode;

							varnode = makeVar(rtindex, varattno,
											  coltype, coltypmod, colcoll,
											  sublevels_up);
							varnode->location = location;

							*colvars = lappend(*colvars, varnode);
						}
						else if (include_dropped)
						{
							/*
							 * It doesn't really matter what type the Const
							 * claims to be.
							 */
							*colvars = lappend(*colvars,
											   makeNullConst(INT4OID, -1,
															 InvalidOid));
						}
					}
				}
			}
			break;
		case RTE_RESULT:
		case RTE_GROUP:
			/* These expose no columns, so nothing to do */
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
	expandTupleDesc(rel->rd_att, eref, rel->rd_att->natts, 0,
					rtindex, sublevels_up,
					location, include_dropped,
					colnames, colvars);
	relation_close(rel, AccessShareLock);
}

/*
 * expandTupleDesc -- expandRTE subroutine
 *
 * Generate names and/or Vars for the first "count" attributes of the tupdesc,
 * and append them to colnames/colvars.  "offset" is added to the varattno
 * that each Var would otherwise have, and we also skip the first "offset"
 * entries in eref->colnames.  (These provisions allow use of this code for
 * an individual composite-returning function in an RTE_FUNCTION RTE.)
 */
static void
expandTupleDesc(TupleDesc tupdesc, Alias *eref, int count, int offset,
				int rtindex, int sublevels_up,
				int location, bool include_dropped,
				List **colnames, List **colvars)
{
	ListCell   *aliascell;
	int			varattno;

	aliascell = (offset < list_length(eref->colnames)) ?
		list_nth_cell(eref->colnames, offset) : NULL;

	Assert(count <= tupdesc->natts);
	for (varattno = 0; varattno < count; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

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
			if (aliascell)
				aliascell = lnext(eref->colnames, aliascell);
			continue;
		}

		if (colnames)
		{
			char	   *label;

			if (aliascell)
			{
				label = strVal(lfirst(aliascell));
				aliascell = lnext(eref->colnames, aliascell);
			}
			else
			{
				/* If we run out of aliases, use the underlying name */
				label = NameStr(attr->attname);
			}
			*colnames = lappend(*colnames, makeString(pstrdup(label)));
		}

		if (colvars)
		{
			Var		   *varnode;

			varnode = makeVar(rtindex, varattno + offset + 1,
							  attr->atttypid, attr->atttypmod,
							  attr->attcollation,
							  sublevels_up);
			varnode->location = location;

			*colvars = lappend(*colvars, varnode);
		}
	}
}

/*
 * expandNSItemVars
 *	  Produce a list of Vars, and optionally a list of column names,
 *	  for the non-dropped columns of the nsitem.
 *
 * The emitted Vars are marked with the given sublevels_up and location.
 *
 * If colnames isn't NULL, a list of String items for the columns is stored
 * there; note that it's just a subset of the RTE's eref list, and hence
 * the list elements mustn't be modified.
 */
List *
expandNSItemVars(ParseState *pstate, ParseNamespaceItem *nsitem,
				 int sublevels_up, int location,
				 List **colnames)
{
	List	   *result = NIL;
	int			colindex;
	ListCell   *lc;

	if (colnames)
		*colnames = NIL;
	colindex = 0;
	foreach(lc, nsitem->p_names->colnames)
	{
		String	   *colnameval = lfirst(lc);
		const char *colname = strVal(colnameval);
		ParseNamespaceColumn *nscol = nsitem->p_nscolumns + colindex;

		if (nscol->p_dontexpand)
		{
			/* skip */
		}
		else if (colname[0])
		{
			Var		   *var;

			Assert(nscol->p_varno > 0);
			var = makeVar(nscol->p_varno,
						  nscol->p_varattno,
						  nscol->p_vartype,
						  nscol->p_vartypmod,
						  nscol->p_varcollid,
						  sublevels_up);
			/* makeVar doesn't offer parameters for these, so set by hand: */
			var->varnosyn = nscol->p_varnosyn;
			var->varattnosyn = nscol->p_varattnosyn;
			var->location = location;

			/* ... and update varnullingrels */
			markNullableIfNeeded(pstate, var);

			result = lappend(result, var);
			if (colnames)
				*colnames = lappend(*colnames, colnameval);
		}
		else
		{
			/* dropped column, ignore */
			Assert(nscol->p_varno == 0);
		}
		colindex++;
	}
	return result;
}

/*
 * expandNSItemAttrs -
 *	  Workhorse for "*" expansion: produce a list of targetentries
 *	  for the attributes of the nsitem
 *
 * pstate->p_next_resno determines the resnos assigned to the TLEs.
 * The referenced columns are marked as requiring SELECT access, if
 * caller requests that.
 */
List *
expandNSItemAttrs(ParseState *pstate, ParseNamespaceItem *nsitem,
				  int sublevels_up, bool require_col_privs, int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	RTEPermissionInfo *perminfo = nsitem->p_perminfo;
	List	   *names,
			   *vars;
	ListCell   *name,
			   *var;
	List	   *te_list = NIL;

	vars = expandNSItemVars(pstate, nsitem, sublevels_up, location, &names);

	/*
	 * Require read access to the table.  This is normally redundant with the
	 * markVarForSelectPriv calls below, but not if the table has zero
	 * columns.  We need not do anything if the nsitem is for a join: its
	 * component tables will have been marked ACL_SELECT when they were added
	 * to the rangetable.  (This step changes things only for the target
	 * relation of UPDATE/DELETE, which cannot be under a join.)
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		Assert(perminfo != NULL);
		perminfo->requiredPerms |= ACL_SELECT;
	}

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

		if (require_col_privs)
		{
			/* Require read access to each column */
			markVarForSelectPriv(pstate, varnode);
		}
	}

	Assert(name == NULL && var == NULL);	/* lists not the same length? */

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
 *
 * It is caller's responsibility to not call this on a dropped attribute.
 * (You will get some answer for such cases, but it might not be sensible.)
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
		return get_attname(rte->relid, attnum, false);

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
				if (!HeapTupleIsValid(tp))	/* shouldn't happen */
					elog(ERROR, "cache lookup failed for attribute %d of relation %u",
						 attnum, rte->relid);
				att_tup = (Form_pg_attribute) GETSTRUCT(tp);
				result = att_tup->attisdropped;
				ReleaseSysCache(tp);
			}
			break;
		case RTE_SUBQUERY:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_GROUP:

			/*
			 * Subselect, Table Functions, Values, CTE, GROUP RTEs never have
			 * dropped columns
			 */
			result = false;
			break;
		case RTE_NAMEDTUPLESTORE:
			{
				/* Check dropped-ness by testing for valid coltype */
				if (attnum <= 0 ||
					attnum > list_length(rte->coltypes))
					elog(ERROR, "invalid varattno %d", attnum);
				result = !OidIsValid((list_nth_oid(rte->coltypes, attnum - 1)));
			}
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
				ListCell   *lc;
				int			atts_done = 0;

				/*
				 * Dropped attributes are only possible with functions that
				 * return named composite types.  In such a case we have to
				 * look up the result type to see if it currently has this
				 * column dropped.  So first, loop over the funcs until we
				 * find the one that covers the requested column.
				 */
				foreach(lc, rte->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					if (attnum > atts_done &&
						attnum <= atts_done + rtfunc->funccolcount)
					{
						TupleDesc	tupdesc;

						/* If it has a coldeflist, it returns RECORD */
						if (rtfunc->funccolnames != NIL)
							return false;	/* can't have any dropped columns */

						tupdesc = get_expr_result_tupdesc(rtfunc->funcexpr,
														  true);
						if (tupdesc)
						{
							/* Composite data type, e.g. a table's row type */
							Form_pg_attribute att_tup;

							Assert(tupdesc);
							Assert(attnum - atts_done <= tupdesc->natts);
							att_tup = TupleDescAttr(tupdesc,
													attnum - atts_done - 1);
							return att_tup->attisdropped;
						}
						/* Otherwise, it can't have any dropped columns */
						return false;
					}
					atts_done += rtfunc->funccolcount;
				}

				/* If we get here, must be looking for the ordinality column */
				if (rte->funcordinality && attnum == atts_done + 1)
					return false;

				/* this probably can't happen ... */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column %d of relation \"%s\" does not exist",
								attnum,
								rte->eref->aliasname)));
				result = false; /* keep compiler quiet */
			}
			break;
		case RTE_RESULT:
			/* this probably can't happen ... */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column %d of relation \"%s\" does not exist",
							attnum,
							rte->eref->aliasname)));
			result = false;		/* keep compiler quiet */
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
 *	table_open()'ed.  Use the cache version get_attnum()
 *	for access to non-opened relations.
 */
int
attnameAttNum(Relation rd, const char *attname, bool sysColOK)
{
	int			i;

	for (i = 0; i < RelationGetNumberOfAttributes(rd); i++)
	{
		Form_pg_attribute att = TupleDescAttr(rd->rd_att, i);

		if (namestrcmp(&(att->attname), attname) == 0 && !att->attisdropped)
			return i + 1;
	}

	if (sysColOK)
	{
		if ((i = specialAttNum(attname)) != InvalidAttrNumber)
			return i;
	}

	/* on failure */
	return InvalidAttrNumber;
}

/* specialAttNum()
 *
 * Check attribute name to see if it is "special", e.g. "xmin".
 * - thomas 2000-02-07
 *
 * Note: this only discovers whether the name could be a system attribute.
 * Caller needs to ensure that it really is an attribute of the rel.
 */
static int
specialAttNum(const char *attname)
{
	const FormData_pg_attribute *sysatt;

	sysatt = SystemAttributeByName(attname);
	if (sysatt != NULL)
		return sysatt->attnum;
	return InvalidAttrNumber;
}


/*
 * given attribute id, return name of that attribute
 *
 *	This should only be used if the relation is already
 *	table_open()'ed.  Use the cache version get_atttype()
 *	for access to non-opened relations.
 */
const NameData *
attnumAttName(Relation rd, int attid)
{
	if (attid <= 0)
	{
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attid);
		return &sysatt->attname;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return &TupleDescAttr(rd->rd_att, attid - 1)->attname;
}

/*
 * given attribute id, return type of that attribute
 *
 *	This should only be used if the relation is already
 *	table_open()'ed.  Use the cache version get_atttype()
 *	for access to non-opened relations.
 */
Oid
attnumTypeId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attid);
		return sysatt->atttypid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return TupleDescAttr(rd->rd_att, attid - 1)->atttypid;
}

/*
 * given attribute id, return collation of that attribute
 *
 *	This should only be used if the relation is already table_open()'ed.
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
	return TupleDescAttr(rd->rd_att, attid - 1)->attcollation;
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
	const char *badAlias = NULL;

	/*
	 * Check to see if there are any potential matches in the query's
	 * rangetable.  (Note: cases involving a bad schema name in the RangeVar
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
		strcmp(rte->eref->aliasname, relation->relname) != 0)
	{
		ParseNamespaceItem *nsitem;
		int			sublevels_up;

		nsitem = refnameNamespaceItem(pstate, NULL, rte->eref->aliasname,
									  relation->location,
									  &sublevels_up);
		if (nsitem && nsitem->p_rte == rte)
			badAlias = rte->eref->aliasname;
	}

	/* If it looks like the user forgot to use an alias, hint about that */
	if (badAlias)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						relation->relname),
				 errhint("Perhaps you meant to reference the table alias \"%s\".",
						 badAlias),
				 parser_errposition(pstate, relation->location)));
	/* Hint about case where we found an (inaccessible) exact match */
	else if (rte)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						relation->relname),
				 errdetail("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						   rte->eref->aliasname),
				 rte_visible_if_lateral(pstate, rte) ?
				 errhint("To reference that table, you must mark this subquery with LATERAL.") : 0,
				 parser_errposition(pstate, relation->location)));
	/* Else, we have nothing to offer but the bald statement of error */
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
				   const char *relname, const char *colname, int location)
{
	FuzzyAttrMatchState *state;

	/*
	 * Search the entire rtable looking for possible matches.  If we find one,
	 * emit a hint about it.
	 */
	state = searchRangeTableForCol(pstate, relname, colname, location);

	/*
	 * If there are exact match(es), they must be inaccessible for some
	 * reason.
	 */
	if (state->rexact1)
	{
		/*
		 * We don't try too hard when there's multiple inaccessible exact
		 * matches, but at least be sure that we don't misleadingly suggest
		 * that there's only one.
		 */
		if (state->rexact2)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 relname ?
					 errmsg("column %s.%s does not exist", relname, colname) :
					 errmsg("column \"%s\" does not exist", colname),
					 errdetail("There are columns named \"%s\", but they are in tables that cannot be referenced from this part of the query.",
							   colname),
					 !relname ? errhint("Try using a table-qualified name.") : 0,
					 parser_errposition(pstate, location)));
		/* Single exact match, so try to determine why it's inaccessible. */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errdetail("There is a column named \"%s\" in table \"%s\", but it cannot be referenced from this part of the query.",
						   colname, state->rexact1->eref->aliasname),
				 rte_visible_if_lateral(pstate, state->rexact1) ?
				 errhint("To reference that column, you must mark this subquery with LATERAL.") :
				 (!relname && rte_visible_if_qualified(pstate, state->rexact1)) ?
				 errhint("To reference that column, you must use a table-qualified name.") : 0,
				 parser_errposition(pstate, location)));
	}

	if (!state->rsecond)
	{
		/* If we found no match at all, we have little to report */
		if (!state->rfirst)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 relname ?
					 errmsg("column %s.%s does not exist", relname, colname) :
					 errmsg("column \"%s\" does not exist", colname),
					 parser_errposition(pstate, location)));
		/* Handle case where we have a single alternative spelling to offer */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errhint("Perhaps you meant to reference the column \"%s.%s\".",
						 state->rfirst->eref->aliasname,
						 strVal(list_nth(state->rfirst->eref->colnames,
										 state->first - 1))),
				 parser_errposition(pstate, location)));
	}
	else
	{
		/* Handle case where there are two equally useful column hints */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errhint("Perhaps you meant to reference the column \"%s.%s\" or the column \"%s.%s\".",
						 state->rfirst->eref->aliasname,
						 strVal(list_nth(state->rfirst->eref->colnames,
										 state->first - 1)),
						 state->rsecond->eref->aliasname,
						 strVal(list_nth(state->rsecond->eref->colnames,
										 state->second - 1))),
				 parser_errposition(pstate, location)));
	}
}

/*
 * Find ParseNamespaceItem for RTE, if it's visible at all.
 * We assume an RTE couldn't appear more than once in the namespace lists.
 */
static ParseNamespaceItem *
findNSItemForRTE(ParseState *pstate, RangeTblEntry *rte)
{
	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

			if (nsitem->p_rte == rte)
				return nsitem;
		}
		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * Would this RTE be visible, if only the user had written LATERAL?
 *
 * This is a helper for deciding whether to issue a HINT about LATERAL.
 * As such, it doesn't need to be 100% accurate; the HINT could be useful
 * even if it's not quite right.  Hence, we don't delve into fine points
 * about whether a found nsitem has the appropriate one of p_rel_visible or
 * p_cols_visible set.
 */
static bool
rte_visible_if_lateral(ParseState *pstate, RangeTblEntry *rte)
{
	ParseNamespaceItem *nsitem;

	/* If LATERAL *is* active, we're clearly barking up the wrong tree */
	if (pstate->p_lateral_active)
		return false;
	nsitem = findNSItemForRTE(pstate, rte);
	if (nsitem)
	{
		/* Found it, report whether it's LATERAL-only */
		return nsitem->p_lateral_only && nsitem->p_lateral_ok;
	}
	return false;
}

/*
 * Would columns in this RTE be visible if qualified?
 */
static bool
rte_visible_if_qualified(ParseState *pstate, RangeTblEntry *rte)
{
	ParseNamespaceItem *nsitem = findNSItemForRTE(pstate, rte);

	if (nsitem)
	{
		/* Found it, report whether it's relation-only */
		return nsitem->p_rel_visible && !nsitem->p_cols_visible;
	}
	return false;
}


/*
 * Examine a fully-parsed query, and return true iff any relation underlying
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
				Relation	rel = table_open(rte->relid, AccessShareLock);
				char		relpersistence = rel->rd_rel->relpersistence;

				table_close(rel, AccessShareLock);
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

/*
 * addRTEPermissionInfo
 *		Creates RTEPermissionInfo for a given RTE and adds it into the
 *		provided list.
 *
 * Returns the RTEPermissionInfo and sets rte->perminfoindex.
 */
RTEPermissionInfo *
addRTEPermissionInfo(List **rteperminfos, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	Assert(OidIsValid(rte->relid));
	Assert(rte->perminfoindex == 0);

	/* Nope, so make one and add to the list. */
	perminfo = makeNode(RTEPermissionInfo);
	perminfo->relid = rte->relid;
	perminfo->inh = rte->inh;
	/* Other information is set by fetching the node as and where needed. */

	*rteperminfos = lappend(*rteperminfos, perminfo);

	/* Note its index (1-based!) */
	rte->perminfoindex = list_length(*rteperminfos);

	return perminfo;
}

/*
 * getRTEPermissionInfo
 *		Find RTEPermissionInfo for a given relation in the provided list.
 *
 * This is a simple list_nth() operation, though it's good to have the
 * function for the various sanity checks.
 */
RTEPermissionInfo *
getRTEPermissionInfo(List *rteperminfos, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	if (rte->perminfoindex == 0 ||
		rte->perminfoindex > list_length(rteperminfos))
		elog(ERROR, "invalid perminfoindex %u in RTE with relid %u",
			 rte->perminfoindex, rte->relid);
	perminfo = list_nth_node(RTEPermissionInfo, rteperminfos,
							 rte->perminfoindex - 1);
	if (perminfo->relid != rte->relid)
		elog(ERROR, "permission info at index %u (with relid=%u) does not match provided RTE (with relid=%u)",
			 rte->perminfoindex, perminfo->relid, rte->relid);

	return perminfo;
}
