/*-------------------------------------------------------------------------
 *
 * stats.c--
 *	  heap access method debugging statistic collection routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/heap/Attic/stats.c,v 1.12 1997/09/07 04:38:13 momjian Exp $
 *
 * NOTES
 *	  initam should be moved someplace else.
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>

#include <postgres.h>

#include <access/heapam.h>
#include <utils/mcxt.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static void		InitHeapAccessStatistics(void);

/* ----------------
 *		InitHeapAccessStatistics
 * ----------------
 */
HeapAccessStatistics heap_access_stats = (HeapAccessStatistics) NULL;

static void
InitHeapAccessStatistics()
{
	MemoryContext	oldContext;
	HeapAccessStatistics stats;

	/* ----------------
	 *	make sure we don't initialize things twice
	 * ----------------
	 */
	if (heap_access_stats != NULL)
		return;

	/* ----------------
	 *	allocate statistics structure from the top memory context
	 * ----------------
	 */
	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	stats = (HeapAccessStatistics)
		palloc(sizeof(HeapAccessStatisticsData));

	/* ----------------
	 *	initialize fields to default values
	 * ----------------
	 */
	stats->global_open = 0;
	stats->global_openr = 0;
	stats->global_close = 0;
	stats->global_beginscan = 0;
	stats->global_rescan = 0;
	stats->global_endscan = 0;
	stats->global_getnext = 0;
	stats->global_fetch = 0;
	stats->global_insert = 0;
	stats->global_delete = 0;
	stats->global_replace = 0;
	stats->global_markpos = 0;
	stats->global_restrpos = 0;
	stats->global_BufferGetRelation = 0;
	stats->global_RelationIdGetRelation = 0;
	stats->global_RelationIdGetRelation_Buf = 0;
	stats->global_getreldesc = 0;
	stats->global_heapgettup = 0;
	stats->global_RelationPutHeapTuple = 0;
	stats->global_RelationPutLongHeapTuple = 0;

	stats->local_open = 0;
	stats->local_openr = 0;
	stats->local_close = 0;
	stats->local_beginscan = 0;
	stats->local_rescan = 0;
	stats->local_endscan = 0;
	stats->local_getnext = 0;
	stats->local_fetch = 0;
	stats->local_insert = 0;
	stats->local_delete = 0;
	stats->local_replace = 0;
	stats->local_markpos = 0;
	stats->local_restrpos = 0;
	stats->local_BufferGetRelation = 0;
	stats->local_RelationIdGetRelation = 0;
	stats->local_RelationIdGetRelation_Buf = 0;
	stats->local_getreldesc = 0;
	stats->local_heapgettup = 0;
	stats->local_RelationPutHeapTuple = 0;
	stats->local_RelationPutLongHeapTuple = 0;
	stats->local_RelationNameGetRelation = 0;
	stats->global_RelationNameGetRelation = 0;

	/* ----------------
	 *	record init times
	 * ----------------
	 */
	time(&stats->init_global_timestamp);
	time(&stats->local_reset_timestamp);
	time(&stats->last_request_timestamp);

	/* ----------------
	 *	return to old memory context
	 * ----------------
	 */
	MemoryContextSwitchTo(oldContext);

	heap_access_stats = stats;
}

#ifdef NOT_USED
/* ----------------
 *		ResetHeapAccessStatistics
 * ----------------
 */
void
ResetHeapAccessStatistics()
{
	HeapAccessStatistics stats;

	/* ----------------
	 *	do nothing if stats aren't initialized
	 * ----------------
	 */
	if (heap_access_stats == NULL)
		return;

	stats = heap_access_stats;

	/* ----------------
	 *	reset local counts
	 * ----------------
	 */
	stats->local_open = 0;
	stats->local_openr = 0;
	stats->local_close = 0;
	stats->local_beginscan = 0;
	stats->local_rescan = 0;
	stats->local_endscan = 0;
	stats->local_getnext = 0;
	stats->local_fetch = 0;
	stats->local_insert = 0;
	stats->local_delete = 0;
	stats->local_replace = 0;
	stats->local_markpos = 0;
	stats->local_restrpos = 0;
	stats->local_BufferGetRelation = 0;
	stats->local_RelationIdGetRelation = 0;
	stats->local_RelationIdGetRelation_Buf = 0;
	stats->local_getreldesc = 0;
	stats->local_heapgettup = 0;
	stats->local_RelationPutHeapTuple = 0;
	stats->local_RelationPutLongHeapTuple = 0;

	/* ----------------
	 *	reset local timestamps
	 * ----------------
	 */
	time(&stats->local_reset_timestamp);
	time(&stats->last_request_timestamp);
}

#endif

#ifdef NOT_USED
/* ----------------
 *		GetHeapAccessStatistics
 * ----------------
 */
HeapAccessStatistics
GetHeapAccessStatistics()
{
	HeapAccessStatistics stats;

	/* ----------------
	 *	return nothing if stats aren't initialized
	 * ----------------
	 */
	if (heap_access_stats == NULL)
		return NULL;

	/* ----------------
	 *	record the current request time
	 * ----------------
	 */
	time(&heap_access_stats->last_request_timestamp);

	/* ----------------
	 *	allocate a copy of the stats and return it to the caller.
	 * ----------------
	 */
	stats = (HeapAccessStatistics)
		palloc(sizeof(HeapAccessStatisticsData));

	memmove(stats,
			heap_access_stats,
			sizeof(HeapAccessStatisticsData));

	return stats;
}

#endif

#ifdef NOT_USED
/* ----------------
 *		PrintHeapAccessStatistics
 * ----------------
 */
void
PrintHeapAccessStatistics(HeapAccessStatistics stats)
{
	/* ----------------
	 *	return nothing if stats aren't valid
	 * ----------------
	 */
	if (stats == NULL)
		return;

	printf("======== heap am statistics ========\n");
	printf("init_global_timestamp:      %s",
		   ctime(&(stats->init_global_timestamp)));

	printf("local_reset_timestamp:      %s",
		   ctime(&(stats->local_reset_timestamp)));

	printf("last_request_timestamp:     %s",
		   ctime(&(stats->last_request_timestamp)));

	printf("local/global_open:                        %6d/%6d\n",
		   stats->local_open, stats->global_open);

	printf("local/global_openr:                       %6d/%6d\n",
		   stats->local_openr, stats->global_openr);

	printf("local/global_close:                       %6d/%6d\n",
		   stats->local_close, stats->global_close);

	printf("local/global_beginscan:                   %6d/%6d\n",
		   stats->local_beginscan, stats->global_beginscan);

	printf("local/global_rescan:                      %6d/%6d\n",
		   stats->local_rescan, stats->global_rescan);

	printf("local/global_endscan:                     %6d/%6d\n",
		   stats->local_endscan, stats->global_endscan);

	printf("local/global_getnext:                     %6d/%6d\n",
		   stats->local_getnext, stats->global_getnext);

	printf("local/global_fetch:                       %6d/%6d\n",
		   stats->local_fetch, stats->global_fetch);

	printf("local/global_insert:                      %6d/%6d\n",
		   stats->local_insert, stats->global_insert);

	printf("local/global_delete:                      %6d/%6d\n",
		   stats->local_delete, stats->global_delete);

	printf("local/global_replace:                     %6d/%6d\n",
		   stats->local_replace, stats->global_replace);

	printf("local/global_markpos:                     %6d/%6d\n",
		   stats->local_markpos, stats->global_markpos);

	printf("local/global_restrpos:                    %6d/%6d\n",
		   stats->local_restrpos, stats->global_restrpos);

	printf("================\n");

	printf("local/global_BufferGetRelation:             %6d/%6d\n",
		   stats->local_BufferGetRelation,
		   stats->global_BufferGetRelation);

	printf("local/global_RelationIdGetRelation:         %6d/%6d\n",
		   stats->local_RelationIdGetRelation,
		   stats->global_RelationIdGetRelation);

	printf("local/global_RelationIdGetRelation_Buf:     %6d/%6d\n",
		   stats->local_RelationIdGetRelation_Buf,
		   stats->global_RelationIdGetRelation_Buf);

	printf("local/global_getreldesc:                    %6d/%6d\n",
		   stats->local_getreldesc, stats->global_getreldesc);

	printf("local/global_heapgettup:                    %6d/%6d\n",
		   stats->local_heapgettup, stats->global_heapgettup);

	printf("local/global_RelationPutHeapTuple:          %6d/%6d\n",
		   stats->local_RelationPutHeapTuple,
		   stats->global_RelationPutHeapTuple);

	printf("local/global_RelationPutLongHeapTuple:      %6d/%6d\n",
		   stats->local_RelationPutLongHeapTuple,
		   stats->global_RelationPutLongHeapTuple);

	printf("===================================\n");

	printf("\n");
}

#endif

#ifdef NOT_USED
/* ----------------
 *		PrintAndFreeHeapAccessStatistics
 * ----------------
 */
void
PrintAndFreeHeapAccessStatistics(HeapAccessStatistics stats)
{
	PrintHeapAccessStatistics(stats);
	if (stats != NULL)
		pfree(stats);
}

#endif

/* ----------------------------------------------------------------
 *					access method initialization
 * ----------------------------------------------------------------
 */
/* ----------------
 *		initam should someday be moved someplace else.
 * ----------------
 */
void
initam(void)
{
	/* ----------------
	 *	initialize heap statistics.
	 * ----------------
	 */
	InitHeapAccessStatistics();
}
