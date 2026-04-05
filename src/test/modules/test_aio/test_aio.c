/*-------------------------------------------------------------------------
 *
 * test_aio.c
 *		Helpers to write tests for AIO
 *
 * This module provides interface functions for C functionality to SQL, to
 * make it possible to test AIO related behavior in a targeted way from SQL.
 * It'd not generally be safe to export these functions to SQL, but for a test
 * that's fine.
 *
 * Copyright (c) 2020-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_aio/test_aio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/checksum.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procnumber.h"
#include "storage/read_stream.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"


PG_MODULE_MAGIC;


/* In shared memory */
typedef struct InjIoErrorState
{
	ConditionVariable cv;

	bool		enabled_short_read;
	bool		enabled_reopen;

	bool		enabled_completion_wait;
	Oid			completion_wait_relfilenode;
	BlockNumber completion_wait_blockno;
	pid_t		completion_wait_pid;
	uint32		completion_wait_event;

	bool		short_read_result_set;
	Oid			short_read_relfilenode;
	pid_t		short_read_pid;
	int			short_read_result;
} InjIoErrorState;

typedef struct BlocksReadStreamData
{
	int			nblocks;
	int			curblock;
	uint32	   *blocks;
} BlocksReadStreamData;


static InjIoErrorState *inj_io_error_state;

/* Shared memory init callbacks */
static void test_aio_shmem_request(void *arg);
static void test_aio_shmem_init(void *arg);
static void test_aio_shmem_attach(void *arg);

static const ShmemCallbacks inj_io_shmem_callbacks = {
	.request_fn = test_aio_shmem_request,
	.init_fn = test_aio_shmem_init,
	.attach_fn = test_aio_shmem_attach,
};


static PgAioHandle *last_handle;



static void
test_aio_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "test_aio injection points",
					   .size = sizeof(InjIoErrorState),
					   .ptr = (void **) &inj_io_error_state,
		);
}

static void
test_aio_shmem_init(void *arg)
{
	/* First time through, initialize */
	inj_io_error_state->enabled_short_read = false;
	inj_io_error_state->enabled_reopen = false;
	inj_io_error_state->enabled_completion_wait = false;

	ConditionVariableInit(&inj_io_error_state->cv);
	inj_io_error_state->completion_wait_event = WaitEventInjectionPointNew("completion_wait");

#ifdef USE_INJECTION_POINTS
	InjectionPointAttach("aio-process-completion-before-shared",
						 "test_aio",
						 "inj_io_completion_hook",
						 NULL,
						 0);
	InjectionPointLoad("aio-process-completion-before-shared");

	InjectionPointAttach("aio-worker-after-reopen",
						 "test_aio",
						 "inj_io_reopen",
						 NULL,
						 0);
	InjectionPointLoad("aio-worker-after-reopen");

#endif
}

static void
test_aio_shmem_attach(void *arg)
{
	/*
	 * Pre-load the injection points now, so we can call them in a critical
	 * section.
	 */
#ifdef USE_INJECTION_POINTS
	InjectionPointLoad("aio-process-completion-before-shared");
	InjectionPointLoad("aio-worker-after-reopen");
	elog(LOG, "injection point loaded");
#endif
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	RegisterShmemCallbacks(&inj_io_shmem_callbacks);
}


PG_FUNCTION_INFO_V1(errno_from_string);
Datum
errno_from_string(PG_FUNCTION_ARGS)
{
	const char *sym = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (strcmp(sym, "EIO") == 0)
		PG_RETURN_INT32(EIO);
	else if (strcmp(sym, "EAGAIN") == 0)
		PG_RETURN_INT32(EAGAIN);
	else if (strcmp(sym, "EINTR") == 0)
		PG_RETURN_INT32(EINTR);
	else if (strcmp(sym, "ENOSPC") == 0)
		PG_RETURN_INT32(ENOSPC);
	else if (strcmp(sym, "EROFS") == 0)
		PG_RETURN_INT32(EROFS);

	ereport(ERROR,
			errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg_internal("%s is not a supported errno value", sym));
	PG_RETURN_INT32(0);
}

PG_FUNCTION_INFO_V1(grow_rel);
Datum
grow_rel(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		nblocks = PG_GETARG_UINT32(1);
	Relation	rel;
#define MAX_BUFFERS_TO_EXTEND_BY 64
	Buffer		victim_buffers[MAX_BUFFERS_TO_EXTEND_BY];

	rel = relation_open(relid, AccessExclusiveLock);

	while (nblocks > 0)
	{
		uint32		extend_by_pages;

		extend_by_pages = Min(nblocks, MAX_BUFFERS_TO_EXTEND_BY);

		ExtendBufferedRelBy(BMR_REL(rel),
							MAIN_FORKNUM,
							NULL,
							0,
							extend_by_pages,
							victim_buffers,
							&extend_by_pages);

		nblocks -= extend_by_pages;

		for (uint32 i = 0; i < extend_by_pages; i++)
		{
			ReleaseBuffer(victim_buffers[i]);
		}
	}

	relation_close(rel, NoLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(modify_rel_block);
Datum
modify_rel_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blkno = PG_GETARG_UINT32(1);
	bool		zero = PG_GETARG_BOOL(2);
	bool		corrupt_header = PG_GETARG_BOOL(3);
	bool		corrupt_checksum = PG_GETARG_BOOL(4);
	Page		page = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
	bool		flushed;
	Relation	rel;
	Buffer		buf;
	PageHeader	ph;

	rel = relation_open(relid, AccessExclusiveLock);

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
							 RBM_ZERO_ON_ERROR, NULL);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * copy the page to local memory, seems nicer than to directly modify in
	 * the buffer pool.
	 */
	memcpy(page, BufferGetPage(buf), BLCKSZ);

	UnlockReleaseBuffer(buf);

	/*
	 * Don't want to have a buffer in-memory that's marked valid where the
	 * on-disk contents are invalid. Particularly not if the in-memory buffer
	 * could be dirty...
	 *
	 * While we hold an AEL on the relation nobody else should be able to read
	 * the buffer in.
	 *
	 * NB: This is probably racy, better don't copy this to non-test code.
	 */
	if (BufferIsLocal(buf))
		InvalidateLocalBuffer(GetLocalBufferDescriptor(-buf - 1), true);
	else
		EvictUnpinnedBuffer(buf, &flushed);

	/*
	 * Now modify the page as asked for by the caller.
	 */
	if (zero)
		memset(page, 0, BufferGetPageSize(buf));

	if (PageIsEmpty(page) && (corrupt_header || corrupt_checksum))
		PageInit(page, BufferGetPageSize(buf), 0);

	ph = (PageHeader) page;

	if (corrupt_header)
		ph->pd_special = BLCKSZ + 1;

	if (corrupt_checksum)
	{
		bool		successfully_corrupted = 0;

		/*
		 * Any single modification of the checksum could just end up being
		 * valid again, due to e.g. corrupt_header changing the data in a way
		 * that'd result in the "corrupted" checksum, or the checksum already
		 * being invalid. Retry in that, unlikely, case.
		 */
		for (int i = 0; i < 100; i++)
		{
			uint16		verify_checksum;
			uint16		old_checksum;

			old_checksum = ph->pd_checksum;
			ph->pd_checksum = old_checksum + 1;

			elog(LOG, "corrupting checksum of blk %u from %u to %u",
				 blkno, old_checksum, ph->pd_checksum);

			verify_checksum = pg_checksum_page(page, blkno);
			if (verify_checksum != ph->pd_checksum)
			{
				successfully_corrupted = true;
				break;
			}
		}

		if (!successfully_corrupted)
			elog(ERROR, "could not corrupt checksum, what's going on?");
	}
	else
	{
		PageSetChecksum(page, blkno);
	}

	smgrwrite(RelationGetSmgr(rel),
			  MAIN_FORKNUM, blkno, page, true);

	relation_close(rel, NoLock);

	PG_RETURN_VOID();
}

/*
 * Ensures a buffer for rel & blkno is in shared buffers, without actually
 * caring about the buffer contents. Used to set up test scenarios.
 */
static Buffer
create_toy_buffer(Relation rel, BlockNumber blkno)
{
	Buffer		buf;
	BufferDesc *buf_hdr;
	uint64		buf_state;
	bool		was_pinned = false;
	uint64		unset_bits = 0;

	/* place buffer in shared buffers without erroring out */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK, NULL);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (RelationUsesLocalBuffers(rel))
	{
		buf_hdr = GetLocalBufferDescriptor(-buf - 1);
		buf_state = pg_atomic_read_u64(&buf_hdr->state);
	}
	else
	{
		buf_hdr = GetBufferDescriptor(buf - 1);
		buf_state = LockBufHdr(buf_hdr);
	}

	/*
	 * We should be the only backend accessing this buffer. This is just a
	 * small bit of belt-and-suspenders defense, none of this code should ever
	 * run in a cluster with real data.
	 */
	if (BUF_STATE_GET_REFCOUNT(buf_state) > 1)
		was_pinned = true;
	else
		unset_bits |= BM_VALID | BM_DIRTY;

	if (RelationUsesLocalBuffers(rel))
	{
		buf_state &= ~unset_bits;
		pg_atomic_unlocked_write_u64(&buf_hdr->state, buf_state);
	}
	else
	{
		UnlockBufHdrExt(buf_hdr, buf_state, 0, unset_bits, 0);
	}

	if (was_pinned)
		elog(ERROR, "toy buffer %d was already pinned",
			 buf);

	return buf;
}

/*
 * A "low level" read. This does similar things to what
 * StartReadBuffers()/WaitReadBuffers() do, but provides more control (and
 * less sanity).
 */
PG_FUNCTION_INFO_V1(read_rel_block_ll);
Datum
read_rel_block_ll(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blkno = PG_GETARG_UINT32(1);
	int			nblocks = PG_GETARG_INT32(2);
	bool		wait_complete = PG_GETARG_BOOL(3);
	bool		batchmode_enter = PG_GETARG_BOOL(4);
	bool		call_smgrreleaseall = PG_GETARG_BOOL(5);
	bool		batchmode_exit = PG_GETARG_BOOL(6);
	bool		zero_on_error = PG_GETARG_BOOL(7);
	Relation	rel;
	Buffer		bufs[PG_IOV_MAX];
	BufferDesc *buf_hdrs[PG_IOV_MAX];
	Page		pages[PG_IOV_MAX];
	uint8		srb_flags = 0;
	PgAioReturn ior;
	PgAioHandle *ioh;
	PgAioWaitRef iow;
	SMgrRelation smgr;

	if (nblocks <= 0 || nblocks > PG_IOV_MAX)
		elog(ERROR, "nblocks is out of range");

	rel = relation_open(relid, AccessShareLock);

	for (int i = 0; i < nblocks; i++)
	{
		bufs[i] = create_toy_buffer(rel, blkno + i);
		pages[i] = BufferGetBlock(bufs[i]);
		buf_hdrs[i] = BufferIsLocal(bufs[i]) ?
			GetLocalBufferDescriptor(-bufs[i] - 1) :
			GetBufferDescriptor(bufs[i] - 1);
	}

	smgr = RelationGetSmgr(rel);

	pgstat_prepare_report_checksum_failure(smgr->smgr_rlocator.locator.dbOid);

	ioh = pgaio_io_acquire(CurrentResourceOwner, &ior);
	pgaio_io_get_wref(ioh, &iow);

	if (RelationUsesLocalBuffers(rel))
	{
		for (int i = 0; i < nblocks; i++)
			StartLocalBufferIO(buf_hdrs[i], true, true, NULL);
		pgaio_io_set_flag(ioh, PGAIO_HF_REFERENCES_LOCAL);
	}
	else
	{
		for (int i = 0; i < nblocks; i++)
			StartSharedBufferIO(buf_hdrs[i], true, true, NULL);
	}

	pgaio_io_set_handle_data_32(ioh, (uint32 *) bufs, nblocks);

	if (zero_on_error | zero_damaged_pages)
		srb_flags |= READ_BUFFERS_ZERO_ON_ERROR;
	if (ignore_checksum_failure)
		srb_flags |= READ_BUFFERS_IGNORE_CHECKSUM_FAILURES;

	pgaio_io_register_callbacks(ioh,
								RelationUsesLocalBuffers(rel) ?
								PGAIO_HCB_LOCAL_BUFFER_READV :
								PGAIO_HCB_SHARED_BUFFER_READV,
								srb_flags);

	if (batchmode_enter)
		pgaio_enter_batchmode();

	smgrstartreadv(ioh, smgr, MAIN_FORKNUM, blkno,
				   (void *) pages, nblocks);

	if (call_smgrreleaseall)
		smgrreleaseall();

	if (batchmode_exit)
		pgaio_exit_batchmode();

	for (int i = 0; i < nblocks; i++)
		ReleaseBuffer(bufs[i]);

	if (wait_complete)
	{
		pgaio_wref_wait(&iow);

		if (ior.result.status != PGAIO_RS_OK)
			pgaio_result_report(ior.result,
								&ior.target_data,
								ior.result.status == PGAIO_RS_ERROR ?
								ERROR : WARNING);
	}

	relation_close(rel, NoLock);

	PG_RETURN_VOID();
}

/* helper for invalidate_rel_block() and evict_rel() */
static void
invalidate_one_block(Relation rel, ForkNumber forknum, BlockNumber blkno)
{
	PrefetchBufferResult pr;
	Buffer		buf;

	/*
	 * This is a gross hack, but there's no other API exposed that allows to
	 * get a buffer ID without actually reading the block in.
	 */
	pr = PrefetchBuffer(rel, forknum, blkno);
	buf = pr.recent_buffer;

	if (BufferIsValid(buf))
	{
		/* if the buffer contents aren't valid, this'll return false */
		if (ReadRecentBuffer(rel->rd_locator, forknum, blkno, buf))
		{
			BufferDesc *buf_hdr = BufferIsLocal(buf) ?
				GetLocalBufferDescriptor(-buf - 1)
				: GetBufferDescriptor(buf - 1);
			bool		flushed;

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			if (pg_atomic_read_u64(&buf_hdr->state) & BM_DIRTY)
			{
				if (BufferIsLocal(buf))
					FlushLocalBuffer(buf_hdr, NULL);
				else
					FlushOneBuffer(buf);
			}
			UnlockReleaseBuffer(buf);

			if (BufferIsLocal(buf))
				InvalidateLocalBuffer(GetLocalBufferDescriptor(-buf - 1), true);
			else if (!EvictUnpinnedBuffer(buf, &flushed))
				elog(ERROR, "couldn't evict");
		}
	}

}

PG_FUNCTION_INFO_V1(invalidate_rel_block);
Datum
invalidate_rel_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blkno = PG_GETARG_UINT32(1);
	Relation	rel;

	rel = relation_open(relid, AccessExclusiveLock);

	invalidate_one_block(rel, MAIN_FORKNUM, blkno);

	relation_close(rel, AccessExclusiveLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(evict_rel);
Datum
evict_rel(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	rel = relation_open(relid, AccessExclusiveLock);

	/*
	 * EvictRelUnpinnedBuffers() doesn't support temp tables, so for temp
	 * tables we have to do it the expensive way and evict every possible
	 * buffer.
	 */
	if (RelationUsesLocalBuffers(rel))
	{
		SMgrRelation smgr = RelationGetSmgr(rel);

		for (int forknum = MAIN_FORKNUM; forknum <= MAX_FORKNUM; forknum++)
		{
			BlockNumber nblocks;

			if (!smgrexists(smgr, forknum))
				continue;

			nblocks = smgrnblocks(smgr, forknum);

			for (int blkno = 0; blkno < nblocks; blkno++)
			{
				invalidate_one_block(rel, forknum, blkno);
			}
		}
	}
	else
	{
		int32		buffers_evicted,
					buffers_flushed,
					buffers_skipped;

		EvictRelUnpinnedBuffers(rel, &buffers_evicted, &buffers_flushed,
								&buffers_skipped);
	}

	relation_close(rel, AccessExclusiveLock);


	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(buffer_create_toy);
Datum
buffer_create_toy(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blkno = PG_GETARG_UINT32(1);
	Relation	rel;
	Buffer		buf;

	rel = relation_open(relid, AccessExclusiveLock);

	buf = create_toy_buffer(rel, blkno);
	ReleaseBuffer(buf);

	relation_close(rel, NoLock);

	PG_RETURN_INT32(buf);
}

PG_FUNCTION_INFO_V1(buffer_call_start_io);
Datum
buffer_call_start_io(PG_FUNCTION_ARGS)
{
	Buffer		buf = PG_GETARG_INT32(0);
	bool		for_input = PG_GETARG_BOOL(1);
	bool		wait = PG_GETARG_BOOL(2);
	StartBufferIOResult result;
	bool		can_start;

	if (BufferIsLocal(buf))
		result = StartLocalBufferIO(GetLocalBufferDescriptor(-buf - 1),
									for_input, wait, NULL);
	else
		result = StartSharedBufferIO(GetBufferDescriptor(buf - 1),
									 for_input, wait, NULL);

	can_start = result == BUFFER_IO_READY_FOR_IO;

	/*
	 * For tests we don't want the resowner release preventing us from
	 * orchestrating odd scenarios.
	 */
	if (can_start && !BufferIsLocal(buf))
		ResourceOwnerForgetBufferIO(CurrentResourceOwner,
									buf);

	ereport(LOG,
			errmsg("buffer %d after StartBufferIO: %s",
				   buf, DebugPrintBufferRefcount(buf)),
			errhidestmt(true), errhidecontext(true));

	PG_RETURN_BOOL(can_start);
}

PG_FUNCTION_INFO_V1(buffer_call_terminate_io);
Datum
buffer_call_terminate_io(PG_FUNCTION_ARGS)
{
	Buffer		buf = PG_GETARG_INT32(0);
	bool		for_input = PG_GETARG_BOOL(1);
	bool		succeed = PG_GETARG_BOOL(2);
	bool		io_error = PG_GETARG_BOOL(3);
	bool		release_aio = PG_GETARG_BOOL(4);
	bool		clear_dirty = false;
	uint64		set_flag_bits = 0;

	if (io_error)
		set_flag_bits |= BM_IO_ERROR;

	if (for_input)
	{
		clear_dirty = false;

		if (succeed)
			set_flag_bits |= BM_VALID;
	}
	else
	{
		if (succeed)
			clear_dirty = true;
	}

	ereport(LOG,
			errmsg("buffer %d before Terminate[Local]BufferIO: %s",
				   buf, DebugPrintBufferRefcount(buf)),
			errhidestmt(true), errhidecontext(true));

	if (BufferIsLocal(buf))
		TerminateLocalBufferIO(GetLocalBufferDescriptor(-buf - 1),
							   clear_dirty, set_flag_bits, release_aio);
	else
		TerminateBufferIO(GetBufferDescriptor(buf - 1),
						  clear_dirty, set_flag_bits, false, release_aio);

	ereport(LOG,
			errmsg("buffer %d after Terminate[Local]BufferIO: %s",
				   buf, DebugPrintBufferRefcount(buf)),
			errhidestmt(true), errhidecontext(true));

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(read_buffers);
/*
 * Infrastructure to test StartReadBuffers()
 */
Datum
read_buffers(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber startblock = PG_GETARG_UINT32(1);
	int32		nblocks = PG_GETARG_INT32(2);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	SMgrRelation smgr;
	int			nblocks_done = 0;
	int			nblocks_disp = 0;
	int			nios = 0;
	ReadBuffersOperation *operations;
	Buffer	   *buffers;
	Datum	   *buffers_datum;
	bool	   *io_reqds;
	int		   *nblocks_per_io;

	Assert(nblocks > 0);

	InitMaterializedSRF(fcinfo, 0);

	/* at worst each block gets its own IO */
	operations = palloc0(sizeof(ReadBuffersOperation) * nblocks);
	buffers = palloc0(sizeof(Buffer) * nblocks);
	buffers_datum = palloc0(sizeof(Datum) * nblocks);
	io_reqds = palloc0(sizeof(bool) * nblocks);
	nblocks_per_io = palloc0(sizeof(int) * nblocks);

	rel = relation_open(relid, AccessShareLock);
	smgr = RelationGetSmgr(rel);

	/*
	 * Do StartReadBuffers() until IO for all the required blocks has been
	 * started (if required).
	 */
	while (nblocks_done < nblocks)
	{
		ReadBuffersOperation *operation = &operations[nios];
		int			nblocks_this_io =
			Min(nblocks - nblocks_done, io_combine_limit);

		operation->rel = rel;
		operation->smgr = smgr;
		operation->persistence = rel->rd_rel->relpersistence;
		operation->strategy = NULL;
		operation->forknum = MAIN_FORKNUM;

		io_reqds[nios] = StartReadBuffers(operation,
										  &buffers[nblocks_done],
										  startblock + nblocks_done,
										  &nblocks_this_io,
										  0);
		nblocks_per_io[nios] = nblocks_this_io;
		nios++;
		nblocks_done += nblocks_this_io;
	}

	/*
	 * Now wait for all operations that required IO. This is done at the end,
	 * as otherwise waiting for IO in progress in other backends could
	 * influence the result for subsequent buffers / blocks.
	 */
	for (int nio = 0; nio < nios; nio++)
	{
		ReadBuffersOperation *operation = &operations[nio];

		if (io_reqds[nio])
			WaitReadBuffers(operation);
	}

	/*
	 * Convert what has been done into SQL SRF return value.
	 */
	for (int nio = 0; nio < nios; nio++)
	{
		ReadBuffersOperation *operation = &operations[nio];
		int			nblocks_this_io = nblocks_per_io[nio];
		Datum		values[6] = {0};
		bool		nulls[6] = {0};
		ArrayType  *buffers_arr;

		/* convert buffer array to datum array */
		for (int i = 0; i < nblocks_this_io; i++)
		{
			Buffer		buf = buffers[nblocks_disp + i];

			Assert(BufferGetBlockNumber(buf) == startblock + nblocks_disp + i);

			buffers_datum[nblocks_disp + i] = Int32GetDatum(buf);
		}

		buffers_arr = construct_array_builtin(&buffers_datum[nblocks_disp],
											  nblocks_this_io,
											  INT4OID);

		/* blockoff */
		values[0] = Int32GetDatum(nblocks_disp);
		nulls[0] = false;

		/* blocknum */
		values[1] = UInt32GetDatum(startblock + nblocks_disp);
		nulls[1] = false;

		/* io_reqd */
		values[2] = BoolGetDatum(io_reqds[nio]);
		nulls[2] = false;

		/* foreign IO - only valid when IO was required */
		values[3] = BoolGetDatum(io_reqds[nio] ? operation->foreign_io : false);
		nulls[3] = false;

		/* nblocks */
		values[4] = Int32GetDatum(nblocks_this_io);
		nulls[4] = false;

		/* array of buffers */
		values[5] = PointerGetDatum(buffers_arr);
		nulls[5] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		nblocks_disp += nblocks_this_io;
	}

	/* release pins on all the buffers */
	for (int i = 0; i < nblocks_done; i++)
		ReleaseBuffer(buffers[i]);

	/*
	 * Free explicitly, to have a chance to detect potential issues with too
	 * long lived references to the operation.
	 */
	pfree(operations);
	pfree(buffers);
	pfree(buffers_datum);
	pfree(io_reqds);
	pfree(nblocks_per_io);

	relation_close(rel, NoLock);

	return (Datum) 0;
}


static BlockNumber
read_stream_for_blocks_cb(ReadStream *stream,
						  void *callback_private_data,
						  void *per_buffer_data)
{
	BlocksReadStreamData *stream_data = callback_private_data;

	if (stream_data->curblock >= stream_data->nblocks)
		return InvalidBlockNumber;
	return stream_data->blocks[stream_data->curblock++];
}

PG_FUNCTION_INFO_V1(read_stream_for_blocks);
Datum
read_stream_for_blocks(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *blocksarray = PG_GETARG_ARRAYTYPE_P(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	BlocksReadStreamData stream_data;
	ReadStream *stream;

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * We expect the input to be an N-element int4 array; verify that. We
	 * don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of N int4 values.
	 */
	if (ARR_NDIM(blocksarray) != 1 ||
		ARR_HASNULL(blocksarray) ||
		ARR_ELEMTYPE(blocksarray) != INT4OID)
		elog(ERROR, "expected 1 dimensional int4 array");

	stream_data.curblock = 0;
	stream_data.nblocks = ARR_DIMS(blocksarray)[0];
	stream_data.blocks = (uint32 *) ARR_DATA_PTR(blocksarray);

	rel = relation_open(relid, AccessShareLock);

	stream = read_stream_begin_relation(READ_STREAM_FULL,
										NULL,
										rel,
										MAIN_FORKNUM,
										read_stream_for_blocks_cb,
										&stream_data,
										0);

	for (int i = 0; i < stream_data.nblocks; i++)
	{
		Buffer		buf = read_stream_next_buffer(stream, NULL);
		Datum		values[3] = {0};
		bool		nulls[3] = {0};

		if (!BufferIsValid(buf))
			elog(ERROR, "read_stream_next_buffer() call %d is unexpectedly invalid", i);

		values[0] = Int32GetDatum(i);
		values[1] = UInt32GetDatum(stream_data.blocks[i]);
		values[2] = UInt32GetDatum(buf);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		ReleaseBuffer(buf);
	}

	if (read_stream_next_buffer(stream, NULL) != InvalidBuffer)
		elog(ERROR, "read_stream_next_buffer() call %d is unexpectedly valid",
			 stream_data.nblocks);

	read_stream_end(stream);

	relation_close(rel, NoLock);

	return (Datum) 0;
}


PG_FUNCTION_INFO_V1(handle_get);
Datum
handle_get(PG_FUNCTION_ARGS)
{
	last_handle = pgaio_io_acquire(CurrentResourceOwner, NULL);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_release_last);
Datum
handle_release_last(PG_FUNCTION_ARGS)
{
	if (!last_handle)
		elog(ERROR, "no handle");

	pgaio_io_release(last_handle);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_get_and_error);
Datum
handle_get_and_error(PG_FUNCTION_ARGS)
{
	pgaio_io_acquire(CurrentResourceOwner, NULL);

	elog(ERROR, "as you command");
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_get_twice);
Datum
handle_get_twice(PG_FUNCTION_ARGS)
{
	pgaio_io_acquire(CurrentResourceOwner, NULL);
	pgaio_io_acquire(CurrentResourceOwner, NULL);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_get_release);
Datum
handle_get_release(PG_FUNCTION_ARGS)
{
	PgAioHandle *handle;

	handle = pgaio_io_acquire(CurrentResourceOwner, NULL);
	pgaio_io_release(handle);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(batch_start);
Datum
batch_start(PG_FUNCTION_ARGS)
{
	pgaio_enter_batchmode();
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(batch_end);
Datum
batch_end(PG_FUNCTION_ARGS)
{
	pgaio_exit_batchmode();
	PG_RETURN_VOID();
}

#ifdef USE_INJECTION_POINTS
extern PGDLLEXPORT void inj_io_completion_hook(const char *name,
											   const void *private_data,
											   void *arg);
extern PGDLLEXPORT void inj_io_reopen(const char *name,
									  const void *private_data,
									  void *arg);

static bool
inj_io_short_read_matches(PgAioHandle *ioh)
{
	PGPROC	   *io_proc;
	int32		io_pid;
	int32		inj_pid;
	PgAioTargetData *td;

	if (!inj_io_error_state->enabled_short_read)
		return false;

	if (!inj_io_error_state->short_read_result_set)
		return false;

	io_proc = GetPGProcByNumber(pgaio_io_get_owner(ioh));
	io_pid = io_proc->pid;
	inj_pid = inj_io_error_state->short_read_pid;

	if (inj_pid != InvalidPid && inj_pid != io_pid)
		return false;

	td = pgaio_io_get_target_data(ioh);

	if (inj_io_error_state->short_read_relfilenode != InvalidOid &&
		td->smgr.rlocator.relNumber != inj_io_error_state->short_read_relfilenode)
		return false;

	/*
	 * Only shorten reads that are actually longer than the target size,
	 * otherwise we can trigger over-reads.
	 */
	if (inj_io_error_state->short_read_result >= ioh->result)
		return false;

	return true;
}

static bool
inj_io_completion_wait_matches(PgAioHandle *ioh)
{
	PGPROC	   *io_proc;
	int32		io_pid;
	PgAioTargetData *td;
	int32		inj_pid;
	BlockNumber io_blockno;
	BlockNumber inj_blockno;
	Oid			inj_relfilenode;

	if (!inj_io_error_state->enabled_completion_wait)
		return false;

	io_proc = GetPGProcByNumber(pgaio_io_get_owner(ioh));
	io_pid = io_proc->pid;
	inj_pid = inj_io_error_state->completion_wait_pid;

	if (inj_pid != InvalidPid && inj_pid != io_pid)
		return false;

	td = pgaio_io_get_target_data(ioh);

	inj_relfilenode = inj_io_error_state->completion_wait_relfilenode;
	if (inj_relfilenode != InvalidOid &&
		td->smgr.rlocator.relNumber != inj_relfilenode)
		return false;

	inj_blockno = inj_io_error_state->completion_wait_blockno;
	io_blockno = td->smgr.blockNum;
	if (inj_blockno != InvalidBlockNumber &&
		!(inj_blockno >= io_blockno && inj_blockno < (io_blockno + td->smgr.nblocks)))
		return false;

	return true;
}

static void
inj_io_completion_wait_hook(const char *name, const void *private_data, void *arg)
{
	PgAioHandle *ioh = (PgAioHandle *) arg;

	if (!inj_io_completion_wait_matches(ioh))
		return;

	ConditionVariablePrepareToSleep(&inj_io_error_state->cv);

	while (true)
	{
		if (!inj_io_completion_wait_matches(ioh))
			break;

		ConditionVariableSleep(&inj_io_error_state->cv,
							   inj_io_error_state->completion_wait_event);
	}

	ConditionVariableCancelSleep();
}

static void
inj_io_short_read_hook(const char *name, const void *private_data, void *arg)
{
	PgAioHandle *ioh = (PgAioHandle *) arg;

	ereport(LOG,
			errmsg("short read injection point called, is enabled: %d",
				   inj_io_error_state->enabled_short_read),
			errhidestmt(true), errhidecontext(true));

	if (inj_io_short_read_matches(ioh))
	{
		struct iovec *iov = &pgaio_ctl->iovecs[ioh->iovec_off];
		int32		old_result = ioh->result;
		int32		new_result = inj_io_error_state->short_read_result;
		int32		processed = 0;

		ereport(LOG,
				errmsg("short read inject point, changing result from %d to %d",
					   old_result, new_result),
				errhidestmt(true), errhidecontext(true));

		/*
		 * The underlying IO actually completed OK, and thus the "invalid"
		 * portion of the IOV actually contains valid data. That can hide a
		 * lot of problems, e.g. if we were to wrongly mark a buffer, that
		 * wasn't read according to the shortened-read, IO as valid, the
		 * contents would look valid and we might miss a bug.
		 *
		 * To avoid that, iterate through the IOV and zero out the "failed"
		 * portion of the IO.
		 */
		for (int i = 0; i < ioh->op_data.read.iov_length; i++)
		{
			if (processed + iov[i].iov_len <= new_result)
				processed += iov[i].iov_len;
			else if (processed <= new_result)
			{
				uint32		ok_part = new_result - processed;

				memset((char *) iov[i].iov_base + ok_part, 0, iov[i].iov_len - ok_part);
				processed += iov[i].iov_len;
			}
			else
			{
				memset((char *) iov[i].iov_base, 0, iov[i].iov_len);
			}
		}

		ioh->result = new_result;
	}
}

void
inj_io_completion_hook(const char *name, const void *private_data, void *arg)
{
	inj_io_completion_wait_hook(name, private_data, arg);
	inj_io_short_read_hook(name, private_data, arg);
}

void
inj_io_reopen(const char *name, const void *private_data, void *arg)
{
	ereport(LOG,
			errmsg("reopen injection point called, is enabled: %d",
				   inj_io_error_state->enabled_reopen),
			errhidestmt(true), errhidecontext(true));

	if (inj_io_error_state->enabled_reopen)
		elog(ERROR, "injection point triggering failure to reopen ");
}
#endif

PG_FUNCTION_INFO_V1(inj_io_completion_wait);
Datum
inj_io_completion_wait(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_completion_wait = true;
	inj_io_error_state->completion_wait_pid =
		PG_ARGISNULL(0) ? InvalidPid : PG_GETARG_INT32(0);
	inj_io_error_state->completion_wait_relfilenode =
		PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);
	inj_io_error_state->completion_wait_blockno =
		PG_ARGISNULL(2) ? InvalidBlockNumber : PG_GETARG_UINT32(2);
#else
	elog(ERROR, "injection points not supported");
#endif

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(inj_io_completion_continue);
Datum
inj_io_completion_continue(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_completion_wait = false;
	inj_io_error_state->completion_wait_pid = InvalidPid;
	inj_io_error_state->completion_wait_relfilenode = InvalidOid;
	inj_io_error_state->completion_wait_blockno = InvalidBlockNumber;
	ConditionVariableBroadcast(&inj_io_error_state->cv);
#else
	elog(ERROR, "injection points not supported");
#endif

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(inj_io_short_read_attach);
Datum
inj_io_short_read_attach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_short_read = true;
	inj_io_error_state->short_read_result_set = !PG_ARGISNULL(0);
	if (inj_io_error_state->short_read_result_set)
		inj_io_error_state->short_read_result = PG_GETARG_INT32(0);
	inj_io_error_state->short_read_pid =
		PG_ARGISNULL(1) ? InvalidPid : PG_GETARG_INT32(1);
	inj_io_error_state->short_read_relfilenode =
		PG_ARGISNULL(2) ? InvalidOid : PG_GETARG_OID(2);
#else
	elog(ERROR, "injection points not supported");
#endif

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(inj_io_short_read_detach);
Datum
inj_io_short_read_detach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_short_read = false;
#else
	elog(ERROR, "injection points not supported");
#endif
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(inj_io_reopen_attach);
Datum
inj_io_reopen_attach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_reopen = true;
#else
	elog(ERROR, "injection points not supported");
#endif

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(inj_io_reopen_detach);
Datum
inj_io_reopen_detach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_reopen = false;
#else
	elog(ERROR, "injection points not supported");
#endif
	PG_RETURN_VOID();
}
