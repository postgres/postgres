/*-------------------------------------------------------------------------
 *
 * parse_target.c
 *	  handle target lists
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_target.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"


static void markTargetListOrigin(ParseState *pstate, TargetEntry *tle,
					 Var *var, int levelsup);
static Node *transformAssignmentIndirection(ParseState *pstate,
							   Node *basenode,
							   const char *targetName,
							   bool targetIsArray,
							   Oid targetTypeId,
							   int32 targetTypMod,
							   Oid targetCollation,
							   ListCell *indirection,
							   Node *rhs,
							   int location);
static Node *transformAssignmentSubscripts(ParseState *pstate,
							  Node *basenode,
							  const char *targetName,
							  Oid targetTypeId,
							  int32 targetTypMod,
							  Oid targetCollation,
							  List *subscripts,
							  bool isSlice,
							  ListCell *next_indirection,
							  Node *rhs,
							  int location);
static List *ExpandColumnRefStar(ParseState *pstate, ColumnRef *cref,
					bool make_target_entry);
static List *ExpandAllTables(ParseState *pstate, int location);
static List *ExpandIndirectionStar(ParseState *pstate, A_Indirection *ind,
					  bool make_target_entry, ParseExprKind exprKind);
static List *ExpandSingleTable(ParseState *pstate, RangeTblEntry *rte,
				  int location, bool make_target_entry);
static List *ExpandRowReference(ParseState *pstate, Node *expr,
				   bool make_target_entry);
static int	FigureColnameInternal(Node *node, char **name);


/*
 * transformTargetEntry()
 *	Transform any ordinary "expression-type" node into a targetlist entry.
 *	This is exported so that parse_clause.c can generate targetlist entries
 *	for ORDER/GROUP BY items that are not already in the targetlist.
 *
 * node		the (untransformed) parse tree for the value expression.
 * expr		the transformed expression, or NULL if caller didn't do it yet.
 * exprKind expression kind (EXPR_KIND_SELECT_TARGET, etc)
 * colname	the column name to be assigned, or NULL if none yet set.
 * resjunk	true if the target should be marked resjunk, ie, it is not
 *			wanted in the final projected tuple.
 */
TargetEntry *
transformTargetEntry(ParseState *pstate,
					 Node *node,
					 Node *expr,
					 ParseExprKind exprKind,
					 char *colname,
					 bool resjunk)
{
	/* Transform the node if caller didn't do it already */
	if (expr == NULL)
		expr = transformExpr(pstate, node, exprKind);

	if (colname == NULL && !resjunk)
	{
		/*
		 * Generate a suitable column name for a column without any explicit
		 * 'AS ColumnName' clause.
		 */
		colname = FigureColname(node);
	}

	return makeTargetEntry((Expr *) expr,
						   (AttrNumber) pstate->p_next_resno++,
						   colname,
						   resjunk);
}


/*
 * transformTargetList()
 * Turns a list of ResTarget's into a list of TargetEntry's.
 *
 * This code acts mostly the same for SELECT, UPDATE, or RETURNING lists;
 * the main thing is to transform the given expressions (the "val" fields).
 * The exprKind parameter distinguishes these cases when necessary.
 */
List *
transformTargetList(ParseState *pstate, List *targetlist,
					ParseExprKind exprKind)
{
	List	   *p_target = NIL;
	ListCell   *o_target;

	/* Shouldn't have any leftover multiassign items at start */
	Assert(pstate->p_multiassign_exprs == NIL);

	foreach(o_target, targetlist)
	{
		ResTarget  *res = (ResTarget *) lfirst(o_target);

		/*
		 * Check for "something.*".  Depending on the complexity of the
		 * "something", the star could appear as the last field in ColumnRef,
		 * or as the last indirection item in A_Indirection.
		 */
		if (IsA(res->val, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) res->val;

			if (IsA(llast(cref->fields), A_Star))
			{
				/* It is something.*, expand into multiple items */
				p_target = list_concat(p_target,
									   ExpandColumnRefStar(pstate, cref,
														   true));
				continue;
			}
		}
		else if (IsA(res->val, A_Indirection))
		{
			A_Indirection *ind = (A_Indirection *) res->val;

			if (IsA(llast(ind->indirection), A_Star))
			{
				/* It is something.*, expand into multiple items */
				p_target = list_concat(p_target,
									   ExpandIndirectionStar(pstate, ind,
															 true, exprKind));
				continue;
			}
		}

		/*
		 * Not "something.*", so transform as a single expression
		 */
		p_target = lappend(p_target,
						   transformTargetEntry(pstate,
												res->val,
												NULL,
												exprKind,
												res->name,
												false));
	}

	/*
	 * If any multiassign resjunk items were created, attach them to the end
	 * of the targetlist.  This should only happen in an UPDATE tlist.  We
	 * don't need to worry about numbering of these items; transformUpdateStmt
	 * will set their resnos.
	 */
	if (pstate->p_multiassign_exprs)
	{
		Assert(exprKind == EXPR_KIND_UPDATE_SOURCE);
		p_target = list_concat(p_target, pstate->p_multiassign_exprs);
		pstate->p_multiassign_exprs = NIL;
	}

	return p_target;
}


/*
 * transformExpressionList()
 *
 * This is the identical transformation to transformTargetList, except that
 * the input list elements are bare expressions without ResTarget decoration,
 * and the output elements are likewise just expressions without TargetEntry
 * decoration.  We use this for ROW() and VALUES() constructs.
 */
List *
transformExpressionList(ParseState *pstate, List *exprlist,
						ParseExprKind exprKind)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, exprlist)
	{
		Node	   *e = (Node *) lfirst(lc);

		/*
		 * Check for "something.*".  Depending on the complexity of the
		 * "something", the star could appear as the last field in ColumnRef,
		 * or as the last indirection item in A_Indirection.
		 */
		if (IsA(e, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) e;

			if (IsA(llast(cref->fields), A_Star))
			{
				/* It is something.*, expand into multiple items */
				result = list_concat(result,
									 ExpandColumnRefStar(pstate, cref,
														 false));
				continue;
			}
		}
		else if (IsA(e, A_Indirection))
		{
			A_Indirection *ind = (A_Indirection *) e;

			if (IsA(llast(ind->indirection), A_Star))
			{
				/* It is something.*, expand into multiple items */
				result = list_concat(result,
									 ExpandIndirectionStar(pstate, ind,
														   false, exprKind));
				continue;
			}
		}

		/*
		 * Not "something.*", so transform as a single expression
		 */
		result = lappend(result,
						 transformExpr(pstate, e, exprKind));
	}

	/* Shouldn't have any multiassign items here */
	Assert(pstate->p_multiassign_exprs == NIL);

	return result;
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
	ListCell   *l;

	foreach(l, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		markTargetListOrigin(pstate, tle, (Var *) tle->expr, 0);
	}
}

/*
 * markTargetListOrigin()
 *		If 'var' is a Var of a plain relation, mark 'tle' with its origin
 *
 * levelsup is an extra offset to interpret the Var's varlevelsup correctly.
 *
 * This is split out so it can recurse for join references.  Note that we
 * do not drill down into views, but report the view as the column owner.
 */
static void
markTargetListOrigin(ParseState *pstate, TargetEntry *tle,
					 Var *var, int levelsup)
{
	int			netlevelsup;
	RangeTblEntry *rte;
	AttrNumber	attnum;

	if (var == NULL || !IsA(var, Var))
		return;
	netlevelsup = var->varlevelsup + levelsup;
	rte = GetRTEByRangeTablePosn(pstate, var->varno, netlevelsup);
	attnum = var->varattno;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* It's a table or view, report it */
			tle->resorigtbl = rte->relid;
			tle->resorigcol = attnum;
			break;
		case RTE_SUBQUERY:
			/* Subselect-in-FROM: copy up from the subselect */
			if (attnum != InvalidAttrNumber)
			{
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				tle->resorigtbl = ste->resorigtbl;
				tle->resorigcol = ste->resorigcol;
			}
			break;
		case RTE_JOIN:
			/* Join RTE --- recursively inspect the alias variable */
			if (attnum != InvalidAttrNumber)
			{
				Var		   *aliasvar;

				Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
				aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);
				/* We intentionally don't strip implicit coercions here */
				markTargetListOrigin(pstate, tle, aliasvar, netlevelsup);
			}
			break;
		case RTE_FUNCTION:
		case RTE_VALUES:
			/* not a simple relation, leave it unmarked */
			break;
		case RTE_CTE:

			/*
			 * CTE reference: copy up from the subquery, if possible. If the
			 * RTE is a recursive self-reference then we can't do anything
			 * because we haven't finished analyzing it yet. However, it's no
			 * big loss because we must be down inside the recursive term of a
			 * recursive CTE, and so any markings on the current targetlist
			 * are not going to affect the results anyway.
			 */
			if (attnum != InvalidAttrNumber && !rte->self_reference)
			{
				CommonTableExpr *cte = GetCTEForRTE(pstate, rte, netlevelsup);
				TargetEntry *ste;

				ste = get_tle_by_resno(GetCTETargetList(cte), attnum);
				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				tle->resorigtbl = ste->resorigtbl;
				tle->resorigcol = ste->resorigcol;
			}
			break;
	}
}


/*
 * transformAssignedExpr()
 *	This is used in INSERT and UPDATE statements only.  It prepares an
 *	expression for assignment to a column of the target table.
 *	This includes coercing the given value to the target column's type
 *	(if necessary), and dealing with any subfield names or subscripts
 *	attached to the target column itself.  The input expression has
 *	already been through transformExpr().
 *
 * pstate		parse state
 * expr			expression to be modified
 * exprKind		indicates which type of statement we're dealing with
 * colname		target column name (ie, name of attribute to be assigned to)
 * attrno		target attribute number
 * indirection	subscripts/field names for target column, if any
 * location		error cursor position for the target column, or -1
 *
 * Returns the modified expression.
 *
 * Note: location points at the target column name (SET target or INSERT
 * column name list entry), and must therefore be -1 in an INSERT that
 * omits the column name list.  So we should usually prefer to use
 * exprLocation(expr) for errors that can happen in a default INSERT.
 */
Expr *
transformAssignedExpr(ParseState *pstate,
					  Expr *expr,
					  ParseExprKind exprKind,
					  char *colname,
					  int attrno,
					  List *indirection,
					  int location)
{
	Relation	rd = pstate->p_target_relation;
	Oid			type_id;		/* type of value provided */
	Oid			attrtype;		/* type of target column */
	int32		attrtypmod;
	Oid			attrcollation;	/* collation of target column */
	ParseExprKind sv_expr_kind;

	/*
	 * Save and restore identity of expression type we're parsing.  We must
	 * set p_expr_kind here because we can parse subscripts without going
	 * through transformExpr().
	 */
	Assert(exprKind != EXPR_KIND_NONE);
	sv_expr_kind = pstate->p_expr_kind;
	pstate->p_expr_kind = exprKind;

	Assert(rd != NULL);
	if (attrno <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot assign to system column \"%s\"",
						colname),
				 parser_errposition(pstate, location)));
	attrtype = attnumTypeId(rd, attrno);
	attrtypmod = rd->rd_att->attrs[attrno - 1]->atttypmod;
	attrcollation = rd->rd_att->attrs[attrno - 1]->attcollation;

	/*
	 * If the expression is a DEFAULT placeholder, insert the attribute's
	 * type/typmod/collation into it so that exprType etc will report the
	 * right things.  (We expect that the eventually substituted default
	 * expression will in fact have this type and typmod.  The collation
	 * likely doesn't matter, but let's set it correctly anyway.)  Also,
	 * reject trying to update a subfield or array element with DEFAULT, since
	 * there can't be any default for portions of a column.
	 */
	if (expr && IsA(expr, SetToDefault))
	{
		SetToDefault *def = (SetToDefault *) expr;

		def->typeId = attrtype;
		def->typeMod = attrtypmod;
		def->collation = attrcollation;
		if (indirection)
		{
			if (IsA(linitial(indirection), A_Indices))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set an array element to DEFAULT"),
						 parser_errposition(pstate, location)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set a subfield to DEFAULT"),
						 parser_errposition(pstate, location)));
		}
	}

	/* Now we can use exprType() safely. */
	type_id = exprType((Node *) expr);

	/*
	 * If there is indirection on the target column, prepare an array or
	 * subfield assignment expression.  This will generate a new column value
	 * that the source value has been inserted into, which can then be placed
	 * in the new tuple constructed by INSERT or UPDATE.
	 */
	if (indirection)
	{
		Node	   *colVar;

		if (pstate->p_is_insert)
		{
			/*
			 * The command is INSERT INTO table (col.something) ... so there
			 * is not really a source value to work with. Insert a NULL
			 * constant as the source value.
			 */
			colVar = (Node *) makeNullConst(attrtype, attrtypmod,
											attrcollation);
		}
		else
		{
			/*
			 * Build a Var for the column to be updated.
			 */
			colVar = (Node *) make_var(pstate,
									   pstate->p_target_rangetblentry,
									   attrno,
									   location);
		}

		expr = (Expr *)
			transformAssignmentIndirection(pstate,
										   colVar,
										   colname,
										   false,
										   attrtype,
										   attrtypmod,
										   attrcollation,
										   list_head(indirection),
										   (Node *) expr,
										   location);
	}
	else
	{
		/*
		 * For normal non-qualified target column, do type checking and
		 * coercion.
		 */
		Node	   *orig_expr = (Node *) expr;

		expr = (Expr *)
			coerce_to_target_type(pstate,
								  orig_expr, type_id,
								  attrtype, attrtypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);
		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is of type %s"
							" but expression is of type %s",
							colname,
							format_type_be(attrtype),
							format_type_be(type_id)),
				 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(orig_expr))));
	}

	pstate->p_expr_kind = sv_expr_kind;

	return expr;
}


/*
 * updateTargetListEntry()
 *	This is used in UPDATE statements (and ON CONFLICT DO UPDATE)
 *	only.  It prepares an UPDATE TargetEntry for assignment to a
 *	column of the target table.  This includes coercing the given
 *	value to the target column's type (if necessary), and dealing with
 *	any subfield names or subscripts attached to the target column
 *	itself.
 *
 * pstate		parse state
 * tle			target list entry to be modified
 * colname		target column name (ie, name of attribute to be assigned to)
 * attrno		target attribute number
 * indirection	subscripts/field names for target column, if any
 * location		error cursor position (should point at column name), or -1
 */
void
updateTargetListEntry(ParseState *pstate,
					  TargetEntry *tle,
					  char *colname,
					  int attrno,
					  List *indirection,
					  int location)
{
	/* Fix up expression as needed */
	tle->expr = transformAssignedExpr(pstate,
									  tle->expr,
									  EXPR_KIND_UPDATE_TARGET,
									  colname,
									  attrno,
									  indirection,
									  location);

	/*
	 * Set the resno to identify the target column --- the rewriter and
	 * planner depend on this.  We also set the resname to identify the target
	 * column, but this is only for debugging purposes; it should not be
	 * relied on.  (In particular, it might be out of date in a stored rule.)
	 */
	tle->resno = (AttrNumber) attrno;
	tle->resname = colname;
}


/*
 * Process indirection (field selection or subscripting) of the target
 * column in INSERT/UPDATE.  This routine recurses for multiple levels
 * of indirection --- but note that several adjacent A_Indices nodes in
 * the indirection list are treated as a single multidimensional subscript
 * operation.
 *
 * In the initial call, basenode is a Var for the target column in UPDATE,
 * or a null Const of the target's type in INSERT.  In recursive calls,
 * basenode is NULL, indicating that a substitute node should be consed up if
 * needed.
 *
 * targetName is the name of the field or subfield we're assigning to, and
 * targetIsArray is true if we're subscripting it.  These are just for
 * error reporting.
 *
 * targetTypeId, targetTypMod, targetCollation indicate the datatype and
 * collation of the object to be assigned to (initially the target column,
 * later some subobject).
 *
 * indirection is the sublist remaining to process.  When it's NULL, we're
 * done recursing and can just coerce and return the RHS.
 *
 * rhs is the already-transformed value to be assigned; note it has not been
 * coerced to any particular type.
 *
 * location is the cursor error position for any errors.  (Note: this points
 * to the head of the target clause, eg "foo" in "foo.bar[baz]".  Later we
 * might want to decorate indirection cells with their own location info,
 * in which case the location argument could probably be dropped.)
 */
static Node *
transformAssignmentIndirection(ParseState *pstate,
							   Node *basenode,
							   const char *targetName,
							   bool targetIsArray,
							   Oid targetTypeId,
							   int32 targetTypMod,
							   Oid targetCollation,
							   ListCell *indirection,
							   Node *rhs,
							   int location)
{
	Node	   *result;
	List	   *subscripts = NIL;
	bool		isSlice = false;
	ListCell   *i;

	if (indirection && !basenode)
	{
		/* Set up a substitution.  We reuse CaseTestExpr for this. */
		CaseTestExpr *ctest = makeNode(CaseTestExpr);

		ctest->typeId = targetTypeId;
		ctest->typeMod = targetTypMod;
		ctest->collation = targetCollation;
		basenode = (Node *) ctest;
	}

	/*
	 * We have to split any field-selection operations apart from
	 * subscripting.  Adjacent A_Indices nodes have to be treated as a single
	 * multidimensional subscript operation.
	 */
	for_each_cell(i, indirection)
	{
		Node	   *n = lfirst(i);

		if (IsA(n, A_Indices))
		{
			subscripts = lappend(subscripts, n);
			if (((A_Indices *) n)->is_slice)
				isSlice = true;
		}
		else if (IsA(n, A_Star))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("row expansion via \"*\" is not supported here"),
					 parser_errposition(pstate, location)));
		}
		else
		{
			FieldStore *fstore;
			Oid			typrelid;
			AttrNumber	attnum;
			Oid			fieldTypeId;
			int32		fieldTypMod;
			Oid			fieldCollation;

			Assert(IsA(n, String));

			/* process subscripts before this field selection */
			if (subscripts)
			{
				/* recurse, and then return because we're done */
				return transformAssignmentSubscripts(pstate,
													 basenode,
													 targetName,
													 targetTypeId,
													 targetTypMod,
													 targetCollation,
													 subscripts,
													 isSlice,
													 i,
													 rhs,
													 location);
			}

			/* No subscripts, so can process field selection here */

			typrelid = typeidTypeRelid(targetTypeId);
			if (!typrelid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot assign to field \"%s\" of column \"%s\" because its type %s is not a composite type",
								strVal(n), targetName,
								format_type_be(targetTypeId)),
						 parser_errposition(pstate, location)));

			attnum = get_attnum(typrelid, strVal(n));
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("cannot assign to field \"%s\" of column \"%s\" because there is no such column in data type %s",
								strVal(n), targetName,
								format_type_be(targetTypeId)),
						 parser_errposition(pstate, location)));
			if (attnum < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("cannot assign to system column \"%s\"",
								strVal(n)),
						 parser_errposition(pstate, location)));

			get_atttypetypmodcoll(typrelid, attnum,
								&fieldTypeId, &fieldTypMod, &fieldCollation);

			/* recurse to create appropriate RHS for field assign */
			rhs = transformAssignmentIndirection(pstate,
												 NULL,
												 strVal(n),
												 false,
												 fieldTypeId,
												 fieldTypMod,
												 fieldCollation,
												 lnext(i),
												 rhs,
												 location);

			/* and build a FieldStore node */
			fstore = makeNode(FieldStore);
			fstore->arg = (Expr *) basenode;
			fstore->newvals = list_make1(rhs);
			fstore->fieldnums = list_make1_int(attnum);
			fstore->resulttype = targetTypeId;

			return (Node *) fstore;
		}
	}

	/* process trailing subscripts, if any */
	if (subscripts)
	{
		/* recurse, and then return because we're done */
		return transformAssignmentSubscripts(pstate,
											 basenode,
											 targetName,
											 targetTypeId,
											 targetTypMod,
											 targetCollation,
											 subscripts,
											 isSlice,
											 NULL,
											 rhs,
											 location);
	}

	/* base case: just coerce RHS to match target type ID */

	result = coerce_to_target_type(pstate,
								   rhs, exprType(rhs),
								   targetTypeId, targetTypMod,
								   COERCION_ASSIGNMENT,
								   COERCE_IMPLICIT_CAST,
								   -1);
	if (result == NULL)
	{
		if (targetIsArray)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("array assignment to \"%s\" requires type %s"
							" but expression is of type %s",
							targetName,
							format_type_be(targetTypeId),
							format_type_be(exprType(rhs))),
				 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("subfield \"%s\" is of type %s"
							" but expression is of type %s",
							targetName,
							format_type_be(targetTypeId),
							format_type_be(exprType(rhs))),
				 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, location)));
	}

	return result;
}

/*
 * helper for transformAssignmentIndirection: process array assignment
 */
static Node *
transformAssignmentSubscripts(ParseState *pstate,
							  Node *basenode,
							  const char *targetName,
							  Oid targetTypeId,
							  int32 targetTypMod,
							  Oid targetCollation,
							  List *subscripts,
							  bool isSlice,
							  ListCell *next_indirection,
							  Node *rhs,
							  int location)
{
	Node	   *result;
	Oid			arrayType;
	int32		arrayTypMod;
	Oid			elementTypeId;
	Oid			typeNeeded;
	Oid			collationNeeded;

	Assert(subscripts != NIL);

	/* Identify the actual array type and element type involved */
	arrayType = targetTypeId;
	arrayTypMod = targetTypMod;
	elementTypeId = transformArrayType(&arrayType, &arrayTypMod);

	/* Identify type that RHS must provide */
	typeNeeded = isSlice ? arrayType : elementTypeId;

	/*
	 * Array normally has same collation as elements, but there's an
	 * exception: we might be subscripting a domain over an array type. In
	 * that case use collation of the base type.
	 */
	if (arrayType == targetTypeId)
		collationNeeded = targetCollation;
	else
		collationNeeded = get_typcollation(arrayType);

	/* recurse to create appropriate RHS for array assign */
	rhs = transformAssignmentIndirection(pstate,
										 NULL,
										 targetName,
										 true,
										 typeNeeded,
										 arrayTypMod,
										 collationNeeded,
										 next_indirection,
										 rhs,
										 location);

	/* process subscripts */
	result = (Node *) transformArraySubscripts(pstate,
											   basenode,
											   arrayType,
											   elementTypeId,
											   arrayTypMod,
											   subscripts,
											   rhs);

	/* If target was a domain over array, need to coerce up to the domain */
	if (arrayType != targetTypeId)
	{
		Oid			resulttype = exprType(result);

		result = coerce_to_target_type(pstate,
									   result, resulttype,
									   targetTypeId, targetTypMod,
									   COERCION_ASSIGNMENT,
									   COERCE_IMPLICIT_CAST,
									   -1);
		/* can fail if we had int2vector/oidvector, but not for true domains */
		if (result == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CANNOT_COERCE),
					 errmsg("cannot cast type %s to %s",
							format_type_be(resulttype),
							format_type_be(targetTypeId)),
					 parser_errposition(pstate, location)));
	}

	return result;
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
			col->location = -1;
			cols = lappend(cols, col);
			*attrnos = lappend_int(*attrnos, i + 1);
		}
	}
	else
	{
		/*
		 * Do initial validation of user-supplied INSERT column list.
		 */
		Bitmapset  *wholecols = NULL;
		Bitmapset  *partialcols = NULL;
		ListCell   *tl;

		foreach(tl, cols)
		{
			ResTarget  *col = (ResTarget *) lfirst(tl);
			char	   *name = col->name;
			int			attrno;

			/* Lookup column name, ereport on failure */
			attrno = attnameAttNum(pstate->p_target_relation, name, false);
			if (attrno == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   name,
						 RelationGetRelationName(pstate->p_target_relation)),
						 parser_errposition(pstate, col->location)));

			/*
			 * Check for duplicates, but only of whole columns --- we allow
			 * INSERT INTO foo (col.subcol1, col.subcol2)
			 */
			if (col->indirection == NIL)
			{
				/* whole column; must not have any other assignment */
				if (bms_is_member(attrno, wholecols) ||
					bms_is_member(attrno, partialcols))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name),
							 parser_errposition(pstate, col->location)));
				wholecols = bms_add_member(wholecols, attrno);
			}
			else
			{
				/* partial column; must not have any whole assignment */
				if (bms_is_member(attrno, wholecols))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name),
							 parser_errposition(pstate, col->location)));
				partialcols = bms_add_member(partialcols, attrno);
			}

			*attrnos = lappend_int(*attrnos, attrno);
		}
	}

	return cols;
}

/*
 * ExpandColumnRefStar()
 *		Transforms foo.* into a list of expressions or targetlist entries.
 *
 * This handles the case where '*' appears as the last or only item in a
 * ColumnRef.  The code is shared between the case of foo.* at the top level
 * in a SELECT target list (where we want TargetEntry nodes in the result)
 * and foo.* in a ROW() or VALUES() construct (where we want just bare
 * expressions).
 *
 * The referenced columns are marked as requiring SELECT access.
 */
static List *
ExpandColumnRefStar(ParseState *pstate, ColumnRef *cref,
					bool make_target_entry)
{
	List	   *fields = cref->fields;
	int			numnames = list_length(fields);

	if (numnames == 1)
	{
		/*
		 * Target item is a bare '*', expand all tables
		 *
		 * (e.g., SELECT * FROM emp, dept)
		 *
		 * Since the grammar only accepts bare '*' at top level of SELECT, we
		 * need not handle the make_target_entry==false case here.
		 */
		Assert(make_target_entry);
		return ExpandAllTables(pstate, cref->location);
	}
	else
	{
		/*
		 * Target item is relation.*, expand that table
		 *
		 * (e.g., SELECT emp.*, dname FROM emp, dept)
		 *
		 * Note: this code is a lot like transformColumnRef; it's tempting to
		 * call that instead and then replace the resulting whole-row Var with
		 * a list of Vars.  However, that would leave us with the RTE's
		 * selectedCols bitmap showing the whole row as needing select
		 * permission, as well as the individual columns.  That would be
		 * incorrect (since columns added later shouldn't need select
		 * permissions).  We could try to remove the whole-row permission bit
		 * after the fact, but duplicating code is less messy.
		 */
		char	   *nspname = NULL;
		char	   *relname = NULL;
		RangeTblEntry *rte = NULL;
		int			levels_up;
		enum
		{
			CRSERR_NO_RTE,
			CRSERR_WRONG_DB,
			CRSERR_TOO_MANY
		}			crserr = CRSERR_NO_RTE;

		/*
		 * Give the PreParseColumnRefHook, if any, first shot.  If it returns
		 * non-null then we should use that expression.
		 */
		if (pstate->p_pre_columnref_hook != NULL)
		{
			Node	   *node;

			node = (*pstate->p_pre_columnref_hook) (pstate, cref);
			if (node != NULL)
				return ExpandRowReference(pstate, node, make_target_entry);
		}

		switch (numnames)
		{
			case 2:
				relname = strVal(linitial(fields));
				rte = refnameRangeTblEntry(pstate, nspname, relname,
										   cref->location,
										   &levels_up);
				break;
			case 3:
				nspname = strVal(linitial(fields));
				relname = strVal(lsecond(fields));
				rte = refnameRangeTblEntry(pstate, nspname, relname,
										   cref->location,
										   &levels_up);
				break;
			case 4:
				{
					char	   *catname = strVal(linitial(fields));

					/*
					 * We check the catalog name and then ignore it.
					 */
					if (strcmp(catname, get_database_name(MyDatabaseId)) != 0)
					{
						crserr = CRSERR_WRONG_DB;
						break;
					}
					nspname = strVal(lsecond(fields));
					relname = strVal(lthird(fields));
					rte = refnameRangeTblEntry(pstate, nspname, relname,
											   cref->location,
											   &levels_up);
					break;
				}
			default:
				crserr = CRSERR_TOO_MANY;
				break;
		}

		/*
		 * Now give the PostParseColumnRefHook, if any, a chance. We cheat a
		 * bit by passing the RangeTblEntry, not a Var, as the planned
		 * translation.  (A single Var wouldn't be strictly correct anyway.
		 * This convention allows hooks that really care to know what is
		 * happening.)
		 */
		if (pstate->p_post_columnref_hook != NULL)
		{
			Node	   *node;

			node = (*pstate->p_post_columnref_hook) (pstate, cref,
													 (Node *) rte);
			if (node != NULL)
			{
				if (rte != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("column reference \"%s\" is ambiguous",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));
				return ExpandRowReference(pstate, node, make_target_entry);
			}
		}

		/*
		 * Throw error if no translation found.
		 */
		if (rte == NULL)
		{
			switch (crserr)
			{
				case CRSERR_NO_RTE:
					errorMissingRTE(pstate, makeRangeVar(nspname, relname,
														 cref->location));
					break;
				case CRSERR_WRONG_DB:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: %s",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));
					break;
				case CRSERR_TOO_MANY:
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("improper qualified name (too many dotted names): %s",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));
					break;
			}
		}

		/*
		 * OK, expand the RTE into fields.
		 */
		return ExpandSingleTable(pstate, rte, cref->location, make_target_entry);
	}
}

/*
 * ExpandAllTables()
 *		Transforms '*' (in the target list) into a list of targetlist entries.
 *
 * tlist entries are generated for each relation visible for unqualified
 * column name access.  We do not consider qualified-name-only entries because
 * that would include input tables of aliasless JOINs, NEW/OLD pseudo-entries,
 * etc.
 *
 * The referenced relations/columns are marked as requiring SELECT access.
 */
static List *
ExpandAllTables(ParseState *pstate, int location)
{
	List	   *target = NIL;
	bool		found_table = false;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
		RangeTblEntry *rte = nsitem->p_rte;

		/* Ignore table-only items */
		if (!nsitem->p_cols_visible)
			continue;
		/* Should not have any lateral-only items when parsing targetlist */
		Assert(!nsitem->p_lateral_only);
		/* Remember we found a p_cols_visible item */
		found_table = true;

		target = list_concat(target,
							 expandRelAttrs(pstate,
											rte,
											RTERangeTablePosn(pstate, rte,
															  NULL),
											0,
											location));
	}

	/*
	 * Check for "SELECT *;".  We do it this way, rather than checking for
	 * target == NIL, because we want to allow SELECT * FROM a zero_column
	 * table.
	 */
	if (!found_table)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT * with no tables specified is not valid"),
				 parser_errposition(pstate, location)));

	return target;
}

/*
 * ExpandIndirectionStar()
 *		Transforms foo.* into a list of expressions or targetlist entries.
 *
 * This handles the case where '*' appears as the last item in A_Indirection.
 * The code is shared between the case of foo.* at the top level in a SELECT
 * target list (where we want TargetEntry nodes in the result) and foo.* in
 * a ROW() or VALUES() construct (where we want just bare expressions).
 * For robustness, we use a separate "make_target_entry" flag to control
 * this rather than relying on exprKind.
 */
static List *
ExpandIndirectionStar(ParseState *pstate, A_Indirection *ind,
					  bool make_target_entry, ParseExprKind exprKind)
{
	Node	   *expr;

	/* Strip off the '*' to create a reference to the rowtype object */
	ind = copyObject(ind);
	ind->indirection = list_truncate(ind->indirection,
									 list_length(ind->indirection) - 1);

	/* And transform that */
	expr = transformExpr(pstate, (Node *) ind, exprKind);

	/* Expand the rowtype expression into individual fields */
	return ExpandRowReference(pstate, expr, make_target_entry);
}

/*
 * ExpandSingleTable()
 *		Transforms foo.* into a list of expressions or targetlist entries.
 *
 * This handles the case where foo has been determined to be a simple
 * reference to an RTE, so we can just generate Vars for the expressions.
 *
 * The referenced columns are marked as requiring SELECT access.
 */
static List *
ExpandSingleTable(ParseState *pstate, RangeTblEntry *rte,
				  int location, bool make_target_entry)
{
	int			sublevels_up;
	int			rtindex;

	rtindex = RTERangeTablePosn(pstate, rte, &sublevels_up);

	if (make_target_entry)
	{
		/* expandRelAttrs handles permissions marking */
		return expandRelAttrs(pstate, rte, rtindex, sublevels_up,
							  location);
	}
	else
	{
		List	   *vars;
		ListCell   *l;

		expandRTE(rte, rtindex, sublevels_up, location, false,
				  NULL, &vars);

		/*
		 * Require read access to the table.  This is normally redundant with
		 * the markVarForSelectPriv calls below, but not if the table has zero
		 * columns.
		 */
		rte->requiredPerms |= ACL_SELECT;

		/* Require read access to each column */
		foreach(l, vars)
		{
			Var		   *var = (Var *) lfirst(l);

			markVarForSelectPriv(pstate, var, rte);
		}

		return vars;
	}
}

/*
 * ExpandRowReference()
 *		Transforms foo.* into a list of expressions or targetlist entries.
 *
 * This handles the case where foo is an arbitrary expression of composite
 * type.
 */
static List *
ExpandRowReference(ParseState *pstate, Node *expr,
				   bool make_target_entry)
{
	List	   *result = NIL;
	TupleDesc	tupleDesc;
	int			numAttrs;
	int			i;

	/*
	 * If the rowtype expression is a whole-row Var, we can expand the fields
	 * as simple Vars.  Note: if the RTE is a relation, this case leaves us
	 * with the RTE's selectedCols bitmap showing the whole row as needing
	 * select permission, as well as the individual columns.  However, we can
	 * only get here for weird notations like (table.*).*, so it's not worth
	 * trying to clean up --- arguably, the permissions marking is correct
	 * anyway for such cases.
	 */
	if (IsA(expr, Var) &&
		((Var *) expr)->varattno == InvalidAttrNumber)
	{
		Var		   *var = (Var *) expr;
		RangeTblEntry *rte;

		rte = GetRTEByRangeTablePosn(pstate, var->varno, var->varlevelsup);
		return ExpandSingleTable(pstate, rte, var->location, make_target_entry);
	}

	/*
	 * Otherwise we have to do it the hard way.  Our current implementation is
	 * to generate multiple copies of the expression and do FieldSelects.
	 * (This can be pretty inefficient if the expression involves nontrivial
	 * computation :-(.)
	 *
	 * Verify it's a composite type, and get the tupdesc.  We use
	 * get_expr_result_type() because that can handle references to functions
	 * returning anonymous record types.  If that fails, use
	 * lookup_rowtype_tupdesc(), which will almost certainly fail as well, but
	 * it will give an appropriate error message.
	 *
	 * If it's a Var of type RECORD, we have to work even harder: we have to
	 * find what the Var refers to, and pass that to get_expr_result_type.
	 * That task is handled by expandRecordVariable().
	 */
	if (IsA(expr, Var) &&
		((Var *) expr)->vartype == RECORDOID)
		tupleDesc = expandRecordVariable(pstate, (Var *) expr, 0);
	else if (get_expr_result_type(expr, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		tupleDesc = lookup_rowtype_tupdesc_copy(exprType(expr),
												exprTypmod(expr));
	Assert(tupleDesc);

	/* Generate a list of references to the individual fields */
	numAttrs = tupleDesc->natts;
	for (i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = tupleDesc->attrs[i];
		FieldSelect *fselect;

		if (att->attisdropped)
			continue;

		fselect = makeNode(FieldSelect);
		fselect->arg = (Expr *) copyObject(expr);
		fselect->fieldnum = i + 1;
		fselect->resulttype = att->atttypid;
		fselect->resulttypmod = att->atttypmod;
		/* save attribute's collation for parse_collate.c */
		fselect->resultcollid = att->attcollation;

		if (make_target_entry)
		{
			/* add TargetEntry decoration */
			TargetEntry *te;

			te = makeTargetEntry((Expr *) fselect,
								 (AttrNumber) pstate->p_next_resno++,
								 pstrdup(NameStr(att->attname)),
								 false);
			result = lappend(result, te);
		}
		else
			result = lappend(result, fselect);
	}

	return result;
}

/*
 * expandRecordVariable
 *		Get the tuple descriptor for a Var of type RECORD, if possible.
 *
 * Since no actual table or view column is allowed to have type RECORD, such
 * a Var must refer to a JOIN or FUNCTION RTE or to a subquery output.  We
 * drill down to find the ultimate defining expression and attempt to infer
 * the tupdesc from it.  We ereport if we can't determine the tupdesc.
 *
 * levelsup is an extra offset to interpret the Var's varlevelsup correctly.
 */
TupleDesc
expandRecordVariable(ParseState *pstate, Var *var, int levelsup)
{
	TupleDesc	tupleDesc;
	int			netlevelsup;
	RangeTblEntry *rte;
	AttrNumber	attnum;
	Node	   *expr;

	/* Check my caller didn't mess up */
	Assert(IsA(var, Var));
	Assert(var->vartype == RECORDOID);

	netlevelsup = var->varlevelsup + levelsup;
	rte = GetRTEByRangeTablePosn(pstate, var->varno, netlevelsup);
	attnum = var->varattno;

	if (attnum == InvalidAttrNumber)
	{
		/* Whole-row reference to an RTE, so expand the known fields */
		List	   *names,
				   *vars;
		ListCell   *lname,
				   *lvar;
		int			i;

		expandRTE(rte, var->varno, 0, var->location, false,
				  &names, &vars);

		tupleDesc = CreateTemplateTupleDesc(list_length(vars), false);
		i = 1;
		forboth(lname, names, lvar, vars)
		{
			char	   *label = strVal(lfirst(lname));
			Node	   *varnode = (Node *) lfirst(lvar);

			TupleDescInitEntry(tupleDesc, i,
							   label,
							   exprType(varnode),
							   exprTypmod(varnode),
							   0);
			TupleDescInitEntryCollation(tupleDesc, i,
										exprCollation(varnode));
			i++;
		}
		Assert(lname == NULL && lvar == NULL);	/* lists same length? */

		return tupleDesc;
	}

	expr = (Node *) var;		/* default if we can't drill down */

	switch (rte->rtekind)
	{
		case RTE_RELATION:
		case RTE_VALUES:

			/*
			 * This case should not occur: a column of a table or values list
			 * shouldn't have type RECORD.  Fall through and fail (most
			 * likely) at the bottom.
			 */
			break;
		case RTE_SUBQUERY:
			{
				/* Subselect-in-FROM: examine sub-select's output expr */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				expr = (Node *) ste->expr;
				if (IsA(expr, Var))
				{
					/*
					 * Recurse into the sub-select to see what its Var refers
					 * to.  We have to build an additional level of ParseState
					 * to keep in step with varlevelsup in the subselect.
					 */
					ParseState	mypstate;

					MemSet(&mypstate, 0, sizeof(mypstate));
					mypstate.parentParseState = pstate;
					mypstate.p_rtable = rte->subquery->rtable;
					/* don't bother filling the rest of the fake pstate */

					return expandRecordVariable(&mypstate, (Var *) expr, 0);
				}
				/* else fall through to inspect the expression */
			}
			break;
		case RTE_JOIN:
			/* Join RTE --- recursively inspect the alias variable */
			Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
			expr = (Node *) list_nth(rte->joinaliasvars, attnum - 1);
			Assert(expr != NULL);
			/* We intentionally don't strip implicit coercions here */
			if (IsA(expr, Var))
				return expandRecordVariable(pstate, (Var *) expr, netlevelsup);
			/* else fall through to inspect the expression */
			break;
		case RTE_FUNCTION:

			/*
			 * We couldn't get here unless a function is declared with one of
			 * its result columns as RECORD, which is not allowed.
			 */
			break;
		case RTE_CTE:
			/* CTE reference: examine subquery's output expr */
			if (!rte->self_reference)
			{
				CommonTableExpr *cte = GetCTEForRTE(pstate, rte, netlevelsup);
				TargetEntry *ste;

				ste = get_tle_by_resno(GetCTETargetList(cte), attnum);
				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				expr = (Node *) ste->expr;
				if (IsA(expr, Var))
				{
					/*
					 * Recurse into the CTE to see what its Var refers to. We
					 * have to build an additional level of ParseState to keep
					 * in step with varlevelsup in the CTE; furthermore it
					 * could be an outer CTE.
					 */
					ParseState	mypstate;
					Index		levelsup;

					MemSet(&mypstate, 0, sizeof(mypstate));
					/* this loop must work, since GetCTEForRTE did */
					for (levelsup = 0;
						 levelsup < rte->ctelevelsup + netlevelsup;
						 levelsup++)
						pstate = pstate->parentParseState;
					mypstate.parentParseState = pstate;
					mypstate.p_rtable = ((Query *) cte->ctequery)->rtable;
					/* don't bother filling the rest of the fake pstate */

					return expandRecordVariable(&mypstate, (Var *) expr, 0);
				}
				/* else fall through to inspect the expression */
			}
			break;
	}

	/*
	 * We now have an expression we can't expand any more, so see if
	 * get_expr_result_type() can do anything with it.  If not, pass to
	 * lookup_rowtype_tupdesc() which will probably fail, but will give an
	 * appropriate error message while failing.
	 */
	if (get_expr_result_type(expr, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		tupleDesc = lookup_rowtype_tupdesc_copy(exprType(expr),
												exprTypmod(expr));

	return tupleDesc;
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
char *
FigureColname(Node *node)
{
	char	   *name = NULL;

	(void) FigureColnameInternal(node, &name);
	if (name != NULL)
		return name;
	/* default result if we can't guess anything */
	return "?column?";
}

/*
 * FigureIndexColname -
 *	  choose the name for an expression column in an index
 *
 * This is actually just like FigureColname, except we return NULL if
 * we can't pick a good name.
 */
char *
FigureIndexColname(Node *node)
{
	char	   *name = NULL;

	(void) FigureColnameInternal(node, &name);
	return name;
}

/*
 * FigureColnameInternal -
 *	  internal workhorse for FigureColname
 *
 * Return value indicates strength of confidence in result:
 *		0 - no information
 *		1 - second-best name choice
 *		2 - good name choice
 * The return value is actually only used internally.
 * If the result isn't zero, *name is set to the chosen name.
 */
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
				char	   *fname = NULL;
				ListCell   *l;

				/* find last field name, if any, ignoring "*" */
				foreach(l, ((ColumnRef *) node)->fields)
				{
					Node	   *i = lfirst(l);

					if (IsA(i, String))
						fname = strVal(i);
				}
				if (fname)
				{
					*name = fname;
					return 2;
				}
			}
			break;
		case T_A_Indirection:
			{
				A_Indirection *ind = (A_Indirection *) node;
				char	   *fname = NULL;
				ListCell   *l;

				/* find last field name, if any, ignoring "*" and subscripts */
				foreach(l, ind->indirection)
				{
					Node	   *i = lfirst(l);

					if (IsA(i, String))
						fname = strVal(i);
				}
				if (fname)
				{
					*name = fname;
					return 2;
				}
				return FigureColnameInternal(ind->arg, name);
			}
			break;
		case T_FuncCall:
			*name = strVal(llast(((FuncCall *) node)->funcname));
			return 2;
		case T_A_Expr:
			if (((A_Expr *) node)->kind == AEXPR_NULLIF)
			{
				/* make nullif() act like a regular function */
				*name = "nullif";
				return 2;
			}
			if (((A_Expr *) node)->kind == AEXPR_PAREN)
			{
				/* look through dummy parenthesis node */
				return FigureColnameInternal(((A_Expr *) node)->lexpr, name);
			}
			break;
		case T_TypeCast:
			strength = FigureColnameInternal(((TypeCast *) node)->arg,
											 name);
			if (strength <= 1)
			{
				if (((TypeCast *) node)->typeName != NULL)
				{
					*name = strVal(llast(((TypeCast *) node)->typeName->names));
					return 1;
				}
			}
			break;
		case T_CollateClause:
			return FigureColnameInternal(((CollateClause *) node)->arg, name);
		case T_GroupingFunc:
			/* make GROUPING() act like a regular function */
			*name = "grouping";
			return 2;
		case T_SubLink:
			switch (((SubLink *) node)->subLinkType)
			{
				case EXISTS_SUBLINK:
					*name = "exists";
					return 2;
				case ARRAY_SUBLINK:
					*name = "array";
					return 2;
				case EXPR_SUBLINK:
					{
						/* Get column name of the subquery's single target */
						SubLink    *sublink = (SubLink *) node;
						Query	   *query = (Query *) sublink->subselect;

						/*
						 * The subquery has probably already been transformed,
						 * but let's be careful and check that.  (The reason
						 * we can see a transformed subquery here is that
						 * transformSubLink is lazy and modifies the SubLink
						 * node in-place.)
						 */
						if (IsA(query, Query))
						{
							TargetEntry *te = (TargetEntry *) linitial(query->targetList);

							if (te->resname)
							{
								*name = te->resname;
								return 2;
							}
						}
					}
					break;
					/* As with other operator-like nodes, these have no names */
				case MULTIEXPR_SUBLINK:
				case ALL_SUBLINK:
				case ANY_SUBLINK:
				case ROWCOMPARE_SUBLINK:
				case CTE_SUBLINK:
					break;
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
		case T_A_ArrayExpr:
			/* make ARRAY[] act like a function */
			*name = "array";
			return 2;
		case T_RowExpr:
			/* make ROW() act like a function */
			*name = "row";
			return 2;
		case T_CoalesceExpr:
			/* make coalesce() act like a regular function */
			*name = "coalesce";
			return 2;
		case T_MinMaxExpr:
			/* make greatest/least act like a regular function */
			switch (((MinMaxExpr *) node)->op)
			{
				case IS_GREATEST:
					*name = "greatest";
					return 2;
				case IS_LEAST:
					*name = "least";
					return 2;
			}
			break;
		case T_XmlExpr:
			/* make SQL/XML functions act like a regular function */
			switch (((XmlExpr *) node)->op)
			{
				case IS_XMLCONCAT:
					*name = "xmlconcat";
					return 2;
				case IS_XMLELEMENT:
					*name = "xmlelement";
					return 2;
				case IS_XMLFOREST:
					*name = "xmlforest";
					return 2;
				case IS_XMLPARSE:
					*name = "xmlparse";
					return 2;
				case IS_XMLPI:
					*name = "xmlpi";
					return 2;
				case IS_XMLROOT:
					*name = "xmlroot";
					return 2;
				case IS_XMLSERIALIZE:
					*name = "xmlserialize";
					return 2;
				case IS_DOCUMENT:
					/* nothing */
					break;
			}
			break;
		case T_XmlSerialize:
			*name = "xmlserialize";
			return 2;
		default:
			break;
	}

	return strength;
}
