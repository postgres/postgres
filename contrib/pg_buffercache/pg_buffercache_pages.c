/*-------------------------------------------------------------------------
 *
 * pg_buffercache_pages.c
 *	  display some contents of the buffer cache
 *
 *	  contrib/pg_buffercache/pg_buffercache_pages.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "port/pg_numa.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


#define NUM_BUFFERCACHE_PAGES_MIN_ELEM	8
#define NUM_BUFFERCACHE_PAGES_ELEM	9
#define NUM_BUFFERCACHE_SUMMARY_ELEM 5
#define NUM_BUFFERCACHE_USAGE_COUNTS_ELEM 4
#define NUM_BUFFERCACHE_EVICT_ELEM 2
#define NUM_BUFFERCACHE_EVICT_RELATION_ELEM 3
#define NUM_BUFFERCACHE_EVICT_ALL_ELEM 3

#define NUM_BUFFERCACHE_NUMA_ELEM	3

PG_MODULE_MAGIC_EXT(
					.name = "pg_buffercache",
					.version = PG_VERSION
);

/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{
	uint32		bufferid;
	RelFileNumber relfilenumber;
	Oid			reltablespace;
	Oid			reldatabase;
	ForkNumber	forknum;
	BlockNumber blocknum;
	bool		isvalid;
	bool		isdirty;
	uint16		usagecount;

	/*
	 * An int32 is sufficiently large, as MAX_BACKENDS prevents a buffer from
	 * being pinned by too many backends and each backend will only pin once
	 * because of bufmgr.c's PrivateRefCount infrastructure.
	 */
	int32		pinning_backends;
} BufferCachePagesRec;


/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{
	TupleDesc	tupdesc;
	BufferCachePagesRec *record;
} BufferCachePagesContext;

/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{
	uint32		bufferid;
	int64		page_num;
	int32		numa_node;
} BufferCacheNumaRec;

/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{
	TupleDesc	tupdesc;
	int			buffers_per_page;
	int			pages_per_buffer;
	int			os_page_size;
	BufferCacheNumaRec *record;
} BufferCacheNumaContext;


/*
 * Function returning data from the shared buffer cache - buffer number,
 * relation node/tablespace/database/blocknum and dirty indicator.
 */
PG_FUNCTION_INFO_V1(pg_buffercache_pages);
PG_FUNCTION_INFO_V1(pg_buffercache_numa_pages);
PG_FUNCTION_INFO_V1(pg_buffercache_summary);
PG_FUNCTION_INFO_V1(pg_buffercache_usage_counts);
PG_FUNCTION_INFO_V1(pg_buffercache_evict);
PG_FUNCTION_INFO_V1(pg_buffercache_evict_relation);
PG_FUNCTION_INFO_V1(pg_buffercache_evict_all);


/* Only need to touch memory once per backend process lifetime */
static bool firstNumaTouch = true;


Datum
pg_buffercache_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	BufferCachePagesContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;

	if (SRF_IS_FIRSTCALL())
	{
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (BufferCachePagesContext *) palloc(sizeof(BufferCachePagesContext));

		/*
		 * To smoothly support upgrades from version 1.0 of this extension
		 * transparently handle the (non-)existence of the pinning_backends
		 * column. We unfortunately have to get the result type for that... -
		 * we can't use the result type determined by the function definition
		 * without potentially crashing when somebody uses the old (or even
		 * wrong) function definition though.
		 */
		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts < NUM_BUFFERCACHE_PAGES_MIN_ELEM ||
			expected_tupledesc->natts > NUM_BUFFERCACHE_PAGES_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows. */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relforknumber",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 7, "isdirty",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 8, "usage_count",
						   INT2OID, -1, 0);

		if (expected_tupledesc->natts == NUM_BUFFERCACHE_PAGES_ELEM)
			TupleDescInitEntry(tupledesc, (AttrNumber) 9, "pinning_backends",
							   INT4OID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/* Allocate NBuffers worth of BufferCachePagesRec records. */
		fctx->record = (BufferCachePagesRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(BufferCachePagesRec) * NBuffers);

		/* Set max calls and remember the user function context. */
		funcctx->max_calls = NBuffers;
		funcctx->user_fctx = fctx;

		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *bufHdr;
			uint32		buf_state;

			bufHdr = GetBufferDescriptor(i);
			/* Lock each buffer header before inspecting. */
			buf_state = LockBufHdr(bufHdr);

			fctx->record[i].bufferid = BufferDescriptorGetBuffer(bufHdr);
			fctx->record[i].relfilenumber = BufTagGetRelNumber(&bufHdr->tag);
			fctx->record[i].reltablespace = bufHdr->tag.spcOid;
			fctx->record[i].reldatabase = bufHdr->tag.dbOid;
			fctx->record[i].forknum = BufTagGetForkNum(&bufHdr->tag);
			fctx->record[i].blocknum = bufHdr->tag.blockNum;
			fctx->record[i].usagecount = BUF_STATE_GET_USAGECOUNT(buf_state);
			fctx->record[i].pinning_backends = BUF_STATE_GET_REFCOUNT(buf_state);

			if (buf_state & BM_DIRTY)
				fctx->record[i].isdirty = true;
			else
				fctx->record[i].isdirty = false;

			/* Note if the buffer is valid, and has storage created */
			if ((buf_state & BM_VALID) && (buf_state & BM_TAG_VALID))
				fctx->record[i].isvalid = true;
			else
				fctx->record[i].isvalid = false;

			UnlockBufHdr(bufHdr, buf_state);
		}
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_BUFFERCACHE_PAGES_ELEM];
		bool		nulls[NUM_BUFFERCACHE_PAGES_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		nulls[0] = false;

		/*
		 * Set all fields except the bufferid to null if the buffer is unused
		 * or not valid.
		 */
		if (fctx->record[i].blocknum == InvalidBlockNumber ||
			fctx->record[i].isvalid == false)
		{
			nulls[1] = true;
			nulls[2] = true;
			nulls[3] = true;
			nulls[4] = true;
			nulls[5] = true;
			nulls[6] = true;
			nulls[7] = true;
			/* unused for v1.0 callers, but the array is always long enough */
			nulls[8] = true;
		}
		else
		{
			values[1] = ObjectIdGetDatum(fctx->record[i].relfilenumber);
			nulls[1] = false;
			values[2] = ObjectIdGetDatum(fctx->record[i].reltablespace);
			nulls[2] = false;
			values[3] = ObjectIdGetDatum(fctx->record[i].reldatabase);
			nulls[3] = false;
			values[4] = ObjectIdGetDatum(fctx->record[i].forknum);
			nulls[4] = false;
			values[5] = Int64GetDatum((int64) fctx->record[i].blocknum);
			nulls[5] = false;
			values[6] = BoolGetDatum(fctx->record[i].isdirty);
			nulls[6] = false;
			values[7] = Int16GetDatum(fctx->record[i].usagecount);
			nulls[7] = false;
			/* unused for v1.0 callers, but the array is always long enough */
			values[8] = Int32GetDatum(fctx->record[i].pinning_backends);
			nulls[8] = false;
		}

		/* Build and return the tuple. */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

/*
 * Inquire about NUMA memory mappings for shared buffers.
 *
 * Returns NUMA node ID for each memory page used by the buffer. Buffers may
 * be smaller or larger than OS memory pages. For each buffer we return one
 * entry for each memory page used by the buffer (if the buffer is smaller,
 * it only uses a part of one memory page).
 *
 * We expect both sizes (for buffers and memory pages) to be a power-of-2, so
 * one is always a multiple of the other.
 *
 * In order to get reliable results we also need to touch memory pages, so
 * that the inquiry about NUMA memory node doesn't return -2 (which indicates
 * unmapped/unallocated pages).
 */
Datum
pg_buffercache_numa_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	MemoryContext oldcontext;
	BufferCacheNumaContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		int			i,
					idx;
		Size		os_page_size;
		void	  **os_page_ptrs;
		int		   *os_page_status;
		uint64		os_page_count;
		int			pages_per_buffer;
		int			max_entries;
		volatile uint64 touch pg_attribute_unused();
		char	   *startptr,
				   *endptr;

		if (pg_numa_init() == -1)
			elog(ERROR, "libnuma initialization failed or NUMA is not supported on this platform");

		/*
		 * The database block size and OS memory page size are unlikely to be
		 * the same. The block size is 1-32KB, the memory page size depends on
		 * platform. On x86 it's usually 4KB, on ARM it's 4KB or 64KB, but
		 * there are also features like THP etc. Moreover, we don't quite know
		 * how the pages and buffers "align" in memory - the buffers may be
		 * shifted in some way, using more memory pages than necessary.
		 *
		 * So we need to be careful about mapping buffers to memory pages. We
		 * calculate the maximum number of pages a buffer might use, so that
		 * we allocate enough space for the entries. And then we count the
		 * actual number of entries as we scan the buffers.
		 *
		 * This information is needed before calling move_pages() for NUMA
		 * node id inquiry.
		 */
		os_page_size = pg_get_shmem_pagesize();

		/*
		 * The pages and block size is expected to be 2^k, so one divides the
		 * other (we don't know in which direction). This does not say
		 * anything about relative alignment of pages/buffers.
		 */
		Assert((os_page_size % BLCKSZ == 0) || (BLCKSZ % os_page_size == 0));

		/*
		 * How many addresses we are going to query? Simply get the page for
		 * the first buffer, and first page after the last buffer, and count
		 * the pages from that.
		 */
		startptr = (char *) TYPEALIGN_DOWN(os_page_size,
										   BufferGetBlock(1));
		endptr = (char *) TYPEALIGN(os_page_size,
									(char *) BufferGetBlock(NBuffers) + BLCKSZ);
		os_page_count = (endptr - startptr) / os_page_size;

		/* Used to determine the NUMA node for all OS pages at once */
		os_page_ptrs = palloc0(sizeof(void *) * os_page_count);
		os_page_status = palloc(sizeof(uint64) * os_page_count);

		/* Fill pointers for all the memory pages. */
		idx = 0;
		for (char *ptr = startptr; ptr < endptr; ptr += os_page_size)
		{
			os_page_ptrs[idx++] = ptr;

			/* Only need to touch memory once per backend process lifetime */
			if (firstNumaTouch)
				pg_numa_touch_mem_if_required(touch, ptr);
		}

		Assert(idx == os_page_count);

		elog(DEBUG1, "NUMA: NBuffers=%d os_page_count=" UINT64_FORMAT " "
			 "os_page_size=%zu", NBuffers, os_page_count, os_page_size);

		/*
		 * If we ever get 0xff back from kernel inquiry, then we probably have
		 * bug in our buffers to OS page mapping code here.
		 */
		memset(os_page_status, 0xff, sizeof(int) * os_page_count);

		/* Query NUMA status for all the pointers */
		if (pg_numa_query_pages(0, os_page_count, os_page_ptrs, os_page_status) == -1)
			elog(ERROR, "failed NUMA pages inquiry: %m");

		/* Initialize the multi-call context, load entries about buffers */

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (BufferCacheNumaContext *) palloc(sizeof(BufferCacheNumaContext));

		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts != NUM_BUFFERCACHE_NUMA_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows. */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "os_page_num",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "numa_node",
						   INT4OID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/*
		 * Each buffer needs at least one entry, but it might be offset in
		 * some way, and use one extra entry. So we allocate space for the
		 * maximum number of entries we might need, and then count the exact
		 * number as we're walking buffers. That way we can do it in one pass,
		 * without reallocating memory.
		 */
		pages_per_buffer = Max(1, BLCKSZ / os_page_size) + 1;
		max_entries = NBuffers * pages_per_buffer;

		/* Allocate entries for BufferCachePagesRec records. */
		fctx->record = (BufferCacheNumaRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(BufferCacheNumaRec) * max_entries);

		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);

		if (firstNumaTouch)
			elog(DEBUG1, "NUMA: page-faulting the buffercache for proper NUMA readouts");

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 *
		 * This loop touches and stores addresses into os_page_ptrs[] as input
		 * to one big move_pages(2) inquiry system call. Basically we ask for
		 * all memory pages for NBuffers.
		 */
		startptr = (char *) TYPEALIGN_DOWN(os_page_size, (char *) BufferGetBlock(1));
		idx = 0;
		for (i = 0; i < NBuffers; i++)
		{
			char	   *buffptr = (char *) BufferGetBlock(i + 1);
			BufferDesc *bufHdr;
			uint32		buf_state;
			uint32		bufferid;
			int32		page_num;
			char	   *startptr_buff,
					   *endptr_buff;

			CHECK_FOR_INTERRUPTS();

			bufHdr = GetBufferDescriptor(i);

			/* Lock each buffer header before inspecting. */
			buf_state = LockBufHdr(bufHdr);
			bufferid = BufferDescriptorGetBuffer(bufHdr);
			UnlockBufHdr(bufHdr, buf_state);

			/* start of the first page of this buffer */
			startptr_buff = (char *) TYPEALIGN_DOWN(os_page_size, buffptr);

			/* end of the buffer (no need to align to memory page) */
			endptr_buff = buffptr + BLCKSZ;

			Assert(startptr_buff < endptr_buff);

			/* calculate ID of the first page for this buffer */
			page_num = (startptr_buff - startptr) / os_page_size;

			/* Add an entry for each OS page overlapping with this buffer. */
			for (char *ptr = startptr_buff; ptr < endptr_buff; ptr += os_page_size)
			{
				fctx->record[idx].bufferid = bufferid;
				fctx->record[idx].page_num = page_num;
				fctx->record[idx].numa_node = os_page_status[page_num];

				/* advance to the next entry/page */
				++idx;
				++page_num;
			}
		}

		Assert((idx >= os_page_count) && (idx <= max_entries));

		/* Set max calls and remember the user function context. */
		funcctx->max_calls = idx;
		funcctx->user_fctx = fctx;

		/* Remember this backend touched the pages */
		firstNumaTouch = false;
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_BUFFERCACHE_NUMA_ELEM];
		bool		nulls[NUM_BUFFERCACHE_NUMA_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		nulls[0] = false;

		values[1] = Int64GetDatum(fctx->record[i].page_num);
		nulls[1] = false;

		values[2] = Int32GetDatum(fctx->record[i].numa_node);
		nulls[2] = false;

		/* Build and return the tuple. */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

Datum
pg_buffercache_summary(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_SUMMARY_ELEM];
	bool		nulls[NUM_BUFFERCACHE_SUMMARY_ELEM];

	int32		buffers_used = 0;
	int32		buffers_unused = 0;
	int32		buffers_dirty = 0;
	int32		buffers_pinned = 0;
	int64		usagecount_total = 0;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr;
		uint32		buf_state;

		/*
		 * This function summarizes the state of all headers. Locking the
		 * buffer headers wouldn't provide an improved result as the state of
		 * the buffer can still change after we release the lock and it'd
		 * noticeably increase the cost of the function.
		 */
		bufHdr = GetBufferDescriptor(i);
		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if (buf_state & BM_VALID)
		{
			buffers_used++;
			usagecount_total += BUF_STATE_GET_USAGECOUNT(buf_state);

			if (buf_state & BM_DIRTY)
				buffers_dirty++;
		}
		else
			buffers_unused++;

		if (BUF_STATE_GET_REFCOUNT(buf_state) > 0)
			buffers_pinned++;
	}

	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum(buffers_used);
	values[1] = Int32GetDatum(buffers_unused);
	values[2] = Int32GetDatum(buffers_dirty);
	values[3] = Int32GetDatum(buffers_pinned);

	if (buffers_used != 0)
		values[4] = Float8GetDatum((double) usagecount_total / buffers_used);
	else
		nulls[4] = true;

	/* Build and return the tuple. */
	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

Datum
pg_buffercache_usage_counts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			usage_counts[BM_MAX_USAGE_COUNT + 1] = {0};
	int			dirty[BM_MAX_USAGE_COUNT + 1] = {0};
	int			pinned[BM_MAX_USAGE_COUNT + 1] = {0};
	Datum		values[NUM_BUFFERCACHE_USAGE_COUNTS_ELEM];
	bool		nulls[NUM_BUFFERCACHE_USAGE_COUNTS_ELEM] = {0};

	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);
		int			usage_count;

		usage_count = BUF_STATE_GET_USAGECOUNT(buf_state);
		usage_counts[usage_count]++;

		if (buf_state & BM_DIRTY)
			dirty[usage_count]++;

		if (BUF_STATE_GET_REFCOUNT(buf_state) > 0)
			pinned[usage_count]++;
	}

	for (int i = 0; i < BM_MAX_USAGE_COUNT + 1; i++)
	{
		values[0] = Int32GetDatum(i);
		values[1] = Int32GetDatum(usage_counts[i]);
		values[2] = Int32GetDatum(dirty[i]);
		values[3] = Int32GetDatum(pinned[i]);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * Helper function to check if the user has superuser privileges.
 */
static void
pg_buffercache_superuser_check(char *func_name)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use %s()",
						func_name)));
}

/*
 * Try to evict a shared buffer.
 */
Datum
pg_buffercache_evict(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_EVICT_ELEM];
	bool		nulls[NUM_BUFFERCACHE_EVICT_ELEM] = {0};

	Buffer		buf = PG_GETARG_INT32(0);
	bool		buffer_flushed;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pg_buffercache_superuser_check("pg_buffercache_evict");

	if (buf < 1 || buf > NBuffers)
		elog(ERROR, "bad buffer ID: %d", buf);

	values[0] = BoolGetDatum(EvictUnpinnedBuffer(buf, &buffer_flushed));
	values[1] = BoolGetDatum(buffer_flushed);

	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * Try to evict specified relation.
 */
Datum
pg_buffercache_evict_relation(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_EVICT_RELATION_ELEM];
	bool		nulls[NUM_BUFFERCACHE_EVICT_RELATION_ELEM] = {0};

	Oid			relOid;
	Relation	rel;

	int32		buffers_evicted = 0;
	int32		buffers_flushed = 0;
	int32		buffers_skipped = 0;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pg_buffercache_superuser_check("pg_buffercache_evict_relation");

	relOid = PG_GETARG_OID(0);

	rel = relation_open(relOid, AccessShareLock);

	if (RelationUsesLocalBuffers(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation uses local buffers, %s() is intended to be used for shared buffers only",
						"pg_buffercache_evict_relation")));

	EvictRelUnpinnedBuffers(rel, &buffers_evicted, &buffers_flushed,
							&buffers_skipped);

	relation_close(rel, AccessShareLock);

	values[0] = Int32GetDatum(buffers_evicted);
	values[1] = Int32GetDatum(buffers_flushed);
	values[2] = Int32GetDatum(buffers_skipped);

	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}


/*
 * Try to evict all shared buffers.
 */
Datum
pg_buffercache_evict_all(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_EVICT_ALL_ELEM];
	bool		nulls[NUM_BUFFERCACHE_EVICT_ALL_ELEM] = {0};

	int32		buffers_evicted = 0;
	int32		buffers_flushed = 0;
	int32		buffers_skipped = 0;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pg_buffercache_superuser_check("pg_buffercache_evict_all");

	EvictAllUnpinnedBuffers(&buffers_evicted, &buffers_flushed,
							&buffers_skipped);

	values[0] = Int32GetDatum(buffers_evicted);
	values[1] = Int32GetDatum(buffers_flushed);
	values[2] = Int32GetDatum(buffers_skipped);

	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}
