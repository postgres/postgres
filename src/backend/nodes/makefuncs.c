/*-------------------------------------------------------------------------
 *
 * makefuncs.c
 *	  creator functions for various nodes. The functions here are for the
 *	  most frequently created nodes.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/makefuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"


/*
 * makeA_Expr -
 *		makes an A_Expr node
 */
A_Expr *
makeA_Expr(A_Expr_Kind kind, List *name,
		   Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = name;
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * makeSimpleA_Expr -
 *		As above, given a simple (unqualified) operator name
 */
A_Expr *
makeSimpleA_Expr(A_Expr_Kind kind, char *name,
				 Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = list_make1(makeString((char *) name));
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * makeVar -
 *	  creates a Var node
 */
Var *
makeVar(int varno,
		AttrNumber varattno,
		Oid vartype,
		int32 vartypmod,
		Oid varcollid,
		Index varlevelsup)
{
	Var		   *var = makeNode(Var);

	var->varno = varno;
	var->varattno = varattno;
	var->vartype = vartype;
	var->vartypmod = vartypmod;
	var->varcollid = varcollid;
	var->varlevelsup = varlevelsup;

	/*
	 * Only a few callers need to make Var nodes with non-null varnullingrels,
	 * or with varnosyn/varattnosyn different from varno/varattno.  We don't
	 * provide separate arguments for them, but just initialize them to NULL
	 * and the given varno/varattno.  This reduces code clutter and chance of
	 * error for most callers.
	 */
	var->varnullingrels = NULL;
	var->varnosyn = (Index) varno;
	var->varattnosyn = varattno;

	/* Likewise, we just set location to "unknown" here */
	var->location = -1;

	return var;
}

/*
 * makeVarFromTargetEntry -
 *		convenience function to create a same-level Var node from a
 *		TargetEntry
 */
Var *
makeVarFromTargetEntry(int varno,
					   TargetEntry *tle)
{
	return makeVar(varno,
				   tle->resno,
				   exprType((Node *) tle->expr),
				   exprTypmod((Node *) tle->expr),
				   exprCollation((Node *) tle->expr),
				   0);
}

/*
 * makeWholeRowVar -
 *	  creates a Var node representing a whole row of the specified RTE
 *
 * A whole-row reference is a Var with varno set to the correct range
 * table entry, and varattno == 0 to signal that it references the whole
 * tuple.  (Use of zero here is unclean, since it could easily be confused
 * with error cases, but it's not worth changing now.)  The vartype indicates
 * a rowtype; either a named composite type, or a domain over a named
 * composite type (only possible if the RTE is a function returning that),
 * or RECORD.  This function encapsulates the logic for determining the
 * correct rowtype OID to use.
 *
 * If allowScalar is true, then for the case where the RTE is a single function
 * returning a non-composite result type, we produce a normal Var referencing
 * the function's result directly, instead of the single-column composite
 * value that the whole-row notation might otherwise suggest.
 */
Var *
makeWholeRowVar(RangeTblEntry *rte,
				int varno,
				Index varlevelsup,
				bool allowScalar)
{
	Var		   *result;
	Oid			toid;
	Node	   *fexpr;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* relation: the rowtype is a named composite type */
			toid = get_rel_type_id(rte->relid);
			if (!OidIsValid(toid))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("relation \"%s\" does not have a composite type",
								get_rel_name(rte->relid))));
			result = makeVar(varno,
							 InvalidAttrNumber,
							 toid,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;

		case RTE_FUNCTION:

			/*
			 * If there's more than one function, or ordinality is requested,
			 * force a RECORD result, since there's certainly more than one
			 * column involved and it can't be a known named type.
			 */
			if (rte->funcordinality || list_length(rte->functions) != 1)
			{
				/* always produces an anonymous RECORD result */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 RECORDOID,
								 -1,
								 InvalidOid,
								 varlevelsup);
				break;
			}

			fexpr = ((RangeTblFunction *) linitial(rte->functions))->funcexpr;
			toid = exprType(fexpr);
			if (type_is_rowtype(toid))
			{
				/* func returns composite; same as relation case */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 toid,
								 -1,
								 InvalidOid,
								 varlevelsup);
			}
			else if (allowScalar)
			{
				/* func returns scalar; just return its output as-is */
				result = makeVar(varno,
								 1,
								 toid,
								 -1,
								 exprCollation(fexpr),
								 varlevelsup);
			}
			else
			{
				/* func returns scalar, but we want a composite result */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 RECORDOID,
								 -1,
								 InvalidOid,
								 varlevelsup);
			}
			break;

		default:

			/*
			 * RTE is a join, subselect, tablefunc, or VALUES.  We represent
			 * this as a whole-row Var of RECORD type. (Note that in most
			 * cases the Var will be expanded to a RowExpr during planning,
			 * but that is not our concern here.)
			 */
			result = makeVar(varno,
							 InvalidAttrNumber,
							 RECORDOID,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;
	}

	return result;
}

/*
 * makeTargetEntry -
 *	  creates a TargetEntry node
 */
TargetEntry *
makeTargetEntry(Expr *expr,
				AttrNumber resno,
				char *resname,
				bool resjunk)
{
	TargetEntry *tle = makeNode(TargetEntry);

	tle->expr = expr;
	tle->resno = resno;
	tle->resname = resname;

	/*
	 * We always set these fields to 0. If the caller wants to change them he
	 * must do so explicitly.  Few callers do that, so omitting these
	 * arguments reduces the chance of error.
	 */
	tle->ressortgroupref = 0;
	tle->resorigtbl = InvalidOid;
	tle->resorigcol = 0;

	tle->resjunk = resjunk;

	return tle;
}

/*
 * flatCopyTargetEntry -
 *	  duplicate a TargetEntry, but don't copy substructure
 *
 * This is commonly used when we just want to modify the resno or substitute
 * a new expression.
 */
TargetEntry *
flatCopyTargetEntry(TargetEntry *src_tle)
{
	TargetEntry *tle = makeNode(TargetEntry);

	Assert(IsA(src_tle, TargetEntry));
	memcpy(tle, src_tle, sizeof(TargetEntry));
	return tle;
}

/*
 * makeFromExpr -
 *	  creates a FromExpr node
 */
FromExpr *
makeFromExpr(List *fromlist, Node *quals)
{
	FromExpr   *f = makeNode(FromExpr);

	f->fromlist = fromlist;
	f->quals = quals;
	return f;
}

/*
 * makeConst -
 *	  creates a Const node
 */
Const *
makeConst(Oid consttype,
		  int32 consttypmod,
		  Oid constcollid,
		  int constlen,
		  Datum constvalue,
		  bool constisnull,
		  bool constbyval)
{
	Const	   *cnst = makeNode(Const);

	/*
	 * If it's a varlena value, force it to be in non-expanded (non-toasted)
	 * format; this avoids any possible dependency on external values and
	 * improves consistency of representation, which is important for equal().
	 */
	if (!constisnull && constlen == -1)
		constvalue = PointerGetDatum(PG_DETOAST_DATUM(constvalue));

	cnst->consttype = consttype;
	cnst->consttypmod = consttypmod;
	cnst->constcollid = constcollid;
	cnst->constlen = constlen;
	cnst->constvalue = constvalue;
	cnst->constisnull = constisnull;
	cnst->constbyval = constbyval;
	cnst->location = -1;		/* "unknown" */

	return cnst;
}

/*
 * makeNullConst -
 *	  creates a Const node representing a NULL of the specified type/typmod
 *
 * This is a convenience routine that just saves a lookup of the type's
 * storage properties.
 */
Const *
makeNullConst(Oid consttype, int32 consttypmod, Oid constcollid)
{
	int16		typLen;
	bool		typByVal;

	get_typlenbyval(consttype, &typLen, &typByVal);
	return makeConst(consttype,
					 consttypmod,
					 constcollid,
					 (int) typLen,
					 (Datum) 0,
					 true,
					 typByVal);
}

/*
 * makeBoolConst -
 *	  creates a Const node representing a boolean value (can be NULL too)
 */
Node *
makeBoolConst(bool value, bool isnull)
{
	/* note that pg_type.h hardwires size of bool as 1 ... duplicate it */
	return (Node *) makeConst(BOOLOID, -1, InvalidOid, 1,
							  BoolGetDatum(value), isnull, true);
}

/*
 * makeBoolExpr -
 *	  creates a BoolExpr node
 */
Expr *
makeBoolExpr(BoolExprType boolop, List *args, int location)
{
	BoolExpr   *b = makeNode(BoolExpr);

	b->boolop = boolop;
	b->args = args;
	b->location = location;

	return (Expr *) b;
}

/*
 * makeAlias -
 *	  creates an Alias node
 *
 * NOTE: the given name is copied, but the colnames list (if any) isn't.
 */
Alias *
makeAlias(const char *aliasname, List *colnames)
{
	Alias	   *a = makeNode(Alias);

	a->aliasname = pstrdup(aliasname);
	a->colnames = colnames;

	return a;
}

/*
 * makeRelabelType -
 *	  creates a RelabelType node
 */
RelabelType *
makeRelabelType(Expr *arg, Oid rtype, int32 rtypmod, Oid rcollid,
				CoercionForm rformat)
{
	RelabelType *r = makeNode(RelabelType);

	r->arg = arg;
	r->resulttype = rtype;
	r->resulttypmod = rtypmod;
	r->resultcollid = rcollid;
	r->relabelformat = rformat;
	r->location = -1;

	return r;
}

/*
 * makeRangeVar -
 *	  creates a RangeVar node (rather oversimplified case)
 */
RangeVar *
makeRangeVar(char *schemaname, char *relname, int location)
{
	RangeVar   *r = makeNode(RangeVar);

	r->catalogname = NULL;
	r->schemaname = schemaname;
	r->relname = relname;
	r->inh = true;
	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->alias = NULL;
	r->location = location;

	return r;
}

/*
 * makeNotNullConstraint -
 *		creates a Constraint node for NOT NULL constraints
 */
Constraint *
makeNotNullConstraint(String *colname)
{
	Constraint *notnull;

	notnull = makeNode(Constraint);
	notnull->contype = CONSTR_NOTNULL;
	notnull->conname = NULL;
	notnull->is_no_inherit = false;
	notnull->deferrable = false;
	notnull->initdeferred = false;
	notnull->location = -1;
	notnull->keys = list_make1(colname);
	notnull->skip_validation = false;
	notnull->initially_valid = true;

	return notnull;
}

/*
 * makeTypeName -
 *	build a TypeName node for an unqualified name.
 *
 * typmod is defaulted, but can be changed later by caller.
 */
TypeName *
makeTypeName(char *typnam)
{
	return makeTypeNameFromNameList(list_make1(makeString(typnam)));
}

/*
 * makeTypeNameFromNameList -
 *	build a TypeName node for a String list representing a qualified name.
 *
 * typmod is defaulted, but can be changed later by caller.
 */
TypeName *
makeTypeNameFromNameList(List *names)
{
	TypeName   *n = makeNode(TypeName);

	n->names = names;
	n->typmods = NIL;
	n->typemod = -1;
	n->location = -1;
	return n;
}

/*
 * makeTypeNameFromOid -
 *	build a TypeName node to represent a type already known by OID/typmod.
 */
TypeName *
makeTypeNameFromOid(Oid typeOid, int32 typmod)
{
	TypeName   *n = makeNode(TypeName);

	n->typeOid = typeOid;
	n->typemod = typmod;
	n->location = -1;
	return n;
}

/*
 * makeColumnDef -
 *	build a ColumnDef node to represent a simple column definition.
 *
 * Type and collation are specified by OID.
 * Other properties are all basic to start with.
 */
ColumnDef *
makeColumnDef(const char *colname, Oid typeOid, int32 typmod, Oid collOid)
{
	ColumnDef  *n = makeNode(ColumnDef);

	n->colname = pstrdup(colname);
	n->typeName = makeTypeNameFromOid(typeOid, typmod);
	n->inhcount = 0;
	n->is_local = true;
	n->is_not_null = false;
	n->is_from_type = false;
	n->storage = 0;
	n->raw_default = NULL;
	n->cooked_default = NULL;
	n->collClause = NULL;
	n->collOid = collOid;
	n->constraints = NIL;
	n->fdwoptions = NIL;
	n->location = -1;

	return n;
}

/*
 * makeFuncExpr -
 *	build an expression tree representing a function call.
 *
 * The argument expressions must have been transformed already.
 */
FuncExpr *
makeFuncExpr(Oid funcid, Oid rettype, List *args,
			 Oid funccollid, Oid inputcollid, CoercionForm fformat)
{
	FuncExpr   *funcexpr;

	funcexpr = makeNode(FuncExpr);
	funcexpr->funcid = funcid;
	funcexpr->funcresulttype = rettype;
	funcexpr->funcretset = false;	/* only allowed case here */
	funcexpr->funcvariadic = false; /* only allowed case here */
	funcexpr->funcformat = fformat;
	funcexpr->funccollid = funccollid;
	funcexpr->inputcollid = inputcollid;
	funcexpr->args = args;
	funcexpr->location = -1;

	return funcexpr;
}

/*
 * makeStringConst -
 * 	build a A_Const node of type T_String for given string
 */
Node *
makeStringConst(char *str, int location)
{
	A_Const    *n = makeNode(A_Const);

	n->val.sval.type = T_String;
	n->val.sval.sval = str;
	n->location = location;

	return (Node *) n;
}

/*
 * makeDefElem -
 *	build a DefElem node
 *
 * This is sufficient for the "typical" case with an unqualified option name
 * and no special action.
 */
DefElem *
makeDefElem(char *name, Node *arg, int location)
{
	DefElem    *res = makeNode(DefElem);

	res->defnamespace = NULL;
	res->defname = name;
	res->arg = arg;
	res->defaction = DEFELEM_UNSPEC;
	res->location = location;

	return res;
}

/*
 * makeDefElemExtended -
 *	build a DefElem node with all fields available to be specified
 */
DefElem *
makeDefElemExtended(char *nameSpace, char *name, Node *arg,
					DefElemAction defaction, int location)
{
	DefElem    *res = makeNode(DefElem);

	res->defnamespace = nameSpace;
	res->defname = name;
	res->arg = arg;
	res->defaction = defaction;
	res->location = location;

	return res;
}

/*
 * makeFuncCall -
 *
 * Initialize a FuncCall struct with the information every caller must
 * supply.  Any non-default parameters have to be inserted by the caller.
 */
FuncCall *
makeFuncCall(List *name, List *args, CoercionForm funcformat, int location)
{
	FuncCall   *n = makeNode(FuncCall);

	n->funcname = name;
	n->args = args;
	n->agg_order = NIL;
	n->agg_filter = NULL;
	n->over = NULL;
	n->agg_within_group = false;
	n->agg_star = false;
	n->agg_distinct = false;
	n->func_variadic = false;
	n->funcformat = funcformat;
	n->location = location;
	return n;
}

/*
 * make_opclause
 *	  Creates an operator clause given its operator info, left operand
 *	  and right operand (pass NULL to create single-operand clause),
 *	  and collation info.
 */
Expr *
make_opclause(Oid opno, Oid opresulttype, bool opretset,
			  Expr *leftop, Expr *rightop,
			  Oid opcollid, Oid inputcollid)
{
	OpExpr	   *expr = makeNode(OpExpr);

	expr->opno = opno;
	expr->opfuncid = InvalidOid;
	expr->opresulttype = opresulttype;
	expr->opretset = opretset;
	expr->opcollid = opcollid;
	expr->inputcollid = inputcollid;
	if (rightop)
		expr->args = list_make2(leftop, rightop);
	else
		expr->args = list_make1(leftop);
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_andclause
 *
 * Creates an 'and' clause given a list of its subclauses.
 */
Expr *
make_andclause(List *andclauses)
{
	BoolExpr   *expr = makeNode(BoolExpr);

	expr->boolop = AND_EXPR;
	expr->args = andclauses;
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_orclause
 *
 * Creates an 'or' clause given a list of its subclauses.
 */
Expr *
make_orclause(List *orclauses)
{
	BoolExpr   *expr = makeNode(BoolExpr);

	expr->boolop = OR_EXPR;
	expr->args = orclauses;
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_notclause
 *
 * Create a 'not' clause given the expression to be negated.
 */
Expr *
make_notclause(Expr *notclause)
{
	BoolExpr   *expr = makeNode(BoolExpr);

	expr->boolop = NOT_EXPR;
	expr->args = list_make1(notclause);
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_and_qual
 *
 * Variant of make_andclause for ANDing two qual conditions together.
 * Qual conditions have the property that a NULL nodetree is interpreted
 * as 'true'.
 *
 * NB: this makes no attempt to preserve AND/OR flatness; so it should not
 * be used on a qual that has already been run through prepqual.c.
 */
Node *
make_and_qual(Node *qual1, Node *qual2)
{
	if (qual1 == NULL)
		return qual2;
	if (qual2 == NULL)
		return qual1;
	return (Node *) make_andclause(list_make2(qual1, qual2));
}

/*
 * The planner and executor usually represent qualification expressions
 * as lists of boolean expressions with implicit AND semantics.
 *
 * These functions convert between an AND-semantics expression list and the
 * ordinary representation of a boolean expression.
 *
 * Note that an empty list is considered equivalent to TRUE.
 */
Expr *
make_ands_explicit(List *andclauses)
{
	if (andclauses == NIL)
		return (Expr *) makeBoolConst(true, false);
	else if (list_length(andclauses) == 1)
		return (Expr *) linitial(andclauses);
	else
		return make_andclause(andclauses);
}

List *
make_ands_implicit(Expr *clause)
{
	/*
	 * NB: because the parser sets the qual field to NULL in a query that has
	 * no WHERE clause, we must consider a NULL input clause as TRUE, even
	 * though one might more reasonably think it FALSE.
	 */
	if (clause == NULL)
		return NIL;				/* NULL -> NIL list == TRUE */
	else if (is_andclause(clause))
		return ((BoolExpr *) clause)->args;
	else if (IsA(clause, Const) &&
			 !((Const *) clause)->constisnull &&
			 DatumGetBool(((Const *) clause)->constvalue))
		return NIL;				/* constant TRUE input -> NIL list */
	else
		return list_make1(clause);
}

/*
 * makeIndexInfo
 *	  create an IndexInfo node
 */
IndexInfo *
makeIndexInfo(int numattrs, int numkeyattrs, Oid amoid, List *expressions,
			  List *predicates, bool unique, bool nulls_not_distinct,
			  bool isready, bool concurrent, bool summarizing,
			  bool withoutoverlaps)
{
	IndexInfo  *n = makeNode(IndexInfo);

	n->ii_NumIndexAttrs = numattrs;
	n->ii_NumIndexKeyAttrs = numkeyattrs;
	Assert(n->ii_NumIndexKeyAttrs != 0);
	Assert(n->ii_NumIndexKeyAttrs <= n->ii_NumIndexAttrs);
	n->ii_Unique = unique;
	n->ii_NullsNotDistinct = nulls_not_distinct;
	n->ii_ReadyForInserts = isready;
	n->ii_CheckedUnchanged = false;
	n->ii_IndexUnchanged = false;
	n->ii_Concurrent = concurrent;
	n->ii_Summarizing = summarizing;
	n->ii_WithoutOverlaps = withoutoverlaps;

	/* summarizing indexes cannot contain non-key attributes */
	Assert(!summarizing || (numkeyattrs == numattrs));

	/* expressions */
	n->ii_Expressions = expressions;
	n->ii_ExpressionsState = NIL;

	/* predicates  */
	n->ii_Predicate = predicates;
	n->ii_PredicateState = NULL;

	/* exclusion constraints */
	n->ii_ExclusionOps = NULL;
	n->ii_ExclusionProcs = NULL;
	n->ii_ExclusionStrats = NULL;

	/* speculative inserts */
	n->ii_UniqueOps = NULL;
	n->ii_UniqueProcs = NULL;
	n->ii_UniqueStrats = NULL;

	/* initialize index-build state to default */
	n->ii_BrokenHotChain = false;
	n->ii_ParallelWorkers = 0;

	/* set up for possible use by index AM */
	n->ii_Am = amoid;
	n->ii_AmCache = NULL;
	n->ii_Context = CurrentMemoryContext;

	return n;
}

/*
 * makeGroupingSet
 *
 */
GroupingSet *
makeGroupingSet(GroupingSetKind kind, List *content, int location)
{
	GroupingSet *n = makeNode(GroupingSet);

	n->kind = kind;
	n->content = content;
	n->location = location;
	return n;
}

/*
 * makeVacuumRelation -
 *	  create a VacuumRelation node
 */
VacuumRelation *
makeVacuumRelation(RangeVar *relation, Oid oid, List *va_cols)
{
	VacuumRelation *v = makeNode(VacuumRelation);

	v->relation = relation;
	v->oid = oid;
	v->va_cols = va_cols;
	return v;
}

/*
 * makeJsonFormat -
 *	  creates a JsonFormat node
 */
JsonFormat *
makeJsonFormat(JsonFormatType type, JsonEncoding encoding, int location)
{
	JsonFormat *jf = makeNode(JsonFormat);

	jf->format_type = type;
	jf->encoding = encoding;
	jf->location = location;

	return jf;
}

/*
 * makeJsonValueExpr -
 *	  creates a JsonValueExpr node
 */
JsonValueExpr *
makeJsonValueExpr(Expr *raw_expr, Expr *formatted_expr,
				  JsonFormat *format)
{
	JsonValueExpr *jve = makeNode(JsonValueExpr);

	jve->raw_expr = raw_expr;
	jve->formatted_expr = formatted_expr;
	jve->format = format;

	return jve;
}

/*
 * makeJsonBehavior -
 *	  creates a JsonBehavior node
 */
JsonBehavior *
makeJsonBehavior(JsonBehaviorType btype, Node *expr, int location)
{
	JsonBehavior *behavior = makeNode(JsonBehavior);

	behavior->btype = btype;
	behavior->expr = expr;
	behavior->location = location;

	return behavior;
}

/*
 * makeJsonKeyValue -
 *	  creates a JsonKeyValue node
 */
Node *
makeJsonKeyValue(Node *key, Node *value)
{
	JsonKeyValue *n = makeNode(JsonKeyValue);

	n->key = (Expr *) key;
	n->value = castNode(JsonValueExpr, value);

	return (Node *) n;
}

/*
 * makeJsonIsPredicate -
 *	  creates a JsonIsPredicate node
 */
Node *
makeJsonIsPredicate(Node *expr, JsonFormat *format, JsonValueType item_type,
					bool unique_keys, int location)
{
	JsonIsPredicate *n = makeNode(JsonIsPredicate);

	n->expr = expr;
	n->format = format;
	n->item_type = item_type;
	n->unique_keys = unique_keys;
	n->location = location;

	return (Node *) n;
}

/*
 * makeJsonTablePathSpec -
 *		Make JsonTablePathSpec node from given path string and name (if any)
 */
JsonTablePathSpec *
makeJsonTablePathSpec(char *string, char *name, int string_location,
					  int name_location)
{
	JsonTablePathSpec *pathspec = makeNode(JsonTablePathSpec);

	Assert(string != NULL);
	pathspec->string = makeStringConst(string, string_location);
	if (name != NULL)
		pathspec->name = pstrdup(name);

	pathspec->name_location = name_location;
	pathspec->location = string_location;

	return pathspec;
}

/*
 * makeJsonTablePath -
 *		Make JsonTablePath node for given path string and name
 */
JsonTablePath *
makeJsonTablePath(Const *pathvalue, char *pathname)
{
	JsonTablePath *path = makeNode(JsonTablePath);

	Assert(IsA(pathvalue, Const));
	path->value = pathvalue;
	path->name = pathname;

	return path;
}
