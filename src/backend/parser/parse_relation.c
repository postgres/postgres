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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_relation.c,v 1.34 2000/01/26 05:56:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


static struct
{
	char	   *field;
	int			code;
}			special_attr[] =

{
	{
		"ctid", SelfItemPointerAttributeNumber
	},
	{
		"oid", ObjectIdAttributeNumber
	},
	{
		"xmin", MinTransactionIdAttributeNumber
	},
	{
		"cmin", MinCommandIdAttributeNumber
	},
	{
		"xmax", MaxTransactionIdAttributeNumber
	},
	{
		"cmax", MaxCommandIdAttributeNumber
	},
};

#define SPECIALS (sizeof(special_attr)/sizeof(*special_attr))

static char *attnum_type[SPECIALS] = {
	"tid",
	"oid",
	"xid",
	"cid",
	"xid",
	"cid",
};

/* given refname, return a pointer to the range table entry */
RangeTblEntry *
refnameRangeTableEntry(ParseState *pstate, char *refname)
{
	List	   *temp;

	while (pstate != NULL)
	{
		foreach(temp, pstate->p_rtable)
		{
			RangeTblEntry *rte = lfirst(temp);

			if (!strcmp(rte->refname, refname))
				return rte;
		}
		/* only allow correlated columns in WHERE clause */
		if (pstate->p_in_where_clause)
			pstate = pstate->parentParseState;
		else
			break;
	}
	return NULL;
}

/* given refname, return id of variable; position starts with 1 */
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

			if (!strcmp(rte->refname, refname))
				return index;
			index++;
		}
		/* only allow correlated columns in WHERE clause */
		if (pstate->p_in_where_clause)
		{
			pstate = pstate->parentParseState;
			if (sublevels_up)
				(*sublevels_up)++;
		}
		else
			break;
	}
	return 0;
}

/*
 * returns range entry if found, else NULL
 */
RangeTblEntry *
colnameRangeTableEntry(ParseState *pstate, char *colname)
{
	List	   *et;
	List	   *rtable;
	RangeTblEntry *rte_result;

	rte_result = NULL;
	while (pstate != NULL)
	{
		if (pstate->p_is_rule)
			rtable = lnext(lnext(pstate->p_rtable));
		else
			rtable = pstate->p_rtable;

		foreach(et, rtable)
		{
			RangeTblEntry *rte = lfirst(et);

			/* only consider RTEs mentioned in FROM or UPDATE/DELETE */
			if (!rte->inFromCl && rte != pstate->p_target_rangetblentry)
				continue;

			if (get_attnum(rte->relid, colname) != InvalidAttrNumber)
			{
				if (rte_result != NULL)
				{
					if (!pstate->p_is_insert ||
						rte != pstate->p_target_rangetblentry)
						elog(ERROR, "Column '%s' is ambiguous", colname);
				}
				else
					rte_result = rte;
			}
		}
		/* only allow correlated columns in WHERE clause */
		if (pstate->p_in_where_clause && rte_result == NULL)
			pstate = pstate->parentParseState;
		else
			break;
	}
	return rte_result;
}

/*
 * put new entry in pstate p_rtable structure, or return pointer
 * if pstate null
 */
RangeTblEntry *
addRangeTableEntry(ParseState *pstate,
				   char *relname,
				   char *refname,
				   bool inh,
				   bool inFromCl,
				   bool inJoinSet)
{
	Relation	relation;
	RangeTblEntry *rte;
	int			sublevels_up;

	if (pstate != NULL)
	{
		int			rt_index = refnameRangeTablePosn(pstate, refname,
													 &sublevels_up);

		if (rt_index != 0 && (!inFromCl || sublevels_up == 0))
		{
			if (!strcmp(refname, "*CURRENT*") || !strcmp(refname, "*NEW*"))
				return (RangeTblEntry *) nth(rt_index - 1, pstate->p_rtable);
			elog(ERROR, "Table name '%s' specified more than once", refname);
		}
	}

	rte = makeNode(RangeTblEntry);

	rte->relname = pstrdup(relname);
	rte->refname = pstrdup(refname);

	/* Get the rel's OID.  This access also ensures that we have an
	 * up-to-date relcache entry for the rel.  We don't need to keep
	 * it open, however.
	 */
	relation = heap_openr(relname, AccessShareLock);
	rte->relid = RelationGetRelid(relation);
	heap_close(relation, AccessShareLock);

	/*
	 * Flags: this RTE should be expanded to include descendant tables,
	 * this RTE is in the FROM clause, this RTE should be included in
	 * the planner's final join.
	 */
	rte->inh = inh;
	rte->inFromCl = inFromCl;
	rte->inJoinSet = inJoinSet;
	rte->skipAcl = false;		/* always starts out false */

	/*
	 * Add completed RTE to range table list.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	return rte;
}

/*
 * expandAll -
 *	  makes a list of attributes
 */
List *
expandAll(ParseState *pstate, char *relname, char *refname, int *this_resno)
{
	List	   *te_list = NIL;
	RangeTblEntry *rte;
	Relation	rel;
	int			varattno,
				maxattrs;

	rte = refnameRangeTableEntry(pstate, refname);
	if (rte == NULL)
	{
		rte = addRangeTableEntry(pstate, relname, refname,
								 FALSE, FALSE, TRUE);
#ifdef WARN_FROM
		elog(NOTICE,"Adding missing FROM-clause entry%s for table %s",
			pstate->parentParseState != NULL ? " in subquery" : "",
			refname);
#endif
	}

	rel = heap_open(rte->relid, AccessShareLock);

	maxattrs = RelationGetNumberOfAttributes(rel);

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		char	   *attrname;
		Var		   *varnode;
		TargetEntry *te = makeNode(TargetEntry);

		attrname = pstrdup(NameStr(rel->rd_att->attrs[varattno]->attname));
		varnode = make_var(pstate, rte->relid, refname, attrname);

		/*
		 * Even if the elements making up a set are complex, the set
		 * itself is not.
		 */

		te->resdom = makeResdom((AttrNumber) (*this_resno)++,
								varnode->vartype,
								varnode->vartypmod,
								attrname,
								(Index) 0,
								(Oid) 0,
								false);
		te->expr = (Node *) varnode;
		te_list = lappend(te_list, te);
	}

	heap_close(rel, AccessShareLock);

	return te_list;
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

	for (i = 0; i < SPECIALS; i++)
		if (!strcmp(special_attr[i].field, a))
			return special_attr[i].code;

	/* on failure */
	elog(ERROR, "Relation '%s' does not have attribute '%s'",
		 RelationGetRelationName(rd), a);
	return 0;					/* lint */
}

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
		if (!strcmp(special_attr[i].field, name))
		{
			return false;		/* no sys attr is a set */
		}
	}
	return get_attisset(RelationGetRelid(rd), name);
}

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
		return typeTypeId(typenameType(attnum_type[-attid - 1]));

	/*
	 * -1 because varattno (where attid comes from) returns one more than
	 * index
	 */
	return rd->rd_att->attrs[attid - 1]->atttypid;
}
