/*-------------------------------------------------------------------------
 *
 * libpq-be-fe.h
 *	  Wrapper functions for using libpq in extensions
 *
 * Code built directly into the backend is not allowed to link to libpq
 * directly. Extension code is allowed to use libpq however. One of the
 * main risks in doing so is leaking the malloc-allocated structures
 * returned by libpq, causing a process-lifespan memory leak.
 *
 * This file provides wrapper objects to help in building memory-safe code.
 * A PGresult object wrapped this way acts much as if it were palloc'd:
 * it will go away when the specified context is reset or deleted.
 * We might later extend the concept to other objects such as PGconns.
 *
 * See also the libpq-be-fe-helpers.h file, which provides additional
 * facilities built on top of this one.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/libpq-be-fe.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_FE_H
#define LIBPQ_BE_FE_H

/*
 * Despite the name, BUILDING_DLL is set only when building code directly part
 * of the backend. Which also is where libpq isn't allowed to be
 * used. Obviously this doesn't protect against libpq-fe.h getting included
 * otherwise, but perhaps still protects against a few mistakes...
 */
#ifdef BUILDING_DLL
#error "libpq may not be used in code directly built into the backend"
#endif

#include "libpq-fe.h"

/*
 * Memory-context-safe wrapper object for a PGresult.
 */
typedef struct libpqsrv_PGresult
{
	PGresult   *res;			/* the wrapped PGresult */
	MemoryContext ctx;			/* the MemoryContext it's attached to */
	MemoryContextCallback cb;	/* the callback that implements freeing */
} libpqsrv_PGresult;


/*
 * Wrap the given PGresult in a libpqsrv_PGresult object, so that it will
 * go away automatically if the current memory context is reset or deleted.
 *
 * To avoid potential memory leaks, backend code must always apply this
 * immediately to the output of any PGresult-yielding libpq function.
 */
static inline libpqsrv_PGresult *
libpqsrv_PQwrap(PGresult *res)
{
	libpqsrv_PGresult *bres;
	MemoryContext ctx = CurrentMemoryContext;

	/* We pass through a NULL result as-is, since there's nothing to free */
	if (res == NULL)
		return NULL;
	/* Attempt to allocate the wrapper ... this had better not throw error */
	bres = (libpqsrv_PGresult *)
		MemoryContextAllocExtended(ctx,
								   sizeof(libpqsrv_PGresult),
								   MCXT_ALLOC_NO_OOM);
	/* If we failed to allocate a wrapper, free the PGresult before failing */
	if (bres == NULL)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	/* Okay, set up the wrapper */
	bres->res = res;
	bres->ctx = ctx;
	bres->cb.func = (MemoryContextCallbackFunction) PQclear;
	bres->cb.arg = res;
	MemoryContextRegisterResetCallback(ctx, &bres->cb);
	return bres;
}

/*
 * Free a wrapped PGresult, after detaching it from the memory context.
 * Like PQclear(), allow the argument to be NULL.
 */
static inline void
libpqsrv_PQclear(libpqsrv_PGresult *bres)
{
	if (bres)
	{
		MemoryContextUnregisterResetCallback(bres->ctx, &bres->cb);
		PQclear(bres->res);
		pfree(bres);
	}
}

/*
 * Move a wrapped PGresult to have a different parent context.
 */
static inline libpqsrv_PGresult *
libpqsrv_PGresultSetParent(libpqsrv_PGresult *bres, MemoryContext ctx)
{
	libpqsrv_PGresult *newres;

	/* We pass through a NULL result as-is */
	if (bres == NULL)
		return NULL;
	/* Make a new wrapper in the target context, raising error on OOM */
	newres = (libpqsrv_PGresult *)
		MemoryContextAlloc(ctx, sizeof(libpqsrv_PGresult));
	/* Okay, set up the new wrapper */
	newres->res = bres->res;
	newres->ctx = ctx;
	newres->cb.func = (MemoryContextCallbackFunction) PQclear;
	newres->cb.arg = bres->res;
	MemoryContextRegisterResetCallback(ctx, &newres->cb);
	/* Disarm and delete the old wrapper */
	MemoryContextUnregisterResetCallback(bres->ctx, &bres->cb);
	pfree(bres);
	return newres;
}

/*
 * Convenience wrapper for PQgetResult.
 *
 * We could supply wrappers for other PGresult-returning functions too,
 * but at present there's no need.
 */
static inline libpqsrv_PGresult *
libpqsrv_PQgetResult(PGconn *conn)
{
	return libpqsrv_PQwrap(PQgetResult(conn));
}

/*
 * Accessor functions for libpqsrv_PGresult.  While it's not necessary to use
 * these, they emulate the behavior of the underlying libpq functions when
 * passed a NULL pointer.  This is particularly important for PQresultStatus,
 * which is often the first check on a result.
 */

static inline ExecStatusType
libpqsrv_PQresultStatus(const libpqsrv_PGresult *res)
{
	if (!res)
		return PGRES_FATAL_ERROR;
	return PQresultStatus(res->res);
}

static inline const char *
libpqsrv_PQresultErrorMessage(const libpqsrv_PGresult *res)
{
	if (!res)
		return "";
	return PQresultErrorMessage(res->res);
}

static inline char *
libpqsrv_PQresultErrorField(const libpqsrv_PGresult *res, int fieldcode)
{
	if (!res)
		return NULL;
	return PQresultErrorField(res->res, fieldcode);
}

static inline char *
libpqsrv_PQcmdStatus(const libpqsrv_PGresult *res)
{
	if (!res)
		return NULL;
	return PQcmdStatus(res->res);
}

static inline int
libpqsrv_PQntuples(const libpqsrv_PGresult *res)
{
	if (!res)
		return 0;
	return PQntuples(res->res);
}

static inline int
libpqsrv_PQnfields(const libpqsrv_PGresult *res)
{
	if (!res)
		return 0;
	return PQnfields(res->res);
}

static inline char *
libpqsrv_PQgetvalue(const libpqsrv_PGresult *res, int tup_num, int field_num)
{
	if (!res)
		return NULL;
	return PQgetvalue(res->res, tup_num, field_num);
}

static inline int
libpqsrv_PQgetlength(const libpqsrv_PGresult *res, int tup_num, int field_num)
{
	if (!res)
		return 0;
	return PQgetlength(res->res, tup_num, field_num);
}

static inline int
libpqsrv_PQgetisnull(const libpqsrv_PGresult *res, int tup_num, int field_num)
{
	if (!res)
		return 1;				/* pretend it is null */
	return PQgetisnull(res->res, tup_num, field_num);
}

static inline char *
libpqsrv_PQfname(const libpqsrv_PGresult *res, int field_num)
{
	if (!res)
		return NULL;
	return PQfname(res->res, field_num);
}

static inline const char *
libpqsrv_PQcmdTuples(const libpqsrv_PGresult *res)
{
	if (!res)
		return "";
	return PQcmdTuples(res->res);
}

/*
 * Redefine these libpq entry point names concerned with PGresults so that
 * they will operate on libpqsrv_PGresults instead.  This avoids needing to
 * convert a lot of pre-existing code, and reduces the notational differences
 * between frontend and backend libpq-using code.
 */
#define PGresult libpqsrv_PGresult
#define PQclear libpqsrv_PQclear
#define PQgetResult libpqsrv_PQgetResult
#define PQresultStatus libpqsrv_PQresultStatus
#define PQresultErrorMessage libpqsrv_PQresultErrorMessage
#define PQresultErrorField libpqsrv_PQresultErrorField
#define PQcmdStatus libpqsrv_PQcmdStatus
#define PQntuples libpqsrv_PQntuples
#define PQnfields libpqsrv_PQnfields
#define PQgetvalue libpqsrv_PQgetvalue
#define PQgetlength libpqsrv_PQgetlength
#define PQgetisnull libpqsrv_PQgetisnull
#define PQfname libpqsrv_PQfname
#define PQcmdTuples libpqsrv_PQcmdTuples

#endif							/* LIBPQ_BE_FE_H */
