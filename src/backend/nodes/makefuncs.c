/*
 * makefuncs.c
 *	  creator functions for primitive nodes. The functions here are for
 *	  the most frequently created nodes.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/makefuncs.c,v 1.24 2001/01/17 17:26:44 momjian Exp $
 *
 * NOTES
 *	  Creator functions in POSTGRES 4.2 are generated automatically. Most of
 *	  them are rarely used. Now we don't generate them any more. If you want
 *	  one, you have to write it yourself.
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Oct 20, 1994	file creation
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"


/*
 * makeOper -
 *	  creates an Oper node
 */
Oper *
makeOper(Oid opno,
		 Oid opid,
		 Oid opresulttype)
{
	Oper	   *oper = makeNode(Oper);

	oper->opno = opno;
	oper->opid = opid;
	oper->opresulttype = opresulttype;
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
	 * We always set the sorting/grouping fields to 0.  If the caller wants
	 * to change them he must do so explicitly.  Few if any callers should
	 * be doing that, so omitting these arguments reduces the chance of error.
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
 * makeAttr -
 *	  creates an Attr node
 */
Attr *
makeAttr(char *relname, char *attname)
{
	Attr	   *a = makeNode(Attr);

	a->relname = pstrdup(relname);
	a->paramNo = NULL;
	if (attname != NULL)
		a->attrs = makeList1(makeString(pstrdup(attname)));
	a->indirection = NULL;

	return a;
}
