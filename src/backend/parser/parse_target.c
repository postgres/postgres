/*-------------------------------------------------------------------------
 *
 * parse_target.c
 *	  handle target lists
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_target.c,v 1.34 1999/02/03 21:16:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"
#include "nodes/print.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static List *ExpandAllTables(ParseState *pstate);
char *FigureColname(Node *expr, Node *resval);

static Node *SizeTargetExpr(ParseState *pstate,
			   Node *expr,
			   Oid attrtype,
			   int32 attrtypmod);

static TargetEntry *
MakeTargetEntryCase(ParseState *pstate,
					ResTarget *res);

/* MakeTargetEntryIdent()
 * Transforms an Ident Node to a Target Entry
 * Created this function to allow the ORDER/GROUP BY clause to be able
 *	to construct a TargetEntry from an Ident.
 *
 * resjunk = TRUE will hide the target entry in the final result tuple.
 *		daveh@insightdist.com	  5/20/98
 *
 * Added more conversion logic to match up types from source to target.
 * - thomas 1998-06-02
 */
TargetEntry *
MakeTargetEntryIdent(ParseState *pstate,
					 Node *node,
					 char **resname,
					 char *refname,
					 char *colname,
					 int16 resjunk)
{
	Node	   *expr = NULL;
	Oid			attrtype_target;
	TargetEntry *tent = makeNode(TargetEntry);

	if (pstate->p_is_insert)
	{
		if (pstate->p_insert_columns != NIL)
		{
			Ident	   *id = lfirst(pstate->p_insert_columns);

			*resname = id->name;
			pstate->p_insert_columns = lnext(pstate->p_insert_columns);
		}
		else
			elog(ERROR, "INSERT has more expressions than target columns");
	}

	if (pstate->p_is_insert || pstate->p_is_update)
	{
		Oid			attrtype_id;
		int			resdomno_id,
					resdomno_target;
		RangeTblEntry *rte;
		char	   *target_colname;
		int32		attrtypmod,
					attrtypmod_target;

		target_colname = *resname;

		/*
		 * this looks strange to me, returning an empty TargetEntry bjm
		 * 1998/08/24
		 */
		if (target_colname == NULL || colname == NULL)
			return tent;

		if (refname != NULL)
			rte = refnameRangeTableEntry(pstate, refname);
		else
		{
			rte = colnameRangeTableEntry(pstate, colname);
			if (rte == (RangeTblEntry *) NULL)
				elog(ERROR, "Attribute %s not found", colname);
			refname = rte->refname;
		}

		resdomno_id = get_attnum(rte->relid, colname);
		attrtype_id = get_atttype(rte->relid, resdomno_id);
		attrtypmod = get_atttypmod(rte->relid, resdomno_id);

		resdomno_target = attnameAttNum(pstate->p_target_relation, target_colname);
		attrtype_target = attnumTypeId(pstate->p_target_relation, resdomno_target);
		attrtypmod_target = get_atttypmod(pstate->p_target_relation->rd_id, resdomno_target);

		if ((attrtype_id != attrtype_target)
			|| ((attrtypmod_target >= 0) && (attrtypmod_target != attrtypmod)))
		{
			if (can_coerce_type(1, &attrtype_id, &attrtype_target))
			{
				expr = coerce_type(pstate, node, attrtype_id, attrtype_target);
				expr = transformExpr(pstate, expr, EXPR_COLUMN_FIRST);
				tent = MakeTargetEntryExpr(pstate, *resname, expr, FALSE, FALSE);
				expr = tent->expr;
			}
			else
			{
				elog(ERROR, "Unable to convert %s to %s for column %s",
					 typeidTypeName(attrtype_id), typeidTypeName(attrtype_target),
					 target_colname);
			}
		}
	}

	/*
	 * here we want to look for column names only, not relation names
	 * (even though they can be stored in Ident nodes, too)
	 */
	if (expr == NULL)
	{
		char	   *name;
		int32		type_mod;

		name = ((*resname != NULL) ? *resname : colname);

		expr = transformExpr(pstate, node, EXPR_COLUMN_FIRST);

		attrtype_target = exprType(expr);
		if (nodeTag(expr) == T_Var)
			type_mod = ((Var *) expr)->vartypmod;
		else
			type_mod = -1;

		tent->resdom = makeResdom((AttrNumber) pstate->p_last_resno++,
								  (Oid) attrtype_target,
								  type_mod,
								  name,
								  (Index) 0,
								  (Oid) 0,
								  resjunk);
		tent->expr = expr;
	}

	return tent;
}	/* MakeTargetEntryIdent() */


/* MakeTargetEntryExpr()
 * Make a TargetEntry from an expression.
 * arrayRef is a list of transformed A_Indices.
 *
 * For type mismatches between expressions and targets, use the same
 *	techniques as for function and operator type coersion.
 * - thomas 1998-05-08
 *
 * Added resjunk flag and made extern so that it can be use by GROUP/
 * ORDER BY a function or expression not in the target_list
 * -  daveh@insightdist.com 1998-07-31
 */
TargetEntry *
MakeTargetEntryExpr(ParseState *pstate,
					char *colname,
					Node *expr,
					List *arrayRef,
					int16 resjunk)
{
	Oid			type_id,
				attrtype;
	int32		type_mod,
				attrtypmod;
	int			resdomno;
	Relation	rd;
	bool		attrisset;
	Resdom	   *resnode;

	if (expr == NULL)
		elog(ERROR, "Invalid use of NULL expression (internal error)");

	type_id = exprType(expr);
	if (nodeTag(expr) == T_Var)
		type_mod = ((Var *) expr)->vartypmod;
	else
		type_mod = -1;

	/* Process target columns that will be receiving results */
	if (pstate->p_is_insert || pstate->p_is_update)
	{

		/*
		 * insert or update query -- insert, update work only on one
		 * relation, so multiple occurence of same resdomno is bogus
		 */
		rd = pstate->p_target_relation;
		Assert(rd != NULL);
		resdomno = attnameAttNum(rd, colname);
		attrisset = attnameIsSet(rd, colname);
		attrtype = attnumTypeId(rd, resdomno);
		if ((arrayRef != NIL) && (lfirst(arrayRef) == NIL))
			attrtype = GetArrayElementType(attrtype);
		attrtypmod = rd->rd_att->attrs[resdomno - 1]->atttypmod;

		/*
		 * Check for InvalidOid since that seems to indicate a NULL
		 * constant...
		 */
		if (type_id != InvalidOid)
		{
			/* Mismatch on types? then try to coerce to target...  */
			if (attrtype != type_id)
			{
				Oid			typelem;

				if (arrayRef && !(((A_Indices *) lfirst(arrayRef))->lidx))
					typelem = typeidTypElem(attrtype);
				else
					typelem = attrtype;

				expr = CoerceTargetExpr(pstate, expr, type_id, typelem);

				if (!HeapTupleIsValid(expr))
					elog(ERROR, "Attribute '%s' is of type '%s'"
						 " but expression is of type '%s'"
						 "\n\tYou will need to rewrite or cast the expression",
						 colname,
						 typeidTypeName(attrtype),
						 typeidTypeName(type_id));
			}

			/*
			 * Apparently going to a fixed-length string? Then explicitly
			 * size for storage...
			 */
			if (attrtypmod > 0)
				expr = SizeTargetExpr(pstate, expr, attrtype, attrtypmod);
		}

		if (arrayRef != NIL)
		{
			Expr	   *target_expr;
			Attr	   *att = makeNode(Attr);
			List	   *ar = arrayRef;
			List	   *upperIndexpr = NIL;
			List	   *lowerIndexpr = NIL;

			att->relname = pstrdup(RelationGetRelationName(rd)->data);
			att->attrs = lcons(makeString(colname), NIL);
			target_expr = (Expr *) ParseNestedFuncOrColumn(pstate, att,
												   &pstate->p_last_resno,
													  EXPR_COLUMN_FIRST);
			while (ar != NIL)
			{
				A_Indices  *ind = lfirst(ar);

				if (lowerIndexpr || (!upperIndexpr && ind->lidx))
				{

					/*
					 * XXX assume all lowerIndexpr is non-null in this
					 * case
					 */
					lowerIndexpr = lappend(lowerIndexpr, ind->lidx);
				}
				upperIndexpr = lappend(upperIndexpr, ind->uidx);
				ar = lnext(ar);
			}

			expr = (Node *) make_array_set(target_expr,
										   upperIndexpr,
										   lowerIndexpr,
										   (Expr *) expr);
			attrtype = attnumTypeId(rd, resdomno);
			attrtypmod = get_atttypmod(RelationGetRelid(rd), resdomno);
		}
	}
	else
	{
		resdomno = pstate->p_last_resno++;
		attrtype = type_id;
		attrtypmod = type_mod;
	}

	resnode = makeResdom((AttrNumber) resdomno,
						 (Oid) attrtype,
						 attrtypmod,
						 colname,
						 (Index) 0,
						 (Oid) 0,
						 resjunk);

	return makeTargetEntry(resnode, expr);
}	/* MakeTargetEntryExpr() */

/*
 *	MakeTargetEntryCase()
 *	Make a TargetEntry from a case node.
 */
static TargetEntry *
MakeTargetEntryCase(ParseState *pstate,
					ResTarget *res)
{
	TargetEntry	*tent;
	CaseExpr	*expr;
	Resdom		*resnode;
	int			 resdomno;
	Oid			 type_id;
	int32		 type_mod;

	expr = (CaseExpr *)transformExpr(pstate, (Node *)res->val, EXPR_COLUMN_FIRST);

	type_id = expr->casetype;
	type_mod = -1;
	handleTargetColname(pstate, &res->name, NULL, NULL);
	if (res->name == NULL)
		res->name = FigureColname((Node *)expr, res->val);

	resdomno = pstate->p_last_resno++;
	resnode = makeResdom((AttrNumber) resdomno,
						 (Oid) type_id,
						 type_mod,
						 res->name,
						 (Index) 0,
						 (Oid) 0,
						 0);

	tent = makeNode(TargetEntry);
	tent->resdom = resnode;
	tent->expr = (Node *)expr;

	return tent;
}	/* MakeTargetEntryCase() */

/*
 *	MakeTargetEntryComplex()
 *	Make a TargetEntry from a complex node.
 */
static TargetEntry *
MakeTargetEntryComplex(ParseState *pstate,
					   ResTarget *res)
{
	Node	   *expr = transformExpr(pstate, (Node *) res->val, EXPR_COLUMN_FIRST);

	handleTargetColname(pstate, &res->name, NULL, NULL);
	/* note indirection has not been transformed */
	if (pstate->p_is_insert && res->indirection != NIL)
	{
		/* this is an array assignment */
		char	   *val;
		char	   *str,
				   *save_str;
		List	   *elt;
		int			i = 0,
					ndims;
		int			lindx[MAXDIM],
					uindx[MAXDIM];
		int			resdomno;
		Relation	rd;
		Value	   *constval;

		if (exprType(expr) != UNKNOWNOID || !IsA(expr, Const))
			elog(ERROR, "String constant expected (internal error)");

		val = (char *) textout((struct varlena *)
							   ((Const *) expr)->constvalue);
		str = save_str = (char *) palloc(strlen(val) + MAXDIM * 25 + 2);
		foreach(elt, res->indirection)
		{
			A_Indices  *aind = (A_Indices *) lfirst(elt);

			aind->uidx = transformExpr(pstate, aind->uidx, EXPR_COLUMN_FIRST);
			if (!IsA(aind->uidx, Const))
				elog(ERROR, "Array Index for Append should be a constant");

			uindx[i] = ((Const *) aind->uidx)->constvalue;
			if (aind->lidx != NULL)
			{
				aind->lidx = transformExpr(pstate, aind->lidx, EXPR_COLUMN_FIRST);
				if (!IsA(aind->lidx, Const))
					elog(ERROR, "Array Index for Append should be a constant");

				lindx[i] = ((Const *) aind->lidx)->constvalue;
			}
			else
				lindx[i] = 1;
			if (lindx[i] > uindx[i])
				elog(ERROR, "Lower index cannot be greater than upper index");

			sprintf(str, "[%d:%d]", lindx[i], uindx[i]);
			str += strlen(str);
			i++;
		}
		sprintf(str, "=%s", val);
		rd = pstate->p_target_relation;
		Assert(rd != NULL);
		resdomno = attnameAttNum(rd, res->name);
		ndims = attnumAttNelems(rd, resdomno);
		if (i != ndims)
			elog(ERROR, "Array dimensions do not match");

		constval = makeNode(Value);
		constval->type = T_String;
		constval->val.str = save_str;
		return MakeTargetEntryExpr(pstate, res->name,
								   (Node *) make_const(constval),
								   NULL, FALSE);
		pfree(save_str);
	}
	else
	{
		/* this is not an array assignment */
		char	  *colname = res->name;

		if (colname == NULL)
		{

			/*
			 * if you're wondering why this is here, look at the yacc
			 * grammar for why a name can be missing. -ay
			 */
			colname = FigureColname(expr, res->val);
		}
		if (res->indirection)
		{
			List	   *ilist = res->indirection;

			while (ilist != NIL)
			{
				A_Indices  *ind = lfirst(ilist);

				ind->lidx = transformExpr(pstate, ind->lidx, EXPR_COLUMN_FIRST);
				ind->uidx = transformExpr(pstate, ind->uidx, EXPR_COLUMN_FIRST);
				ilist = lnext(ilist);
			}
		}
		res->name = colname;
		return MakeTargetEntryExpr(pstate, res->name, expr,
								   res->indirection, FALSE);
	}
}

/*
 *	MakeTargetEntryAttr()
 *	Make a TargetEntry from a complex node.
 */
static TargetEntry *
MakeTargetEntryAttr(ParseState *pstate,
					ResTarget *res)
{
	Oid			type_id;
	int32		type_mod;
	Attr	   *att = (Attr *) res->val;
	Node	   *result;
	char	   *attrname;
	char	   *resname;
	Resdom	   *resnode;
	int			resdomno;
	List	   *attrs = att->attrs;
	TargetEntry *tent;

	attrname = strVal(lfirst(att->attrs));

	/*
	 * Target item is fully specified: ie. relation.attribute
	 */
	result = ParseNestedFuncOrColumn(pstate, att, &pstate->p_last_resno, EXPR_COLUMN_FIRST);
	handleTargetColname(pstate, &res->name, att->relname, attrname);
	if (att->indirection != NIL)
	{
		List	   *ilist = att->indirection;

		while (ilist != NIL)
		{
			A_Indices  *ind = lfirst(ilist);

			ind->lidx = transformExpr(pstate, ind->lidx, EXPR_COLUMN_FIRST);
			ind->uidx = transformExpr(pstate, ind->uidx, EXPR_COLUMN_FIRST);
			ilist = lnext(ilist);
		}
		result = (Node *) make_array_ref(result, att->indirection);
	}
	type_id = exprType(result);
	if (nodeTag(result) == T_Var)
		type_mod = ((Var *) result)->vartypmod;
	else
		type_mod = -1;
	/* move to last entry */
	while (lnext(attrs) != NIL)
		attrs = lnext(attrs);
	resname = (res->name) ? res->name : strVal(lfirst(attrs));
	if (pstate->p_is_insert || pstate->p_is_update)
	{
		Relation	rd;

		/*
		 * insert or update query -- insert, update work only on one
		 * relation, so multiple occurence of same resdomno is bogus
		 */
		rd = pstate->p_target_relation;
		Assert(rd != NULL);
		resdomno = attnameAttNum(rd, res->name);
	}
	else
		resdomno = pstate->p_last_resno++;
	resnode = makeResdom((AttrNumber) resdomno,
						 (Oid) type_id,
						 type_mod,
						 resname,
						 (Index) 0,
						 (Oid) 0,
						 0);
	tent = makeNode(TargetEntry);
	tent->resdom = resnode;
	tent->expr = result;
	return tent;
}


/* transformTargetList()
 * Turns a list of ResTarget's into a list of TargetEntry's.
 */
List *
transformTargetList(ParseState *pstate, List *targetlist)
{
	List	   *p_target = NIL;
	List	   *tail_p_target = NIL;

	while (targetlist != NIL)
	{
		ResTarget  *res = (ResTarget *) lfirst(targetlist);
		TargetEntry *tent = NULL;

		switch (nodeTag(res->val))
		{
			case T_Ident:
				{
					char	   *identname;

					identname = ((Ident *) res->val)->name;
					tent = MakeTargetEntryIdent(pstate, (Node *) res->val, &res->name, NULL, identname, FALSE);
					break;
				}
			case T_ParamNo:
			case T_FuncCall:
			case T_A_Const:
			case T_A_Expr:
				{
					tent = MakeTargetEntryComplex(pstate, res);
					break;
				}
			case T_CaseExpr:
				{
					tent = MakeTargetEntryCase(pstate, res);
					break;
				}
			case T_Attr:
				{
					bool		expand_star = false;
					char	   *attrname;
					Attr	   *att = (Attr *) res->val;

					/*
					 * Target item is a single '*', expand all tables (eg.
					 * SELECT * FROM emp)
					 */
					if (att->relname != NULL && !strcmp(att->relname, "*"))
					{
						if (tail_p_target == NIL)
							p_target = tail_p_target = ExpandAllTables(pstate);
						else
							lnext(tail_p_target) = ExpandAllTables(pstate);
						expand_star = true;
					}
					else
					{

						/*
						 * Target item is relation.*, expand the table
						 * (eg. SELECT emp.*, dname FROM emp, dept)
						 */
						attrname = strVal(lfirst(att->attrs));
						if (att->attrs != NIL && !strcmp(attrname, "*"))
						{

							/*
							 * tail_p_target is the target list we're
							 * building in the while loop. Make sure we
							 * fix it after appending more nodes.
							 */
							if (tail_p_target == NIL)
								p_target = tail_p_target = expandAll(pstate, att->relname,
									att->relname, &pstate->p_last_resno);
							else
								lnext(tail_p_target) = expandAll(pstate, att->relname, att->relname,
											  &pstate->p_last_resno);
							expand_star = true;
						}
					}
					if (expand_star)
					{
						while (lnext(tail_p_target) != NIL)
							/* make sure we point to the last target entry */
							tail_p_target = lnext(tail_p_target);

						/*
						 * skip rest of while loop
						 */
						targetlist = lnext(targetlist);
						continue;
					}
					else
					{
						tent = MakeTargetEntryAttr(pstate, res);
						break;
					}
				}
			default:
				/* internal error */
				elog(ERROR, "Unable to transform targetlist (internal error)");
				break;
		}

		if (p_target == NIL)
			p_target = tail_p_target = lcons(tent, NIL);
		else
		{
			lnext(tail_p_target) = lcons(tent, NIL);
			tail_p_target = lnext(tail_p_target);
		}
		targetlist = lnext(targetlist);
	}

	return p_target;
}	/* transformTargetList() */


Node *
CoerceTargetExpr(ParseState *pstate,
				 Node *expr,
				 Oid type_id,
				 Oid attrtype)
{
	if (can_coerce_type(1, &type_id, &attrtype))
	{
		expr = coerce_type(pstate, expr, type_id, attrtype);
	}

#ifndef DISABLE_STRING_HACKS

	/*
	 * string hacks to get transparent conversions w/o explicit
	 * conversions
	 */
	else if ((attrtype == BPCHAROID) || (attrtype == VARCHAROID))
	{
		Oid			text_id = TEXTOID;

		if (type_id == TEXTOID)
		{
		}
		else if (can_coerce_type(1, &type_id, &text_id))
			expr = coerce_type(pstate, expr, type_id, text_id);
		else
			expr = NULL;
	}
#endif

	else
		expr = NULL;

	return expr;
}	/* CoerceTargetExpr() */


/* SizeTargetExpr()
 * Apparently going to a fixed-length string?
 * Then explicitly size for storage...
 */
static Node *
SizeTargetExpr(ParseState *pstate,
			   Node *expr,
			   Oid attrtype,
			   int32 attrtypmod)
{
	int			i;
	HeapTuple	ftup;
	char	   *funcname;
	Oid			oid_array[8];

	FuncCall   *func;
	A_Const    *cons;

	funcname = typeidTypeName(attrtype);
	oid_array[0] = attrtype;
	oid_array[1] = INT4OID;
	for (i = 2; i < 8; i++)
		oid_array[i] = InvalidOid;

	/* attempt to find with arguments exactly as specified... */
	ftup = SearchSysCacheTuple(PRONAME,
							   PointerGetDatum(funcname),
							   Int32GetDatum(2),
							   PointerGetDatum(oid_array),
							   0);

	if (HeapTupleIsValid(ftup))
	{
		func = makeNode(FuncCall);
		func->funcname = funcname;

		cons = makeNode(A_Const);
		cons->val.type = T_Integer;
		cons->val.val.ival = attrtypmod;
		func->args = lappend(lcons(expr, NIL), cons);

		expr = transformExpr(pstate, (Node *) func, EXPR_COLUMN_FIRST);
	}

	return expr;
}	/* SizeTargetExpr() */


/*
 * makeTargetNames -
 *	  generate a list of column names if not supplied or
 *	  test supplied column names to make sure they are in target table
 *	  (used exclusively for inserts)
 */
List *
makeTargetNames(ParseState *pstate, List *cols)
{
	List	   *tl = NULL;

	/* Generate ResTarget if not supplied */

	if (cols == NIL)
	{
		int			numcol;
		int			i;
		Form_pg_attribute *attr = pstate->p_target_relation->rd_att->attrs;

		numcol = pstate->p_target_relation->rd_rel->relnatts;
		for (i = 0; i < numcol; i++)
		{
			Ident	   *id = makeNode(Ident);

			id->name = palloc(NAMEDATALEN);
			StrNCpy(id->name, attr[i]->attname.data, NAMEDATALEN);
			id->indirection = NIL;
			id->isRel = false;
			if (tl == NIL)
				cols = tl = lcons(id, NIL);
			else
			{
				lnext(tl) = lcons(id, NIL);
				tl = lnext(tl);
			}
		}
	}
	else
	{
		foreach(tl, cols)
		{
			List	   *nxt;
			char	   *name = ((Ident *) lfirst(tl))->name;

			/* elog on failure */
			attnameAttNum(pstate->p_target_relation, name);
			foreach(nxt, lnext(tl))
				if (!strcmp(name, ((Ident *) lfirst(nxt))->name))
				elog(ERROR, "Attribute '%s' should be specified only once", name);
		}
	}

	return cols;
}

/*
 * ExpandAllTables -
 *	  turns '*' (in the target list) into a list of attributes
 *	   (of all relations in the range table)
 */
static List *
ExpandAllTables(ParseState *pstate)
{
	List	   *target = NIL;
	List	   *legit_rtable = NIL;
	List	   *rt,
			   *rtable;

	rtable = pstate->p_rtable;
	if (pstate->p_is_rule)
	{

		/*
		 * skip first two entries, "*new*" and "*current*"
		 */
		rtable = lnext(lnext(pstate->p_rtable));
	}

	/* this should not happen */
	if (rtable == NULL)
		elog(ERROR, "Cannot expand tables; null p_rtable (internal error)");

	/*
	 * go through the range table and make a list of range table entries
	 * which we will expand.
	 */
	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst(rt);

		/*
		 * we only expand those specify in the from clause. (This will
		 * also prevent us from using the wrong table in inserts: eg.
		 * tenk2 in "insert into tenk2 select * from tenk1;")
		 */
		if (!rte->inFromCl)
			continue;
		legit_rtable = lappend(legit_rtable, rte);
	}

	foreach(rt, legit_rtable)
	{
		RangeTblEntry *rte = lfirst(rt);
		List	   *temp = target;

		if (temp == NIL)
			target = expandAll(pstate, rte->relname, rte->refname,
							   &pstate->p_last_resno);
		else
		{
			while (temp != NIL && lnext(temp) != NIL)
				temp = lnext(temp);
			lnext(temp) = expandAll(pstate, rte->relname, rte->refname,
									&pstate->p_last_resno);
		}
	}
	return target;
}

/*
 * FigureColname -
 *	  if the name of the resulting column is not specified in the target
 *	  list, we have to guess.
 *
 */
char *
FigureColname(Node *expr, Node *resval)
{
	switch (nodeTag(expr))
	{
		case T_Aggref:
			return (char *) ((Aggref *) expr)->aggname;
		case T_Expr:
			if (((Expr *) expr)->opType == FUNC_EXPR)
			{
				if (nodeTag(resval) == T_FuncCall)
					return ((FuncCall *) resval)->funcname;
			}
			break;
		case T_CaseExpr:
			{
				char *name;
				name = FigureColname(((CaseExpr *) expr)->defresult, resval);
				if (!strcmp(name, "?column?"))
					name = "case";
				return name;
			}
			break;
		default:
			break;
	}

	return "?column?";
}
