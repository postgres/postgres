/*-------------------------------------------------------------------------
 *
 * parse_target.c
 *	  handle target lists
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_target.c,v 1.10 1998/02/13 19:45:44 momjian Exp $
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
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

static List *expandAllTables(ParseState *pstate);
static char *figureColname(Node *expr, Node *resval);
static TargetEntry *make_targetlist_expr(ParseState *pstate,
					 char *colname,
					 Node *expr,
					 List *arrayRef);

/*
 * transformTargetList -
 *	  turns a list of ResTarget's into a list of TargetEntry's
 */
List *
transformTargetList(ParseState *pstate, List *targetlist)
{
	List	   *p_target = NIL;
	List	   *tail_p_target = NIL;

	while (targetlist != NIL)
	{
		ResTarget  *res = (ResTarget *) lfirst(targetlist);
		TargetEntry *tent = makeNode(TargetEntry);

		switch (nodeTag(res->val))
		{
			case T_Ident:
				{
					Node	   *expr;
					Oid			type_id;
					int16		type_mod;
					char	   *identname;
					char	   *resname;

					identname = ((Ident *) res->val)->name;
					handleTargetColname(pstate, &res->name, NULL, identname);

					/*
					 * here we want to look for column names only, not relation
					 * names (even though they can be stored in Ident nodes, too)
					 */
					expr = transformIdent(pstate, (Node *) res->val, EXPR_COLUMN_FIRST);
					type_id = exprType(expr);
					if (nodeTag(expr) == T_Var)
						type_mod = ((Var *)expr)->vartypmod;
					else
						type_mod = -1;
					resname = (res->name) ? res->name : identname;
					tent->resdom = makeResdom((AttrNumber) pstate->p_last_resno++,
											  (Oid) type_id,
											  type_mod,
											  resname,
											  (Index) 0,
											  (Oid) 0,
											  0);

					tent->expr = expr;
					break;
				}
			case T_ParamNo:
			case T_FuncCall:
			case T_A_Const:
			case T_A_Expr:
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

						if (exprType(expr) != UNKNOWNOID ||
							!IsA(expr, Const))
							elog(ERROR, "yyparse: string constant expected");

						val = (char *) textout((struct varlena *)
										   ((Const *) expr)->constvalue);
						str = save_str = (char *) palloc(strlen(val) + MAXDIM * 25 + 2);
						foreach(elt, res->indirection)
						{
							A_Indices  *aind = (A_Indices *) lfirst(elt);

							aind->uidx = transformExpr(pstate, aind->uidx, EXPR_COLUMN_FIRST);
							if (!IsA(aind->uidx, Const))
								elog(ERROR,
									 "Array Index for Append should be a constant");
							uindx[i] = ((Const *) aind->uidx)->constvalue;
							if (aind->lidx != NULL)
							{
								aind->lidx = transformExpr(pstate, aind->lidx, EXPR_COLUMN_FIRST);
								if (!IsA(aind->lidx, Const))
									elog(ERROR,
										 "Array Index for Append should be a constant");
								lindx[i] = ((Const *) aind->lidx)->constvalue;
							}
							else
							{
								lindx[i] = 1;
							}
							if (lindx[i] > uindx[i])
								elog(ERROR, "yyparse: lower index cannot be greater than upper index");
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
							elog(ERROR, "yyparse: array dimensions do not match");
						constval = makeNode(Value);
						constval->type = T_String;
						constval->val.str = save_str;
						tent = make_targetlist_expr(pstate, res->name,
										   (Node *) make_const(constval),
													NULL);
						pfree(save_str);
					}
					else
					{
						char	   *colname = res->name;

						/* this is not an array assignment */
						if (colname == NULL)
						{

							/*
							 * if you're wondering why this is here, look
							 * at the yacc grammar for why a name can be
							 * missing. -ay
							 */
							colname = figureColname(expr, res->val);
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
						tent = make_targetlist_expr(pstate, res->name, expr,
													res->indirection);
					}
					break;
				}
			case T_Attr:
				{
					Oid			type_id;
					int16		type_mod;
					Attr	   *att = (Attr *) res->val;
					Node	   *result;
					char	   *attrname;
					char	   *resname;
					Resdom	   *resnode;
					List	   *attrs = att->attrs;

					/*
					 * Target item is a single '*', expand all tables (eg.
					 * SELECT * FROM emp)
					 */
					if (att->relname != NULL && !strcmp(att->relname, "*"))
					{
						if (tail_p_target == NIL)
							p_target = tail_p_target = expandAllTables(pstate);
						else
							lnext(tail_p_target) = expandAllTables(pstate);

						while (lnext(tail_p_target) != NIL)
							/* make sure we point to the last target entry */
							tail_p_target = lnext(tail_p_target);

						/*
						 * skip rest of while loop
						 */
						targetlist = lnext(targetlist);
						continue;
					}

					/*
					 * Target item is relation.*, expand the table (eg.
					 * SELECT emp.*, dname FROM emp, dept)
					 */
					attrname = strVal(lfirst(att->attrs));
					if (att->attrs != NIL && !strcmp(attrname, "*"))
					{

						/*
						 * tail_p_target is the target list we're building
						 * in the while loop. Make sure we fix it after
						 * appending more nodes.
						 */
						if (tail_p_target == NIL)
							p_target = tail_p_target = expandAll(pstate, att->relname,
									att->relname, &pstate->p_last_resno);
						else
							lnext(tail_p_target) =
								expandAll(pstate, att->relname, att->relname,
										  &pstate->p_last_resno);
						while (lnext(tail_p_target) != NIL)
							/* make sure we point to the last target entry */
							tail_p_target = lnext(tail_p_target);

						/*
						 * skip the rest of the while loop
						 */
						targetlist = lnext(targetlist);
						continue;
					}


					/*
					 * Target item is fully specified: ie. relation.attribute
					 */
					result = ParseNestedFuncOrColumn(pstate, att, &pstate->p_last_resno,EXPR_COLUMN_FIRST);
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
						type_mod = ((Var *)result)->vartypmod;
					else
						type_mod = -1;
					/* move to last entry */
					while (lnext(attrs) != NIL)
						attrs = lnext(attrs);
					resname = (res->name) ? res->name : strVal(lfirst(attrs));
					resnode = makeResdom((AttrNumber) pstate->p_last_resno++,
										 (Oid) type_id,
										 type_mod,
										 resname,
										 (Index) 0,
										 (Oid) 0,
										 0);
					tent->resdom = resnode;
					tent->expr = result;
					break;
				}
			default:
				/* internal error */
				elog(ERROR,
					 "internal error: do not know how to transform targetlist");
				break;
		}

		if (p_target == NIL)
		{
			p_target = tail_p_target = lcons(tent, NIL);
		}
		else
		{
			lnext(tail_p_target) = lcons(tent, NIL);
			tail_p_target = lnext(tail_p_target);
		}
		targetlist = lnext(targetlist);
	}

	return p_target;
}


/*
 * make_targetlist_expr -
 *	  make a TargetEntry from an expression
 *
 * arrayRef is a list of transformed A_Indices
 */
static TargetEntry *
make_targetlist_expr(ParseState *pstate,
					 char *colname,
					 Node *expr,
					 List *arrayRef)
{
	Oid			type_id,
				attrtype;
	int16		type_mod,
				attrtypmod;
	int			resdomno;
	Relation	rd;
	bool		attrisset;
	TargetEntry *tent;
	Resdom	   *resnode;

	if (expr == NULL)
		elog(ERROR, "make_targetlist_expr: invalid use of NULL expression");

	type_id = exprType(expr);
	if (nodeTag(expr) == T_Var)
		type_mod = ((Var *)expr)->vartypmod;
	else
		type_mod = -1;

	/* Processes target columns that will be receiving results */
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
#if 0
		if (Input_is_string && Typecast_ok)
		{
			Datum		val;

			if (type_id == typeTypeId(type("unknown")))
			{
				val = (Datum) textout((struct varlena *)
									  ((Const) lnext(expr))->constvalue);
			}
			else
			{
				val = ((Const) lnext(expr))->constvalue;
			}
			if (attrisset)
			{
				lnext(expr) = makeConst(attrtype,
										attrlen,
										val,
										false,
										true,
										true,	/* is set */
										false);
			}
			else
			{
				lnext(expr) =
					makeConst(attrtype,
							  attrlen,
							  (Datum) fmgr(typeidInfunc(attrtype),
										 val, typeidTypElem(attrtype), -1),
							  false,
							  true /* Maybe correct-- 80% chance */ ,
							  false,	/* is not a set */
							  false);
			}
		}
		else if ((Typecast_ok) && (attrtype != type_id))
		{
			lnext(expr) =
				parser_typecast2(expr, typeidType(attrtype));
		}
		else if (attrtype != type_id)
		{
			if ((attrtype == INT2OID) && (type_id == INT4OID))
				lfirst(expr) = lispInteger(INT2OID);	/* handle CASHOID too */
			else if ((attrtype == FLOAT4OID) && (type_id == FLOAT8OID))
				lfirst(expr) = lispInteger(FLOAT4OID);
			else
				elog(ERROR, "unequal type in tlist : %s \n", colname);
		}

		Input_is_string = false;
		Input_is_integer = false;
		Typecast_ok = true;
#endif

		if (attrtype != type_id)
		{
			if (IsA(expr, Const))
			{
				/* try to cast the constant */
				if (arrayRef && !(((A_Indices *) lfirst(arrayRef))->lidx))
				{
					/* updating a single item */
					Oid			typelem = typeidTypElem(attrtype);

					expr = (Node *) parser_typecast2(expr,
													 type_id,
													 typeidType(typelem),
													 attrtypmod);
				}
				else
					expr = (Node *) parser_typecast2(expr,
													 type_id,
													 typeidType(attrtype),
													 attrtypmod);
			}
			else
			{
				/* currently, we can't handle casting of expressions */
				elog(ERROR, "parser: attribute '%s' is of type '%s' but expression is of type '%s'",
					 colname,
					 typeidTypeName(attrtype),
					 typeidTypeName(type_id));
			}
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
			attrtypmod = get_atttypmod(rd->rd_id, resdomno);
		}
	}
	else
	{
		resdomno = pstate->p_last_resno++;
		attrtype = type_id;
		attrtypmod = type_mod;
	}
	tent = makeNode(TargetEntry);

	resnode = makeResdom((AttrNumber) resdomno,
						 (Oid) attrtype,
						 attrtypmod,
						 colname,
						 (Index) 0,
						 (Oid) 0,
						 0);

	tent->resdom = resnode;
	tent->expr = expr;

	return tent;
}

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
		AttributeTupleForm *attr = pstate->p_target_relation->rd_att->attrs;

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
 * expandAllTables -
 *	  turns '*' (in the target list) into a list of attributes
 *	   (of all relations in the range table)
 */
static List *
expandAllTables(ParseState *pstate)
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
		elog(ERROR, "cannot expand: null p_rtable");

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
 * figureColname -
 *	  if the name of the resulting column is not specified in the target
 *	  list, we have to guess.
 *
 */
static char *
figureColname(Node *expr, Node *resval)
{
	switch (nodeTag(expr))
	{
		case T_Aggreg:
			return (char *)	((Aggreg *) expr)->aggname;
		case T_Expr:
			if (((Expr *) expr)->opType == FUNC_EXPR)
			{
				if (nodeTag(resval) == T_FuncCall)
					return ((FuncCall *) resval)->funcname;
			}
			break;
		default:
			break;
	}

	return "?column?";
}
