/*-------------------------------------------------------------------------
 *
 * pg_freespacemap.c
 *	  display some contents of the free space relation and page maps.
 *
 *	  $PostgreSQL: pgsql/contrib/pg_freespacemap/pg_freespacemap.c,v 1.4 2006/04/26 22:46:09 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "storage/freespace.h"
#include "utils/relcache.h"

#define		NUM_FREESPACE_PAGES_ELEM 	5
#define		NUM_FREESPACE_RELATIONS_ELEM 	6

#if defined(WIN32) || defined(__CYGWIN__)
/* Need DLLIMPORT for some things that are not so marked in main headers */
extern DLLIMPORT int	MaxFSMPages;
extern DLLIMPORT int	MaxFSMRelations;
extern DLLIMPORT volatile uint32 InterruptHoldoffCount;
#endif

Datum		pg_freespacemap_pages(PG_FUNCTION_ARGS);
Datum		pg_freespacemap_relations(PG_FUNCTION_ARGS);


/*
 * Record structure holding the to be exposed free space page data.
 */
typedef struct
{

	uint32				reltablespace;
	uint32				reldatabase;
	uint32				relfilenode;
	uint32				relblocknumber;
	uint32				bytes;
	bool				isindex;

}	FreeSpacePagesRec;


/*
 * Record structure holding the to be exposed free space relation data.
 */
typedef struct
{

	uint32				reltablespace;
	uint32				reldatabase;
	uint32				relfilenode;
	int64				avgrequest;
	int					lastpagecount;
	int					nextpage;

}	FreeSpaceRelationsRec;



/*
 * Function context for page data persisting over repeated calls.
 */
typedef struct
{

	AttInMetadata 		*attinmeta;
	FreeSpacePagesRec	*record;
	char	   			*values[NUM_FREESPACE_PAGES_ELEM];

}	FreeSpacePagesContext;


/*
 * Function context for relation data persisting over repeated calls.
 */
typedef struct
{

	AttInMetadata 		*attinmeta;
	FreeSpaceRelationsRec	*record;
	char	   			*values[NUM_FREESPACE_RELATIONS_ELEM];

}	FreeSpaceRelationsContext;


/*
 * Function returning page data from the Free Space Map (FSM).
 */
PG_FUNCTION_INFO_V1(pg_freespacemap_pages);
Datum
pg_freespacemap_pages(PG_FUNCTION_ARGS)
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
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "bytes",
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

					fctx->record[i].reltablespace = fsmrel->key.spcNode;
					fctx->record[i].reldatabase = fsmrel->key.dbNode;
					fctx->record[i].relfilenode = fsmrel->key.relNode;
					fctx->record[i].relblocknumber = IndexFSMPageGetPageNum(page);	
					fctx->record[i].bytes = 0;	
					fctx->record[i].isindex = true;	

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
					fctx->record[i].reltablespace = fsmrel->key.spcNode;
					fctx->record[i].reldatabase = fsmrel->key.dbNode;
					fctx->record[i].relfilenode = fsmrel->key.relNode;
					fctx->record[i].relblocknumber = FSMPageGetPageNum(page);
					fctx->record[i].bytes = FSMPageGetSpace(page);	
					fctx->record[i].isindex = false;	
					
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
		char		*values[NUM_FREESPACE_PAGES_ELEM];
		int			j;

		/*
		 * Use a temporary values array, initially pointing to fctx->values,
		 * so it can be reassigned w/o losing the storage for subsequent
		 * calls.
		 */
		for (j = 0; j < NUM_FREESPACE_PAGES_ELEM; j++)
		{
			values[j] = fctx->values[j];
		}


		sprintf(values[0], "%u", fctx->record[i].reltablespace);
		sprintf(values[1], "%u", fctx->record[i].reldatabase);
		sprintf(values[2], "%u", fctx->record[i].relfilenode);
		sprintf(values[3], "%u", fctx->record[i].relblocknumber);


		/*
		 * Set (free) bytes to NULL for an index relation.
		 */
		if (fctx->record[i].isindex == true)
		{
			values[4] = NULL;
		}
		else
		{
			sprintf(values[4], "%u", fctx->record[i].bytes);
		}


		/* Build and return the tuple. */
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);


		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);

}


/*
 * Function returning relation data from the Free Space Map (FSM).
 */
PG_FUNCTION_INFO_V1(pg_freespacemap_relations);
Datum
pg_freespacemap_relations(PG_FUNCTION_ARGS)
{

	FuncCallContext			*funcctx;
	Datum					result;
	MemoryContext 			oldcontext;
	FreeSpaceRelationsContext	*fctx;				/* User function context. */
	TupleDesc				tupledesc;
	HeapTuple				tuple;

	FSMHeader				*FreeSpaceMap; 		/* FSM main structure. */
	FSMRelation				*fsmrel;			/* Individual relation. */


	if (SRF_IS_FIRSTCALL())
	{
		uint32				i;
		uint32				numRelations;	/* Max no. of Relations in map. */
		
		/*
		 * Get the free space map data structure.
		 */
		FreeSpaceMap = GetFreeSpaceMap();

		numRelations = MaxFSMRelations;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Construct a tuple to return. */
		tupledesc = CreateTemplateTupleDesc(NUM_FREESPACE_RELATIONS_ELEM, false);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "avgrequest",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "lastpageCount",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "nextpage",
						   INT4OID, -1, 0);

		/* Generate attribute metadata needed later to produce tuples */
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupledesc);

		/*
		 * Create a function context for cross-call persistence and initialize
		 * the counters.
		 */
		fctx = (FreeSpaceRelationsContext *) palloc(sizeof(FreeSpaceRelationsContext));
		funcctx->user_fctx = fctx;

		/* Set an upper bound on the calls */
		funcctx->max_calls = numRelations;	


		/* Allocate numRelations worth of FreeSpaceRelationsRec records, 
		 * this is also an upper bound.
		 */
		fctx->record = (FreeSpaceRelationsRec *) palloc(sizeof(FreeSpaceRelationsRec) * numRelations);

		/* allocate the strings for tuple formation */
		fctx->values[0] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[1] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[2] = (char *) palloc(3 * sizeof(uint32) + 1);
		fctx->values[3] = (char *) palloc(3 * sizeof(int64) + 1);
		fctx->values[4] = (char *) palloc(3 * sizeof(int32) + 1);
		fctx->values[5] = (char *) palloc(3 * sizeof(int32) + 1);


		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);


		/*
		 * Lock free space map and scan though all the relations, 
		 */
		LWLockAcquire(FreeSpaceLock, LW_EXCLUSIVE);


		i = 0;

		for (fsmrel = FreeSpaceMap->usageList; fsmrel; fsmrel = fsmrel->nextUsage) 
		{

			fctx->record[i].reltablespace = fsmrel->key.spcNode;
			fctx->record[i].reldatabase = fsmrel->key.dbNode;
			fctx->record[i].relfilenode = fsmrel->key.relNode;
			fctx->record[i].avgrequest = (int64)fsmrel->avgRequest;
			fctx->record[i].lastpagecount = fsmrel->lastPageCount;
			fctx->record[i].nextpage = fsmrel->nextPage;

			i++;


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
		char		*values[NUM_FREESPACE_RELATIONS_ELEM];
		int			j;

		/*
		 * Use a temporary values array, initially pointing to fctx->values,
		 * so it can be reassigned w/o losing the storage for subsequent
		 * calls.
		 */
		for (j = 0; j < NUM_FREESPACE_RELATIONS_ELEM; j++)
		{
			values[j] = fctx->values[j];
		}


		sprintf(values[0], "%u", fctx->record[i].reltablespace);
		sprintf(values[1], "%u", fctx->record[i].reldatabase);
		sprintf(values[2], "%u", fctx->record[i].relfilenode);
		sprintf(values[3], INT64_FORMAT, fctx->record[i].avgrequest);
		sprintf(values[4], "%d", fctx->record[i].lastpagecount);
		sprintf(values[5], "%d", fctx->record[i].nextpage);



		/* Build and return the tuple. */
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);


		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);


}
