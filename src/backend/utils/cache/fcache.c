/*-------------------------------------------------------------------------
 *
 * fcache.c
 *	  Code for the 'function cache' used in Oper and Func nodes....
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/fcache.c,v 1.31 2000/05/28 17:56:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fcache2.h"
#include "utils/syscache.h"

static Oid	GetDynamicFuncArgType(Var *arg, ExprContext *econtext);
static FunctionCachePtr init_fcache(Oid foid,
									List *argList,
									ExprContext *econtext);

#define FuncArgTypeIsDynamic(arg) \
	(IsA(arg,Var) && ((Var*)arg)->varattno == InvalidAttrNumber)

static Oid
GetDynamicFuncArgType(Var *arg, ExprContext *econtext)
{
	char	   *relname;
	int			rtid;
	HeapTuple	tup;

	Assert(IsA(arg, Var));

	rtid = ((Var *) arg)->varno;
	relname = (char *) getrelname(rtid, econtext->ecxt_range_table);

	tup = SearchSysCacheTuple(TYPENAME,
							  PointerGetDatum(relname),
							  0, 0, 0);
	if (!tup)
		elog(ERROR, "Lookup failed on type tuple for class %s",
			 relname);

	return tup->t_data->t_oid;
}

/*-----------------------------------------------------------------
 *
 * Initialize a 'FunctionCache' struct given the PG_PROC oid.
 *
 *-----------------------------------------------------------------
 */
static FunctionCachePtr
init_fcache(Oid foid,
			List *argList,
			ExprContext *econtext)
{
	HeapTuple	procedureTuple;
	HeapTuple	typeTuple;
	Form_pg_proc procedureStruct;
	Form_pg_type typeStruct;
	FunctionCachePtr retval;
	int			nargs;
	text	   *tmp;
	bool		isNull;

	retval = (FunctionCachePtr) palloc(sizeof(FunctionCache));
	MemSet(retval, 0, sizeof(FunctionCache));

	/* ----------------
	 *	 get the procedure tuple corresponding to the given functionOid
	 * ----------------
	 */
	procedureTuple = SearchSysCacheTuple(PROCOID,
										 ObjectIdGetDatum(foid),
										 0, 0, 0);

	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "init_fcache: Cache lookup failed for procedure %u",
			 foid);

	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/* ----------------
	 *	 get the return type from the procedure tuple
	 * ----------------
	 */
	typeTuple = SearchSysCacheTuple(TYPEOID,
						   ObjectIdGetDatum(procedureStruct->prorettype),
									0, 0, 0);

	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "init_fcache: Cache lookup failed for type %u",
			 procedureStruct->prorettype);

	typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	/* ----------------
	 *	 get the type length and by-value flag from the type tuple
	 * ----------------
	 */
	retval->typlen = typeStruct->typlen;
	if (typeStruct->typrelid == InvalidOid)
	{
		/* The return type is not a relation, so just use byval */
		retval->typbyval = typeStruct->typbyval;
	}
	else
	{

		/*
		 * This is a hack.	We assume here that any function returning a
		 * relation returns it by reference.  This needs to be fixed.
		 */
		retval->typbyval = false;
	}
	retval->foid = foid;
	retval->language = procedureStruct->prolang;
	retval->func_state = (char *) NULL;
	retval->setArg = (Datum) 0;
	retval->hasSetArg = false;
	retval->oneResult = !procedureStruct->proretset;

	/*
	 * If we are returning exactly one result then we have to copy tuples
	 * and by reference results because we have to end the execution
	 * before we return the results.  When you do this everything
	 * allocated by the executor (i.e. slots and tuples) is freed.
	 */
	if ((retval->language == SQLlanguageId) &&
		retval->oneResult &&
		!retval->typbyval)
	{
		Form_pg_class relationStruct;
		HeapTuple	relationTuple;
		TupleDesc	td;
		TupleTableSlot *slot;

		slot = makeNode(TupleTableSlot);
		slot->ttc_shouldFree = true;
		slot->ttc_descIsNew = true;
		slot->ttc_tupleDescriptor = (TupleDesc) NULL;
		slot->ttc_buffer = InvalidBuffer;
		slot->ttc_whichplan = -1;

		relationTuple =
			SearchSysCacheTuple(RELNAME,
								PointerGetDatum(&typeStruct->typname),
								0, 0, 0);

		if (relationTuple)
		{
			relationStruct = (Form_pg_class) GETSTRUCT(relationTuple);
			td = CreateTemplateTupleDesc(relationStruct->relnatts);
		}
		else
			td = CreateTemplateTupleDesc(1);

		slot->ttc_tupleDescriptor = td;

		retval->funcSlot = (Pointer) slot;
	}
	else
		retval->funcSlot = (Pointer) NULL;

	nargs = procedureStruct->pronargs;
	retval->nargs = nargs;

	if (nargs > 0)
	{
		Oid		   *argTypes;

		if (retval->language == SQLlanguageId)
		{
			int			i;
			List	   *oneArg;

			retval->argOidVect = (Oid *) palloc(retval->nargs * sizeof(Oid));
			argTypes = procedureStruct->proargtypes;
			memmove(retval->argOidVect,
					argTypes,
					(retval->nargs) * sizeof(Oid));

			for (i = 0;
				 argList;
				 i++, argList = lnext(argList))
			{
				oneArg = lfirst(argList);
				if (FuncArgTypeIsDynamic(oneArg))
					retval->argOidVect[i] = GetDynamicFuncArgType((Var *) oneArg,
															   econtext);
			}
		}
		else
			retval->argOidVect = (Oid *) NULL;
	}
	else
	{
		retval->argOidVect = (Oid *) NULL;
	}

	if (procedureStruct->prolang == SQLlanguageId)
	{
		tmp = (text *) SysCacheGetAttr(PROCOID,
									   procedureTuple,
									   Anum_pg_proc_prosrc,
									   &isNull);
		if (isNull)
			elog(ERROR, "init_fcache: null prosrc for procedure %u",
				 foid);
		retval->src = textout(tmp);
		retval->bin = (char *) NULL;
	}
	else
	{
		retval->src = (char *) NULL;
		if (procedureStruct->proistrusted)
			retval->bin = (char *) NULL;
		else
		{
			tmp = (text *) SysCacheGetAttr(PROCOID,
										   procedureTuple,
										   Anum_pg_proc_probin,
										   &isNull);
			if (isNull)
				elog(ERROR, "init_fcache: null probin for procedure %u",
					 foid);
			retval->bin = textout(tmp);
		}
	}

	if (retval->language != SQLlanguageId)
	{
		fmgr_info(foid, &(retval->func));
		retval->nargs = retval->func.fn_nargs;
	}
	else
		retval->func.fn_addr = (PGFunction) NULL;

	return retval;
}

void
setFcache(Node *node, Oid foid, List *argList, ExprContext *econtext)
{
	Func	   *fnode;
	Oper	   *onode;
	FunctionCachePtr fcache;

	fcache = init_fcache(foid, argList, econtext);

	if (IsA(node, Oper))
	{
		onode = (Oper *) node;
		onode->op_fcache = fcache;
	}
	else if (IsA(node, Func))
	{
		fnode = (Func *) node;
		fnode->func_fcache = fcache;
	}
	else
		elog(ERROR, "init_fcache: node must be Oper or Func!");
}
