/*-------------------------------------------------------------------------
 *
 * parse_target.c
 *	  handle target lists
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_target.c,v 1.103 2003/06/25 04:19:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"


static void markTargetListOrigin(ParseState *pstate, Resdom *res, Var *var);
static List *ExpandAllTables(ParseState *pstate);
static char *FigureColname(Node *node);
static int	FigureColnameInternal(Node *node, char **name);


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
		expr = transformExpr(pstate, node);

	if (IsA(expr, RangeVar))
		elog(ERROR, "You can't use relation names alone in the target list, try relation.*.");

	type_id = exprType(expr);
	type_mod = exprTypmod(expr);

	if (colname == NULL)
	{
		/*
		 * Generate a suitable column name for a column without any
		 * explicit 'AS ColumnName' clause.
		 */
		colname = FigureColname(node);
	}

	resnode = makeResdom((AttrNumber) pstate->p_next_resno++,
						 type_id,
						 type_mod,
						 colname,
						 resjunk);

	return makeTargetEntry(resnode, (Expr *) expr);
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

		if (IsA(res->val, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) res->val;
			List	   *fields = cref->fields;
			int			numnames = length(fields);

			if (numnames == 1 && strcmp(strVal(lfirst(fields)), "*") == 0)
			{
				/*
				 * Target item is a single '*', expand all tables (eg.
				 * SELECT * FROM emp)
				 */
				p_target = nconc(p_target,
								 ExpandAllTables(pstate));
			}
			else if (strcmp(strVal(nth(numnames - 1, fields)), "*") == 0)
			{
				/*
				 * Target item is relation.*, expand that table (eg.
				 * SELECT emp.*, dname FROM emp, dept)
				 */
				char	   *schemaname;
				char	   *relname;
				RangeTblEntry *rte;
				int			sublevels_up;

				switch (numnames)
				{
					case 2:
						schemaname = NULL;
						relname = strVal(lfirst(fields));
						break;
					case 3:
						schemaname = strVal(lfirst(fields));
						relname = strVal(lsecond(fields));
						break;
					case 4:
						{
							char	   *name1 = strVal(lfirst(fields));

							/*
							 * We check the catalog name and then ignore
							 * it.
							 */
							if (strcmp(name1, DatabaseName) != 0)
								elog(ERROR, "Cross-database references are not implemented");
							schemaname = strVal(lsecond(fields));
							relname = strVal(lthird(fields));
							break;
						}
					default:
						elog(ERROR, "Invalid qualified name syntax (too many names)");
						schemaname = NULL;		/* keep compiler quiet */
						relname = NULL;
						break;
				}

				rte = refnameRangeTblEntry(pstate, schemaname, relname,
										   &sublevels_up);
				if (rte == NULL)
					rte = addImplicitRTE(pstate, makeRangeVar(schemaname,
															  relname));

				p_target = nconc(p_target,
								 expandRelAttrs(pstate, rte));
			}
			else
			{
				/* Plain ColumnRef node, treat it as an expression */
				p_target = lappend(p_target,
								   transformTargetEntry(pstate,
														res->val,
														NULL,
														res->name,
														false));
			}
		}
		else if (IsA(res->val, SetToDefault))
		{
			/*
			 * If this is a DEFAULT element, we make a standard entry using
			 * the default for the target expression.  rewriteTargetList will
			 * substitute the columns default for this expression.
			 */
			p_target = lappend(p_target,
							   makeTargetEntry(makeResdom((AttrNumber) pstate->p_next_resno++,
														  UNKNOWNOID,
														  -1,
														  res->name,
														  false),
											   (Expr *) res->val));
		}
		else
		{
			/* Everything else but ColumnRef and SetToDefault */
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
 * markTargetListOrigins()
 *		Mark targetlist columns that are simple Vars with the source
 *		table's OID and column number.
 *
 * Currently, this is done only for SELECT targetlists, since we only
 * need the info if we are going to send it to the frontend.
 */
void
markTargetListOrigins(ParseState *pstate, List *targetlist)
{
	List	   *l;

	foreach(l, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		markTargetListOrigin(pstate, tle->resdom, (Var *) tle->expr);
	}
}

/*
 * markTargetListOrigin()
 *		If 'var' is a Var of a plain relation, mark 'res' with its origin
 *
 * This is split out so it can recurse for join references.  Note that we
 * do not drill down into views, but report the view as the column owner.
 */
static void
markTargetListOrigin(ParseState *pstate, Resdom *res, Var *var)
{
	Index		levelsup;
	RangeTblEntry *rte;
	AttrNumber	attnum;

	if (var == NULL || !IsA(var, Var))
		return;
	levelsup = var->varlevelsup;
	while (levelsup-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	Assert(var->varno > 0 &&
		   (int) var->varno <= length(pstate->p_rtable));
	rte = rt_fetch(var->varno, pstate->p_rtable);
	attnum = var->varattno;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* It's a table or view, report it */
			res->resorigtbl = rte->relid;
			res->resorigcol = attnum;
			break;
		case RTE_SUBQUERY:
			{
				/* Subselect-in-FROM: copy up from the subselect */
				List	   *subtl;

				foreach(subtl, rte->subquery->targetList)
				{
					TargetEntry *subte = (TargetEntry *) lfirst(subtl);

					if (subte->resdom->resjunk ||
						subte->resdom->resno != attnum)
						continue;
					res->resorigtbl = subte->resdom->resorigtbl;
					res->resorigcol = subte->resdom->resorigcol;
					break;
				}
				/* falling off end of list shouldn't happen... */
				if (subtl == NIL)
					elog(ERROR, "Subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
			}
			break;
		case RTE_JOIN:
			{
				/* Join RTE --- recursively inspect the alias variable */
				Var	   *aliasvar;

				Assert(attnum > 0 && attnum <= length(rte->joinaliasvars));
				aliasvar = (Var *) nth(attnum - 1, rte->joinaliasvars);
				markTargetListOrigin(pstate, res, aliasvar);
			}
			break;
		case RTE_SPECIAL:
		case RTE_FUNCTION:
			/* not a simple relation, leave it unmarked */
			break;
	}
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
	Oid			type_id; 		/* type of value provided */
	Oid			attrtype;		/* type of target column */
	int32		attrtypmod;
	bool		isDefault = false;
	Resdom	   *resnode = tle->resdom;
	Relation	rd = pstate->p_target_relation;

	Assert(rd != NULL);
	if (attrno <= 0)
		elog(ERROR, "Cannot assign to system attribute '%s'", colname);
	attrtype = attnumTypeId(rd, attrno);
	attrtypmod = rd->rd_att->attrs[attrno - 1]->atttypmod;

	/* The type of the default column is equivalent to that of the column */
	if (tle->expr != NULL && IsA(tle->expr, SetToDefault))
	{
		type_id = attrtype;
		isDefault = true;
	}

	/* Otherwise the expression holds the type */
	else
		type_id = exprType((Node *) tle->expr);

	/*
	 * If there are subscripts on the target column, prepare an array
	 * assignment expression.  This will generate an array value that the
	 * source value has been inserted into, which can then be placed in
	 * the new tuple constructed by INSERT or UPDATE. Note that
	 * transformArraySubscripts takes care of type coercion.
	 */
	if (indirection)
	{
		Node	   *arrayBase;
		ArrayRef   *aref;

		if (pstate->p_is_insert)
		{
			/*
			 * The command is INSERT INTO table (arraycol[subscripts]) ...
			 * so there is not really a source array value to work with.
			 * Let the executor do something reasonable, if it can. Notice
			 * that we force transformArraySubscripts to treat the
			 * subscripting op as an array-slice op below, so the source
			 * data will have been coerced to the array type.
			 */
			arrayBase = NULL;	/* signal there is no source array */
		}
		else
		{
			/*
			 * Build a Var for the array to be updated.
			 */
			arrayBase = (Node *) make_var(pstate,
										  pstate->p_target_rangetblentry,
										  attrno);
		}

		aref = transformArraySubscripts(pstate,
										arrayBase,
										attrtype,
										attrtypmod,
										indirection,
										pstate->p_is_insert,
										(Node *) tle->expr);
		tle->expr = (Expr *) aref;
	}
	else
	{
		/*
		 * For normal non-subscripted target column, do type checking and
		 * coercion.  But accept InvalidOid, which indicates the source is
		 * a NULL constant.  (XXX is that still true?)
		 */
		if (!isDefault && type_id != InvalidOid)
		{
			tle->expr = (Expr *)
				coerce_to_target_type(pstate,
									  (Node *) tle->expr, type_id,
									  attrtype, attrtypmod,
									  COERCION_ASSIGNMENT,
									  COERCE_IMPLICIT_CAST);
			if (tle->expr == NULL)
				elog(ERROR, "column \"%s\" is of type %s"
					 " but expression is of type %s"
					 "\n\tYou will need to rewrite or cast the expression",
					 colname,
					 format_type_be(attrtype),
					 format_type_be(type_id));
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


/*
 * checkInsertTargets -
 *	  generate a list of INSERT column targets if not supplied, or
 *	  test supplied column names to make sure they are in target table.
 *	  Also return an integer list of the columns' attribute numbers.
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
			ResTarget  *col;

			if (attr[i]->attisdropped)
				continue;

			col = makeNode(ResTarget);
			col->name = pstrdup(NameStr(attr[i]->attname));
			col->indirection = NIL;
			col->val = NULL;
			cols = lappend(cols, col);
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
			char	   *name = ((ResTarget *) lfirst(tl))->name;
			int			attrno;

			/* Lookup column name, elog on failure */
			attrno = attnameAttNum(pstate->p_target_relation, name, false);
			/* Check for duplicates */
			if (intMember(attrno, *attrnos))
				elog(ERROR, "Attribute '%s' specified more than once", name);
			*attrnos = lappendi(*attrnos, attrno);
		}
	}

	return cols;
}

/* ExpandAllTables()
 * Turns '*' (in the target list) into a list of targetlist entries.
 *
 * tlist entries are generated for each relation appearing at the top level
 * of the query's namespace, except for RTEs marked not inFromCl.  (These
 * may include NEW/OLD pseudo-entries, implicit RTEs, etc.)
 */
static List *
ExpandAllTables(ParseState *pstate)
{
	List	   *target = NIL;
	bool		found_table = false;
	List	   *ns;

	foreach(ns, pstate->p_namespace)
	{
		Node	   *n = (Node *) lfirst(ns);
		RangeTblEntry *rte;

		if (IsA(n, RangeTblRef))
			rte = rt_fetch(((RangeTblRef *) n)->rtindex,
						   pstate->p_rtable);
		else if (IsA(n, JoinExpr))
			rte = rt_fetch(((JoinExpr *) n)->rtindex,
						   pstate->p_rtable);
		else
		{
			elog(ERROR, "ExpandAllTables: unexpected node (internal error)"
				 "\n\t%s", nodeToString(n));
			rte = NULL;			/* keep compiler quiet */
		}

		/*
		 * Ignore added-on relations that were not listed in the FROM
		 * clause.
		 */
		if (!rte->inFromCl)
			continue;

		found_table = true;
		target = nconc(target, expandRelAttrs(pstate, rte));
	}

	/* Check for SELECT *; */
	if (!found_table)
		elog(ERROR, "Wildcard with no tables specified not allowed");

	return target;
}

/*
 * FigureColname -
 *	  if the name of the resulting column is not specified in the target
 *	  list, we have to guess a suitable name.  The SQL spec provides some
 *	  guidance, but not much...
 *
 * Note that the argument is the *untransformed* parse tree for the target
 * item.  This is a shade easier to work with than the transformed tree.
 */
static char *
FigureColname(Node *node)
{
	char	   *name = NULL;

	FigureColnameInternal(node, &name);
	if (name != NULL)
		return name;
	/* default result if we can't guess anything */
	return "?column?";
}

static int
FigureColnameInternal(Node *node, char **name)
{
	int			strength = 0;

	if (node == NULL)
		return strength;

	switch (nodeTag(node))
	{
		case T_ColumnRef:
			{
				char	   *cname = strVal(llast(((ColumnRef *) node)->fields));

				if (strcmp(cname, "*") != 0)
				{
					*name = cname;
					return 2;
				}
			}
			break;
		case T_ExprFieldSelect:
			{
				ExprFieldSelect *efs = (ExprFieldSelect *) node;

				if (efs->fields)
				{
					char	   *fname = strVal(llast(efs->fields));

					if (strcmp(fname, "*") != 0)
					{
						*name = fname;
						return 2;
					}
				}
				return FigureColnameInternal(efs->arg, name);
			}
			break;
		case T_FuncCall:
			*name = strVal(llast(((FuncCall *) node)->funcname));
			return 2;
		case T_A_Expr:
			/* make nullif() act like a regular function */
			if (((A_Expr *) node)->kind == AEXPR_NULLIF)
			{
				*name = "nullif";
				return 2;
			}
			break;
		case T_A_Const:
			if (((A_Const *) node)->typename != NULL)
			{
				*name = strVal(llast(((A_Const *) node)->typename->names));
				return 1;
			}
			break;
		case T_TypeCast:
			strength = FigureColnameInternal(((TypeCast *) node)->arg,
											 name);
			if (strength <= 1)
			{
				if (((TypeCast *) node)->typename != NULL)
				{
					*name = strVal(llast(((TypeCast *) node)->typename->names));
					return 1;
				}
			}
			break;
		case T_CaseExpr:
			strength = FigureColnameInternal((Node *) ((CaseExpr *) node)->defresult,
											 name);
			if (strength <= 1)
			{
				*name = "case";
				return 1;
			}
			break;
		case T_ArrayExpr:
			/* make ARRAY[] act like a function */
			*name = "array";
			return 2;
		case T_CoalesceExpr:
			/* make coalesce() act like a regular function */
			*name = "coalesce";
			return 2;
		default:
			break;
	}

	return strength;
}
