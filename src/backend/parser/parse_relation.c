/*-------------------------------------------------------------------------
 *
 * parse_relation.c--
 *	  parser support routines dealing with relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_relation.c,v 1.5 1998/01/05 03:32:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

static void checkTargetTypes(ParseState *pstate, char *target_colname,
				 char *refname, char *colname);

struct
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
refnameRangeTableEntry(List *rtable, char *refname)
{
	List	   *temp;

	foreach(temp, rtable)
	{
		RangeTblEntry *rte = lfirst(temp);

		if (!strcmp(rte->refname, refname))
			return rte;
	}
	return NULL;
}

/* given refname, return id of variable; position starts with 1 */
int
refnameRangeTablePosn(List *rtable, char *refname)
{
	int			index;
	List	   *temp;

	index = 1;
	foreach(temp, rtable)
	{
		RangeTblEntry *rte = lfirst(temp);

		if (!strcmp(rte->refname, refname))
			return index;
		index++;
	}
	return (0);
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

	if (pstate->p_is_rule)
		rtable = lnext(lnext(pstate->p_rtable));
	else
		rtable = pstate->p_rtable;

	rte_result = NULL;
	foreach(et, rtable)
	{
		RangeTblEntry *rte = lfirst(et);

		/* only entries on outer(non-function?) scope */
		if (!rte->inFromCl && rte != pstate->p_target_rangetblentry)
			continue;

		if (get_attnum(rte->relid, colname) != InvalidAttrNumber)
		{
			if (rte_result != NULL)
			{
				if (!pstate->p_is_insert ||
					rte != pstate->p_target_rangetblentry)
					elog(ERROR, "Column %s is ambiguous", colname);
			}
			else
				rte_result = rte;
		}
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
				   bool inFromCl)
{
	Relation	relation;
	RangeTblEntry *rte = makeNode(RangeTblEntry);

	if (pstate != NULL &&
		refnameRangeTableEntry(pstate->p_rtable, refname) != NULL)
		elog(ERROR, "Table name %s specified more than once", refname);

	rte->relname = pstrdup(relname);
	rte->refname = pstrdup(refname);

	relation = heap_openr(relname);
	if (relation == NULL)
	{
		elog(ERROR, "%s: %s",
			 relname, aclcheck_error_strings[ACLCHECK_NO_CLASS]);
	}

	/*
	 * Flags - zero or more from inheritance,union,version or
	 * recursive (transitive closure) [we don't support them all -- ay
	 * 9/94 ]
	 */
	rte->inh = inh;

	/* RelOID */
	rte->relid = RelationGetRelationId(relation);

	rte->inFromCl = inFromCl;

	/*
	 * close the relation we're done with it for now.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	heap_close(relation);

	return rte;
}

/*
 * expandAll -
 *	  makes a list of attributes
 *	  assumes reldesc caching works
 */
List	   *
expandAll(ParseState *pstate, char *relname, char *refname, int *this_resno)
{
	Relation	rdesc;
	List	   *te_tail = NIL,
			   *te_head = NIL;
	Var		   *varnode;
	int			varattno,
				maxattrs;
	Oid			type_id;
	int			type_len;
	RangeTblEntry *rte;

	rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	if (rte == NULL)
		rte = addRangeTableEntry(pstate, relname, refname, FALSE, FALSE);

	rdesc = heap_open(rte->relid);

	if (rdesc == NULL)
	{
		elog(ERROR, "Unable to expand all -- heap_open failed on %s",
			 rte->refname);
		return NIL;
	}
	maxattrs = RelationGetNumberOfAttributes(rdesc);

	for (varattno = 0; varattno <= maxattrs - 1; varattno++)
	{
		char	   *attrname;
		char	   *resname = NULL;
		TargetEntry *te = makeNode(TargetEntry);

		attrname = pstrdup((rdesc->rd_att->attrs[varattno]->attname).data);
		varnode = (Var *) make_var(pstate, refname, attrname, &type_id);
		type_len = (int) typeLen(typeidType(type_id));

		handleTargetColname(pstate, &resname, refname, attrname);
		if (resname != NULL)
			attrname = resname;

		/*
		 * Even if the elements making up a set are complex, the set
		 * itself is not.
		 */

		te->resdom = makeResdom((AttrNumber) (*this_resno)++,
								type_id,
								(Size) type_len,
								attrname,
								(Index) 0,
								(Oid) 0,
								0);
		te->expr = (Node *) varnode;
		if (te_head == NIL)
			te_head = te_tail = lcons(te, NIL);
		else
			te_tail = lappend(te_tail, te);
	}

	heap_close(rdesc);
	return (te_head);
}

/* given relation and att name, return id of variable */
int
attnameAttNum(Relation rd, char *a)
{
	int			i;

	for (i = 0; i < rd->rd_rel->relnatts; i++)
		if (!namestrcmp(&(rd->rd_att->attrs[i]->attname), a))
			return (i + 1);

	for (i = 0; i < SPECIALS; i++)
		if (!strcmp(special_attr[i].field, a))
			return (special_attr[i].code);

	/* on failure */
	elog(ERROR, "Relation %s does not have attribute %s",
		 RelationGetRelationName(rd), a);
	return 0;  /* lint */
}

/* Given range variable, return whether attribute of this name
 * is a set.
 * NOTE the ASSUMPTION here that no system attributes are, or ever
 * will be, sets.
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
			return (false);		/* no sys attr is a set */
		}
	}
	return (get_attisset(rd->rd_id, name));
}

/*-------------
 * given an attribute number and a relation, return its relation name
 */
char	   *
attnumAttName(Relation rd, int attrno)
{
	char	   *name;
	int			i;

	if (attrno < 0)
	{
		for (i = 0; i < SPECIALS; i++)
		{
			if (special_attr[i].code == attrno)
			{
				name = special_attr[i].field;
				return (name);
			}
		}
		elog(ERROR, "Illegal attr no %d for relation %s",
			 attrno, RelationGetRelationName(rd));
	}
	else if (attrno >= 1 && attrno <= RelationGetNumberOfAttributes(rd))
	{
		name = (rd->rd_att->attrs[attrno - 1]->attname).data;
		return (name);
	}
	else
	{
		elog(ERROR, "Illegal attr no %d for relation %s",
			 attrno, RelationGetRelationName(rd));
	}

	/*
	 * Shouldn't get here, but we want lint to be happy...
	 */

	return (NULL);
}

int
attnumAttNelems(Relation rd, int attid)
{
	return (rd->rd_att->attrs[attid - 1]->attnelems);
}

/* given attribute id, return type of that attribute */
/* XXX Special case for pseudo-attributes is a hack */
Oid
attnumTypeId(Relation rd, int attid)
{

	if (attid < 0)
		return (typeTypeId(typenameType(attnum_type[-attid - 1])));

	/*
	 * -1 because varattno (where attid comes from) returns one more than
	 * index
	 */
	return (rd->rd_att->attrs[attid - 1]->atttypid);
}

/*
 * handleTargetColname -
 *	  use column names from insert
 */
void
handleTargetColname(ParseState *pstate, char **resname,
					char *refname, char *colname)
{
	if (pstate->p_is_insert)
	{
		if (pstate->p_insert_columns != NIL)
		{
			Ident	   *id = lfirst(pstate->p_insert_columns);

			*resname = id->name;
			pstate->p_insert_columns = lnext(pstate->p_insert_columns);
		}
		else
			elog(ERROR, "insert: more expressions than target columns");
	}
	if (pstate->p_is_insert || pstate->p_is_update)
		checkTargetTypes(pstate, *resname, refname, colname);
}

/*
 * checkTargetTypes -
 *	  checks value and target column types
 */
static void
checkTargetTypes(ParseState *pstate, char *target_colname,
				 char *refname, char *colname)
{
	Oid			attrtype_id,
				attrtype_target;
	int			resdomno_id,
				resdomno_target;
	Relation	rd;
	RangeTblEntry *rte;

	if (target_colname == NULL || colname == NULL)
		return;

	if (refname != NULL)
		rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	else
	{
		rte = colnameRangeTableEntry(pstate, colname);
		if (rte == (RangeTblEntry *) NULL)
			elog(ERROR, "attribute %s not found", colname);
		refname = rte->refname;
	}

/*
	if (pstate->p_is_insert && rte == pstate->p_target_rangetblentry)
		elog(ERROR, "%s not available in this context", colname);
*/
	rd = heap_open(rte->relid);

	resdomno_id = attnameAttNum(rd, colname);
	attrtype_id = attnumTypeId(rd, resdomno_id);

	resdomno_target = attnameAttNum(pstate->p_target_relation, target_colname);
	attrtype_target = attnumTypeId(pstate->p_target_relation, resdomno_target);

	if (attrtype_id != attrtype_target)
		elog(ERROR, "Type of %s does not match target column %s",
			 colname, target_colname);

	if ((attrtype_id == BPCHAROID || attrtype_id == VARCHAROID) &&
		rd->rd_att->attrs[resdomno_id - 1]->attlen !=
	pstate->p_target_relation->rd_att->attrs[resdomno_target - 1]->attlen)
		elog(ERROR, "Length of %s does not match length of target column %s",
			 colname, target_colname);

	heap_close(rd);
}
