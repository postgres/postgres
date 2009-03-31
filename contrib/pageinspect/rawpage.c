/*-------------------------------------------------------------------------
 *
 * rawpage.c
 *	  Functions to extract a raw page as bytea and inspect it
 *
 * Access-method specific inspection functions are in separate files.
 *
 * Copyright (c) 2007-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/pageinspect/rawpage.c,v 1.4.2.1 2009/03/31 22:54:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

Datum		get_raw_page(PG_FUNCTION_ARGS);
Datum		page_header(PG_FUNCTION_ARGS);

/*
 * get_raw_page
 *
 * Returns a copy of a page from shared buffers as a bytea
 */
PG_FUNCTION_INFO_V1(get_raw_page);

Datum
get_raw_page(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	uint32		blkno = PG_GETARG_UINT32(1);

	Relation	rel;
	RangeVar   *relrv;
	bytea	   *raw_page;
	char	   *raw_page_data;
	Buffer		buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	/* Check that this relation has storage */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from view \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from composite type \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Reject attempts to read non-local temporary relations; we would
	 * be likely to get wrong data since we have no visibility into the
	 * owning session's local buffers.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (blkno >= RelationGetNumberOfBlocks(rel))
		elog(ERROR, "block number %u is out of range for relation \"%s\"",
			 blkno, RelationGetRelationName(rel));

	/* Initialize buffer to copy to */
	raw_page = (bytea *) palloc(BLCKSZ + VARHDRSZ);
	SET_VARSIZE(raw_page, BLCKSZ + VARHDRSZ);
	raw_page_data = VARDATA(raw_page);

	/* Take a verbatim copy of the page */

	buf = ReadBuffer(rel, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	memcpy(raw_page_data, BufferGetPage(buf), BLCKSZ);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	relation_close(rel, AccessShareLock);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * page_header
 *
 * Allows inspection of page header fields of a raw page
 */

PG_FUNCTION_INFO_V1(page_header);

Datum
page_header(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	int			raw_page_size;

	TupleDesc	tupdesc;

	Datum		result;
	HeapTuple	tuple;
	Datum		values[9];
	bool		nulls[9];

	PageHeader	page;
	XLogRecPtr	lsn;
	char		lsnchar[64];

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw page functions"))));

	raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

	/*
	 * Check that enough data was supplied, so that we don't try to access
	 * fields outside the supplied buffer.
	 */
	if (raw_page_size < sizeof(PageHeaderData))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page too small (%d bytes)", raw_page_size)));

	page = (PageHeader) VARDATA(raw_page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Extract information from the page header */

	lsn = PageGetLSN(page);
	snprintf(lsnchar, sizeof(lsnchar), "%X/%X", lsn.xlogid, lsn.xrecoff);

	values[0] = DirectFunctionCall1(textin, CStringGetDatum(lsnchar));
	values[1] = UInt16GetDatum(PageGetTLI(page));
	values[2] = UInt16GetDatum(page->pd_flags);
	values[3] = UInt16GetDatum(page->pd_lower);
	values[4] = UInt16GetDatum(page->pd_upper);
	values[5] = UInt16GetDatum(page->pd_special);
	values[6] = UInt16GetDatum(PageGetPageSize(page));
	values[7] = UInt16GetDatum(PageGetPageLayoutVersion(page));
	values[8] = TransactionIdGetDatum(page->pd_prune_xid);

	/* Build and return the tuple. */

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}
