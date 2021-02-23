/*-------------------------------------------------------------------------
 *
 * rawpage.c
 *	  Functions to extract a raw page as bytea and inspect it
 *
 * Access-method specific inspection functions are in separate files.
 *
 * Copyright (c) 2007-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/rawpage.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "storage/bufmgr.h"
#include "storage/checksum.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

static bytea *get_raw_page_internal(text *relname, ForkNumber forknum,
									BlockNumber blkno);


/*
 * get_raw_page
 *
 * Returns a copy of a page from shared buffers as a bytea
 */
PG_FUNCTION_INFO_V1(get_raw_page_1_9);

Datum
get_raw_page_1_9(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	int64		blkno = PG_GETARG_INT64(1);
	bytea	   *raw_page;

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	raw_page = get_raw_page_internal(relname, MAIN_FORKNUM, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * entry point for old extension version
 */
PG_FUNCTION_INFO_V1(get_raw_page);

Datum
get_raw_page(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	bytea	   *raw_page;

	/*
	 * We don't normally bother to check the number of arguments to a C
	 * function, but here it's needed for safety because early 8.4 beta
	 * releases mistakenly redefined get_raw_page() as taking three arguments.
	 */
	if (PG_NARGS() != 2)
		ereport(ERROR,
				(errmsg("wrong number of arguments to get_raw_page()"),
				 errhint("Run the updated pageinspect.sql script.")));

	raw_page = get_raw_page_internal(relname, MAIN_FORKNUM, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * get_raw_page_fork
 *
 * Same, for any fork
 */
PG_FUNCTION_INFO_V1(get_raw_page_fork_1_9);

Datum
get_raw_page_fork_1_9(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	text	   *forkname = PG_GETARG_TEXT_PP(1);
	int64		blkno = PG_GETARG_INT64(2);
	bytea	   *raw_page;
	ForkNumber	forknum;

	forknum = forkname_to_number(text_to_cstring(forkname));

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	raw_page = get_raw_page_internal(relname, forknum, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * Entry point for old extension version
 */
PG_FUNCTION_INFO_V1(get_raw_page_fork);

Datum
get_raw_page_fork(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	text	   *forkname = PG_GETARG_TEXT_PP(1);
	uint32		blkno = PG_GETARG_UINT32(2);
	bytea	   *raw_page;
	ForkNumber	forknum;

	forknum = forkname_to_number(text_to_cstring(forkname));

	raw_page = get_raw_page_internal(relname, forknum, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * workhorse
 */
static bytea *
get_raw_page_internal(text *relname, ForkNumber forknum, BlockNumber blkno)
{
	bytea	   *raw_page;
	RangeVar   *relrv;
	Relation	rel;
	char	   *raw_page_data;
	Buffer		buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

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
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from foreign table \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from partitioned table \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from partitioned index \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (blkno >= RelationGetNumberOfBlocksInFork(rel, forknum))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(rel))));

	/* Initialize buffer to copy to */
	raw_page = (bytea *) palloc(BLCKSZ + VARHDRSZ);
	SET_VARSIZE(raw_page, BLCKSZ + VARHDRSZ);
	raw_page_data = VARDATA(raw_page);

	/* Take a verbatim copy of the page */

	buf = ReadBufferExtended(rel, forknum, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	memcpy(raw_page_data, BufferGetPage(buf), BLCKSZ);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	relation_close(rel, AccessShareLock);

	return raw_page;
}


/*
 * get_page_from_raw
 *
 * Get a palloc'd, maxalign'ed page image from the result of get_raw_page()
 *
 * On machines with MAXALIGN = 8, the payload of a bytea is not maxaligned,
 * since it will start 4 bytes into a palloc'd value.  On alignment-picky
 * machines, this will cause failures in accesses to 8-byte-wide values
 * within the page.  We don't need to worry if accessing only 4-byte or
 * smaller fields, but when examining a struct that contains 8-byte fields,
 * use this function for safety.
 */
Page
get_page_from_raw(bytea *raw_page)
{
	Page		page;
	int			raw_page_size;

	raw_page_size = VARSIZE_ANY_EXHDR(raw_page);

	if (raw_page_size != BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid page size"),
				 errdetail("Expected %d bytes, got %d.",
						   BLCKSZ, raw_page_size)));

	page = palloc(raw_page_size);

	memcpy(page, VARDATA_ANY(raw_page), raw_page_size);

	return page;
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

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

	/*
	 * Check that enough data was supplied, so that we don't try to access
	 * fields outside the supplied buffer.
	 */
	if (raw_page_size < SizeOfPageHeaderData)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page too small (%d bytes)", raw_page_size)));

	page = (PageHeader) VARDATA(raw_page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Extract information from the page header */

	lsn = PageGetLSN(page);

	/* pageinspect >= 1.2 uses pg_lsn instead of text for the LSN field. */
	if (TupleDescAttr(tupdesc, 0)->atttypid == TEXTOID)
	{
		char		lsnchar[64];

		snprintf(lsnchar, sizeof(lsnchar), "%X/%X", LSN_FORMAT_ARGS(lsn));
		values[0] = CStringGetTextDatum(lsnchar);
	}
	else
		values[0] = LSNGetDatum(lsn);
	values[1] = UInt16GetDatum(page->pd_checksum);
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

/*
 * page_checksum
 *
 * Compute checksum of a raw page
 */

PG_FUNCTION_INFO_V1(page_checksum_1_9);
PG_FUNCTION_INFO_V1(page_checksum);

static Datum
page_checksum_internal(PG_FUNCTION_ARGS, enum pageinspect_version ext_version)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	int64		blkno = (ext_version == PAGEINSPECT_V1_8 ? PG_GETARG_UINT32(1) : PG_GETARG_INT64(1));
	int			raw_page_size;
	PageHeader	page;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

	/*
	 * Check that the supplied page is of the right size.
	 */
	if (raw_page_size != BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("incorrect size of input page (%d bytes)", raw_page_size)));

	page = (PageHeader) VARDATA(raw_page);

	PG_RETURN_INT16(pg_checksum_page((char *) page, blkno));
}

Datum
page_checksum_1_9(PG_FUNCTION_ARGS)
{
	return page_checksum_internal(fcinfo, PAGEINSPECT_V1_9);
}

/*
 * Entry point for old extension version
 */
Datum
page_checksum(PG_FUNCTION_ARGS)
{
	return page_checksum_internal(fcinfo, PAGEINSPECT_V1_8);
}
