/*-------------------------------------------------------------------------
 *
 * parse_target.c
 *	  handle target lists
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_target.c,v 1.59 2000/05/30 00:49:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"


static List *ExpandAllTables(ParseState *pstate);
static char *FigureColname(Node *expr, Node *resval);


/*
 * transformTargetEntry()
 *	Transform any ordinary "expression-type" node into a targetlist entry.
 *	This is exported so that parse_clause.c can generate targetlist entries
 *	for ORDER/GROUP BY items that are not already in the targetlist.
 *
 * node		the (untransformed) parse tree for the value expression.
 * expr		the transformed expression, or NULL if caller didn't do it yet.
 * colname	the column name to be assigned, or NULL if none yet set.
 * resjunk	true if the target should be marked resjunk, ie, it is not
 *			wanted in the final projected tuple.
 */
TargetEntry *
transformTargetEntry(ParseState *pstate,
					 Node *node,
					 Node *expr,
					 char *colname,
					 bool resjunk)
{
	Oid			type_id;
	int32		type_mod;
	Resdom	   *resnode;

	/* Transform the node if caller didn't do it already */
	if (expr == NULL)
		expr = transformExpr(pstate, node, EXPR_COLUMN_FIRST);

	type_id = exprType(expr);
	type_mod = exprTypmod(expr);

	if (colname == NULL)
	{

		/*
		 * Generate a suitable column name for a column without any
		 * explicit 'AS ColumnName' clause.
		 */
		colname = FigureColname(expr, node);
	}

	resnode = makeResdom((AttrNumber) pstate->p_last_resno++,
						 type_id,
						 type_mod,
						 colname,
						 (Index) 0,
						 (Oid) InvalidOid,
						 resjunk);

	return makeTargetEntry(resnode, expr);
}


/*
 * transformTargetList()
 * Turns a list of ResTarget's into a list of TargetEntry's.
 *
 * At this point, we don't care whether we are doing SELECT, INSERT,
 * or UPDATE; we just transform the given expressions.
 */
List *
transformTargetList(ParseState *pstate, List *targetlist)
{
	List	   *p_target = NIL;

	while (targetlist != NIL)
	{
		ResTarget  *res = (ResTarget *) lfirst(targetlist);

		if (IsA(res->val, Attr))
		{
			Attr	   *att = (Attr *) res->val;

			if (att->relname != NULL && strcmp(att->relname, "*") == 0)
			{

				/*
				 * Target item is a single '*', expand all tables (eg.
				 * SELECT * FROM emp)
				 */
				if (pstate->p_shape != NULL)
				{
					List	   *s,
							   *a;
					int			i;

					Assert(length(pstate->p_shape) == length(pstate->p_alias));

					s = pstate->p_shape;
					a = pstate->p_alias;
					for (i = 0; i < length(pstate->p_shape); i++)
					{
						TargetEntry *te;
						char	   *colname;
						Attr	   *shape = lfirst(s);
						Attr	   *alias = lfirst(a);

						Assert(IsA(shape, Attr) &&IsA(alias, Attr));

						colname = strVal(lfirst(alias->attrs));
						te = transformTargetEntry(pstate, (Node *) shape,
												  NULL, colname, false);
						p_target = lappend(p_target, te);
						s = lnext(s);
						a = lnext(a);
					}
				}
				else
					p_target = nconc(p_target,
									 ExpandAllTables(pstate));
			}
			else if (att->attrs != NIL &&
					 strcmp(strVal(lfirst(att->attrs)), "*") == 0)
			{

				/*
				 * Target item is relation.*, expand that table (eg.
				 * SELECT emp.*, dname FROM emp, dept)
				 */
				p_target = nconc(p_target,
								 expandAll(pstate, att->relname,
										   makeAttr(att->relname, NULL),
										   &pstate->p_last_resno));
			}
			else
			{
				/* Plain Attr node, treat it as an expression */
				p_target = lappend(p_target,
								   transformTargetEntry(pstate,
														res->val,
														NULL,
														res->name,
														false));
			}
		}
		else
		{
			/* Everything else but Attr */
			p_target = lappend(p_target,
							   transformTargetEntry(pstate,
													res->val,
													NULL,
													res->name,
													false));
		}

		targetlist = lnext(targetlist);
	}

	return p_target;
}


/*
 * updateTargetListEntry()
 *	This is used in INSERT and UPDATE statements only.	It prepares a
 *	TargetEntry for assignment to a column of the target table.
 *	This includes coercing the given value to the target column's type
 *	(if necessary), and dealing with any subscripts attached to the target
 *	column itself.
 *
 * pstate		parse state
 * tle			target list entry to be modified
 * colname		target column name (ie, name of attribute to be assigned to)
 * attrno		target attribute number
 * indirection	subscripts for target column, if any
 */
void
updateTargetListEntry(ParseState *pstate,
					  TargetEntry *tle,
					  char *colname,
					  int attrno,
					  List *indirection)
{
	Oid			type_id = exprType(tle->expr);	/* type of value provided */
	Oid			attrtype;		/* type of target column */
	int32		attrtypmod;
	Resdom	   *resnode = tle->resdom;
	Relation	rd = pstate->p_target_relation;

	Assert(rd != NULL);
	if (attrno <= 0)
		elog(ERROR, "Cannot assign to system attribute '%s'", colname);
	attrtype = attnumTypeId(rd, attrno);
	attrtypmod = rd->rd_att->attrs[attrno - 1]->atttypmod;

	/*
	 * If there are subscripts on the target column, prepare an array
	 * assignment expression.  This will generate an array value that the
	 * source value has been inserted into, which can then be placed in
	 * the new tuple constructed by INSERT or UPDATE. Note that
	 * transformArraySubscripts takes care of type coercion.
	 */
	if (indirection)
	{
#ifndef DISABLE_JOIN_SYNTAX
		Attr	   *att = makeAttr(pstrdup(RelationGetRelationName(rd)), colname);

#else
		Attr	   *att = makeNode(Attr);

#endif
		Node	   *arrayBase;
		ArrayRef   *aref;

#ifdef DISABLE_JOIN_SYNTAX
		att->relname = pstrdup(RelationGetRelationName(rd));
		att->attrs = lcons(makeString(colname), NIL);
#endif
		arrayBase = ParseNestedFuncOrColumn(pstate, att,
											&pstate->p_last_resno,
											EXPR_COLUMN_FIRST);
		aref = transformArraySubscripts(pstate, arrayBase,
										indirection,
										pstate->p_is_insert,
										tle->expr);
		if (pstate->p_is_insert)
		{

			/*
			 * The command is INSERT INTO table (arraycol[subscripts]) ...
			 * so there is not really a source array value to work with.
			 * Let the executor do something reasonable, if it can. Notice
			 * that we forced transformArraySubscripts to treat the
			 * subscripting op as an array-slice op above, so the source
			 * data will have been coerced to array type.
			 */
			aref->refexpr = NULL;		/* signal there is no source array */
		}
		tle->expr = (Node *) aref;
	}
	else
	{

		/*
		 * For normal non-subscripted target column, do type checking and
		 * coercion.  But accept InvalidOid, which indicates the source is
		 * a NULL constant.
		 */
		if (type_id != InvalidOid)
		{
			if (type_id != attrtype)
			{
				tle->expr = CoerceTargetExpr(pstate, tle->expr, type_id,
											 attrtype, attrtypmod);
				if (tle->expr == NULL)
					elog(ERROR, "Attribute '%s' is of type '%s'"
						 " but expression is of type '%s'"
					"\n\tYou will need to rewrite or cast the expression",
						 colname,
						 typeidTypeName(attrtype),
						 typeidTypeName(type_id));
			}

			/*
			 * If the target is a fixed-length type, it may need a length
			 * coercion as well as a type coercion.
			 */
			tle->expr = coerce_type_typmod(pstate, tle->expr,
										   attrtype, attrtypmod);
		}
	}

	/*
	 * The result of the target expression should now match the
	 * destination column's type.  Also, reset the resname and resno to
	 * identify the destination column --- rewriter and planner depend on
	 * that!
	 */
	resnode->restype = attrtype;
	resnode->restypmod = attrtypmod;
	resnode->resname = colname;
	resnode->resno = (AttrNumber) attrno;
}


Node *
CoerceTargetExpr(ParseState *pstate,
				 Node *expr,
				 Oid type_id,
				 Oid attrtype,
				 int32 attrtypmod)
{
	if (can_coerce_type(1, &type_id, &attrtype))
		expr = coerce_type(pstate, expr, type_id, attrtype, attrtypmod);

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
			expr = coerce_type(pstate, expr, type_id, text_id, attrtypmod);
		else
			expr = NULL;
	}
#endif

	else
		expr = NULL;

	return expr;
}


/*
 * checkInsertTargets -
 *	  generate a list of column names if not supplied or
 *	  test supplied column names to make sure they are in target table.
 *	  Also return an integer list of the columns' attribute numbers.
 *	  (used exclusively for inserts)
 */
List *
checkInsertTargets(ParseState *pstate, List *cols, List **attrnos)
{
	*attrnos = NIL;

	if (cols == NIL)
	{

		/*
		 * Generate default column list for INSERT.
		 */
		Form_pg_attribute *attr = pstate->p_target_relation->rd_att->attrs;
		int			numcol = pstate->p_target_relation->rd_rel->relnatts;
		int			i;

		for (i = 0; i < numcol; i++)
		{
			Ident	   *id = makeNode(Ident);

#ifdef	_DROP_COLUMN_HACK__
			if (COLUMN_IS_DROPPED(attr[i]))
				continue;
#endif	 /* _DROP_COLUMN_HACK__ */
			id->name = palloc(NAMEDATALEN);
			StrNCpy(id->name, NameStr(attr[i]->attname), NAMEDATALEN);
			id->indirection = NIL;
			id->isRel = false;
			cols = lappend(cols, id);
			*attrnos = lappendi(*attrnos, i + 1);
		}
	}
	else
	{

		/*
		 * Do initial validation of user-supplied INSERT column list.
		 */
		List	   *tl;

		foreach(tl, cols)
		{
			char	   *name = ((Ident *) lfirst(tl))->name;
			int			attrno;

			/* Lookup column name, elog on failure */
			attrno = attnameAttNum(pstate->p_target_relation, name);
			/* Check for duplicates */
			if (intMember(attrno, *attrnos))
				elog(ERROR, "Attribute '%s' specified more than once", name);
			*attrnos = lappendi(*attrnos, attrno);
		}
	}

	return cols;
}

/* ExpandAllTables()
 * Turns '*' (in the target list) into a list of attributes
 * (of all relations in the range table)
 */
static List *
ExpandAllTables(ParseState *pstate)
{
	List	   *target = NIL;
	List	   *rt,
			   *rtable;

	rtable = pstate->p_rtable;
	if (pstate->p_is_rule)
	{

		/*
		 * skip first two entries, "*new*" and "*current*"
		 */
		rtable = lnext(lnext(rtable));
	}

	/* SELECT *; */
	if (rtable == NIL)
		elog(ERROR, "Wildcard with no tables specified not allowed");

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst(rt);

		/*
		 * we only expand those listed in the from clause. (This will also
		 * prevent us from using the wrong table in inserts: eg. tenk2 in
		 * "insert into tenk2 select * from tenk1;")
		 */
		if (!rte->inFromCl)
			continue;

		target = nconc(target,
					   expandAll(pstate, rte->eref->relname, rte->eref,
								 &pstate->p_last_resno));
	}
	return target;
}

/*
 * FigureColname -
 *	  if the name of the resulting column is not specified in the target
 *	  list, we have to guess a suitable name.  The SQL spec provides some
 *	  guidance, but not much...
 *
 */
static char *
FigureColname(Node *expr, Node *resval)
{
	/* Some of these are easiest to do with the untransformed node */
	switch (nodeTag(resval))
	{
			case T_Ident:
			return ((Ident *) resval)->name;
		case T_Attr:
			{
				List	   *attrs = ((Attr *) resval)->attrs;

				if (attrs)
				{
					while (lnext(attrs) != NIL)
						attrs = lnext(attrs);
					return strVal(lfirst(attrs));
				}
			}
			break;
		default:
			break;
	}
	/* Otherwise, work with the transformed node */
	switch (nodeTag(expr))
	{
		case T_Expr:
			if (((Expr *) expr)->opType == FUNC_EXPR && IsA(resval, FuncCall))
				return ((FuncCall *) resval)->funcname;
			break;
		case T_Aggref:
			return ((Aggref *) expr)->aggname;
		case T_CaseExpr:
			{
				char	   *name;

				name = FigureColname(((CaseExpr *) expr)->defresult, resval);
				if (strcmp(name, "?column?") == 0)
					name = "case";
				return name;
			}
			break;
		default:
			break;
	}

	return "?column?";
}
