/*-------------------------------------------------------------------------
 *
 * funcapi.h
 *	  Definitions for functions which return composite type and/or sets
 *
 * This file must be included by all Postgres modules that either define
 * or call FUNCAPI-callable functions or macros.
 *
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCAPI_H
#define FUNCAPI_H

#include "postgres.h"

#include "fmgr.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/executor.h"
#include "executor/tuptable.h"

/*
 * All functions that can be called directly by fmgr must have this signature.
 * (Other functions can be called by using a handler that does have this
 * signature.)
 */


/*-------------------------------------------------------------------------
 *	Support to ease writing Functions returning composite types
 *-------------------------------------------------------------------------
 *
 * This struct holds arrays of individual attribute information
 * needed to create a tuple from raw C strings. It also requires
 * a copy of the TupleDesc. The information carried here
 * is derived from the TupleDesc, but it is stored here to
 * avoid redundant cpu cycles on each call to an SRF.
 */
typedef struct
{
	/* full TupleDesc */
	TupleDesc	   tupdesc;

	/* pointer to array of attribute "type"in finfo */
	FmgrInfo	   *attinfuncs;

	/* pointer to array of attribute type typelem */
	Oid			   *attelems;

	/* pointer to array of attribute type typtypmod */
	int4		   *atttypmods;

}	AttInMetadata;

/*-------------------------------------------------------------------------
 *		Support struct to ease writing Set Returning Functions (SRFs)
 *-------------------------------------------------------------------------
 * 
 * This struct holds function context for Set Returning Functions.
 * Use fn_extra to hold a pointer to it across calls
 */
typedef struct
{
	/*
	 * Number of times we've been called before.
	 * 
	 * call_cntr is initialized to 0 for you by SRF_FIRSTCALL_INIT(), and
	 * incremented for you every time SRF_RETURN_NEXT() is called.
	 */
	uint32			call_cntr;

	/*
	 * OPTIONAL maximum number of calls
	 *
	 * max_calls is here for convenience ONLY and setting it is OPTIONAL.
	 * If not set, you must provide alternative means to know when the
	 * function is done.
	 */
	uint32			max_calls;

	/*
	 * OPTIONAL pointer to result slot
	 * 
	 * slot is for use when returning tuples (i.e. composite data types)
	 * and is not needed when returning base (i.e. scalar) data types.
	 */
	TupleTableSlot *slot;

	/*
	 * OPTIONAL pointer to misc user provided context info
	 * 
	 * user_fctx is for use as a pointer to your own struct to retain
	 * arbitrary context information between calls for your function.
	 */
	void		   *user_fctx;

	/*
	 * OPTIONAL pointer to struct containing arrays of attribute type input
	 * metainfo
	 * 
	 * attinmeta is for use when returning tuples (i.e. composite data types)
	 * and is not needed when returning base (i.e. scalar) data types. It
	 * is ONLY needed if you intend to use BuildTupleFromCStrings() to create
	 * the return tuple.
	 */
	AttInMetadata	   *attinmeta;

	/*
	 * memory context used to initialize structure
	 *
	 * fmctx is set by SRF_FIRSTCALL_INIT() for you, and used by
	 * SRF_RETURN_DONE() for cleanup. It is primarily for internal use
	 * by the API.
	 */
	MemoryContext	fmctx;

}	FuncCallContext;

/*-------------------------------------------------------------------------
 *	Support to ease writing Functions returning composite types
 *
 * External declarations:
 * TupleDesc RelationNameGetTupleDesc(char *relname) - Use to get a TupleDesc
 *		based on the function's return type relation.
 * TupleDesc TypeGetTupleDesc(Oid typeoid, List *colaliases) - Use to get a
 *		TupleDesc based on the function's type oid. This can be used to get
 *		a TupleDesc for a base (scalar), or composite (relation) type.
 * TupleTableSlot *TupleDescGetSlot(TupleDesc tupdesc) - Initialize a slot
 *		given a TupleDesc.
 * AttInMetadata *TupleDescGetAttInMetadata(TupleDesc tupdesc) - Get a pointer
 *		to AttInMetadata based on the function's TupleDesc. AttInMetadata can
 *		be used in conjunction with C strings to produce a properly formed
 *		tuple. Store the metadata here for use across calls to avoid redundant
 *		work.
 * HeapTuple BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values) -
 *		build a HeapTuple given user data in C string form. values is an array
 *		of C strings, one for each attribute of the return tuple.
 *
 * Macro declarations:
 * TupleGetDatum(TupleTableSlot *slot, HeapTuple tuple) - get a Datum
 *		given a tuple and a slot.
 */

/* from tupdesc.c */
extern TupleDesc RelationNameGetTupleDesc(char *relname);
extern TupleDesc TypeGetTupleDesc(Oid typeoid, List *colaliases);

/* from execTuples.c */
extern TupleTableSlot *TupleDescGetSlot(TupleDesc tupdesc);
extern AttInMetadata *TupleDescGetAttInMetadata(TupleDesc tupdesc);
extern HeapTuple BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values);

/* from funcapi.c */
extern void get_type_metadata(Oid typeid, Oid *attinfuncid, Oid *attelem);

#define TupleGetDatum(_slot, _tuple) \
	PointerGetDatum(ExecStoreTuple(_tuple, _slot, InvalidBuffer, true))

/*-------------------------------------------------------------------------
 *		Support for Set Returning Functions (SRFs)
 *
 * The basic API for SRFs looks something like:
 *
 * Datum
 * my_Set_Returning_Function(PG_FUNCTION_ARGS)
 * {
 * 	FuncCallContext	   *funcctx;
 * 	Datum				result;
 * 	<user defined declarations>
 * 
 * 	if(SRF_IS_FIRSTCALL())
 * 	{
 * 		<user defined code>
 * 		funcctx = SRF_FIRSTCALL_INIT();
 * 		<if returning composite>
 * 			<obtain slot>
 * 			funcctx->slot = slot;
 * 		<endif returning composite>
 * 		<user defined code>
 *  }
 * 	<user defined code>
 * 	funcctx = SRF_PERCALL_SETUP();
 * 	<user defined code>
 * 
 * 	if (funcctx->call_cntr < funcctx->max_calls)
 * 	{
 * 		<user defined code>
 * 		<obtain result Datum>
 * 		SRF_RETURN_NEXT(funcctx, result);
 * 	}
 * 	else
 * 	{
 * 		SRF_RETURN_DONE(funcctx);
 * 	}
 * }
 *
 */

/* from funcapi.c */
extern FuncCallContext *init_MultiFuncCall(PG_FUNCTION_ARGS);
extern FuncCallContext *per_MultiFuncCall(PG_FUNCTION_ARGS);
extern void end_MultiFuncCall(PG_FUNCTION_ARGS, FuncCallContext *funcctx);

#define SRF_IS_FIRSTCALL() (fcinfo->flinfo->fn_extra == NULL)
#define SRF_FIRSTCALL_INIT() init_MultiFuncCall(fcinfo)
#define SRF_PERCALL_SETUP() per_MultiFuncCall(fcinfo)
#define SRF_RETURN_NEXT(_funcctx, _result) \
	do { \
		ReturnSetInfo	   *rsi; \
		_funcctx->call_cntr++; \
		rsi = (ReturnSetInfo *) fcinfo->resultinfo; \
		rsi->isDone = ExprMultipleResult; \
		PG_RETURN_DATUM(_result); \
	} while (0)

#define  SRF_RETURN_DONE(_funcctx) \
	do { \
		ReturnSetInfo	   *rsi; \
		end_MultiFuncCall(fcinfo, _funcctx); \
		rsi = (ReturnSetInfo *) fcinfo->resultinfo; \
		rsi->isDone = ExprEndResult; \
		_funcctx->slot = NULL; \
		PG_RETURN_NULL(); \
	} while (0)

#endif   /* FUNCAPI_H */
