/*
 * makefuncs.c--
 *	  creator functions for primitive nodes. The functions here are for
 *	  the most frequently created nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/makefuncs.c,v 1.3 1997/09/07 04:42:48 momjian Exp $
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
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/makefuncs.h"

/*
 * makeOper -
 *	  creates an Oper node
 */
Oper		   *
makeOper(Oid opno,
		 Oid opid,
		 Oid opresulttype,
		 int opsize,
		 FunctionCachePtr op_fcache)
{
	Oper		   *oper = makeNode(Oper);

	oper->opno = opno;
	oper->opid = opid;
	oper->opresulttype = opresulttype;
	oper->opsize = opsize;
	oper->op_fcache = op_fcache;
	return oper;
}

/*
 * makeVar -
 *	  creates a Var node
 *
 */
Var			   *
makeVar(Index varno,
		AttrNumber varattno,
		Oid vartype,
		Index varnoold,
		AttrNumber varoattno)
{
	Var			   *var = makeNode(Var);

	var->varno = varno;
	var->varattno = varattno;
	var->vartype = vartype;
	var->varnoold = varnoold;
	var->varoattno = varoattno;

	return var;
}

/*
 * makeResdom -
 *	  creates a Resdom (Result Domain) node
 */
Resdom		   *
makeResdom(AttrNumber resno,
		   Oid restype,
		   int reslen,
		   char *resname,
		   Index reskey,
		   Oid reskeyop,
		   int resjunk)
{
	Resdom		   *resdom = makeNode(Resdom);

	resdom->resno = resno;
	resdom->restype = restype;
	resdom->reslen = reslen;
	resdom->resname = resname;
	resdom->reskey = reskey;
	resdom->reskeyop = reskeyop;
	resdom->resjunk = resjunk;
	return resdom;
}

/*
 * makeConst -
 *	  creates a Const node
 */
Const		   *
makeConst(Oid consttype,
		  Size constlen,
		  Datum constvalue,
		  bool constisnull,
		  bool constbyval,
		  bool constisset,
		  bool constiscast)
{
	Const		   *cnst = makeNode(Const);

	cnst->consttype = consttype;
	cnst->constlen = constlen;
	cnst->constvalue = constvalue;
	cnst->constisnull = constisnull;
	cnst->constbyval = constbyval;
	cnst->constisset = constisset;
	cnst->constiscast = constiscast;
	return cnst;
}
