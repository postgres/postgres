/*-------------------------------------------------------------------------
 *
 * fastpath.c
 *	  routines to handle function requests from the frontend
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/fastpath.c
 *
 * NOTES
 *	  This cruft is the server side of PQfn.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "tcop/fastpath.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * Formerly, this code attempted to cache the function and type info
 * looked up by fetch_fp_info, but only for the duration of a single
 * transaction command (since in theory the info could change between
 * commands).  This was utterly useless, because postgres.c executes
 * each fastpath call as a separate transaction command, and so the
 * cached data could never actually have been reused.  If it had worked
 * as intended, it would have had problems anyway with dangling references
 * in the FmgrInfo struct.  So, forget about caching and just repeat the
 * syscache fetches on each usage.  They're not *that* expensive.
 */
struct fp_info
{
	Oid			funcid;
	FmgrInfo	flinfo;			/* function lookup info for funcid */
	Oid			namespace;		/* other stuff from pg_proc */
	Oid			rettype;
	Oid			argtypes[FUNC_MAX_ARGS];
	char		fname[NAMEDATALEN]; /* function name for logging */
};


static int16 parse_fcall_arguments(StringInfo msgBuf, struct fp_info *fip,
								   FunctionCallInfo fcinfo);

/* ----------------
 *		SendFunctionResult
 * ----------------
 */
static void
SendFunctionResult(Datum retval, bool isnull, Oid rettype, int16 format)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'V');

	if (isnull)
	{
		pq_sendint32(&buf, -1);
	}
	else
	{
		if (format == 0)
		{
			Oid			typoutput;
			bool		typisvarlena;
			char	   *outputstr;

			getTypeOutputInfo(rettype, &typoutput, &typisvarlena);
			outputstr = OidOutputFunctionCall(typoutput, retval);
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr), false);
			pfree(outputstr);
		}
		else if (format == 1)
		{
			Oid			typsend;
			bool		typisvarlena;
			bytea	   *outputbytes;

			getTypeBinaryOutputInfo(rettype, &typsend, &typisvarlena);
			outputbytes = OidSendFunctionCall(typsend, retval);
			pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}

	pq_endmessage(&buf);
}

/*
 * fetch_fp_info
 *
 * Performs catalog lookups to load a struct fp_info 'fip' for the
 * function 'func_id'.
 */
static void
fetch_fp_info(Oid func_id, struct fp_info *fip)
{
	HeapTuple	func_htp;
	Form_pg_proc pp;

	Assert(OidIsValid(func_id));
	Assert(fip != NULL);

	/*
	 * Since the validity of this structure is determined by whether the
	 * funcid is OK, we clear the funcid here.  It must not be set to the
	 * correct value until we are about to return with a good struct fp_info,
	 * since we can be interrupted (i.e., with an ereport(ERROR, ...)) at any
	 * time.  [No longer really an issue since we don't save the struct
	 * fp_info across transactions anymore, but keep it anyway.]
	 */
	MemSet(fip, 0, sizeof(struct fp_info));
	fip->funcid = InvalidOid;

	fmgr_info(func_id, &fip->flinfo);

	func_htp = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_id));
	if (!HeapTupleIsValid(func_htp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", func_id)));
	pp = (Form_pg_proc) GETSTRUCT(func_htp);

	/* watch out for catalog entries with more than FUNC_MAX_ARGS args */
	if (pp->pronargs > FUNC_MAX_ARGS)
		elog(ERROR, "function %s has more than %d arguments",
			 NameStr(pp->proname), FUNC_MAX_ARGS);

	fip->namespace = pp->pronamespace;
	fip->rettype = pp->prorettype;
	memcpy(fip->argtypes, pp->proargtypes.values, pp->pronargs * sizeof(Oid));
	strlcpy(fip->fname, NameStr(pp->proname), NAMEDATALEN);

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
 *		postgres.c has already read the message body and will pass it in
 *		msgBuf.
 *
 * Note: palloc()s done here and in the called function do not need to be
 * cleaned up explicitly.  We are called from PostgresMain() in the
 * MessageContext memory context, which will be automatically reset when
 * control returns to PostgresMain.
 */
void
HandleFunctionRequest(StringInfo msgBuf)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	Oid			fid;
	AclResult	aclresult;
	int16		rformat;
	Datum		retval;
	struct fp_info my_fp;
	struct fp_info *fip;
	bool		callit;
	bool		was_logged = false;
	char		msec_str[32];

	/*
	 * We only accept COMMIT/ABORT if we are in an aborted transaction, and
	 * COMMIT/ABORT cannot be executed through the fastpath interface.
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block")));

	/*
	 * Now that we know we are in a valid transaction, set snapshot in case
	 * needed by function itself or one of the datatype I/O routines.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Begin parsing the buffer contents.
	 */
	fid = (Oid) pq_getmsgint(msgBuf, 4);	/* function oid */

	/*
	 * There used to be a lame attempt at caching lookup info here. Now we
	 * just do the lookups on every call.
	 */
	fip = &my_fp;
	fetch_fp_info(fid, fip);

	/* Log as soon as we have the function OID and name */
	if (log_statement == LOGSTMT_ALL)
	{
		ereport(LOG,
				(errmsg("fastpath function call: \"%s\" (OID %u)",
						fip->fname, fid)));
		was_logged = true;
	}

	/*
	 * Check permission to access and call function.  Since we didn't go
	 * through a normal name lookup, we need to check schema usage too.
	 */
	aclresult = pg_namespace_aclcheck(fip->namespace, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(fip->namespace));
	InvokeNamespaceSearchHook(fip->namespace, true);

	aclresult = pg_proc_aclcheck(fid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   get_func_name(fid));
	InvokeFunctionExecuteHook(fid);

	/*
	 * Prepare function call info block and insert arguments.
	 *
	 * Note: for now we pass collation = InvalidOid, so collation-sensitive
	 * functions can't be called this way.  Perhaps we should pass
	 * DEFAULT_COLLATION_OID, instead?
	 */
	InitFunctionCallInfoData(*fcinfo, &fip->flinfo, 0, InvalidOid, NULL, NULL);

	rformat = parse_fcall_arguments(msgBuf, fip, fcinfo);

	/* Verify we reached the end of the message where expected. */
	pq_getmsgend(msgBuf);

	/*
	 * If func is strict, must not call it for null args.
	 */
	callit = true;
	if (fip->flinfo.fn_strict)
	{
		int			i;

		for (i = 0; i < fcinfo->nargs; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				callit = false;
				break;
			}
		}
	}

	if (callit)
	{
		/* Okay, do it ... */
		retval = FunctionCallInvoke(fcinfo);
	}
	else
	{
		fcinfo->isnull = true;
		retval = (Datum) 0;
	}

	/* ensure we do at least one CHECK_FOR_INTERRUPTS per function call */
	CHECK_FOR_INTERRUPTS();

	SendFunctionResult(retval, fcinfo->isnull, fip->rettype, rformat);

	/* We no longer need the snapshot */
	PopActiveSnapshot();

	/*
	 * Emit duration logging if appropriate.
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  fastpath function call: \"%s\" (OID %u)",
							msec_str, fip->fname, fid)));
			break;
	}
}

/*
 * Parse function arguments in a 3.0 protocol message
 *
 * Argument values are loaded into *fcinfo, and the desired result format
 * is returned.
 */
static int16
parse_fcall_arguments(StringInfo msgBuf, struct fp_info *fip,
					  FunctionCallInfo fcinfo)
{
	int			nargs;
	int			i;
	int			numAFormats;
	int16	   *aformats = NULL;
	StringInfoData abuf;

	/* Get the argument format codes */
	numAFormats = pq_getmsgint(msgBuf, 2);
	if (numAFormats > 0)
	{
		aformats = (int16 *) palloc(numAFormats * sizeof(int16));
		for (i = 0; i < numAFormats; i++)
			aformats[i] = pq_getmsgint(msgBuf, 2);
	}

	nargs = pq_getmsgint(msgBuf, 2);	/* # of arguments */

	if (fip->flinfo.fn_nargs != nargs || nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d arguments but function requires %d",
						nargs, fip->flinfo.fn_nargs)));

	fcinfo->nargs = nargs;

	if (numAFormats > 1 && numAFormats != nargs)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d argument formats but %d arguments",
						numAFormats, nargs)));

	initStringInfo(&abuf);

	/*
	 * Copy supplied arguments into arg vector.
	 */
	for (i = 0; i < nargs; ++i)
	{
		int			argsize;
		int16		aformat;

		argsize = pq_getmsgint(msgBuf, 4);
		if (argsize == -1)
		{
			fcinfo->args[i].isnull = true;
		}
		else
		{
			fcinfo->args[i].isnull = false;
			if (argsize < 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid argument size %d in function call message",
								argsize)));

			/* Reset abuf to empty, and insert raw data into it */
			resetStringInfo(&abuf);
			appendBinaryStringInfo(&abuf,
								   pq_getmsgbytes(msgBuf, argsize),
								   argsize);
		}

		if (numAFormats > 1)
			aformat = aformats[i];
		else if (numAFormats > 0)
			aformat = aformats[0];
		else
			aformat = 0;		/* default = text */

		if (aformat == 0)
		{
			Oid			typinput;
			Oid			typioparam;
			char	   *pstring;

			getTypeInputInfo(fip->argtypes[i], &typinput, &typioparam);

			/*
			 * Since stringinfo.c keeps a trailing null in place even for
			 * binary data, the contents of abuf are a valid C string.  We
			 * have to do encoding conversion before calling the typinput
			 * routine, though.
			 */
			if (argsize == -1)
				pstring = NULL;
			else
				pstring = pg_client_to_server(abuf.data, argsize);

			fcinfo->args[i].value = OidInputFunctionCall(typinput, pstring,
														 typioparam, -1);
			/* Free result of encoding conversion, if any */
			if (pstring && pstring != abuf.data)
				pfree(pstring);
		}
		else if (aformat == 1)
		{
			Oid			typreceive;
			Oid			typioparam;
			StringInfo	bufptr;

			/* Call the argument type's binary input converter */
			getTypeBinaryInputInfo(fip->argtypes[i], &typreceive, &typioparam);

			if (argsize == -1)
				bufptr = NULL;
			else
				bufptr = &abuf;

			fcinfo->args[i].value = OidReceiveFunctionCall(typreceive, bufptr,
														   typioparam, -1);

			/* Trouble if it didn't eat the whole buffer */
			if (argsize != -1 && abuf.cursor != abuf.len)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("incorrect binary data format in function argument %d",
								i + 1)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", aformat)));
	}

	/* Return result format code */
	return (int16) pq_getmsgint(msgBuf, 2);
}
