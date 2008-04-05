/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  parser support routines dealing with relations
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_relation.c,v 1.125.2.1 2008/04/05 01:58:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/heapam.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* GUC parameter */
bool		add_missing_from;

static RangeTblEntry *scanNameSpaceForRefname(ParseState *pstate,
						const char *refname);
static RangeTblEntry *scanNameSpaceForRelid(ParseState *pstate, Oid relid);
static bool isLockedRel(ParseState *pstate, char *refname);
static void expandRelation(Oid relid, Alias *eref,
			   int rtindex, int sublevels_up,
			   bool include_dropped,
			   List **colnames, List **colvars);
static void expandTupleDesc(TupleDesc tupdesc, Alias *eref,
				int rtindex, int sublevels_up,
				bool include_dropped,
				List **colnames, List **colvars);
static int	specialAttNum(const char *attname);
static void warnAutoRange(ParseState *pstate, RangeVar *relation,
			  int location);


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
 * peculiar, but it's what SQL92 says to do.
 */
RangeTblEntry *
refnameRangeTblEntry(ParseState *pstate,
					 const char *schemaname,
					 const char *refname,
					 int *sublevels_up)
{
	Oid			relId = InvalidOid;

	if (sublevels_up)
		*sublevels_up = 0;

	if (schemaname != NULL)
	{
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname);
		relId = get_relname_relid(refname, namespaceId);
		if (!OidIsValid(relId))
			return NULL;
	}

	while (pstate != NULL)
	{
		RangeTblEntry *result;

		if (OidIsValid(relId))
			result = scanNameSpaceForRelid(pstate, relId);
		else
			result = scanNameSpaceForRefname(pstate, refname);

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
scanNameSpaceForRefname(ParseState *pstate, const char *refname)
{
	RangeTblEntry *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_relnamespace)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (strcmp(rte->eref->aliasname, refname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference \"%s\" is ambiguous",
								refname)));
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
scanNameSpaceForRelid(ParseState *pstate, Oid relid)
{
	RangeTblEntry *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_relnamespace)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		/* yes, the test for alias == NULL should be there... */
		if (rte->rtekind == RTE_RELATION &&
			rte->relid == relid &&
			rte->alias == NULL)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference %u is ambiguous",
								relid)));
			result = rte;
		}
	}
	return result;
}

/*
 * searchRangeTable
 *	  See if any RangeTblEntry could possibly match the RangeVar.
 *	  If so, return a pointer to the RangeTblEntry; else return NULL.
 *
 * This is different from refnameRangeTblEntry in that it considers every
 * entry in the ParseState's rangetable(s), not only those that are currently
 * visible in the p_relnamespace lists.  This behavior is invalid per the SQL
 * spec, and it may give ambiguous results (there might be multiple equally
 * valid matches, but only one will be returned).  This must be used ONLY
 * as a heuristic in giving suitable error messages.  See warnAutoRange.
 *
 * Notice that we consider both matches on actual relation name and matches
 * on alias.
 */
static RangeTblEntry *
searchRangeTable(ParseState *pstate, RangeVar *relation)
{
	Oid			relId = RangeVarGetRelid(relation, true);
	char	   *refname = relation->relname;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

			if (OidIsValid(relId) &&
				rte->rtekind == RTE_RELATION &&
				rte->relid == relId)
				return rte;
			if (strcmp(rte->eref->aliasname, refname) == 0)
				return rte;
		}

		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * Check for relation-name conflicts between two relnamespace lists.
 * Raise an error if any is found.
 *
 * Note: we assume that each given argument does not contain conflicts
 * itself; we just want to know if the two can be merged together.
 *
 * Per SQL92, two alias-less plain relation RTEs do not conflict even if
 * they have the same eref->aliasname (ie, same relation name), if they
 * are for different relation OIDs (implying they are in different schemas).
 */
void
checkNameSpaceConflicts(ParseState *pstate, List *namespace1,
						List *namespace2)
{
	ListCell   *l1;

	foreach(l1, namespace1)
	{
		RangeTblEntry *rte1 = (RangeTblEntry *) lfirst(l1);
		const char *aliasname1 = rte1->eref->aliasname;
		ListCell   *l2;

		foreach(l2, namespace2)
		{
			RangeTblEntry *rte2 = (RangeTblEntry *) lfirst(l2);

			if (strcmp(rte2->eref->aliasname, aliasname1) != 0)
				continue;		/* definitely no conflict */
			if (rte1->rtekind == RTE_RELATION && rte1->alias == NULL &&
				rte2->rtekind == RTE_RELATION && rte2->alias == NULL &&
				rte1->relid != rte2->relid)
				continue;		/* no conflict per SQL92 rule */
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
 * scanRTEForColumn
 *	  Search the column names of a single RTE for the given name.
 *	  If found, return an appropriate Var node, else return NULL.
 *	  If the name proves ambiguous within this RTE, raise error.
 *
 * Side effect: if we find a match, mark the RTE as requiring read access.
 * See comments in setTargetTable().
 *
 * NOTE: if the RTE is for a join, marking it as requiring read access does
 * nothing.  It might seem that we need to propagate the mark to all the
 * contained RTEs, but that is not necessary.  This is so because a join
 * expression can only appear in a FROM clause, and any table named in
 * FROM will be marked as requiring read access from the beginning.
 */
Node *
scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte, char *colname,
				 int location)
{
	Node	   *result = NULL;
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
			result = (Node *) make_var(pstate, rte, attnum);
			/* Require read access */
			rte->requiredPerms |= ACL_SELECT;
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
			if (SearchSysCacheExists(ATTNUM,
									 ObjectIdGetDatum(rte->relid),
									 Int16GetDatum(attnum),
									 0, 0))
			{
				result = (Node *) make_var(pstate, rte, attnum);
				/* Require read access */
				rte->requiredPerms |= ACL_SELECT;
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

		foreach(l, pstate->p_varnamespace)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
			Node	   *newresult;

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
 * qualifiedNameToVar
 *	  Search for a qualified column name: either refname.colname or
 *	  schemaname.relname.colname.
 *
 *	  If found, return the appropriate Var node.
 *	  If not found, return NULL.  If the name proves ambiguous, raise error.
 */
Node *
qualifiedNameToVar(ParseState *pstate,
				   char *schemaname,
				   char *refname,
				   char *colname,
				   bool implicitRTEOK,
				   int location)
{
	RangeTblEntry *rte;
	int			sublevels_up;

	rte = refnameRangeTblEntry(pstate, schemaname, refname, &sublevels_up);

	if (rte == NULL)
	{
		if (!implicitRTEOK)
			return NULL;
		rte = addImplicitRTE(pstate, makeRangeVar(schemaname, refname),
							 location);
	}

	return scanRTEForColumn(pstate, rte, colname, location);
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
	lockmode = isLockedRel(pstate, refname) ? RowShareLock : AccessShareLock;
	rel = heap_openrv(relation, lockmode);
	rte->relid = RelationGetRelid(rel);

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

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for appropriate access rights.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 *----------
	 */
	rte->inh = inh;
	rte->inFromCl = inFromCl;

	rte->requiredPerms = ACL_SELECT;
	rte->checkAsUser = InvalidOid;		/* not set-uid by default, either */

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

	/*
	 * Build the list of effective column names using user-supplied aliases
	 * and/or actual column names.
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for appropriate access rights.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 *----------
	 */
	rte->inh = inh;
	rte->inFromCl = inFromCl;

	rte->requiredPerms = ACL_SELECT;
	rte->checkAsUser = InvalidOid;		/* not set-uid by default, either */

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

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for appropriate access rights.
	 *
	 * Subqueries are never checked for access rights.
	 *----------
	 */
	rte->inh = false;			/* never true for subqueries */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;

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
					 errmsg("a column definition list is only allowed for functions returning \"record\"")));
	}
	else
	{
		if (functypclass == TYPEFUNC_RECORD)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("a column definition list is required for functions returning \"record\"")));
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
		 * funccoltypes/funccoltypmods lists.
		 */
		foreach(col, coldeflist)
		{
			ColumnDef  *n = (ColumnDef *) lfirst(col);
			char	   *attrname;
			Oid			attrtype;
			int32		attrtypmod;

			attrname = pstrdup(n->colname);
			if (n->typename->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("column \"%s\" cannot be declared SETOF",
								attrname)));
			eref->colnames = lappend(eref->colnames, makeString(attrname));
			attrtype = typenameTypeId(pstate, n->typename);
			attrtypmod = n->typename->typmod;
			rte->funccoltypes = lappend_oid(rte->funccoltypes, attrtype);
			rte->funccoltypmods = lappend_int(rte->funccoltypmods, attrtypmod);
		}
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("function \"%s\" in FROM has unsupported return type %s",
					funcname, format_type_be(funcrettype))));

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for appropriate access rights.
	 *
	 * Functions are never checked for access rights (at least, not by
	 * the RTE permissions mechanism).
	 *----------
	 */
	rte->inh = false;			/* never true for functions */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;

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
							Alias *alias,
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

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for appropriate access rights.
	 *
	 * Subqueries are never checked for access rights.
	 *----------
	 */
	rte->inh = false;			/* never true for values RTEs */
	rte->inFromCl = inFromCl;
	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;

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
	 * Fail if join has too many columns --- we must be able to reference
	 * any of the columns with an AttrNumber.
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

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for appropriate access rights.
	 *
	 * Joins are never checked for access rights.
	 *----------
	 */
	rte->inh = false;			/* never true for joins */
	rte->inFromCl = inFromCl;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;

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
 * Note: we pay no attention to whether it's FOR UPDATE vs FOR SHARE.
 */
static bool
isLockedRel(ParseState *pstate, char *refname)
{
	/* Outer loop to check parent query levels as well as this one */
	while (pstate != NULL)
	{
		ListCell   *l;

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
					char	   *rname = strVal(lfirst(l2));

					if (strcmp(refname, rname) == 0)
						return true;
				}
			}
		}
		pstate = pstate->parentParseState;
	}
	return false;
}

/*
 * Add the given RTE as a top-level entry in the pstate's join list
 * and/or name space lists.  (We assume caller has checked for any
 * namespace conflicts.)
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
	if (addToRelNameSpace)
		pstate->p_relnamespace = lappend(pstate->p_relnamespace, rte);
	if (addToVarNameSpace)
		pstate->p_varnamespace = lappend(pstate->p_varnamespace, rte);
}

/*
 * Add a POSTQUEL-style implicit RTE.
 *
 * We assume caller has already checked that there is no RTE or join with
 * a conflicting name.
 */
RangeTblEntry *
addImplicitRTE(ParseState *pstate, RangeVar *relation, int location)
{
	RangeTblEntry *rte;

	/* issue warning or error as needed */
	warnAutoRange(pstate, relation, location);

	/*
	 * Note that we set inFromCl true, so that the RTE will be listed
	 * explicitly if the parsetree is ever decompiled by ruleutils.c. This
	 * provides a migration path for views/rules that were originally written
	 * with implicit-RTE syntax.
	 */
	rte = addRangeTableEntry(pstate, relation, NULL, false, true);
	/* Add to joinlist and relnamespace, but not varnamespace */
	addRTEtoQuery(pstate, rte, true, true, false);

	return rte;
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
 * rtindex and sublevels_up are the varno and varlevelsup values to use
 * in the created Vars.  Ordinarily rtindex should match the actual position
 * of the RTE in its rangetable.
 *
 * The output lists go into *colnames and *colvars.
 * If only one of the two kinds of output list is needed, pass NULL for the
 * output pointer for the unwanted one.
 */
void
expandRTE(RangeTblEntry *rte, int rtindex, int sublevels_up,
		  bool include_dropped,
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
			expandRelation(rte->relid, rte->eref, rtindex, sublevels_up,
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
										  sublevels_up);

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
					expandTupleDesc(tupdesc, rte->eref, rtindex, sublevels_up,
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
										  sublevels_up);

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
						int			attnum = 0;

						forboth(l1, rte->funccoltypes, l2, rte->funccoltypmods)
						{
							Oid			attrtype = lfirst_oid(l1);
							int32		attrtypmod = lfirst_int(l2);
							Var		   *varnode;

							attnum++;
							varnode = makeVar(rtindex,
											  attnum,
											  attrtype,
											  attrtypmod,
											  sublevels_up);
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
				ListCell   *lc;

				varattno = 0;
				foreach(lc, (List *) linitial(rte->values_lists))
				{
					Node	   *col = (Node *) lfirst(lc);

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
					 * deleted columns in the join; but we have to check since
					 * this routine is also used by the rewriter, and joins
					 * found in stored rules might have join columns for
					 * since-deleted columns.  This will be signaled by a NULL
					 * Const in the alias-vars list.
					 */
					if (IsA(avar, Const))
					{
						if (include_dropped)
						{
							if (colnames)
								*colnames = lappend(*colnames,
													makeString(pstrdup("")));
							if (colvars)
								*colvars = lappend(*colvars,
												   copyObject(avar));
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
			   bool include_dropped,
			   List **colnames, List **colvars)
{
	Relation	rel;

	/* Get the tupledesc and turn it over to expandTupleDesc */
	rel = relation_open(relid, AccessShareLock);
	expandTupleDesc(rel->rd_att, eref, rtindex, sublevels_up, include_dropped,
					colnames, colvars);
	relation_close(rel, AccessShareLock);
}

/*
 * expandTupleDesc -- expandRTE subroutine
 */
static void
expandTupleDesc(TupleDesc tupdesc, Alias *eref,
				int rtindex, int sublevels_up,
				bool include_dropped,
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
					*colvars = lappend(*colvars, makeNullConst(INT4OID));
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
							  sublevels_up);

			*colvars = lappend(*colvars, varnode);
		}
	}
}

/*
 * expandRelAttrs -
 *	  Workhorse for "*" expansion: produce a list of targetentries
 *	  for the attributes of the rte
 *
 * As with expandRTE, rtindex/sublevels_up determine the varno/varlevelsup
 * fields of the Vars produced.  pstate->p_next_resno determines the resnos
 * assigned to the TLEs.
 */
List *
expandRelAttrs(ParseState *pstate, RangeTblEntry *rte,
			   int rtindex, int sublevels_up)
{
	List	   *names,
			   *vars;
	ListCell   *name,
			   *var;
	List	   *te_list = NIL;

	expandRTE(rte, rtindex, sublevels_up, false,
			  &names, &vars);

	forboth(name, names, var, vars)
	{
		char	   *label = strVal(lfirst(name));
		Node	   *varnode = (Node *) lfirst(var);
		TargetEntry *te;

		te = makeTargetEntry((Expr *) varnode,
							 (AttrNumber) pstate->p_next_resno++,
							 label,
							 false);
		te_list = lappend(te_list, te);
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
 *		Get attribute type information from a RangeTblEntry
 */
void
get_rte_attribute_type(RangeTblEntry *rte, AttrNumber attnum,
					   Oid *vartype, int32 *vartypmod)
{
	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				/* Plain relation RTE --- get the attribute's type info */
				HeapTuple	tp;
				Form_pg_attribute att_tup;

				tp = SearchSysCache(ATTNUM,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(attnum),
									0, 0);
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
				}
				else if (functypclass == TYPEFUNC_SCALAR)
				{
					/* Base data type, i.e. scalar */
					*vartype = funcrettype;
					*vartypmod = -1;
				}
				else if (functypclass == TYPEFUNC_RECORD)
				{
					*vartype = list_nth_oid(rte->funccoltypes, attnum - 1);
					*vartypmod = list_nth_int(rte->funccoltypmods, attnum - 1);
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
				List	   *collist = (List *) linitial(rte->values_lists);
				Node	   *col;

				if (attnum < 1 || attnum > list_length(collist))
					elog(ERROR, "values list %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				col = (Node *) list_nth(collist, attnum - 1);
				*vartype = exprType(col);
				*vartypmod = exprTypmod(col);
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
				*vartype = exprType(aliasvar);
				*vartypmod = exprTypmod(aliasvar);
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

				tp = SearchSysCache(ATTNUM,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(attnum),
									0, 0);
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
			/* Subselect and Values RTEs never have dropped columns */
			result = false;
			break;
		case RTE_JOIN:
			{
				/*
				 * A join RTE would not have dropped columns when constructed,
				 * but one in a stored rule might contain columns that were
				 * dropped from the underlying tables, if said columns are
				 * nowhere explicitly referenced in the rule.  This will be
				 * signaled to us by a NULL Const in the joinaliasvars list.
				 */
				Var		   *aliasvar;

				if (attnum <= 0 ||
					attnum > list_length(rte->joinaliasvars))
					elog(ERROR, "invalid varattno %d", attnum);
				aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);

				result = IsA(aliasvar, Const);
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

					tp = SearchSysCache(ATTNUM,
										ObjectIdGetDatum(funcrelid),
										Int16GetDatum(attnum),
										0, 0);
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
get_rowmark(Query *qry, Index rtindex)
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
 * Generate a warning or error about an implicit RTE, if appropriate.
 *
 * If ADD_MISSING_FROM is not enabled, raise an error. Otherwise, emit
 * a warning.
 */
static void
warnAutoRange(ParseState *pstate, RangeVar *relation, int location)
{
	RangeTblEntry *rte;
	int			sublevels_up;
	const char *badAlias = NULL;

	/*
	 * Check to see if there are any potential matches in the query's
	 * rangetable.	This affects the message we provide.
	 */
	rte = searchRangeTable(pstate, relation);

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
							 &sublevels_up) == rte)
		badAlias = rte->eref->aliasname;

	if (!add_missing_from)
	{
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
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 (pstate->parentParseState ?
			 errmsg("missing FROM-clause entry in subquery for table \"%s\"",
					relation->relname) :
					  errmsg("missing FROM-clause entry for table \"%s\"",
							 relation->relname)),
					 parser_errposition(pstate, location)));
	}
	else
	{
		/* just issue a warning */
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 (pstate->parentParseState ?
				  errmsg("adding missing FROM-clause entry in subquery for table \"%s\"",
						 relation->relname) :
				  errmsg("adding missing FROM-clause entry for table \"%s\"",
						 relation->relname)),
				 (badAlias ?
			errhint("Perhaps you meant to reference the table alias \"%s\".",
					badAlias) :
				  (rte ?
				   errhint("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						   rte->eref->aliasname) : 0)),
				 parser_errposition(pstate, location)));
	}
}
