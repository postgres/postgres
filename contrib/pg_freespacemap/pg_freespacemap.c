/*-------------------------------------------------------------------------
 *
 * pg_freespacemap.c
 *	  display some contents of the free space map.
 *
 *	  $PostgreSQL: pgsql/contrib/pg_freespacemap/pg_freespacemap.c,v 1.1 2006/02/12 03:55:53 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "storage/freespace.h"
#include "utils/relcache.h"

#define		NUM_FREESPACE_PAGES_ELEM 	6

#if defined(WIN32) || defined(__CYGWIN__)
extern DLLIMPORT volatile uint32 InterruptHoldoffCount;
#endif

Datum		pg_freespacemap(PG_FUNCTION_ARGS);


/*
 * Record structure holding the to be exposed free space data.
 */
typedef struct
{

	uint32				blockid;
	uint32				relfilenode;
	uint32				reltablespace;
	uint32				reldatabase;
	uint32				relblocknumber;
	uint32				blockfreebytes;

}	FreeSpacePagesRec;


/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{

	AttInMetadata 		*attinmeta;
	FreeSpacePagesRec	*record;
	char	   			*values[NUM_FREESPACE_PAGES_ELEM];

}	FreeSpacePagesContext;


/*
 * Function returning data from the Free Space Map (FSM).
 */
PG_FUNCTION_INFO_V1(pg_freespacemap);
Datum
pg_freespacemap(PG_FUNCTION_ARGS)
{

	FuncCallContext			*funcctx;
	Datum					result;
	MemoryContext 			oldcontext;
	FreeSpacePagesContext	*fctx;				/* User function context. */
	TupleDesc				tupledesc;
	HeapTuple				tuple;

	FSMHeader				*FreeSpaceMap; 		/* FSM main structure. */
	FSMRelation				*fsmrel;			/* Individual relation. */


	if (SRF_IS_FIRSTCALL())
	{
		uint32				i;
		uint32				numPages;	/* Max possible no. of pages in map. */
		int					nPages;		/* Mapped pages for a relation. */
		
		/*
		 * Get the free space map data structure.
		 */
		FreeSpaceMap = GetFreeSpaceMap();

		numPages = MaxFSMPages;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Construct a tuple to return. */
		tupledesc = CreateTemplateTupleDesc(NUM_FREESPACE_PAGES_ELEM, false);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "blockid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "blockfreebytes",
						   INT4OID, -1, 0);

		/* Generate attribute metadata needed later to produce tuples */
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupledesc);

		/*
		 * Create a function context for cross-call persistence and initialize
		 * the counters.
		 */
		fctx = (FreeSpacePagesContext *) palloc(sizeof(FreeSpacePagesContext));
		funcctx->user_fctx = fctx;

		/* Set an upper bound on the calls */
		funcctx->max_calls = numPages;	


		/* Allocate numPages worth of FreeSpacePagesRec records, this is also
		 * an upper bound.
		 */
		fctx->record = (FreeSpacePagesRec *) palloc(sizeof(FreeSpacePagesRec) * numPages);

		/* allocate the strings for tuple formation */
		fctx->values[0] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[1] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[2] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[3] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[4] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[5] = (char *) palloc(3 * sizeof(uint32) + 1);


		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);


		/*
		 * Lock free space map and scan though all the relations, 
		 * for each relation, gets all its mapped pages.
		 */
		LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);


		i = 0;

		for (fsmrel = FreeSpaceMap->usageList; fsmrel; fsmrel = fsmrel->nextUsage) 
		{

			if (fsmrel->isIndex)	
			{	/* Index relation. */
				IndexFSMPageData *page;

				page = (IndexFSMPageData *)
						(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);

				for (nPages = 0; nPages < fsmrel->storedPages; nPages++)
				{

					fctx->record[i].blockid = i;
					fctx->record[i].relfilenode = fsmrel->key.relNode;
					fctx->record[i].reltablespace = fsmrel->key.spcNode;
					fctx->record[i].reldatabase = fsmrel->key.dbNode;
					fctx->record[i].relblocknumber = IndexFSMPageGetPageNum(page);	
					fctx->record[i].blockfreebytes = 0;	/* index.*/

					page++;
					i++;
				}
			}
			else
			{	/* Heap relation. */
				FSMPageData *page;

				page = (FSMPageData *)
						(FreeSpaceMap->arena + fsmrel->firstChunk * CHUNKBYTES);

				for (nPages = 0; nPages < fsmrel->storedPages; nPages++)
				{
					fctx->record[i].blockid = i;
					fctx->record[i].relfilenode = fsmrel->key.relNode;
					fctx->record[i].reltablespace = fsmrel->key.spcNode;
					fctx->record[i].reldatabase = fsmrel->key.dbNode;
					fctx->record[i].relblocknumber = FSMPageGetPageNum(page);
					fctx->record[i].blockfreebytes = FSMPageGetSpace(page);	
					
					page++;
					i++;
				}
			
			}

		}

		/* Set the real no. of calls as we know it now! */
		funcctx->max_calls = i;

		/* Release free space map. */
		LWLockRelease(FreeSpaceLock);
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;


	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;


		sprintf(fctx->values[0], "%u", fctx->record[i].blockid);
		sprintf(fctx->values[1], "%u", fctx->record[i].relfilenode);
		sprintf(fctx->values[2], "%u", fctx->record[i].reltablespace);
		sprintf(fctx->values[3], "%u", fctx->record[i].reldatabase);
		sprintf(fctx->values[4], "%u", fctx->record[i].relblocknumber);
		sprintf(fctx->values[5], "%u", fctx->record[i].blockfreebytes);



		/* Build and return the tuple. */
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, fctx->values);
		result = HeapTupleGetDatum(tuple);


		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);

}
