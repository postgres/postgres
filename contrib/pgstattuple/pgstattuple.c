/*
 * $Header: /cvsroot/pgsql/contrib/pgstattuple/pgstattuple.c,v 1.3 2001/12/19 20:28:41 tgl Exp $
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
	Name		p = PG_GETARG_NAME(0);

	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	BlockNumber nblocks;
	BlockNumber block = 0;		/* next block to count free space in */
	BlockNumber tupblock;
	Buffer		buffer;
	double		table_len;
	uint64		tuple_len = 0;
	uint64		dead_tuple_len = 0;
	uint64		tuple_count = 0;
	uint64		dead_tuple_count = 0;
	double		tuple_percent;
	double		dead_tuple_percent;

	uint64		free_space = 0; /* free/reusable space in bytes */
	double		free_percent;	/* free/reusable space in % */

	rel = heap_openr(NameStr(*p), AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(rel);
	scan = heap_beginscan(rel, false, SnapshotAny, 0, NULL);

	while ((tuple = heap_getnext(scan, 0)))
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

	table_len = (double) nblocks *BLCKSZ;

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

	elog(NOTICE, "physical length: %.2fMB live tuples: %.0f (%.2fMB, %.2f%%) dead tuples: %.0f (%.2fMB, %.2f%%) free/reusable space: %.2fMB (%.2f%%) overhead: %.2f%%",

		 table_len / (1024 * 1024),		/* physical length in MB */

		 (double) tuple_count,	/* number of live tuples */
		 (double) tuple_len / (1024 * 1024),		/* live tuples in MB */
		 tuple_percent,			/* live tuples in % */

		 (double) dead_tuple_count,	/* number of dead tuples */
		 (double) dead_tuple_len / (1024 * 1024), /* dead tuples in MB */
		 dead_tuple_percent,	/* dead tuples in % */

		 (double) free_space / (1024 * 1024), /* free/available space in
											   * MB */

		 free_percent,			/* free/available space in % */

	/* overhead in % */
		 (nblocks == 0) ? 0.0 : 100.0
		 - tuple_percent
		 - dead_tuple_percent
		 - free_percent);

	PG_RETURN_FLOAT8(dead_tuple_percent);
}
