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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_relation.c,v 1.40 2000/04/12 17:15:27 momjian Exp $
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

#define SPECIALS ((int) (sizeof(special_attr)/sizeof(*special_attr)))

static char *attnum_type[SPECIALS] = {
	"tid",
	"oid",
	"xid",
	"cid",
	"xid",
	"cid",
};

/* refnameRangeTableEntries()
 * Given refname, return a list of range table entries
 * This is possible with JOIN syntax, where tables in a join
 * acquire the same reference name.
 * - thomas 2000-01-20
 * But at the moment we aren't carrying along a full list of
 * table/column aliases, so we don't have the full mechanism
 * to support outer joins in place yet.
 * - thomas 2000-03-04
 */
List *
			refnameRangeTableEntries(ParseState *pstate, char *refname);

List *
refnameRangeTableEntries(ParseState *pstate, char *refname)
{
	List	   *rteList = NULL;
	List	   *temp;

	while (pstate != NULL)
	{
		foreach(temp, pstate->p_rtable)
		{
			RangeTblEntry *rte = lfirst(temp);

			if (strcmp(rte->eref->relname, refname) == 0)
				rteList = lappend(rteList, rte);
		}
		pstate = pstate->parentParseState;
	}
	return rteList;
}

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

			if (strcmp(rte->eref->relname, refname) == 0)
				return rte;
		}
		pstate = pstate->parentParseState;
	}
	return NULL;
}

/* given refname, return RT index (starting with 1) of the relation,
 * and optionally get its nesting depth (0 = current).	If sublevels_up
 * is NULL, only consider rels at the current nesting level.
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
 * returns range entry if found, else NULL
 */
RangeTblEntry *
colnameRangeTableEntry(ParseState *pstate, char *colname)
{
	List	   *et;
	List	   *rtable;
	RangeTblEntry *rte_result = NULL;

	while (pstate != NULL)
	{
		if (pstate->p_is_rule)
			rtable = lnext(lnext(pstate->p_rtable));
		else
			rtable = pstate->p_rtable;

		foreach(et, rtable)
		{
			RangeTblEntry *rte_candidate = NULL;
			RangeTblEntry *rte = lfirst(et);

			/* only consider RTEs mentioned in FROM or UPDATE/DELETE */
			if (!rte->inFromCl && rte != pstate->p_target_rangetblentry)
				continue;

			if (rte->eref->attrs != NULL)
			{
				List	   *c;

				foreach(c, rte->ref->attrs)
				{
					if (strcmp(strVal(lfirst(c)), colname) == 0)
					{
						if (rte_candidate != NULL)
							elog(ERROR, "Column '%s' is ambiguous"
								 " (internal error)", colname);
						rte_candidate = rte;
					}
				}
			}

			/*
			 * Even if we have an attribute list in the RTE, look for the
			 * column here anyway. This is the only way we will find
			 * implicit columns like "oid". - thomas 2000-02-07
			 */
			if ((rte_candidate == NULL)
				&& (get_attnum(rte->relid, colname) != InvalidAttrNumber))
				rte_candidate = rte;

			if (rte_candidate == NULL)
				continue;

			if (rte_result != NULL)
			{
				if (!pstate->p_is_insert ||
					rte != pstate->p_target_rangetblentry)
					elog(ERROR, "Column '%s' is ambiguous", colname);
			}
			else
				rte_result = rte;
		}

		if (rte_result != NULL)
			break;				/* found */

		pstate = pstate->parentParseState;
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
				   Attr *ref,
				   bool inh,
				   bool inFromCl,
				   bool inJoinSet)
{
	Relation	rel;
	RangeTblEntry *rte;
	Attr	   *eref;
	int			maxattrs;
	int			sublevels_up;
	int			varattno;

	/* Look for an existing rte, if available... */
	if (pstate != NULL)
	{
		int			rt_index = refnameRangeTablePosn(pstate, ref->relname,
													 &sublevels_up);

		if (rt_index != 0 && (!inFromCl || sublevels_up == 0))
		{
			if (!strcmp(ref->relname, "*CURRENT*") || !strcmp(ref->relname, "*NEW*"))
				return (RangeTblEntry *) nth(rt_index - 1, pstate->p_rtable);
			elog(ERROR, "Table name '%s' specified more than once", ref->relname);
		}
	}

	rte = makeNode(RangeTblEntry);

	rte->relname = relname;
	rte->ref = ref;

	/*
	 * Get the rel's OID.  This access also ensures that we have an
	 * up-to-date relcache entry for the rel.  We don't need to keep it
	 * open, however. Since this is open anyway, let's check that the
	 * number of column aliases is reasonable. - Thomas 2000-02-04
	 */
	rel = heap_openr(relname, AccessShareLock);
	rte->relid = RelationGetRelid(rel);
	maxattrs = RelationGetNumberOfAttributes(rel);

	eref = copyObject(ref);
	if (maxattrs < length(eref->attrs))
		elog(ERROR, "Table '%s' has %d columns available but %d columns specified",
			 relname, maxattrs, length(eref->attrs));

	/* fill in any unspecified alias columns */
	for (varattno = length(eref->attrs); varattno < maxattrs; varattno++)
	{
		char	   *attrname;

		attrname = pstrdup(NameStr(rel->rd_att->attrs[varattno]->attname));
		eref->attrs = lappend(eref->attrs, makeString(attrname));
	}
	heap_close(rel, AccessShareLock);
	rte->eref = eref;

	/*
	 * Flags: - this RTE should be expanded to include descendant tables,
	 * - this RTE is in the FROM clause, - this RTE should be included in
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

/* expandTable()
 * Populates an Attr with table name and column names
 * This is similar to expandAll(), but does not create an RTE
 * if it does not already exist.
 * - thomas 2000-01-19
 */
Attr *
expandTable(ParseState *pstate, char *refname, bool getaliases)
{
	Attr	   *attr;
	RangeTblEntry *rte;
	Relation	rel;
	int			varattno,
				maxattrs;

	rte = refnameRangeTableEntry(pstate, refname);

	if (getaliases && (rte != NULL))
		return rte->eref;

	if (rte != NULL)
		rel = heap_open(rte->relid, AccessShareLock);
	else
		rel = heap_openr(refname, AccessShareLock);

	if (rel == NULL)
		elog(ERROR, "Relation '%s' not found", refname);

	maxattrs = RelationGetNumberOfAttributes(rel);

	attr = makeAttr(refname, NULL);

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		char	   *attrname;

#ifdef	_DROP_COLUMN_HACK__
		if (COLUMN_IS_DROPPED(rel->rd_att->attrs[varattno]))
			continue;
#endif	 /* _DROP_COLUMN_HACK__ */
		attrname = pstrdup(NameStr(rel->rd_att->attrs[varattno]->attname));
		attr->attrs = lappend(attr->attrs, makeString(attrname));
	}

	heap_close(rel, AccessShareLock);

	return attr;
}

/*
 * expandAll -
 *	  makes a list of attributes
 */
List *
expandAll(ParseState *pstate, char *relname, Attr *ref, int *this_resno)
{
	List	   *te_list = NIL;
	RangeTblEntry *rte;
	Relation	rel;
	int			varattno,
				maxattrs;

	rte = refnameRangeTableEntry(pstate, ref->relname);
	if (rte == NULL)
	{
		rte = addRangeTableEntry(pstate, relname, ref,
								 FALSE, FALSE, TRUE);
#ifdef WARN_FROM
		elog(NOTICE, "Adding missing FROM-clause entry%s for table %s",
			 pstate->parentParseState != NULL ? " in subquery" : "",
			 refname);
#endif
	}

	rel = heap_open(rte->relid, AccessShareLock);

	maxattrs = RelationGetNumberOfAttributes(rel);

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		char	   *attrname;
		char	   *label;
		Var		   *varnode;
		TargetEntry *te = makeNode(TargetEntry);

#ifdef	_DROP_COLUMN_HACK__
		if (COLUMN_IS_DROPPED(rel->rd_att->attrs[varattno]))
			continue;
#endif	 /* _DROP_COLUMN_HACK__ */
		attrname = pstrdup(NameStr(rel->rd_att->attrs[varattno]->attname));

		/*
		 * varattno is zero-based, so check that length() is always
		 * greater
		 */
		if (length(rte->eref->attrs) > varattno)
			label = pstrdup(strVal(nth(varattno, rte->eref->attrs)));
		else
			label = attrname;
		varnode = make_var(pstate, rte->relid, relname, attrname);

		/*
		 * Even if the elements making up a set are complex, the set
		 * itself is not.
		 */

		te->resdom = makeResdom((AttrNumber) (*this_resno)++,
								varnode->vartype,
								varnode->vartypmod,
								label,
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
		if (!strcmp(special_attr[i].field, a))
			return special_attr[i].code;

	return InvalidAttrNumber;
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
