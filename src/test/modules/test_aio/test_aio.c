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
 * Copyright (c) 2020-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_aio/test_aio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/checksum.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/rel.h"


PG_MODULE_MAGIC;


typedef struct InjIoErrorState
{
	bool		enabled_short_read;
	bool		enabled_reopen;

	bool		short_read_result_set;
	int			short_read_result;
}			InjIoErrorState;

static InjIoErrorState * inj_io_error_state;

/* Shared memory init callbacks */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;


static PgAioHandle *last_handle;



static void
test_aio_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(sizeof(InjIoErrorState));
}

static void
test_aio_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Create or attach to the shared memory state */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	inj_io_error_state = ShmemInitStruct("injection_points",
										 sizeof(InjIoErrorState),
										 &found);

	if (!found)
	{
		/* First time through, initialize */
		inj_io_error_state->enabled_short_read = false;
		inj_io_error_state->enabled_reopen = false;

#ifdef USE_INJECTION_POINTS
		InjectionPointAttach("aio-process-completion-before-shared",
							 "test_aio",
							 "inj_io_short_read",
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
	else
	{
		/*
		 * Pre-load the injection points now, so we can call them in a
		 * critical section.
		 */
#ifdef USE_INJECTION_POINTS
		InjectionPointLoad("aio-process-completion-before-shared");
		InjectionPointLoad("aio-worker-after-reopen");
		elog(LOG, "injection point loaded");
#endif
	}

	LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_aio_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_aio_shmem_startup;
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

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	ReleaseBuffer(buf);

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
		PageSetChecksumInplace(page, blkno);
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
	uint32		buf_state;
	bool		was_pinned = false;

	/* place buffer in shared buffers without erroring out */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK, NULL);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (RelationUsesLocalBuffers(rel))
	{
		buf_hdr = GetLocalBufferDescriptor(-buf - 1);
		buf_state = pg_atomic_read_u32(&buf_hdr->state);
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
		buf_state &= ~(BM_VALID | BM_DIRTY);

	if (RelationUsesLocalBuffers(rel))
		pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
	else
		UnlockBufHdr(buf_hdr, buf_state);

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

	rel = relation_open(relid, AccessExclusiveLock);

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
			StartLocalBufferIO(buf_hdrs[i], true, false);
		pgaio_io_set_flag(ioh, PGAIO_HF_REFERENCES_LOCAL);
	}
	else
	{
		for (int i = 0; i < nblocks; i++)
			StartBufferIO(buf_hdrs[i], true, false);
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

PG_FUNCTION_INFO_V1(invalidate_rel_block);
Datum
invalidate_rel_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blkno = PG_GETARG_UINT32(1);
	Relation	rel;
	PrefetchBufferResult pr;
	Buffer		buf;

	rel = relation_open(relid, AccessExclusiveLock);

	/*
	 * This is a gross hack, but there's no other API exposed that allows to
	 * get a buffer ID without actually reading the block in.
	 */
	pr = PrefetchBuffer(rel, MAIN_FORKNUM, blkno);
	buf = pr.recent_buffer;

	if (BufferIsValid(buf))
	{
		/* if the buffer contents aren't valid, this'll return false */
		if (ReadRecentBuffer(rel->rd_locator, MAIN_FORKNUM, blkno, buf))
		{
			BufferDesc *buf_hdr = BufferIsLocal(buf) ?
				GetLocalBufferDescriptor(-buf - 1)
				: GetBufferDescriptor(buf - 1);
			bool		flushed;

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			if (pg_atomic_read_u32(&buf_hdr->state) & BM_DIRTY)
			{
				if (BufferIsLocal(buf))
					FlushLocalBuffer(buf_hdr, NULL);
				else
					FlushOneBuffer(buf);
			}
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);

			if (BufferIsLocal(buf))
				InvalidateLocalBuffer(GetLocalBufferDescriptor(-buf - 1), true);
			else if (!EvictUnpinnedBuffer(buf, &flushed))
				elog(ERROR, "couldn't evict");
		}
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
	bool		nowait = PG_GETARG_BOOL(2);
	bool		can_start;

	if (BufferIsLocal(buf))
		can_start = StartLocalBufferIO(GetLocalBufferDescriptor(-buf - 1),
									   for_input, nowait);
	else
		can_start = StartBufferIO(GetBufferDescriptor(buf - 1),
								  for_input, nowait);

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
	uint32		set_flag_bits = 0;

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
extern PGDLLEXPORT void inj_io_short_read(const char *name, const void *private_data);
extern PGDLLEXPORT void inj_io_reopen(const char *name, const void *private_data);

void
inj_io_short_read(const char *name, const void *private_data)
{
	PgAioHandle *ioh;

	ereport(LOG,
			errmsg("short read injection point called, is enabled: %d",
				   inj_io_error_state->enabled_reopen),
			errhidestmt(true), errhidecontext(true));

	if (inj_io_error_state->enabled_short_read)
	{
		ioh = pgaio_inj_io_get();

		/*
		 * Only shorten reads that are actually longer than the target size,
		 * otherwise we can trigger over-reads.
		 */
		if (inj_io_error_state->short_read_result_set
			&& ioh->op == PGAIO_OP_READV
			&& inj_io_error_state->short_read_result <= ioh->result)
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
			 * portion of the IOV actually contains valid data. That can hide
			 * a lot of problems, e.g. if we were to wrongly mark a buffer,
			 * that wasn't read according to the shortened-read, IO as valid,
			 * the contents would look valid and we might miss a bug.
			 *
			 * To avoid that, iterate through the IOV and zero out the
			 * "failed" portion of the IO.
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
}

void
inj_io_reopen(const char *name, const void *private_data)
{
	ereport(LOG,
			errmsg("reopen injection point called, is enabled: %d",
				   inj_io_error_state->enabled_reopen),
			errhidestmt(true), errhidecontext(true));

	if (inj_io_error_state->enabled_reopen)
		elog(ERROR, "injection point triggering failure to reopen ");
}
#endif

PG_FUNCTION_INFO_V1(inj_io_short_read_attach);
Datum
inj_io_short_read_attach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled_short_read = true;
	inj_io_error_state->short_read_result_set = !PG_ARGISNULL(0);
	if (inj_io_error_state->short_read_result_set)
		inj_io_error_state->short_read_result = PG_GETARG_INT32(0);
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
