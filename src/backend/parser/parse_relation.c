/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  parser support routines dealing with relations
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_relation.c,v 1.52 2001/02/14 21:35:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


static Node *scanNameSpaceForRefname(ParseState *pstate, Node *nsnode,
									 char *refname);
static Node *scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
							  char *colname);
static Node *scanJoinForColumn(JoinExpr *join, char *colname,
							   int sublevels_up);
static bool isForUpdate(ParseState *pstate, char *relname);
static List *expandNamesVars(ParseState *pstate, List *names, List *vars);
static void warnAutoRange(ParseState *pstate, char *refname);


/*
 * Information defining the "system" attributes of every relation.
 */
static struct
{
	char	   *attrname;		/* name of system attribute */
	int			attrnum;		/* its attribute number (always < 0) */
	Oid			attrtype;		/* its type id */
}			special_attr[] =

{
	{
		"ctid", SelfItemPointerAttributeNumber, TIDOID
	},
	{
		"oid", ObjectIdAttributeNumber, OIDOID
	},
	{
		"xmin", MinTransactionIdAttributeNumber, XIDOID
	},
	{
		"cmin", MinCommandIdAttributeNumber, CIDOID
	},
	{
		"xmax", MaxTransactionIdAttributeNumber, XIDOID
	},
	{
		"cmax", MaxCommandIdAttributeNumber, CIDOID
	},
	{
		"tableoid", TableOidAttributeNumber, OIDOID
	}
};

#define SPECIALS ((int) (sizeof(special_attr)/sizeof(special_attr[0])))


/*
 * refnameRangeOrJoinEntry
 *	  Given a refname, look to see if it matches any RTE or join table.
 *	  If so, return a pointer to the RangeTblEntry or JoinExpr.
 *	  Optionally get its nesting depth (0 = current).	If sublevels_up
 *	  is NULL, only consider items at the current nesting level.
 */
Node *
refnameRangeOrJoinEntry(ParseState *pstate,
						char *refname,
						int *sublevels_up)
{
	if (sublevels_up)
		*sublevels_up = 0;

	while (pstate != NULL)
	{
		Node	   *rte;

		rte = scanNameSpaceForRefname(pstate,
									  (Node *) pstate->p_namespace,
									  refname);
		if (rte)
			return rte;

		pstate = pstate->parentParseState;
		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;
	}
	return NULL;
}

/*
 * Recursively search a namespace for an RTE or joinexpr with given refname.
 *
 * The top level of p_namespace is a list, and we recurse into any joins
 * that are not subqueries.  It is also possible to pass an individual
 * join subtree (useful when checking for name conflicts within a scope).
 *
 * Note: we do not worry about the possibility of multiple matches;
 * we assume the code that built the namespace checked for duplicates.
 */
static Node *
scanNameSpaceForRefname(ParseState *pstate, Node *nsnode,
						char *refname)
{
	Node	   *result = NULL;

	if (nsnode == NULL)
		return NULL;
	if (IsA(nsnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) nsnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, pstate->p_rtable);

		if (strcmp(rte->eref->relname, refname) == 0)
			result = (Node *) rte;
	}
	else if (IsA(nsnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) nsnode;

		if (j->alias)
		{
			if (strcmp(j->alias->relname, refname) == 0)
				return (Node *) j; /* matched a join alias */
			/*
			 * Tables within an aliased join are invisible from outside
			 * the join, according to the scope rules of SQL92 (the join
			 * is considered a subquery).  So, stop here.
			 */
			return NULL;
		}
		result = scanNameSpaceForRefname(pstate, j->larg, refname);
		if (! result)
			result = scanNameSpaceForRefname(pstate, j->rarg, refname);
	}
	else if (IsA(nsnode, List))
	{
		List	   *l;

		foreach(l, (List *) nsnode)
		{
			result = scanNameSpaceForRefname(pstate, lfirst(l), refname);
			if (result)
				break;
		}
	}
	else
		elog(ERROR, "scanNameSpaceForRefname: unexpected node type %d",
			 nodeTag(nsnode));
	return result;
}

/* Convenience subroutine for checkNameSpaceConflicts */
static void
scanNameSpaceForConflict(ParseState *pstate, Node *nsnode,
						 char *refname)
{
	if (scanNameSpaceForRefname(pstate, nsnode, refname) != NULL)
		elog(ERROR, "Table name \"%s\" specified more than once", refname);
}

/*
 * Recursively check for refname conflicts between two namespaces or
 * namespace subtrees.  Raise an error if any is found.
 *
 * Works by recursively scanning namespace1 in the same way that
 * scanNameSpaceForRefname does, and then looking in namespace2 for
 * a match to each refname found in namespace1.
 *
 * Note: we assume that each given argument does not contain conflicts
 * itself; we just want to know if the two can be merged together.
 */
void
checkNameSpaceConflicts(ParseState *pstate, Node *namespace1,
						Node *namespace2)
{
	if (namespace1 == NULL)
		return;
	if (IsA(namespace1, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) namespace1)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, pstate->p_rtable);

		scanNameSpaceForConflict(pstate, namespace2, rte->eref->relname);
	}
	else if (IsA(namespace1, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) namespace1;

		if (j->alias)
		{
			scanNameSpaceForConflict(pstate, namespace2, j->alias->relname);
			/*
			 * Tables within an aliased join are invisible from outside
			 * the join, according to the scope rules of SQL92 (the join
			 * is considered a subquery).  So, stop here.
			 */
			return;
		}
		checkNameSpaceConflicts(pstate, j->larg, namespace2);
		checkNameSpaceConflicts(pstate, j->rarg, namespace2);
	}
	else if (IsA(namespace1, List))
	{
		List	   *l;

		foreach(l, (List *) namespace1)
		{
			checkNameSpaceConflicts(pstate, lfirst(l), namespace2);
		}
	}
	else
		elog(ERROR, "checkNameSpaceConflicts: unexpected node type %d",
			 nodeTag(namespace1));
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
	List	   *temp;

	if (sublevels_up)
		*sublevels_up = 0;

	while (pstate != NULL)
	{
		index = 1;
		foreach(temp, pstate->p_rtable)
		{
			if (rte == (RangeTblEntry *) lfirst(temp))
				return index;
			index++;
		}
		pstate = pstate->parentParseState;
		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;
	}

	elog(ERROR, "RTERangeTablePosn: RTE not found (internal error)");
	return 0;					/* keep compiler quiet */
}

/*
 * scanRTEForColumn
 *	  Search the column names of a single RTE for the given name.
 *	  If found, return an appropriate Var node, else return NULL.
 *	  If the name proves ambiguous within this RTE, raise error.
 *
 * Side effect: if we find a match, mark the RTE as requiring read access.
 * See comments in setTargetTable().
 */
static Node *
scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte, char *colname)
{
	Node	   *result = NULL;
	int			attnum = 0;
	List	   *c;

	/*
	 * Scan the user column names (or aliases) for a match.
	 * Complain if multiple matches.
	 */
	foreach(c, rte->eref->attrs)
	{
		attnum++;
		if (strcmp(strVal(lfirst(c)), colname) == 0)
		{
			if (result)
				elog(ERROR, "Column reference \"%s\" is ambiguous", colname);
			result = (Node *) make_var(pstate, rte, attnum);
			rte->checkForRead = true;
		}
	}

	/*
	 * If we have a unique match, return it.  Note that this allows a user
	 * alias to override a system column name (such as OID) without error.
	 */
	if (result)
		return result;

	/*
	 * If the RTE represents a table (not a sub-select), consider system
	 * column names.
	 */
	if (rte->relid != InvalidOid)
	{
		attnum = specialAttNum(colname);
		if (attnum != InvalidAttrNumber)
		{
			result = (Node *) make_var(pstate, rte, attnum);
			rte->checkForRead = true;
		}
	}

	return result;
}

/*
 * scanJoinForColumn
 *	  Search the column names of a single join table for the given name.
 *	  If found, return an appropriate Var node or expression, else return NULL.
 *	  If the name proves ambiguous within this jointable, raise error.
 *
 * NOTE: unlike scanRTEForColumn, there's no need to worry about forcing
 * checkForRead true for the referenced tables.  This is so because a join
 * expression can only appear in a FROM clause, and any table named in
 * FROM will be marked checkForRead from the beginning.
 */
static Node *
scanJoinForColumn(JoinExpr *join, char *colname, int sublevels_up)
{
	Node	   *result = NULL;
	int			attnum = 0;
	List	   *c;

	foreach(c, join->colnames)
	{
		attnum++;
		if (strcmp(strVal(lfirst(c)), colname) == 0)
		{
			if (result)
				elog(ERROR, "Column reference \"%s\" is ambiguous", colname);
			result = copyObject(nth(attnum-1, join->colvars));
			/*
			 * If referencing an uplevel join item, we must adjust
			 * sublevels settings in the copied expression.
			 */
			if (sublevels_up > 0)
				IncrementVarSublevelsUp(result, sublevels_up, 0);
		}
	}
	return result;
}

/*
 * colnameToVar
 *	  Search for an unqualified column name.
 *	  If found, return the appropriate Var node (or expression).
 *	  If not found, return NULL.  If the name proves ambiguous, raise error.
 */
Node *
colnameToVar(ParseState *pstate, char *colname)
{
	Node	   *result = NULL;
	ParseState *orig_pstate = pstate;
	int			levels_up = 0;

	while (pstate != NULL)
	{
		List	   *ns;

		/*
		 * We need to look only at top-level namespace items, and even for
		 * those, ignore RTEs that are marked as not inFromCl and not
		 * the query's target relation.
		 */
		foreach(ns, pstate->p_namespace)
		{
			Node   *nsnode = (Node *) lfirst(ns);
			Node   *newresult = NULL;

			if (IsA(nsnode, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) nsnode)->rtindex;
				RangeTblEntry *rte = rt_fetch(varno, pstate->p_rtable);

				if (! rte->inFromCl &&
					rte != pstate->p_target_rangetblentry)
					continue;

				/* use orig_pstate here to get the right sublevels_up */
				newresult = scanRTEForColumn(orig_pstate, rte, colname);
			}
			else if (IsA(nsnode, JoinExpr))
			{
				JoinExpr   *j = (JoinExpr *) nsnode;

				newresult = scanJoinForColumn(j, colname, levels_up);
			}
			else
				elog(ERROR, "colnameToVar: unexpected node type %d",
					 nodeTag(nsnode));

			if (newresult)
			{
				if (result)
					elog(ERROR, "Column reference \"%s\" is ambiguous",
						 colname);
				result = newresult;
			}
		}

		if (result != NULL)
			break;				/* found */

		pstate = pstate->parentParseState;
		levels_up++;
	}

	return result;
}

/*
 * qualifiedNameToVar
 *	  Search for a qualified column name (refname + column name).
 *	  If found, return the appropriate Var node (or expression).
 *	  If not found, return NULL.  If the name proves ambiguous, raise error.
 */
Node *
qualifiedNameToVar(ParseState *pstate, char *refname, char *colname,
				   bool implicitRTEOK)
{
	Node	   *result;
	Node	   *rteorjoin;
	int			sublevels_up;

	rteorjoin = refnameRangeOrJoinEntry(pstate, refname, &sublevels_up);

	if (rteorjoin == NULL)
	{
		if (! implicitRTEOK)
			return NULL;
		rteorjoin = (Node *) addImplicitRTE(pstate, refname);
		sublevels_up = 0;
	}

	if (IsA(rteorjoin, RangeTblEntry))
		result = scanRTEForColumn(pstate, (RangeTblEntry *) rteorjoin,
								  colname);
	else if (IsA(rteorjoin, JoinExpr))
		result = scanJoinForColumn((JoinExpr *) rteorjoin,
								   colname, sublevels_up);
	else
	{
		elog(ERROR, "qualifiedNameToVar: unexpected node type %d",
			 nodeTag(rteorjoin));
		result = NULL;			/* keep compiler quiet */
	}

	return result;
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
				   char *relname,
				   Attr *alias,
				   bool inh,
				   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->relname : relname;
	LOCKMODE	lockmode;
	Relation	rel;
	Attr	   *eref;
	int			maxattrs;
	int			numaliases;
	int			varattno;

	rte->relname = relname;
	rte->alias = alias;
	rte->subquery = NULL;

	/*
	 * Get the rel's OID.  This access also ensures that we have an
	 * up-to-date relcache entry for the rel.  Since this is typically
	 * the first access to a rel in a statement, be careful to get the
	 * right access level depending on whether we're doing SELECT FOR UPDATE.
	 */
	lockmode = isForUpdate(pstate, relname) ? RowShareLock : AccessShareLock;
	rel = heap_openr(relname, lockmode);
	rte->relid = RelationGetRelid(rel);

	eref = alias ? (Attr *) copyObject(alias) : makeAttr(refname, NULL);
	numaliases = length(eref->attrs);

	/*
	 * Since the rel is open anyway, let's check that the
	 * number of column aliases is reasonable. - Thomas 2000-02-04
	 */
	maxattrs = RelationGetNumberOfAttributes(rel);
	if (maxattrs < numaliases)
		elog(ERROR, "Table \"%s\" has %d columns available but %d columns specified",
			 refname, maxattrs, numaliases);

	/* fill in any unspecified alias columns */
	for (varattno = numaliases; varattno < maxattrs; varattno++)
	{
		char	   *attrname;

		attrname = pstrdup(NameStr(rel->rd_att->attrs[varattno]->attname));
		eref->attrs = lappend(eref->attrs, makeString(attrname));
	}
	rte->eref = eref;

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
	 * - this RTE should be checked for read/write access rights.
	 *
	 * The initial default on access checks is always check-for-READ-access,
	 * which is the right thing for all except target tables.
	 *----------
	 */
	rte->inh = inh;
	rte->inFromCl = inFromCl;
	rte->checkForRead = true;
	rte->checkForWrite = false;

	rte->checkAsUser = InvalidOid; /* not set-uid by default, either */

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
							  Attr *alias,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias->relname;
	Attr	   *eref;
	int			numaliases;
	int			varattno;
	List	   *tlistitem;

	rte->relname = NULL;
	rte->relid = InvalidOid;
	rte->subquery = subquery;
	rte->alias = alias;

	eref = copyObject(alias);
	numaliases = length(eref->attrs);

	/* fill in any unspecified alias columns */
	varattno = 0;
	foreach(tlistitem, subquery->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

		if (te->resdom->resjunk)
			continue;
		varattno++;
		Assert(varattno == te->resdom->resno);
		if (varattno > numaliases)
		{
			char	   *attrname;

			attrname = pstrdup(te->resdom->resname);
			eref->attrs = lappend(eref->attrs, makeString(attrname));
		}
	}
	if (varattno < numaliases)
		elog(ERROR, "Table \"%s\" has %d columns available but %d columns specified",
			 refname, varattno, numaliases);

	rte->eref = eref;

	/*----------
	 * Flags:
	 * - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause,
	 * - this RTE should be checked for read/write access rights.
	 *
	 * Subqueries are never checked for access rights.
	 *----------
	 */
	rte->inh = false;			/* never true for subqueries */
	rte->inFromCl = inFromCl;
	rte->checkForRead = false;
	rte->checkForWrite = false;

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
 * Has the specified relname been selected FOR UPDATE?
 */
static bool
isForUpdate(ParseState *pstate, char *relname)
{
	/* Outer loop to check parent query levels as well as this one */
	while (pstate != NULL)
	{
		if (pstate->p_forUpdate != NIL)
		{
			if (lfirst(pstate->p_forUpdate) == NULL)
			{
				/* all tables used in query */
				return true;
			}
			else
			{
				/* just the named tables */
				List   *l;

				foreach(l, pstate->p_forUpdate)
				{
					char	   *rname = lfirst(l);

					if (strcmp(relname, rname) == 0)
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
 * and/or name space list.  (We assume caller has checked for any
 * namespace conflict.)
 */
void
addRTEtoQuery(ParseState *pstate, RangeTblEntry *rte,
			  bool addToJoinList, bool addToNameSpace)
{
	int			rtindex = RTERangeTablePosn(pstate, rte, NULL);
	RangeTblRef *rtr = makeNode(RangeTblRef);

	rtr->rtindex = rtindex;

	if (addToJoinList)
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	if (addToNameSpace)
		pstate->p_namespace = lappend(pstate->p_namespace, rtr);
}

/*
 * Add a POSTQUEL-style implicit RTE.
 *
 * We assume caller has already checked that there is no RTE or join with
 * a conflicting name.
 */
RangeTblEntry *
addImplicitRTE(ParseState *pstate, char *relname)
{
	RangeTblEntry *rte;

	rte = addRangeTableEntry(pstate, relname, NULL, false, false);
	addRTEtoQuery(pstate, rte, true, true);
	warnAutoRange(pstate, relname);

	return rte;
}

/* expandRTE()
 *
 * Given a rangetable entry, create lists of its column names (aliases if
 * provided, else real names) and Vars for each column.  Only user columns
 * are considered, since this is primarily used to expand '*' and determine
 * the contents of JOIN tables.
 *
 * If only one of the two kinds of output list is needed, pass NULL for the
 * output pointer for the unwanted one.
 */
void
expandRTE(ParseState *pstate, RangeTblEntry *rte,
		  List **colnames, List **colvars)
{
	int			rtindex,
				sublevels_up,
				varattno;

	if (colnames)
		*colnames = NIL;
	if (colvars)
		*colvars = NIL;

	/* Need the RT index of the entry for creating Vars */
	rtindex = RTERangeTablePosn(pstate, rte, &sublevels_up);

	if (rte->relname)
	{
		/* Ordinary relation RTE */
		Relation	rel;
		int			maxattrs;

		rel = heap_openr(rte->relname, AccessShareLock);

		maxattrs = RelationGetNumberOfAttributes(rel);

		for (varattno = 0; varattno < maxattrs; varattno++)
		{
			Form_pg_attribute attr = rel->rd_att->attrs[varattno];

#ifdef	_DROP_COLUMN_HACK__
			if (COLUMN_IS_DROPPED(attr))
				continue;
#endif	 /* _DROP_COLUMN_HACK__ */

			if (colnames)
			{
				char	   *label;

				if (varattno < length(rte->eref->attrs))
					label = strVal(nth(varattno, rte->eref->attrs));
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

		heap_close(rel, AccessShareLock);
	}
	else
	{
		/* Subquery RTE */
		List	   *aliasp = rte->eref->attrs;
		List	   *tlistitem;

		varattno = 0;
		foreach(tlistitem, rte->subquery->targetList)
		{
			TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

			if (te->resdom->resjunk)
				continue;
			varattno++;
			Assert(varattno == te->resdom->resno);

			if (colnames)
			{
				/* Assume there is one alias per target item */
				char	   *label = strVal(lfirst(aliasp));

				*colnames = lappend(*colnames, makeString(pstrdup(label)));
				aliasp = lnext(aliasp);
			}

			if (colvars)
			{
				Var		   *varnode;

				varnode = makeVar(rtindex, varattno,
								  te->resdom->restype,
								  te->resdom->restypmod,
								  sublevels_up);

				*colvars = lappend(*colvars, varnode);
			}
		}
	}
}

/*
 * expandRelAttrs -
 *	  makes a list of TargetEntry nodes for the attributes of the rel
 */
List *
expandRelAttrs(ParseState *pstate, RangeTblEntry *rte)
{
	List	   *name_list,
			   *var_list;

	expandRTE(pstate, rte, &name_list, &var_list);

	return expandNamesVars(pstate, name_list, var_list);
}

/*
 * expandJoinAttrs -
 *	  makes a list of TargetEntry nodes for the attributes of the join
 */
List *
expandJoinAttrs(ParseState *pstate, JoinExpr *join, int sublevels_up)
{
	List	   *vars;

	vars = copyObject(join->colvars);
	/*
	 * If referencing an uplevel join item, we must adjust
	 * sublevels settings in the copied expression.
	 */
	if (sublevels_up > 0)
		IncrementVarSublevelsUp((Node *) vars, sublevels_up, 0);

	return expandNamesVars(pstate,
						   copyObject(join->colnames),
						   vars);
}

/*
 * expandNamesVars -
 *		Workhorse for "*" expansion: produce a list of targetentries
 *		given lists of column names (as String nodes) and var references.
 */
static List *
expandNamesVars(ParseState *pstate, List *names, List *vars)
{
	List	   *te_list = NIL;

	while (names)
	{
		char	   *label = strVal(lfirst(names));
		Node	   *varnode = (Node *) lfirst(vars);
		TargetEntry *te = makeNode(TargetEntry);

		te->resdom = makeResdom((AttrNumber) (pstate->p_last_resno)++,
								exprType(varnode),
								exprTypmod(varnode),
								label,
								false);
		te->expr = varnode;
		te_list = lappend(te_list, te);

		names = lnext(names);
		vars = lnext(vars);
	}

	Assert(vars == NIL);		/* lists not same length? */

	return te_list;
}

/* ----------
 * get_rte_attribute_name
 *		Get an attribute name from a RangeTblEntry
 *
 * This is unlike get_attname() because we use aliases if available.
 * In particular, it will work on an RTE for a subselect, whereas
 * get_attname() only works on real relations.
 *
 * XXX Actually, this is completely bogus, because refnames of RTEs are
 * not guaranteed unique, and may not even have scope across the whole
 * query.  Cleanest fix would be to add refname/attname to Var nodes and
 * just print those, rather than indulging in this hack.
 * ----------
 */
char *
get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum)
{
	char	   *attname;

	/*
	 * If there is an alias, use it
	 */
	if (attnum > 0 && attnum <= length(rte->eref->attrs))
		return strVal(nth(attnum-1, rte->eref->attrs));
	/*
	 * Can get here for a system attribute (which never has an alias),
	 * or if alias name list is too short (which probably can't happen
	 * anymore).  Neither of these cases is valid for a subselect RTE.
	 */
	if (rte->relid == InvalidOid)
		elog(ERROR, "Invalid attnum %d for rangetable entry %s",
			 attnum, rte->eref->relname);
	/*
	 * Use the real name of the table's column
	 */
	attname = get_attname(rte->relid, attnum);
	if (attname == NULL)
		elog(ERROR, "cache lookup of attribute %d in relation %u failed",
			 attnum, rte->relid);
	return attname;
}

/*
 *	given relation and att name, return id of variable
 *
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version get_attnum()
 *	for access to non-opened relations.
 */
int
attnameAttNum(Relation rd, char *a)
{
	int			i;

	for (i = 0; i < rd->rd_rel->relnatts; i++)
		if (!namestrcmp(&(rd->rd_att->attrs[i]->attname), a))
			return i + 1;

	if ((i = specialAttNum(a)) != InvalidAttrNumber)
		return i;

	/* on failure */
	elog(ERROR, "Relation '%s' does not have attribute '%s'",
		 RelationGetRelationName(rd), a);
	return InvalidAttrNumber;	/* lint */
}

/* specialAttNum()
 * Check attribute name to see if it is "special", e.g. "oid".
 * - thomas 2000-02-07
 */
int
specialAttNum(char *a)
{
	int			i;

	for (i = 0; i < SPECIALS; i++)
		if (strcmp(special_attr[i].attrname, a) == 0)
			return special_attr[i].attrnum;

	return InvalidAttrNumber;
}


#ifdef NOT_USED
/*
 * Given range variable, return whether attribute of this name
 * is a set.
 * NOTE the ASSUMPTION here that no system attributes are, or ever
 * will be, sets.
 *
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version get_attisset()
 *	for access to non-opened relations.
 */
bool
attnameIsSet(Relation rd, char *name)
{
	int			i;

	/* First check if this is a system attribute */
	for (i = 0; i < SPECIALS; i++)
	{
		if (strcmp(special_attr[i].attrname, name) == 0)
			return false;		/* no sys attr is a set */
	}
	return get_attisset(RelationGetRelid(rd), name);
}
#endif

#ifdef NOT_USED
/*
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version
 *	for access to non-opened relations.
 */
int
attnumAttNelems(Relation rd, int attid)
{
	return rd->rd_att->attrs[attid - 1]->attnelems;
}
#endif

/* given attribute id, return type of that attribute */
/*
 *	This should only be used if the relation is already
 *	heap_open()'ed.  Use the cache version get_atttype()
 *	for access to non-opened relations.
 */
Oid
attnumTypeId(Relation rd, int attid)
{
	if (attid < 0)
	{
		int			i;

		for (i = 0; i < SPECIALS; i++)
		{
			if (special_attr[i].attrnum == attid)
				return special_attr[i].attrtype;
		}
		/* negative but not a valid system attr? */
		elog(ERROR, "attnumTypeId: bogus attribute number %d", attid);
	}

	/*
	 * -1 because attid is 1-based
	 */
	return rd->rd_att->attrs[attid - 1]->atttypid;
}

/*
 * Generate a warning about an implicit RTE, if appropriate.
 *
 * Our current theory on this is that we should allow "SELECT foo.*"
 * but warn about a mixture of explicit and implicit RTEs.
 */
static void
warnAutoRange(ParseState *pstate, char *refname)
{
	bool		foundInFromCl = false;
	List	   *temp;

	foreach(temp, pstate->p_rtable)
	{
		RangeTblEntry *rte = lfirst(temp);

		if (rte->inFromCl)
		{
			foundInFromCl = true;
			break;
		}
	}
	if (foundInFromCl)
		elog(NOTICE, "Adding missing FROM-clause entry%s for table \"%s\"",
			 pstate->parentParseState != NULL ? " in subquery" : "",
			 refname);
}
