/*
 * $Header: /cvsroot/pgsql/contrib/pgstattuple/pgstattuple.c,v 1.7 2002/08/23 08:19:49 ishii Exp $
 *
 * Copyright (c) 2001,2002  Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "fmgr.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "utils/builtins.h"


PG_FUNCTION_INFO_V1(pgstattuple);

extern Datum pgstattuple(PG_FUNCTION_ARGS);

/* ----------
 * pgstattuple:
 * returns live/dead tuples info
 *
 * C FUNCTION definition
 * pgstattuple(TEXT) returns setof pgstattuple_view
 * see pgstattuple.sql for pgstattuple_view
 * ----------
 */

#define DUMMY_TUPLE "pgstattuple_view"
#define NCOLUMNS 9
#define NCHARS 32

Datum
pgstattuple(PG_FUNCTION_ARGS)
{
	text	   *relname;
	RangeVar   *relrv;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	BlockNumber nblocks;
	BlockNumber block = 0;		/* next block to count free space in */
	BlockNumber tupblock;
	Buffer		buffer;
	uint64		table_len;
	uint64		tuple_len = 0;
	uint64		dead_tuple_len = 0;
	uint64		tuple_count = 0;
	uint64		dead_tuple_count = 0;
	double		tuple_percent;
	double		dead_tuple_percent;
	uint64		free_space = 0; /* free/reusable space in bytes */
	double		free_percent;	/* free/reusable space in % */

	FuncCallContext	   *funcctx;
	int					call_cntr;
	int					max_calls;
	TupleDesc			tupdesc;
	TupleTableSlot	   *slot;
	AttInMetadata	   *attinmeta;

	char **values;
	int i;
	Datum		result;

	/* stuff done only on the first call of the function */
	if(SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
    
		/* total number of tuples to be returned */
		funcctx->max_calls = 1;
    
		/*
		 * Build a tuple description for a pgstattupe_view tuple
		 */
		tupdesc = RelationNameGetTupleDesc(DUMMY_TUPLE);
    
		/* allocate a slot for a tuple with this tupdesc */
		slot = TupleDescGetSlot(tupdesc);
    
		/* assign slot to function context */
		funcctx->slot = slot;
    
		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	slot = funcctx->slot;
	attinmeta = funcctx->attinmeta;

	/* Are we done? */
	if (call_cntr >= max_calls)
	{
		SRF_RETURN_DONE(funcctx);
	}

	/* open relation */
	relname = PG_GETARG_TEXT_P(0);
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname,"pgstattuple"));
	rel = heap_openrv(relrv, AccessShareLock);

	nblocks = RelationGetNumberOfBlocks(rel);
	scan = heap_beginscan(rel, SnapshotAny, 0, NULL);

	/* scan the relation */
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		if (HeapTupleSatisfiesNow(tuple->t_data))
		{
			tuple_len += tuple->t_len;
			tuple_count++;
		}
		else
		{
			dead_tuple_len += tuple->t_len;
			dead_tuple_count++;
		}

		/*
		 * To avoid physically reading the table twice, try to do the
		 * free-space scan in parallel with the heap scan.  However,
		 * heap_getnext may find no tuples on a given page, so we cannot
		 * simply examine the pages returned by the heap scan.
		 */
		tupblock = BlockIdGetBlockNumber(&tuple->t_self.ip_blkid);

		while (block <= tupblock)
		{
			buffer = ReadBuffer(rel, block);
			free_space += PageGetFreeSpace((Page) BufferGetPage(buffer));
			ReleaseBuffer(buffer);
			block++;
		}
	}
	heap_endscan(scan);

	while (block < nblocks)
	{
		buffer = ReadBuffer(rel, block);
		free_space += PageGetFreeSpace((Page) BufferGetPage(buffer));
		ReleaseBuffer(buffer);
		block++;
	}

	heap_close(rel, AccessShareLock);

	table_len = (uint64)nblocks *BLCKSZ;

	if (nblocks == 0)
	{
		tuple_percent = 0.0;
		dead_tuple_percent = 0.0;
		free_percent = 0.0;
	}
	else
	{
		tuple_percent = (double) tuple_len *100.0 / table_len;
		dead_tuple_percent = (double) dead_tuple_len *100.0 / table_len;
		free_percent = (double) free_space *100.0 / table_len;
	}

	/*
	 * Prepare a values array for storage in our slot.
	 * This should be an array of C strings which will
	 * be processed later by the appropriate "in" functions.
	 */
	values = (char **) palloc(NCOLUMNS * sizeof(char *));
	for (i=0;i<NCOLUMNS;i++)
	{
		values[i] = (char *) palloc(NCHARS * sizeof(char));
	}
	i = 0;
	snprintf(values[i++], NCHARS, "%lld", table_len);
	snprintf(values[i++], NCHARS, "%lld", tuple_count);
	snprintf(values[i++], NCHARS, "%lld", tuple_len);
	snprintf(values[i++], NCHARS, "%.2f", tuple_percent);
	snprintf(values[i++], NCHARS, "%lld", dead_tuple_count);
	snprintf(values[i++], NCHARS, "%lld", dead_tuple_len);
	snprintf(values[i++], NCHARS, "%.2f", dead_tuple_percent);
	snprintf(values[i++], NCHARS, "%lld", free_space);
	snprintf(values[i++], NCHARS, "%.2f", free_percent);
    
	/* build a tuple */
	tuple = BuildTupleFromCStrings(attinmeta, values);
    
	/* make the tuple into a datum */
	result = TupleGetDatum(slot, tuple);
    
	/* Clean up */
	for (i=0;i<NCOLUMNS;i++)
	{
		pfree(values[i]);
	}
	pfree(values);
    
	SRF_RETURN_NEXT(funcctx, result);
}
