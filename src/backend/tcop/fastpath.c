/*-------------------------------------------------------------------------
 *
 * fastpath.c
 *	  routines to handle function requests from the frontend
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/fastpath.c,v 1.45 2000/11/16 22:30:30 tgl Exp $
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
 *	 OLD COMMENTS FOLLOW
 *
 *	  The VAR_LENGTH_{ARGS,RESULT} stuff is limited to MAX_STRING_LENGTH
 *	  (see src/backend/tmp/fastpath.h) for no obvious reason.  Since its
 *	  primary use (for us) is for Inversion path names, it should probably
 *	  be increased to 256 (MAXPATHLEN for Inversion, hidden in pg_type
 *	  as well as utils/adt/filename.c).
 *
 *	  Quoth PMA on 08/15/93:
 *
 *	  This code has been almost completely rewritten with an eye to
 *	  keeping it as compatible as possible with the previous (broken)
 *	  implementation.
 *
 *	  The previous implementation would assume (1) that any value of
 *	  length <= 4 bytes was passed-by-value, and that any other value
 *	  was a struct varlena (by-reference).	There was NO way to pass a
 *	  fixed-length by-reference argument (like name) or a struct
 *	  varlena of size <= 4 bytes.
 *
 *	  The new implementation checks the catalogs to determine whether
 *	  a value is by-value (type "0" is null-delimited character string,
 *	  as it is for, e.g., the parser).	The only other item obtained
 *	  from the catalogs is whether or not the value should be placed in
 *	  a struct varlena or not.	Otherwise, the size given by the
 *	  frontend is assumed to be correct (probably a bad decision, but
 *	  we do strange things in the name of compatibility).
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "tcop/fastpath.h"
#include "utils/syscache.h"


/* ----------------
 *		SendFunctionResult
 * ----------------
 */
static void
SendFunctionResult(Datum retval, /* actual return value */
				   bool retbyval,
				   int retlen)	/* the length according to the catalogs */
{
	StringInfoData buf;

	pq_beginmessage(&buf);
	pq_sendbyte(&buf, 'V');

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
			if (retlen < 0)
			{					/* ... varlena */
				struct varlena *v = (struct varlena *) DatumGetPointer(retval);

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
 * This structure saves enough state so that one can avoid having to
 * do catalog lookups over and over again.	(Each RPC can require up
 * to FUNC_MAX_ARGS+2 lookups, which is quite tedious.)
 *
 * The previous incarnation of this code just assumed that any argument
 * of size <= 4 was by value; this is not correct.	There is no cheap
 * way to determine function argument length etc.; one must simply pay
 * the price of catalog lookups.
 */
struct fp_info
{
	Oid			funcid;
	FmgrInfo	flinfo;			/* function lookup info for funcid */
	bool		argbyval[FUNC_MAX_ARGS];
	int32		arglen[FUNC_MAX_ARGS];	/* signed (for varlena) */
	bool		retbyval;
	int32		retlen;			/* signed (for varlena) */
	TransactionId xid;			/* when the lookup was done */
	CommandId	cid;
};

/*
 * We implement one-back caching here.	If we need to do more, we can.
 * Most routines in tight loops (like PQfswrite -> F_LOWRITE) will do
 * the same thing repeatedly.
 */
static struct fp_info last_fp = {InvalidOid};

/*
 * valid_fp_info
 *
 * RETURNS:
 *		T if the state in 'fip' is valid for the given func OID
 *		F otherwise
 *
 * "invalid" means:
 * The saved state was either uninitialized, for another function,
 * or from a previous command.	(Commands can do updates, which
 * may invalidate catalog entries for subsequent commands.	This
 * is overly pessimistic but since there is no smarter invalidation
 * scheme...).
 */
static bool
valid_fp_info(Oid func_id, struct fp_info * fip)
{
	Assert(OidIsValid(func_id));
	Assert(fip != (struct fp_info *) NULL);

	return (OidIsValid(fip->funcid) &&
			func_id == fip->funcid &&
			TransactionIdIsCurrentTransactionId(fip->xid) &&
			CommandIdIsCurrentCommandId(fip->cid));
}

/*
 * update_fp_info
 *
 * Performs catalog lookups to load a struct fp_info 'fip' for the
 * function 'func_id'.
 *
 * RETURNS:
 *		The correct information in 'fip'.  Sets 'fip->funcid' to
 *		InvalidOid if an exception occurs.
 */
static void
update_fp_info(Oid func_id, struct fp_info * fip)
{
	Oid		   *argtypes;		/* an oidvector */
	Oid			rettype;
	HeapTuple	func_htp,
				type_htp;
	Form_pg_type tp;
	Form_pg_proc pp;
	int			i;

	Assert(OidIsValid(func_id));
	Assert(fip != (struct fp_info *) NULL);

	/*
	 * Since the validity of this structure is determined by whether the
	 * funcid is OK, we clear the funcid here.	It must not be set to the
	 * correct value until we are about to return with a good struct
	 * fp_info, since we can be interrupted (i.e., with an elog(ERROR,
	 * ...)) at any time.
	 */
	MemSet((char *) fip, 0, (int) sizeof(struct fp_info));
	fip->funcid = InvalidOid;

	func_htp = SearchSysCache(PROCOID,
							  ObjectIdGetDatum(func_id),
							  0, 0, 0);
	if (!HeapTupleIsValid(func_htp))
		elog(ERROR, "update_fp_info: cache lookup for function %u failed",
			 func_id);
	pp = (Form_pg_proc) GETSTRUCT(func_htp);
	rettype = pp->prorettype;
	argtypes = pp->proargtypes;

	fmgr_info(func_id, &fip->flinfo);

	for (i = 0; i < fip->flinfo.fn_nargs; ++i)
	{
		if (OidIsValid(argtypes[i]))
		{
			type_htp = SearchSysCache(TYPEOID,
									  ObjectIdGetDatum(argtypes[i]),
									  0, 0, 0);
			if (!HeapTupleIsValid(type_htp))
				elog(ERROR, "update_fp_info: bad argument type %u for %u",
					 argtypes[i], func_id);
			tp = (Form_pg_type) GETSTRUCT(type_htp);
			fip->argbyval[i] = tp->typbyval;
			fip->arglen[i] = tp->typlen;
			ReleaseSysCache(type_htp);
		}						/* else it had better be VAR_LENGTH_ARG */
	}

	if (OidIsValid(rettype))
	{
		type_htp = SearchSysCache(TYPEOID,
								  ObjectIdGetDatum(rettype),
								  0, 0, 0);
		if (!HeapTupleIsValid(type_htp))
			elog(ERROR, "update_fp_info: bad return type %u for %u",
				 rettype, func_id);
		tp = (Form_pg_type) GETSTRUCT(type_htp);
		fip->retbyval = tp->typbyval;
		fip->retlen = tp->typlen;
		ReleaseSysCache(type_htp);
	}							/* else it had better by VAR_LENGTH_RESULT */

	fip->xid = GetCurrentTransactionId();
	fip->cid = GetCurrentCommandId();

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
 * RETURNS:
 *		0 if successful completion, EOF if frontend connection lost.
 *
 * Note: All ordinary errors result in elog(ERROR,...).  However,
 * if we lose the frontend connection there is no one to elog to,
 * and no use in proceeding...
 *
 * Note: palloc()s done here and in the called function do not need to be
 * cleaned up explicitly.  We are called from PostgresMain() in the
 * QueryContext memory context, which will be automatically reset when
 * control returns to PostgresMain.
 */
int
HandleFunctionRequest(void)
{
	Oid			fid;
	int			argsize;
	int			nargs;
	int			tmp;
	FunctionCallInfoData fcinfo;
	Datum		retval;
	int			i;
	char	   *p;
	struct fp_info *fip;

	/*
	 * XXX FIXME: This protocol is misdesigned.
	 *
	 * We really do not want to elog() before having swallowed all of the
	 * frontend's fastpath message; otherwise we will lose sync with the input
	 * datastream.  What should happen is we absorb all of the input message
	 * per protocol syntax, and *then* do error checking (including lookup of
	 * the given function ID) and elog if appropriate.  Unfortunately, because
	 * we cannot even read the message properly without knowing whether the
	 * data types are pass-by-ref or pass-by-value, it's not all that easy to
	 * do :-(.  The protocol should require the client to supply what it
	 * thinks is the typbyval and typlen value for each arg, so that we can
	 * read the data without having to do any lookups.  Then after we've read
	 * the message, we should do the lookups, verify agreement of the actual
	 * function arg types with what we received, and finally call the function.
	 *
	 * As things stand, not only will we lose sync for an invalid message
	 * (such as requested function OID doesn't exist), but we may lose sync
	 * for a perfectly valid message if we are in transaction-aborted state!
	 * This can happen because our database lookup attempts may fail entirely
	 * in abort state.
	 *
	 * Unfortunately I see no way to fix this without breaking a lot of
	 * existing clients.  Maybe do it as part of next protocol version change.
	 */

	if (pq_getint(&tmp, 4))		/* function oid */
		return EOF;
	fid = (Oid) tmp;
	if (pq_getint(&nargs, 4))	/* # of arguments */
		return EOF;

	/*
	 * This is where the one-back caching is done. If you want to save
	 * more state, make this a loop around an array.  Given the relatively
	 * short lifespan of the cache, not clear that there's any win possible.
	 */
	fip = &last_fp;
	if (!valid_fp_info(fid, fip))
		update_fp_info(fid, fip);

	if (fip->flinfo.fn_nargs != nargs || nargs > FUNC_MAX_ARGS)
	{
		elog(ERROR, "HandleFunctionRequest: actual arguments (%d) != registered arguments (%d)",
			 nargs, fip->flinfo.fn_nargs);
	}

	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &fip->flinfo;
	fcinfo.nargs = nargs;

	/*
	 * Copy supplied arguments into arg vector.  Note there is no way for
	 * frontend to specify a NULL argument --- more misdesign.
	 */
	for (i = 0; i < nargs; ++i)
	{
		if (pq_getint(&argsize, 4))
			return EOF;
		if (fip->argbyval[i])
		{						/* by-value */
			if (argsize < 1 || argsize > 4)
				elog(ERROR, "HandleFunctionRequest: bogus argsize %d",
					 argsize);
			/* XXX should we demand argsize == fip->arglen[i] ? */
			if (pq_getint(&tmp, argsize))
				return EOF;
			fcinfo.arg[i] = (Datum) tmp;
		}
		else
		{						/* by-reference ... */
			if (fip->arglen[i] < 0)
			{					/* ... varlena */
				if (argsize < 0)
					elog(ERROR, "HandleFunctionRequest: bogus argsize %d",
						 argsize);
				/* I suspect this +1 isn't really needed - tgl 5/2000 */
				p = palloc(argsize + VARHDRSZ + 1);	/* Added +1 to solve
													 * memory leak - Peter
													 * 98 Jan 6 */
				VARATT_SIZEP(p) = argsize + VARHDRSZ;
				if (pq_getbytes(VARDATA(p), argsize))
					return EOF;
			}
			else
			{					/* ... fixed */
				if (argsize != fip->arglen[i])
					elog(ERROR, "HandleFunctionRequest: bogus argsize %d, should be %d",
						 argsize, fip->arglen[i]);
				p = palloc(argsize + 1); /* +1 in case argsize is 0 */
				if (pq_getbytes(p, argsize))
					return EOF;
			}
			fcinfo.arg[i] = PointerGetDatum(p);
		}
	}

	/*
	 * Now that we've eaten the input message, check to see if we actually
	 * want to do the function call or not.
	 *
	 * Currently, we report an error if in ABORT state, or return a dummy
	 * NULL response if fastpath support has been compiled out.
	 */
	if (IsAbortedTransactionBlockState())
		elog(ERROR, "current transaction is aborted, "
			 "queries ignored until end of transaction block");

#ifdef NO_FASTPATH
	/* force a NULL return */
	retval = (Datum) 0;
	fcinfo.isnull = true;
#else
	retval = FunctionCallInvoke(&fcinfo);
#endif	 /* NO_FASTPATH */

	if (fcinfo.isnull)
		SendFunctionResult(retval, fip->retbyval, 0);
	else
		SendFunctionResult(retval, fip->retbyval, fip->retlen);

	return 0;
}
