/*-------------------------------------------------------------------------
 *
 * fastpath.c
 *	  routines to handle function requests from the frontend
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/fastpath.c,v 1.69 2003/09/25 06:58:02 petere Exp $
 *
 * NOTES
 *	  This cruft is the server side of PQfn.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <netinet/in.h>
#include <arpa/inet.h>

#include "catalog/pg_proc.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "tcop/fastpath.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


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
	Oid			namespace;		/* other stuff from pg_proc */
	Oid			rettype;
	Oid			argtypes[FUNC_MAX_ARGS];
};


static int16 parse_fcall_arguments(StringInfo msgBuf, struct fp_info * fip,
					  FunctionCallInfo fcinfo);
static int16 parse_fcall_arguments_20(StringInfo msgBuf, struct fp_info * fip,
						 FunctionCallInfo fcinfo);


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
		if (argsize < -1)
		{
			/* FATAL here since no hope of regaining message sync */
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
			  errmsg("invalid argument size %d in function call message",
					 argsize)));
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
 * Note: although this routine doesn't check, the format had better be 1
 * (binary) when talking to a pre-3.0 client.
 * ----------------
 */
static void
SendFunctionResult(Datum retval, bool isnull, Oid rettype, int16 format)
{
	bool		newstyle = (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3);
	StringInfoData buf;

	pq_beginmessage(&buf, 'V');

	if (isnull)
	{
		if (newstyle)
			pq_sendint(&buf, -1, 4);
	}
	else
	{
		if (!newstyle)
			pq_sendbyte(&buf, 'G');

		if (format == 0)
		{
			Oid			typoutput,
						typelem;
			bool		typisvarlena;
			char	   *outputstr;

			getTypeOutputInfo(rettype,
							  &typoutput, &typelem, &typisvarlena);
			outputstr = DatumGetCString(OidFunctionCall3(typoutput,
														 retval,
											   ObjectIdGetDatum(typelem),
													 Int32GetDatum(-1)));
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr), false);
			pfree(outputstr);
		}
		else if (format == 1)
		{
			Oid			typsend,
						typelem;
			bool		typisvarlena;
			bytea	   *outputbytes;

			getTypeBinaryOutputInfo(rettype,
									&typsend, &typelem, &typisvarlena);
			outputbytes = DatumGetByteaP(OidFunctionCall2(typsend,
														  retval,
											 ObjectIdGetDatum(typelem)));
			/* We assume the result will not have been toasted */
			pq_sendint(&buf, VARSIZE(outputbytes) - VARHDRSZ, 4);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}

	if (!newstyle)
		pq_sendbyte(&buf, '0');

	pq_endmessage(&buf);
}

/*
 * fetch_fp_info
 *
 * Performs catalog lookups to load a struct fp_info 'fip' for the
 * function 'func_id'.
 */
static void
fetch_fp_info(Oid func_id, struct fp_info * fip)
{
	HeapTuple	func_htp;
	Form_pg_proc pp;

	Assert(OidIsValid(func_id));
	Assert(fip != (struct fp_info *) NULL);

	/*
	 * Since the validity of this structure is determined by whether the
	 * funcid is OK, we clear the funcid here.	It must not be set to the
	 * correct value until we are about to return with a good struct
	 * fp_info, since we can be interrupted (i.e., with an ereport(ERROR,
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
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", func_id)));
	pp = (Form_pg_proc) GETSTRUCT(func_htp);

	fip->namespace = pp->pronamespace;
	fip->rettype = pp->prorettype;
	memcpy(fip->argtypes, pp->proargtypes, FUNC_MAX_ARGS * sizeof(Oid));

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
 * Note: All ordinary errors result in ereport(ERROR,...).	However,
 * if we lose the frontend connection there is no one to ereport to,
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
	AclResult	aclresult;
	FunctionCallInfoData fcinfo;
	int16		rformat;
	Datum		retval;
	struct fp_info my_fp;
	struct fp_info *fip;
	bool		callit;

	/*
	 * Read message contents if not already done.
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
	{
		if (GetOldFunctionMessage(msgBuf))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF on client connection")));
			return EOF;
		}
	}

	/*
	 * Now that we've eaten the input message, check to see if we actually
	 * want to do the function call or not.  It's now safe to ereport();
	 * we won't lose sync with the frontend.
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
					 "commands ignored until end of transaction block")));

	/*
	 * Begin parsing the buffer contents.
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
		(void) pq_getmsgstring(msgBuf); /* dummy string */

	fid = (Oid) pq_getmsgint(msgBuf, 4);		/* function oid */

	/*
	 * There used to be a lame attempt at caching lookup info here. Now we
	 * just do the lookups on every call.
	 */
	fip = &my_fp;
	fetch_fp_info(fid, fip);

	/*
	 * Check permission to access and call function.  Since we didn't go
	 * through a normal name lookup, we need to check schema usage too.
	 */
	aclresult = pg_namespace_aclcheck(fip->namespace, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(fip->namespace));

	aclresult = pg_proc_aclcheck(fid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC,
					   get_func_name(fid));

	/*
	 * Set up a query snapshot in case function needs one.
	 */
	SetQuerySnapshot();

	/*
	 * Prepare function call info block and insert arguments.
	 */
	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &fip->flinfo;

	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
		rformat = parse_fcall_arguments(msgBuf, fip, &fcinfo);
	else
		rformat = parse_fcall_arguments_20(msgBuf, fip, &fcinfo);

	/* Verify we reached the end of the message where expected. */
	pq_getmsgend(msgBuf);

	/*
	 * If func is strict, must not call it for null args.
	 */
	callit = true;
	if (fip->flinfo.fn_strict)
	{
		int			i;

		for (i = 0; i < fcinfo.nargs; i++)
		{
			if (fcinfo.argnull[i])
			{
				callit = false;
				break;
			}
		}
	}

	if (callit)
	{
		/* Okay, do it ... */
		retval = FunctionCallInvoke(&fcinfo);
	}
	else
	{
		fcinfo.isnull = true;
		retval = (Datum) 0;
	}

	SendFunctionResult(retval, fcinfo.isnull, fip->rettype, rformat);

	return 0;
}

/*
 * Parse function arguments in a 3.0 protocol message
 *
 * Argument values are loaded into *fcinfo, and the desired result format
 * is returned.
 */
static int16
parse_fcall_arguments(StringInfo msgBuf, struct fp_info * fip,
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
			fcinfo->argnull[i] = true;
			continue;
		}
		if (argsize < 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
			  errmsg("invalid argument size %d in function call message",
					 argsize)));

		/* Reset abuf to empty, and insert raw data into it */
		abuf.len = 0;
		abuf.data[0] = '\0';
		abuf.cursor = 0;

		appendBinaryStringInfo(&abuf,
							   pq_getmsgbytes(msgBuf, argsize),
							   argsize);

		if (numAFormats > 1)
			aformat = aformats[i];
		else if (numAFormats > 0)
			aformat = aformats[0];
		else
			aformat = 0;		/* default = text */

		if (aformat == 0)
		{
			Oid			typInput;
			Oid			typElem;
			char	   *pstring;

			getTypeInputInfo(fip->argtypes[i], &typInput, &typElem);

			/*
			 * Since stringinfo.c keeps a trailing null in place even for
			 * binary data, the contents of abuf are a valid C string.	We
			 * have to do encoding conversion before calling the typinput
			 * routine, though.
			 */
			pstring = (char *)
				pg_client_to_server((unsigned char *) abuf.data,
									argsize);
			fcinfo->arg[i] =
				OidFunctionCall3(typInput,
								 CStringGetDatum(pstring),
								 ObjectIdGetDatum(typElem),
								 Int32GetDatum(-1));
			/* Free result of encoding conversion, if any */
			if (pstring != abuf.data)
				pfree(pstring);
		}
		else if (aformat == 1)
		{
			Oid			typReceive;
			Oid			typElem;

			/* Call the argument type's binary input converter */
			getTypeBinaryInputInfo(fip->argtypes[i], &typReceive, &typElem);

			fcinfo->arg[i] = OidFunctionCall2(typReceive,
											  PointerGetDatum(&abuf),
											  ObjectIdGetDatum(typElem));

			/* Trouble if it didn't eat the whole buffer */
			if (abuf.cursor != abuf.len)
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

/*
 * Parse function arguments in a 2.0 protocol message
 *
 * Argument values are loaded into *fcinfo, and the desired result format
 * is returned.
 */
static int16
parse_fcall_arguments_20(StringInfo msgBuf, struct fp_info * fip,
						 FunctionCallInfo fcinfo)
{
	int			nargs;
	int			i;
	StringInfoData abuf;

	nargs = pq_getmsgint(msgBuf, 4);	/* # of arguments */

	if (fip->flinfo.fn_nargs != nargs || nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d arguments but function requires %d",
						nargs, fip->flinfo.fn_nargs)));

	fcinfo->nargs = nargs;

	initStringInfo(&abuf);

	/*
	 * Copy supplied arguments into arg vector.  In protocol 2.0 these are
	 * always assumed to be supplied in binary format.
	 *
	 * Note: although the original protocol 2.0 code did not have any way for
	 * the frontend to specify a NULL argument, we now choose to interpret
	 * length == -1 as meaning a NULL.
	 */
	for (i = 0; i < nargs; ++i)
	{
		int			argsize;
		Oid			typReceive;
		Oid			typElem;

		argsize = pq_getmsgint(msgBuf, 4);
		if (argsize == -1)
		{
			fcinfo->argnull[i] = true;
			continue;
		}
		if (argsize < 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
			  errmsg("invalid argument size %d in function call message",
					 argsize)));

		/* Reset abuf to empty, and insert raw data into it */
		abuf.len = 0;
		abuf.data[0] = '\0';
		abuf.cursor = 0;

		appendBinaryStringInfo(&abuf,
							   pq_getmsgbytes(msgBuf, argsize),
							   argsize);

		/* Call the argument type's binary input converter */
		getTypeBinaryInputInfo(fip->argtypes[i], &typReceive, &typElem);

		fcinfo->arg[i] = OidFunctionCall2(typReceive,
										  PointerGetDatum(&abuf),
										  ObjectIdGetDatum(typElem));

		/* Trouble if it didn't eat the whole buffer */
		if (abuf.cursor != abuf.len)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			errmsg("incorrect binary data format in function argument %d",
				   i + 1)));
	}

	/* Desired result format is always binary in protocol 2.0 */
	return 1;
}
