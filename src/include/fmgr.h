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
 * $Id: fmgr.h,v 1.2 2000/05/29 01:59:09 tgl Exp $
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

/* Macros for fetching arguments of standard types */

#define PG_GETARG_INT32(n)   DatumGetInt32(fcinfo->arg[n])
#define PG_GETARG_INT16(n)   DatumGetInt16(fcinfo->arg[n])
#define PG_GETARG_CHAR(n)    DatumGetChar(fcinfo->arg[n])
#define PG_GETARG_BOOL(n)    DatumGetBool(fcinfo->arg[n])
#define PG_GETARG_OID(n)     DatumGetObjectId(fcinfo->arg[n])
#define PG_GETARG_POINTER(n) DatumGetPointer(fcinfo->arg[n])
#define PG_GETARG_NAME(n)    DatumGetName(fcinfo->arg[n])
/* these macros hide the pass-by-reference-ness of the datatype: */
#define PG_GETARG_FLOAT4(n)  DatumGetFloat4(fcinfo->arg[n])
#define PG_GETARG_FLOAT8(n)  DatumGetFloat8(fcinfo->arg[n])
#define PG_GETARG_INT64(n)   DatumGetInt64(fcinfo->arg[n])
/* use this if you want the raw, possibly-toasted input datum: */
#define PG_GETARG_RAW_VARLENA_P(n)  ((struct varlena *) PG_GETARG_POINTER(n))
/* use this if you want the input datum de-toasted: */
#define PG_GETARG_VARLENA_P(n)  \
	(VARATT_IS_EXTENDED(PG_GETARG_RAW_VARLENA_P(n)) ?  \
	 (struct varlena *) heap_tuple_untoast_attr((varattrib *) PG_GETARG_RAW_VARLENA_P(n)) :  \
	 PG_GETARG_RAW_VARLENA_P(n))
/* GETARG macros for varlena types will typically look like this: */
#define PG_GETARG_TEXT_P(n) ((text *) PG_GETARG_VARLENA_P(n))

/* To return a NULL do this: */
#define PG_RETURN_NULL()  \
	do { fcinfo->isnull = true; return (Datum) 0; } while (0)

/* Macros for returning results of standard types */

#define PG_RETURN_INT32(x)   return Int32GetDatum(x)
#define PG_RETURN_INT16(x)   return Int16GetDatum(x)
#define PG_RETURN_CHAR(x)    return CharGetDatum(x)
#define PG_RETURN_BOOL(x)    return BoolGetDatum(x)
#define PG_RETURN_OID(x)     return ObjectIdGetDatum(x)
#define PG_RETURN_POINTER(x) return PointerGetDatum(x)
#define PG_RETURN_NAME(x)    return NameGetDatum(x)
/* these macros hide the pass-by-reference-ness of the datatype: */
#define PG_RETURN_FLOAT4(x)  return Float4GetDatum(x)
#define PG_RETURN_FLOAT8(x)  return Float8GetDatum(x)
#define PG_RETURN_INT64(x)   return Int64GetDatum(x)
/* RETURN macros for other pass-by-ref types will typically look like this: */
#define PG_RETURN_TEXT_P(x)  PG_RETURN_POINTER(x)


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


/*-------------------------------------------------------------------------
 *
 * !!! OLD INTERFACE !!!
 *
 * All the definitions below here are associated with the old fmgr API.
 * They will go away as soon as we have converted all call points to use
 * the new API.  Note that old-style callee functions do not depend on
 * these definitions, so we don't need to have converted all of them before
 * dropping the old API ... just all the old-style call points.
 *
 *-------------------------------------------------------------------------
 */

/* ptr to func returning (char *) */
#if defined(__mc68000__) && defined(__ELF__)
/* The m68k SVR4 ABI defines that pointers are returned in %a0 instead of
 * %d0. So if a function pointer is declared to return a pointer, the
 * compiler may look only into %a0, but if the called function was declared
 * to return return an integer type, it puts its value only into %d0. So the
 * caller doesn't pink up the correct return value. The solution is to
 * declare the function pointer to return int, so the compiler picks up the
 * return value from %d0. (Functions returning pointers put their value
 * *additionally* into %d0 for compability.) The price is that there are
 * some warnings about int->pointer conversions...
 */
typedef int32 ((*func_ptr) ());
#else
typedef char *((*func_ptr) ());
#endif

typedef struct {
    char *data[FUNC_MAX_ARGS];
} FmgrValues;

/*
 * defined in fmgr.c
 */
extern char *fmgr(Oid procedureId, ... );
extern char *fmgr_faddr_link(char *arg0, ...);

/*
 *	Macros for calling through the result of fmgr_info.
 */

/* We don't make this static so fmgr_faddr() macros can access it */
extern FmgrInfo        *fmgr_pl_finfo;

#define fmgr_faddr(finfo) (fmgr_pl_finfo = (finfo), (func_ptr) fmgr_faddr_link)

#define	FMGR_PTR2(FINFO, ARG1, ARG2)  ((*(fmgr_faddr(FINFO))) (ARG1, ARG2))

/*
 *	Flags for the builtin oprrest selectivity routines.
 *  XXX These do not belong here ... put 'em in some planner/optimizer header.
 */
#define	SEL_CONSTANT 	1		/* operator's non-var arg is a constant */
#define	SEL_RIGHT	2			/* operator's non-var arg is on the right */

#endif	/* FMGR_H */
