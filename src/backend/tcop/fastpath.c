/*-------------------------------------------------------------------------
 *
 * fastpath.c
 *	  routines to handle function requests from the frontend
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/fastpath.c,v 1.61 2003/05/05 00:44:56 tgl Exp $
 *
 * NOTES
 *	  This cruft is the server side of PQfn.
 *
 *	  - jolly 07/11/95:
 *
 *	  no longer rely on return sizes provided by the frontend.	Always
 *	  use the true lengths for the catalogs.  Assume that the frontend
 *	  has allocated enough space to handle the result value returned.
 *
 *	  trust that the user knows what he is doing with the args.  If the
 *	  sys catalog says it is a varlena, assume that the user is only sending
 *	  down VARDATA and that the argsize is the VARSIZE.  If the arg is
 *	  fixed len, assume that the argsize given by the user is correct.
 *
 *	  if the function returns by value, then only send 4 bytes value
 *	  back to the frontend.  If the return returns by reference,
 *	  send down only the data portion and set the return size appropriately.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "tcop/fastpath.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/* ----------------
 *		GetOldFunctionMessage
 *
 * In pre-3.0 protocol, there is no length word on the message, so we have
 * to have code that understands the message layout to absorb the message
 * into a buffer.  We want to do this before we start execution, so that
 * we do not lose sync with the frontend if there's an error.
 *
 * The caller should already have initialized buf to empty.
 * ----------------
 */
static int
GetOldFunctionMessage(StringInfo buf)
{
	int32		ibuf;
	int			nargs;

	/* Dummy string argument */
	if (pq_getstring(buf))
		return EOF;
	/* Function OID */
	if (pq_getbytes((char *) &ibuf, 4))
		return EOF;
	appendBinaryStringInfo(buf, (char *) &ibuf, 4);
	/* Number of arguments */
	if (pq_getbytes((char *) &ibuf, 4))
		return EOF;
	appendBinaryStringInfo(buf, (char *) &ibuf, 4);
	nargs = ntohl(ibuf);
	/* For each argument ... */
	while (nargs-- > 0)
	{
		int			argsize;

		/* argsize */
		if (pq_getbytes((char *) &ibuf, 4))
			return EOF;
		appendBinaryStringInfo(buf, (char *) &ibuf, 4);
		argsize = ntohl(ibuf);
		if (argsize < 0)
		{
			/* FATAL here since no hope of regaining message sync */
			elog(FATAL, "HandleFunctionRequest: bogus argsize %d",
				 argsize);
		}
		/* and arg contents */
		if (argsize > 0)
		{
			/* Allocate space for arg */
			enlargeStringInfo(buf, argsize);
			/* And grab it */
			if (pq_getbytes(buf->data + buf->len, argsize))
				return EOF;
			buf->len += argsize;
			/* Place a trailing null per StringInfo convention */
			buf->data[buf->len] = '\0';
		}
	}
	return 0;
}

/* ----------------
 *		SendFunctionResult
 *
 * retlen is 0 if returning NULL, else the typlen according to the catalogs
 * ----------------
 */
static void
SendFunctionResult(Datum retval, bool retbyval, int retlen)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'V');

	if (retlen != 0)
	{
		pq_sendbyte(&buf, 'G');
		if (retbyval)
		{						/* by-value */
			pq_sendint(&buf, retlen, 4);
			pq_sendint(&buf, DatumGetInt32(retval), retlen);
		}
		else
		{						/* by-reference ... */
			if (retlen == -1)
			{					/* ... varlena */
				struct varlena *v = PG_DETOAST_DATUM(retval);

				pq_sendint(&buf, VARSIZE(v) - VARHDRSZ, VARHDRSZ);
				pq_sendbytes(&buf, VARDATA(v), VARSIZE(v) - VARHDRSZ);
			}
			else
			{					/* ... fixed */
				pq_sendint(&buf, retlen, 4);
				pq_sendbytes(&buf, DatumGetPointer(retval), retlen);
			}
		}
	}

	pq_sendbyte(&buf, '0');
	pq_endmessage(&buf);
}

/*
 * Formerly, this code attempted to cache the function and type info
 * looked up by fetch_fp_info, but only for the duration of a single
 * transaction command (since in theory the info could change between
 * commands).  This was utterly useless, because postgres.c executes
 * each fastpath call as a separate transaction command, and so the
 * cached data could never actually have been reused.  If it had worked
 * as intended, it would have had problems anyway with dangling references
 * in the FmgrInfo struct.	So, forget about caching and just repeat the
 * syscache fetches on each usage.	They're not *that* expensive.
 */
struct fp_info
{
	Oid			funcid;
	FmgrInfo	flinfo;			/* function lookup info for funcid */
	int16		arglen[FUNC_MAX_ARGS];
	bool		argbyval[FUNC_MAX_ARGS];
	int16		retlen;
	bool		retbyval;
};

/*
 * fetch_fp_info
 *
 * Performs catalog lookups to load a struct fp_info 'fip' for the
 * function 'func_id'.
 */
static void
fetch_fp_info(Oid func_id, struct fp_info * fip)
{
	Oid		   *argtypes;		/* an oidvector */
	Oid			rettype;
	HeapTuple	func_htp;
	Form_pg_proc pp;
	int			i;

	Assert(OidIsValid(func_id));
	Assert(fip != (struct fp_info *) NULL);

	/*
	 * Since the validity of this structure is determined by whether the
	 * funcid is OK, we clear the funcid here.	It must not be set to the
	 * correct value until we are about to return with a good struct
	 * fp_info, since we can be interrupted (i.e., with an elog(ERROR,
	 * ...)) at any time.  [No longer really an issue since we don't save
	 * the struct fp_info across transactions anymore, but keep it
	 * anyway.]
	 */
	MemSet((char *) fip, 0, sizeof(struct fp_info));
	fip->funcid = InvalidOid;

	fmgr_info(func_id, &fip->flinfo);

	func_htp = SearchSysCache(PROCOID,
							  ObjectIdGetDatum(func_id),
							  0, 0, 0);
	if (!HeapTupleIsValid(func_htp))
		elog(ERROR, "fetch_fp_info: cache lookup for function %u failed",
			 func_id);
	pp = (Form_pg_proc) GETSTRUCT(func_htp);
	rettype = pp->prorettype;
	argtypes = pp->proargtypes;

	for (i = 0; i < pp->pronargs; ++i)
	{
		get_typlenbyval(argtypes[i], &fip->arglen[i], &fip->argbyval[i]);
		/* We don't support cstring in fastpath protocol */
		if (fip->arglen[i] == -2)
			elog(ERROR, "CSTRING not supported in fastpath protocol");
	}

	get_typlenbyval(rettype, &fip->retlen, &fip->retbyval);
	if (fip->retlen == -2)
		elog(ERROR, "CSTRING not supported in fastpath protocol");

	ReleaseSysCache(func_htp);

	/*
	 * This must be last!
	 */
	fip->funcid = func_id;
}


/*
 * HandleFunctionRequest
 *
 * Server side of PQfn (fastpath function calls from the frontend).
 * This corresponds to the libpq protocol symbol "F".
 *
 * INPUT:
 *		In protocol version 3, postgres.c has already read the message body
 *		and will pass it in msgBuf.
 *		In old protocol, the passed msgBuf is empty and we must read the
 *		message here.
 * 
 * RETURNS:
 *		0 if successful completion, EOF if frontend connection lost.
 *
 * Note: All ordinary errors result in elog(ERROR,...).  However,
 * if we lose the frontend connection there is no one to elog to,
 * and no use in proceeding...
 *
 * Note: palloc()s done here and in the called function do not need to be
 * cleaned up explicitly.  We are called from PostgresMain() in the
 * MessageContext memory context, which will be automatically reset when
 * control returns to PostgresMain.
 */
int
HandleFunctionRequest(StringInfo msgBuf)
{
	Oid			fid;
	int			nargs;
	AclResult	aclresult;
	FunctionCallInfoData fcinfo;
	Datum		retval;
	int			i;
	struct fp_info my_fp;
	struct fp_info *fip;

	/*
	 * Read message contents if not already done.
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
	{
		if (GetOldFunctionMessage(msgBuf))
		{
			elog(COMMERROR, "unexpected EOF on client connection");
			return EOF;
		}
	}

	/*
	 * Now that we've eaten the input message, check to see if we actually
	 * want to do the function call or not.  It's now safe to elog(); we won't
	 * lose sync with the frontend.
	 */
	if (IsAbortedTransactionBlockState())
		elog(ERROR, "current transaction is aborted, "
			 "queries ignored until end of transaction block");

	/*
	 * Parse the buffer contents.
	 */
	(void) pq_getmsgstring(msgBuf);	/* dummy string */
	fid = (Oid) pq_getmsgint(msgBuf, 4); /* function oid */
	nargs = pq_getmsgint(msgBuf, 4);	/* # of arguments */

	/*
	 * There used to be a lame attempt at caching lookup info here. Now we
	 * just do the lookups on every call.
	 */
	fip = &my_fp;
	fetch_fp_info(fid, fip);

	/* Check permission to call function */
	aclresult = pg_proc_aclcheck(fid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_func_name(fid));

	/*
	 * Set up a query snapshot in case function needs one.
	 */
	SetQuerySnapshot();

	/*
	 * Prepare function call info block.
	 */
	if (fip->flinfo.fn_nargs != nargs || nargs > FUNC_MAX_ARGS)
		elog(ERROR, "HandleFunctionRequest: actual arguments (%d) != registered arguments (%d)",
			 nargs, fip->flinfo.fn_nargs);

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &fip->flinfo;
	fcinfo.nargs = nargs;

	/*
	 * Copy supplied arguments into arg vector.  Note there is no way for
	 * frontend to specify a NULL argument --- this protocol is misdesigned.
	 */
	for (i = 0; i < nargs; ++i)
	{
		int			argsize;
		char	   *p;

		argsize = pq_getmsgint(msgBuf, 4);
		if (fip->argbyval[i])
		{						/* by-value */
			if (argsize < 1 || argsize > 4)
				elog(ERROR, "HandleFunctionRequest: bogus argsize %d",
					 argsize);
			/* XXX should we demand argsize == fip->arglen[i] ? */
			fcinfo.arg[i] = (Datum) pq_getmsgint(msgBuf, argsize);
		}
		else
		{						/* by-reference ... */
			if (fip->arglen[i] == -1)
			{					/* ... varlena */
				if (argsize < 0)
					elog(ERROR, "HandleFunctionRequest: bogus argsize %d",
						 argsize);
				p = palloc(argsize + VARHDRSZ);
				VARATT_SIZEP(p) = argsize + VARHDRSZ;
				pq_copymsgbytes(msgBuf, VARDATA(p), argsize);
			}
			else
			{					/* ... fixed */
				if (argsize != fip->arglen[i])
					elog(ERROR, "HandleFunctionRequest: bogus argsize %d, should be %d",
						 argsize, fip->arglen[i]);
				p = palloc(argsize + 1);		/* +1 in case argsize is 0 */
				pq_copymsgbytes(msgBuf, p, argsize);
			}
			fcinfo.arg[i] = PointerGetDatum(p);
		}
	}

	/* Verify we reached the end of the message where expected. */
	pq_getmsgend(msgBuf);

#ifdef NO_FASTPATH
	/* force a NULL return */
	retval = (Datum) 0;
	fcinfo.isnull = true;
#else
	retval = FunctionCallInvoke(&fcinfo);
#endif   /* NO_FASTPATH */

	if (fcinfo.isnull)
		SendFunctionResult(retval, fip->retbyval, 0);
	else
		SendFunctionResult(retval, fip->retbyval, fip->retlen);

	return 0;
}
