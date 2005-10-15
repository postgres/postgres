/*-------------------------------------------------------------------------
 *
 * pg_buffercache_pages.c
 *	  display some contents of the buffer cache
 *
 *	  $PostgreSQL: pgsql/contrib/pg_buffercache/pg_buffercache_pages.c,v 1.6 2005/10/15 02:49:05 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/relcache.h"


#define NUM_BUFFERCACHE_PAGES_ELEM	6

#if defined(WIN32) || defined(__CYGWIN__)
extern DLLIMPORT BufferDesc *BufferDescriptors;
extern DLLIMPORT volatile uint32 InterruptHoldoffCount;
#endif

Datum		pg_buffercache_pages(PG_FUNCTION_ARGS);


/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{

	uint32		bufferid;
	Oid			relfilenode;
	Oid			reltablespace;
	Oid			reldatabase;
	BlockNumber blocknum;
	bool		isvalid;
	bool		isdirty;

}	BufferCachePagesRec;


/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{

	AttInMetadata *attinmeta;
	BufferCachePagesRec *record;
	char	   *values[NUM_BUFFERCACHE_PAGES_ELEM];

}	BufferCachePagesContext;


/*
 * Function returning data from the shared buffer cache - buffer number,
 * relation node/tablespace/database/blocknum and dirty indicator.
 */
PG_FUNCTION_INFO_V1(pg_buffercache_pages);
Datum
pg_buffercache_pages(PG_FUNCTION_ARGS)
{

	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	BufferCachePagesContext *fctx;		/* User function context. */
	TupleDesc	tupledesc;
	HeapTuple	tuple;

	if (SRF_IS_FIRSTCALL())
	{
		uint32		i;
		volatile BufferDesc *bufHdr;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Construct a tuple to return. */
		tupledesc = CreateTemplateTupleDesc(NUM_BUFFERCACHE_PAGES_ELEM, false);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "isdirty",
						   BOOLOID, -1, 0);

		/* Generate attribute metadata needed later to produce tuples */
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupledesc);

		/*
		 * Create a function context for cross-call persistence and initialize
		 * the buffer counters.
		 */
		fctx = (BufferCachePagesContext *) palloc(sizeof(BufferCachePagesContext));
		funcctx->max_calls = NBuffers;
		funcctx->user_fctx = fctx;


		/* Allocate NBuffers worth of BufferCachePagesRec records. */
		fctx->record = (BufferCachePagesRec *) palloc(sizeof(BufferCachePagesRec) * NBuffers);

		/* allocate the strings for tuple formation */
		fctx->values[0] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[1] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[2] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[3] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[4] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[5] = (char *) palloc(2);


		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);


		/*
		 * Lock Buffer map and scan though all the buffers, saving the
		 * relevant fields in the fctx->record structure.
		 */
		LWLockAcquire(BufMappingLock, LW_SHARED);

		for (i = 0, bufHdr = BufferDescriptors; i < NBuffers; i++, bufHdr++)
		{
			/* Lock each buffer header before inspecting. */
			LockBufHdr(bufHdr);

			fctx->record[i].bufferid = BufferDescriptorGetBuffer(bufHdr);
			fctx->record[i].relfilenode = bufHdr->tag.rnode.relNode;
			fctx->record[i].reltablespace = bufHdr->tag.rnode.spcNode;
			fctx->record[i].reldatabase = bufHdr->tag.rnode.dbNode;
			fctx->record[i].blocknum = bufHdr->tag.blockNum;

			if (bufHdr->flags & BM_DIRTY)
				fctx->record[i].isdirty = true;
			else
				fctx->record[i].isdirty = false;

			/* Note if the buffer is valid, and has storage created */
			if ((bufHdr->flags & BM_VALID) && (bufHdr->flags & BM_TAG_VALID))
				fctx->record[i].isvalid = true;
			else
				fctx->record[i].isvalid = false;

			UnlockBufHdr(bufHdr);
		}

		/* Release Buffer map. */
		LWLockRelease(BufMappingLock);
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;


	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		char	   *values[NUM_BUFFERCACHE_PAGES_ELEM];
		int			j;

		/*
		 * Use a temporary values array, initially pointing to fctx->values,
		 * so it can be reassigned w/o losing the storage for subsequent
		 * calls.
		 */
		for (j = 0; j < NUM_BUFFERCACHE_PAGES_ELEM; j++)
		{
			values[j] = fctx->values[j];
		}


		/*
		 * Set all fields except the bufferid to null if the buffer is unused
		 * or not valid.
		 */
		if (fctx->record[i].blocknum == InvalidBlockNumber ||
			fctx->record[i].isvalid == false)
		{

			sprintf(values[0], "%u", fctx->record[i].bufferid);
			values[1] = NULL;
			values[2] = NULL;
			values[3] = NULL;
			values[4] = NULL;
			values[5] = NULL;

		}
		else
		{

			sprintf(values[0], "%u", fctx->record[i].bufferid);
			sprintf(values[1], "%u", fctx->record[i].relfilenode);
			sprintf(values[2], "%u", fctx->record[i].reltablespace);
			sprintf(values[3], "%u", fctx->record[i].reldatabase);
			sprintf(values[4], "%u", fctx->record[i].blocknum);
			if (fctx->record[i].isdirty)
			{
				strcpy(values[5], "t");
			}
			else
			{
				strcpy(values[5], "f");
			}

		}


		/* Build and return the tuple. */
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);


		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);

}
