/*-------------------------------------------------------------------------
 *
 * fmgr.h
 *    Definitions for the Postgres function manager and function-call
 *    interface.
 *
 * This file must be included by all Postgres modules that either define
 * or call fmgr-callable functions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fmgr.h,v 1.5 2000/06/13 07:35:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	FMGR_H
#define FMGR_H


/*
 * All functions that can be called directly by fmgr must have this signature.
 * (Other functions can be called by using a handler that does have this
 * signature.)
 */

typedef struct FunctionCallInfoData    *FunctionCallInfo;

typedef Datum (*PGFunction) (FunctionCallInfo fcinfo);

/*
 * This struct holds the system-catalog information that must be looked up
 * before a function can be called through fmgr.  If the same function is
 * to be called multiple times, the lookup need be done only once and the
 * info struct saved for re-use.
 */
typedef struct
{
    PGFunction  fn_addr;    /* pointer to function or handler to be called */
    Oid         fn_oid;     /* OID of function (NOT of handler, if any) */
    short       fn_nargs;   /* 0..FUNC_MAX_ARGS, or -1 if variable arg count */
    bool        fn_strict;  /* function is "strict" (NULL in => NULL out) */
    void       *fn_extra;   /* extra space for use by handler */
} FmgrInfo;

/*
 * This struct is the data actually passed to an fmgr-called function.
 */
typedef struct FunctionCallInfoData
{
    FmgrInfo   *flinfo;			/* ptr to lookup info used for this call */
    struct Node *context;		/* pass info about context of call */
    struct Node *resultinfo;	/* pass or return extra info about result */
    bool        isnull;         /* function must set true if result is NULL */
	short		nargs;          /* # arguments actually passed */
    Datum       arg[FUNC_MAX_ARGS];	/* Arguments passed to function */
    bool        argnull[FUNC_MAX_ARGS];	/* T if arg[i] is actually NULL */
} FunctionCallInfoData;

/*
 * This routine fills a FmgrInfo struct, given the OID
 * of the function to be called.
 */
extern void fmgr_info(Oid functionId, FmgrInfo *finfo);

/*
 * This macro invokes a function given a filled-in FunctionCallInfoData
 * struct.  The macro result is the returned Datum --- but note that
 * caller must still check fcinfo->isnull!  Also, if function is strict,
 * it is caller's responsibility to verify that no null arguments are present
 * before calling.
 */
#define FunctionCallInvoke(fcinfo)  ((* (fcinfo)->flinfo->fn_addr) (fcinfo))


/*-------------------------------------------------------------------------
 *		Support macros to ease writing fmgr-compatible functions
 *
 * A C-coded fmgr-compatible function should be declared as
 *
 *		Datum
 *		function_name(PG_FUNCTION_ARGS)
 *		{
 *			...
 *		}
 *
 * It should access its arguments using appropriate PG_GETARG_xxx macros
 * and should return its result using PG_RETURN_xxx.
 *
 *-------------------------------------------------------------------------
 */

/* Standard parameter list for fmgr-compatible functions */
#define PG_FUNCTION_ARGS	FunctionCallInfo fcinfo

/* If function is not marked "proisstrict" in pg_proc, it must check for
 * null arguments using this macro.  Do not try to GETARG a null argument!
 */
#define PG_ARGISNULL(n)  (fcinfo->argnull[n])

#if 1
/* VERY TEMPORARY until some TOAST support is committed ... */
#define PG_DETOAST_DATUM(datum)  \
	 ((struct varlena *) DatumGetPointer(datum))
#else
/* Eventually it will look more like this... */
#define PG_DETOAST_DATUM(datum)  \
	(VARATT_IS_EXTENDED(DatumGetPointer(datum)) ?  \
	 (struct varlena *) heap_tuple_untoast_attr((varattrib *) DatumGetPointer(datum)) :  \
	 (struct varlena *) DatumGetPointer(datum))
#endif

/* Macros for fetching arguments of standard types */

#define PG_GETARG_DATUM(n)   (fcinfo->arg[n])
#define PG_GETARG_INT32(n)   DatumGetInt32(PG_GETARG_DATUM(n))
#define PG_GETARG_UINT32(n)  DatumGetUInt32(PG_GETARG_DATUM(n))
#define PG_GETARG_INT16(n)   DatumGetInt16(PG_GETARG_DATUM(n))
#define PG_GETARG_UINT16(n)  DatumGetUInt16(PG_GETARG_DATUM(n))
#define PG_GETARG_CHAR(n)    DatumGetChar(PG_GETARG_DATUM(n))
#define PG_GETARG_BOOL(n)    DatumGetBool(PG_GETARG_DATUM(n))
#define PG_GETARG_OID(n)     DatumGetObjectId(PG_GETARG_DATUM(n))
#define PG_GETARG_POINTER(n) DatumGetPointer(PG_GETARG_DATUM(n))
#define PG_GETARG_CSTRING(n) DatumGetCString(PG_GETARG_DATUM(n))
#define PG_GETARG_NAME(n)    DatumGetName(PG_GETARG_DATUM(n))
/* these macros hide the pass-by-reference-ness of the datatype: */
#define PG_GETARG_FLOAT4(n)  DatumGetFloat4(PG_GETARG_DATUM(n))
#define PG_GETARG_FLOAT8(n)  DatumGetFloat8(PG_GETARG_DATUM(n))
#define PG_GETARG_INT64(n)   DatumGetInt64(PG_GETARG_DATUM(n))
/* use this if you want the raw, possibly-toasted input datum: */
#define PG_GETARG_RAW_VARLENA_P(n)  ((struct varlena *) PG_GETARG_POINTER(n))
/* use this if you want the input datum de-toasted: */
#define PG_GETARG_VARLENA_P(n) PG_DETOAST_DATUM(PG_GETARG_DATUM(n))
/* DatumGetFoo macros for varlena types will typically look like this: */
#define DatumGetByteaP(X)    ((bytea *) PG_DETOAST_DATUM(X))
#define DatumGetTextP(X)     ((text *) PG_DETOAST_DATUM(X))
#define DatumGetBpCharP(X)   ((BpChar *) PG_DETOAST_DATUM(X))
#define DatumGetVarCharP(X)  ((VarChar *) PG_DETOAST_DATUM(X))
/* GETARG macros for varlena types will typically look like this: */
#define PG_GETARG_BYTEA_P(n)   DatumGetByteaP(PG_GETARG_DATUM(n))
#define PG_GETARG_TEXT_P(n)    DatumGetTextP(PG_GETARG_DATUM(n))
#define PG_GETARG_BPCHAR_P(n)  DatumGetBpCharP(PG_GETARG_DATUM(n))
#define PG_GETARG_VARCHAR_P(n) DatumGetVarCharP(PG_GETARG_DATUM(n))

/* To return a NULL do this: */
#define PG_RETURN_NULL()  \
	do { fcinfo->isnull = true; return (Datum) 0; } while (0)

/* Macros for returning results of standard types */

#define PG_RETURN_INT32(x)   return Int32GetDatum(x)
#define PG_RETURN_UINT32(x)  return UInt32GetDatum(x)
#define PG_RETURN_INT16(x)   return Int16GetDatum(x)
#define PG_RETURN_CHAR(x)    return CharGetDatum(x)
#define PG_RETURN_BOOL(x)    return BoolGetDatum(x)
#define PG_RETURN_OID(x)     return ObjectIdGetDatum(x)
#define PG_RETURN_POINTER(x) return PointerGetDatum(x)
#define PG_RETURN_CSTRING(x) return CStringGetDatum(x)
#define PG_RETURN_NAME(x)    return NameGetDatum(x)
/* these macros hide the pass-by-reference-ness of the datatype: */
#define PG_RETURN_FLOAT4(x)  return Float4GetDatum(x)
#define PG_RETURN_FLOAT8(x)  return Float8GetDatum(x)
#define PG_RETURN_INT64(x)   return Int64GetDatum(x)
/* RETURN macros for other pass-by-ref types will typically look like this: */
#define PG_RETURN_BYTEA_P(x)   PG_RETURN_POINTER(x)
#define PG_RETURN_TEXT_P(x)    PG_RETURN_POINTER(x)
#define PG_RETURN_BPCHAR_P(x)  PG_RETURN_POINTER(x)
#define PG_RETURN_VARCHAR_P(x) PG_RETURN_POINTER(x)


/*-------------------------------------------------------------------------
 *		Support routines and macros for callers of fmgr-compatible functions
 *-------------------------------------------------------------------------
 */

/* These are for invocation of a specifically named function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
extern Datum DirectFunctionCall1(PGFunction func, Datum arg1);
extern Datum DirectFunctionCall2(PGFunction func, Datum arg1, Datum arg2);
extern Datum DirectFunctionCall3(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3);
extern Datum DirectFunctionCall4(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3, Datum arg4);
extern Datum DirectFunctionCall5(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3, Datum arg4, Datum arg5);
extern Datum DirectFunctionCall6(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3, Datum arg4, Datum arg5,
								 Datum arg6);
extern Datum DirectFunctionCall7(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3, Datum arg4, Datum arg5,
								 Datum arg6, Datum arg7);
extern Datum DirectFunctionCall8(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3, Datum arg4, Datum arg5,
								 Datum arg6, Datum arg7, Datum arg8);
extern Datum DirectFunctionCall9(PGFunction func, Datum arg1, Datum arg2,
								 Datum arg3, Datum arg4, Datum arg5,
								 Datum arg6, Datum arg7, Datum arg8,
								 Datum arg9);

/* These are for invocation of a previously-looked-up function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
extern Datum FunctionCall1(FmgrInfo *flinfo, Datum arg1);
extern Datum FunctionCall2(FmgrInfo *flinfo, Datum arg1, Datum arg2);
extern Datum FunctionCall3(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3);
extern Datum FunctionCall4(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3, Datum arg4);
extern Datum FunctionCall5(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3, Datum arg4, Datum arg5);
extern Datum FunctionCall6(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3, Datum arg4, Datum arg5,
						   Datum arg6);
extern Datum FunctionCall7(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3, Datum arg4, Datum arg5,
						   Datum arg6, Datum arg7);
extern Datum FunctionCall8(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3, Datum arg4, Datum arg5,
						   Datum arg6, Datum arg7, Datum arg8);
extern Datum FunctionCall9(FmgrInfo *flinfo, Datum arg1, Datum arg2,
						   Datum arg3, Datum arg4, Datum arg5,
						   Datum arg6, Datum arg7, Datum arg8,
						   Datum arg9);

/* These are for invocation of a function identified by OID with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  These are essentially FunctionLookup() followed
 * by FunctionCallN().  If the same function is to be invoked repeatedly,
 * do the FunctionLookup() once and then use FunctionCallN().
 */
extern Datum OidFunctionCall1(Oid functionId, Datum arg1);
extern Datum OidFunctionCall2(Oid functionId, Datum arg1, Datum arg2);
extern Datum OidFunctionCall3(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3);
extern Datum OidFunctionCall4(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3, Datum arg4);
extern Datum OidFunctionCall5(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3, Datum arg4, Datum arg5);
extern Datum OidFunctionCall6(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3, Datum arg4, Datum arg5,
							  Datum arg6);
extern Datum OidFunctionCall7(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3, Datum arg4, Datum arg5,
							  Datum arg6, Datum arg7);
extern Datum OidFunctionCall8(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3, Datum arg4, Datum arg5,
							  Datum arg6, Datum arg7, Datum arg8);
extern Datum OidFunctionCall9(Oid functionId, Datum arg1, Datum arg2,
							  Datum arg3, Datum arg4, Datum arg5,
							  Datum arg6, Datum arg7, Datum arg8,
							  Datum arg9);


/*
 * Routines in fmgr.c
 */
extern Oid fmgr_internal_language(const char *proname);

/*
 * Routines in dfmgr.c
 */
extern PGFunction fmgr_dynamic(Oid functionId);
extern PGFunction load_external_function(char *filename, char *funcname);
extern void load_file(char *filename);


/*
 * !!! OLD INTERFACE !!!
 *
 * fmgr() is the only remaining vestige of the old-style caller support
 * functions.  It's no longer used anywhere in the Postgres distribution,
 * but we should leave it around for a release or two to ease the transition
 * for user-supplied C functions.  OidFunctionCallN() replaces it for new
 * code.
 */

/*
 * DEPRECATED, DO NOT USE IN NEW CODE
 */
extern char *fmgr(Oid procedureId, ... );

#endif	/* FMGR_H */
