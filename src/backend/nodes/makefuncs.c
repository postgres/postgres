/*
 * makefuncs.c
 *	  creator functions for primitive nodes. The functions here are for
 *	  the most frequently created nodes.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/makefuncs.c,v 1.35 2002/09/18 21:35:21 tgl Exp $
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"


/*
 * makeA_Expr -
 *		makes an A_Expr node
 */
A_Expr *
makeA_Expr(int oper, List *name, Node *lexpr, Node *rexpr)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->oper = oper;
	a->name = name;
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	return a;
}

/*
 * makeSimpleA_Expr -
 *		As above, given a simple (unqualified) operator name
 */
A_Expr *
makeSimpleA_Expr(int oper, const char *name,
				 Node *lexpr, Node *rexpr)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->oper = oper;
	a->name = makeList1(makeString((char *) name));
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	return a;
}

/*
 * makeOper -
 *	  creates an Oper node
 */
Oper *
makeOper(Oid opno,
		 Oid opid,
		 Oid opresulttype,
		 bool opretset)
{
	Oper	   *oper = makeNode(Oper);

	oper->opno = opno;
	oper->opid = opid;
	oper->opresulttype = opresulttype;
	oper->opretset = opretset;
	oper->op_fcache = NULL;
	return oper;
}

/*
 * makeVar -
 *	  creates a Var node
 *
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
	 * Since few if any routines ever create Var nodes with
	 * varnoold/varoattno different from varno/varattno, we don't provide
	 * separate arguments for them, but just initialize them to the given
	 * varno/varattno. This reduces code clutter and chance of error for
	 * most callers.
	 */
	var->varnoold = varno;
	var->varoattno = varattno;

	return var;
}

/*
 * makeTargetEntry -
 *	  creates a TargetEntry node(contains a Resdom)
 */
TargetEntry *
makeTargetEntry(Resdom *resdom, Node *expr)
{
	TargetEntry *rt = makeNode(TargetEntry);

	rt->resdom = resdom;
	rt->expr = expr;
	return rt;
}

/*
 * makeResdom -
 *	  creates a Resdom (Result Domain) node
 */
Resdom *
makeResdom(AttrNumber resno,
		   Oid restype,
		   int32 restypmod,
		   char *resname,
		   bool resjunk)
{
	Resdom	   *resdom = makeNode(Resdom);

	resdom->resno = resno;
	resdom->restype = restype;
	resdom->restypmod = restypmod;
	resdom->resname = resname;

	/*
	 * We always set the sorting/grouping fields to 0.	If the caller
	 * wants to change them he must do so explicitly.  Few if any callers
	 * should be doing that, so omitting these arguments reduces the
	 * chance of error.
	 */
	resdom->ressortgroupref = 0;
	resdom->reskey = 0;
	resdom->reskeyop = InvalidOid;

	resdom->resjunk = resjunk;
	return resdom;
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
		  bool constbyval,
		  bool constisset,
		  bool constiscast)
{
	Const	   *cnst = makeNode(Const);

	cnst->consttype = consttype;
	cnst->constlen = constlen;
	cnst->constvalue = constvalue;
	cnst->constisnull = constisnull;
	cnst->constbyval = constbyval;
	cnst->constisset = constisset;
	cnst->constiscast = constiscast;
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
					 typByVal,
					 false,
					 false);
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
makeRelabelType(Node *arg, Oid rtype, int32 rtypmod, CoercionForm rformat)
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
 */
TypeName *
makeTypeName(char *typnam)
{
	TypeName   *n = makeNode(TypeName);

	n->names = makeList1(makeString(typnam));
	n->typmod = -1;
	return n;
}
