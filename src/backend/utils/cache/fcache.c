/*-------------------------------------------------------------------------
 *
 * fcache.c--
 *	  Code for the 'function cache' used in Oper and Func nodes....
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/fcache.c,v 1.14 1998/07/26 04:30:55 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <nodes/parsenodes.h>
#include <fmgr.h>

#include "access/htup.h"
#include "utils/catcache.h"
#include "utils/syscache.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#ifdef MULTIBYTE
#include "catalog/pg_class_mb.h"
#else
#include "catalog/pg_class.h"
#endif
#include "parser/parsetree.h"	/* for getrelname() */
#include "utils/builtins.h"
#include "utils/fcache.h"
#include "utils/fcache2.h"
#include "nodes/primnodes.h"
#include "nodes/execnodes.h"
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static Oid	GetDynamicFuncArgType(Var *arg, ExprContext *econtext);
static FunctionCachePtr
init_fcache(Oid foid,
			bool use_syscache,
			List *argList,
			ExprContext *econtext);

/*-----------------------------------------------------------------
 *
 * Initialize the 'FunctionCache' given the PG_PROC oid.
 *
 *
 * NOTE:  This function can be called when the system cache is being
 *		  initialized.	Therefore, use_syscache should ONLY be true
 *		  when the function return type is interesting (ie: set_fcache).
 *-----------------------------------------------------------------
 */
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


	tup = SearchSysCacheTuple(TYPNAME, PointerGetDatum(relname),
							  0, 0, 0);
	if (!tup)
		elog(ERROR, "Lookup failed on type tuple for class %s",
			 relname);

	return tup->t_oid;
}

static FunctionCachePtr
init_fcache(Oid foid,
			bool use_syscache,
			List *argList,
			ExprContext *econtext)
{
	HeapTuple	procedureTuple;
	HeapTuple	typeTuple;
	Form_pg_proc procedureStruct;
	TypeTupleForm typeStruct;
	FunctionCachePtr retval;
	text	   *tmp;
	int			nargs;

	/* ----------------
	 *	 get the procedure tuple corresponding to the given
	 *	 functionOid.  If this fails, returnValue has been
	 *	 pre-initialized to "null" so we just return it.
	 * ----------------
	 */
	retval = (FunctionCachePtr) palloc(sizeof(FunctionCache));
	memset(retval, 0, sizeof(FunctionCache));

	if (!use_syscache)
		elog(ERROR, "what the ????, init the fcache without the catalogs?");

	procedureTuple = SearchSysCacheTuple(PROOID,
										 ObjectIdGetDatum(foid),
										 0, 0, 0);

	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR,
			 "init_fcache: %s %d",
			 "Cache lookup failed for procedure", foid);

	/* ----------------
	 *	 get the return type from the procedure tuple
	 * ----------------
	 */
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/* ----------------
	 *	 get the type tuple corresponding to the return type
	 *	 If this fails, returnValue has been pre-initialized
	 *	 to "null" so we just return it.
	 * ----------------
	 */
	typeTuple = SearchSysCacheTuple(TYPOID,
						   ObjectIdGetDatum(procedureStruct->prorettype),
									0, 0, 0);

	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR,
			 "init_fcache: %s %d",
			 "Cache lookup failed for type",
			 (procedureStruct)->prorettype);

	/* ----------------
	 *	 get the type length and by-value from the type tuple and
	 *	 save the information in our one element cache.
	 * ----------------
	 */
	typeStruct = (TypeTupleForm) GETSTRUCT(typeTuple);

	retval->typlen = (typeStruct)->typlen;
	if ((typeStruct)->typrelid == InvalidOid)
	{
		/* The return type is not a relation, so just use byval */
		retval->typbyval = (typeStruct)->typbyval ? true : false;
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
	retval->setArg = NULL;
	retval->hasSetArg = false;
	retval->oneResult = !procedureStruct->proretset;
	retval->istrusted = procedureStruct->proistrusted;

	/*
	 * If we are returning exactly one result then we have to copy tuples
	 * and by reference results because we have to end the execution
	 * before we return the results.  When you do this everything
	 * allocated by the executor (i.e. slots and tuples) is freed.
	 */
	if ((retval->language == SQLlanguageId) &&
		(retval->oneResult) &&
		!(retval->typbyval))
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
		retval->funcSlot = (Pointer) slot;

		relationTuple = (HeapTuple)
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

		((TupleTableSlot *) retval->funcSlot)->ttc_tupleDescriptor = td;
	}
	else
		retval->funcSlot = (char *) NULL;

	nargs = procedureStruct->pronargs;
	retval->nargs = nargs;

	if (nargs > 0)
	{
		Oid		   *argTypes;

		retval->nullVect = (bool *) palloc((retval->nargs) * sizeof(bool));

		if (retval->language == SQLlanguageId)
		{
			int			i;
			List	   *oneArg;

			retval->argOidVect =
				(Oid *) palloc(retval->nargs * sizeof(Oid));
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
		retval->nullVect = (BoolPtr) NULL;
	}

	/*
	 * XXX this is the first varlena in the struct.  If the order changes
	 * for some reason this will fail.
	 */
	if (procedureStruct->prolang == SQLlanguageId)
	{
		retval->src = textout(&(procedureStruct->prosrc));
		retval->bin = (char *) NULL;
	}
	else
	{

		/*
		 * I'm not sure that we even need to do this at all.
		 */

		/*
		 * We do for untrusted functions.
		 */

		if (procedureStruct->proistrusted)
			retval->bin = (char *) NULL;
		else
		{
			tmp = (text *)
				SearchSysCacheGetAttribute(PROOID,
										   Anum_pg_proc_probin,
										   ObjectIdGetDatum(foid),
										   0, 0, 0);
			retval->bin = textout(tmp);
		}
		retval->src = (char *) NULL;
	}




	if (retval->language != SQLlanguageId)
	{
		fmgr_info(foid, &(retval->func));
		retval->nargs = retval->func.fn_nargs;
	}
	else
		retval->func.fn_addr = (func_ptr) NULL;


	return (retval);
}

void
setFcache(Node *node, Oid foid, List *argList, ExprContext *econtext)
{
	Func	   *fnode;
	Oper	   *onode;
	FunctionCachePtr fcache;

	fcache = init_fcache(foid, true, argList, econtext);

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
