/*
 * $Header: /cvsroot/pgsql/contrib/pgstattuple/pgstattuple.c,v 1.1 2001/10/01 01:52:38 ishii Exp $
 *
 * Copyright (c) 2001  Tatsuo Ishii
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

PG_FUNCTION_INFO_V1(pgstattuple);

extern Datum pgstattuple(PG_FUNCTION_ARGS);

/* ----------
 * pgstattuple:
 * returns the percentage of dead tuples
 *
 * C FUNCTION definition
 * pgstattuple(NAME) returns FLOAT8
 * ----------
 */
Datum
pgstattuple(PG_FUNCTION_ARGS)
{
    Name	p = PG_GETARG_NAME(0);

    Relation	rel;
    HeapScanDesc	scan;
    HeapTuple	tuple;
    BlockNumber nblocks;
    BlockNumber block = InvalidBlockNumber;
    double	table_len;
    uint64	tuple_len = 0;
    uint64	dead_tuple_len = 0;
    uint32	tuple_count = 0;
    uint32	dead_tuple_count = 0;
    double	tuple_percent;
    double	dead_tuple_percent;

    Buffer	buffer = InvalidBuffer;
    uint64	free_space = 0;	/* free/reusable space in bytes */
    double	free_percent;		/* free/reusable space in % */

    rel = heap_openr(NameStr(*p), NoLock);
    nblocks = RelationGetNumberOfBlocks(rel);
    scan = heap_beginscan(rel, false, SnapshotAny, 0, NULL);

    while ((tuple = heap_getnext(scan,0)))
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

	if (!BlockNumberIsValid(block) ||
	    block != BlockIdGetBlockNumber(&tuple->t_self.ip_blkid))
	{
	    block = BlockIdGetBlockNumber(&tuple->t_self.ip_blkid);
	    buffer = ReadBuffer(rel, block);
	    free_space += PageGetFreeSpace((Page)BufferGetPage(buffer));
	    ReleaseBuffer(buffer);
	}
    }
    heap_endscan(scan);
    heap_close(rel, NoLock);

    table_len = (double)nblocks*BLCKSZ;

    if (nblocks == 0)
    {
	tuple_percent = 0.0;
	dead_tuple_percent = 0.0;
	free_percent = 0.0;
    }
    else
    {
	tuple_percent = (double)tuple_len*100.0/table_len;
	dead_tuple_percent = (double)dead_tuple_len*100.0/table_len;
	free_percent = (double)free_space*100.0/table_len;
    }

    elog(NOTICE,"physical length: %.2fMB live tuples: %u (%.2fMB, %.2f%%) dead tuples: %u (%.2fMB, %.2f%%) free/reusable space: %.2fMB (%.2f%%) overhead: %.2f%%",

	 table_len/1024/1024,	 /* phsical length in MB */

	 tuple_count,	/* number of live tuples */
	 (double)tuple_len/1024/1024,	/* live tuples in MB */
	 tuple_percent,	/* live tuples in % */

	 dead_tuple_count,	/* number of dead tuples */
	 (double)dead_tuple_len/1024/1024,	/* dead tuples in MB */
	 dead_tuple_percent,	/* dead tuples in % */

	 (double)free_space/1024/1024,	/* free/available space in MB */

	 free_percent,	/* free/available space in % */

	 /* overhead in % */
	 (nblocks == 0)?0.0: 100.0
	 - tuple_percent
	 - dead_tuple_percent
	- free_percent);

    PG_RETURN_FLOAT8(dead_tuple_percent);
}
