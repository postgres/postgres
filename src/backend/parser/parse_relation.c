/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  parser support routines dealing with relations
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_relation.c,v 1.49 2000/09/29 18:21:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"

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


static Node *scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
							  char *colname);
static Node *scanJoinForColumn(JoinExpr *join, char *colname,
							   int sublevels_up);
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
		List	   *temp;
		JoinExpr   *join;

		/*
		 * Check the rangetable for RTEs; if no match, recursively scan
		 * the joinlist for join tables.  We assume that no duplicate
		 * entries have been made in any one nesting level.
		 */
		foreach(temp, pstate->p_rtable)
		{
			RangeTblEntry *rte = lfirst(temp);

			if (strcmp(rte->eref->relname, refname) == 0)
				return (Node *) rte;
		}

		join = scanJoinListForRefname((Node *) pstate->p_joinlist, refname);
		if (join)
			return (Node *) join;

		pstate = pstate->parentParseState;
		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;
	}
	return NULL;
}

/*
 * Recursively search a joinlist for a joinexpr with given refname
 *
 * Note that during parse analysis, we don't expect to find a FromExpr node
 * in p_joinlist; its top level is just a bare List.
 */
JoinExpr *
scanJoinListForRefname(Node *jtnode, char *refname)
{
	JoinExpr   *result = NULL;

	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, List))
	{
		List	   *l;

		foreach(l, (List *) jtnode)
		{
			result = scanJoinListForRefname(lfirst(l), refname);
			if (result)
				break;
		}
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		/* ignore ... */
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (j->alias && strcmp(j->alias->relname, refname) == 0)
			return j;
		result = scanJoinListForRefname(j->larg, refname);
		if (! result)
			result = scanJoinListForRefname(j->rarg, refname);
	}
	else
		elog(ERROR, "scanJoinListForRefname: unexpected node type %d",
			 nodeTag(jtnode));
	return result;
}

/*
 * given refname, return a pointer to the range table entry.
 *
 * NOTE that this routine will ONLY find RTEs, not join tables.
 */
RangeTblEntry *
refnameRangeTableEntry(ParseState *pstate, char *refname)
{
	List	   *temp;

	while (pstate != NULL)
	{
		foreach(temp, pstate->p_rtable)
		{
			RangeTblEntry *rte = lfirst(temp);

			if (strcmp(rte->eref->relname, refname) == 0)
				return rte;
		}
		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * given refname, return RT index (starting with 1) of the relation,
 * and optionally get its nesting depth (0 = current).	If sublevels_up
 * is NULL, only consider rels at the current nesting level.
 * A zero result means name not found.
 *
 * NOTE that this routine will ONLY find RTEs, not join tables.
 */
int
refnameRangeTablePosn(ParseState *pstate, char *refname, int *sublevels_up)
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
			RangeTblEntry *rte = lfirst(temp);

			if (strcmp(rte->eref->relname, refname) == 0)
				return index;
			index++;
		}
		pstate = pstate->parentParseState;
		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;
	}
	return 0;
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
		List	   *jt;

		/*
		 * We want to look only at top-level jointree items, and even for
		 * those, ignore RTEs that are marked as not inFromCl and not
		 * the query's target relation.
		 */
		foreach(jt, pstate->p_joinlist)
		{
			Node   *jtnode = (Node *) lfirst(jt);
			Node   *newresult = NULL;

			if (IsA(jtnode, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) jtnode)->rtindex;
				RangeTblEntry *rte = rt_fetch(varno, pstate->p_rtable);

				if (! rte->inFromCl &&
					rte != pstate->p_target_rangetblentry)
					continue;

				/* use orig_pstate here to get the right sublevels_up */
				newresult = scanRTEForColumn(orig_pstate, rte, colname);
			}
			else if (IsA(jtnode, JoinExpr))
			{
				JoinExpr   *j = (JoinExpr *) jtnode;

				newresult = scanJoinForColumn(j, colname, levels_up);
			}
			else
				elog(ERROR, "colnameToVar: unexpected node type %d",
					 nodeTag(jtnode));

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
 * If the specified refname is already present, raise error.
 *
 * If pstate is NULL, we just build an RTE and return it without worrying
 * about membership in an rtable list.
 */
RangeTblEntry *
addRangeTableEntry(ParseState *pstate,
				   char *relname,
				   Attr *alias,
				   bool inh,
				   bool inFromCl)
{
	char	   *refname = alias ? alias->relname : relname;
	Relation	rel;
	RangeTblEntry *rte;
	Attr	   *eref;
	int			maxattrs;
	int			numaliases;
	int			varattno;

	/* Check for conflicting RTE or jointable alias (at level 0 only) */
	if (pstate != NULL)
	{
		Node   *rteorjoin = refnameRangeOrJoinEntry(pstate, refname, NULL);

		if (rteorjoin)
			elog(ERROR, "Table name \"%s\" specified more than once",
				 refname);
	}

	rte = makeNode(RangeTblEntry);

	rte->relname = relname;
	rte->alias = alias;
	rte->subquery = NULL;

	/*
	 * Get the rel's OID.  This access also ensures that we have an
	 * up-to-date relcache entry for the rel.  We don't need to keep it
	 * open, however. Since this is open anyway, let's check that the
	 * number of column aliases is reasonable. - Thomas 2000-02-04
	 */
	rel = heap_openr(relname, AccessShareLock);
	rte->relid = RelationGetRelid(rel);
	maxattrs = RelationGetNumberOfAttributes(rel);

	eref = alias ? (Attr *) copyObject(alias) : makeAttr(refname, NULL);
	numaliases = length(eref->attrs);

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

	heap_close(rel, AccessShareLock);

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
	 * Add completed RTE to range table list.
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
	char	   *refname = alias->relname;
	RangeTblEntry *rte;
	Attr	   *eref;
	int			numaliases;
	int			varattno;
	List	   *tlistitem;

	/* Check for conflicting RTE or jointable alias (at level 0 only) */
	if (pstate != NULL)
	{
		Node   *rteorjoin = refnameRangeOrJoinEntry(pstate, refname, NULL);

		if (rteorjoin)
			elog(ERROR, "Table name \"%s\" specified more than once",
				 refname);
	}

	rte = makeNode(RangeTblEntry);

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
	 * Add completed RTE to range table list.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * Add the given RTE as a top-level entry in the pstate's join list,
 * unless there already is an entry for it.
 */
void
addRTEtoJoinList(ParseState *pstate, RangeTblEntry *rte)
{
	int			rtindex = RTERangeTablePosn(pstate, rte, NULL);
	List	   *jt;
	RangeTblRef *rtr;

	foreach(jt, pstate->p_joinlist)
	{
		Node	   *n = (Node *) lfirst(jt);

		if (IsA(n, RangeTblRef))
		{
			if (rtindex == ((RangeTblRef *) n)->rtindex)
				return;			/* it's already being joined to */
		}
	}

	/* Not present, so add it */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rtindex;
	pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
}

/*
 * Add a POSTQUEL-style implicit RTE.
 *
 * We assume caller has already checked that there is no such RTE now.
 */
RangeTblEntry *
addImplicitRTE(ParseState *pstate, char *relname)
{
	RangeTblEntry *rte;

	rte = addRangeTableEntry(pstate, relname, NULL, false, false);
	addRTEtoJoinList(pstate, rte);
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

