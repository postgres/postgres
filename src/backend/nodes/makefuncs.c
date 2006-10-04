/*-------------------------------------------------------------------------
 *
 * makefuncs.c
 *	  creator functions for primitive nodes. The functions here are for
 *	  the most frequently created nodes.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/makefuncs.c,v 1.52 2006/10/04 00:29:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
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
makeSimpleA_Expr(A_Expr_Kind kind, const char *name,
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
makeVar(Index varno,
		AttrNumber varattno,
		Oid vartype,
		int32 vartypmod,
		Index varlevelsup)
{
	Var		   *var = makeNode(Var);

	var->varno = varno;
	var->varattno = varattno;
	var->vartype = vartype;
	var->vartypmod = vartypmod;
	var->varlevelsup = varlevelsup;

	/*
	 * Since few if any routines ever create Var nodes with varnoold/varoattno
	 * different from varno/varattno, we don't provide separate arguments for
	 * them, but just initialize them to the given varno/varattno. This
	 * reduces code clutter and chance of error for most callers.
	 */
	var->varnoold = varno;
	var->varoattno = varattno;

	return var;
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
 * makeConst -
 *	  creates a Const node
 */
Const *
makeConst(Oid consttype,
		  int constlen,
		  Datum constvalue,
		  bool constisnull,
		  bool constbyval)
{
	Const	   *cnst = makeNode(Const);

	cnst->consttype = consttype;
	cnst->constlen = constlen;
	cnst->constvalue = constvalue;
	cnst->constisnull = constisnull;
	cnst->constbyval = constbyval;

	return cnst;
}

/*
 * makeNullConst -
 *	  creates a Const node representing a NULL of the specified type
 */
Const *
makeNullConst(Oid consttype)
{
	int16		typLen;
	bool		typByVal;

	get_typlenbyval(consttype, &typLen, &typByVal);
	return makeConst(consttype,
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
	return (Node *) makeConst(BOOLOID, 1, BoolGetDatum(value), isnull, true);
}

/*
 * makeBoolExpr -
 *	  creates a BoolExpr node
 */
Expr *
makeBoolExpr(BoolExprType boolop, List *args)
{
	BoolExpr   *b = makeNode(BoolExpr);

	b->boolop = boolop;
	b->args = args;

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
makeRelabelType(Expr *arg, Oid rtype, int32 rtypmod, CoercionForm rformat)
{
	RelabelType *r = makeNode(RelabelType);

	r->arg = arg;
	r->resulttype = rtype;
	r->resulttypmod = rtypmod;
	r->relabelformat = rformat;

	return r;
}

/*
 * makeRangeVar -
 *	  creates a RangeVar node (rather oversimplified case)
 */
RangeVar *
makeRangeVar(char *schemaname, char *relname)
{
	RangeVar   *r = makeNode(RangeVar);

	r->catalogname = NULL;
	r->schemaname = schemaname;
	r->relname = relname;
	r->inhOpt = INH_DEFAULT;
	r->istemp = false;
	r->alias = NULL;

	return r;
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
	TypeName   *n = makeNode(TypeName);

	n->names = list_make1(makeString(typnam));
	n->typmod = -1;
	n->location = -1;
	return n;
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
	n->typmod = -1;
	n->location = -1;
	return n;
}

/*
 * makeTypeNameFromOid -
 *	build a TypeName node to represent a type already known by OID.
 */
TypeName *
makeTypeNameFromOid(Oid typeid, int32 typmod)
{
	TypeName   *n = makeNode(TypeName);

	n->typeid = typeid;
	n->typmod = typmod;
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
makeFuncExpr(Oid funcid, Oid rettype, List *args, CoercionForm fformat)
{
	FuncExpr   *funcexpr;

	funcexpr = makeNode(FuncExpr);
	funcexpr->funcid = funcid;
	funcexpr->funcresulttype = rettype;
	funcexpr->funcretset = false;		/* only allowed case here */
	funcexpr->funcformat = fformat;
	funcexpr->args = args;

	return funcexpr;
}

/*
 * makeDefElem -
 *	build a DefElem node
 */
DefElem *
makeDefElem(char *name, Node *arg)
{
	DefElem    *res = makeNode(DefElem);

	res->defname = name;
	res->arg = arg;
	return res;
}
