/*-------------------------------------------------------------------------
 *
 * aio.c
 *    AIO - Core Logic
 *
 * For documentation about how AIO works on a higher level, including a
 * schematic example, see README.md.
 *
 *
 * AIO is a complicated subsystem. To keep things navigable, it is split
 * across a number of files:
 *
 * - method_*.c - different ways of executing AIO (e.g. worker process)
 *
 * - aio_target.c - IO on different kinds of targets
 *
 * - aio_io.c - method-independent code for specific IO ops (e.g. readv)
 *
 * - aio_callback.c - callbacks at IO operation lifecycle events
 *
 * - aio_init.c - per-server and per-backend initialization
 *
 * - aio.c - all other topics
 *
 * - read_stream.c - helper for reading buffered relation data
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/aio_subsys.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/resowner.h"
#include "utils/wait_event_types.h"

#ifdef USE_INJECTION_POINTS
#include "utils/injection_point.h"
#endif


static inline void pgaio_io_update_state(PgAioHandle *ioh, PgAioHandleState new_state);
static void pgaio_io_reclaim(PgAioHandle *ioh);
static void pgaio_io_resowner_register(PgAioHandle *ioh);
static void pgaio_io_wait_for_free(void);
static PgAioHandle *pgaio_io_from_wref(PgAioWaitRef *iow, uint64 *ref_generation);
static const char *pgaio_io_state_get_name(PgAioHandleState s);
static void pgaio_io_wait(PgAioHandle *ioh, uint64 ref_generation);


/* Options for io_method. */
const struct config_enum_entry io_method_options[] = {
	{"sync", IOMETHOD_SYNC, false},
	{"worker", IOMETHOD_WORKER, false},
	{NULL, 0, false}
};

/* GUCs */
int			io_method = DEFAULT_IO_METHOD;
int			io_max_concurrency = -1;

/* global control for AIO */
PgAioCtl   *pgaio_ctl;

/* current backend's per-backend state */
PgAioBackend *pgaio_my_backend;


static const IoMethodOps *const pgaio_method_ops_table[] = {
	[IOMETHOD_SYNC] = &pgaio_sync_ops,
	[IOMETHOD_WORKER] = &pgaio_worker_ops,
};

/* callbacks for the configured io_method, set by assign_io_method */
const IoMethodOps *pgaio_method_ops;


/*
 * Currently there's no infrastructure to pass arguments to injection points,
 * so we instead set this up for the duration of the injection point
 * invocation. See pgaio_io_call_inj().
 */
#ifdef USE_INJECTION_POINTS
static PgAioHandle *pgaio_inj_cur_handle;
#endif



/* --------------------------------------------------------------------------------
 * Public Functions related to PgAioHandle
 * --------------------------------------------------------------------------------
 */

/*
 * Acquire an AioHandle, waiting for IO completion if necessary.
 *
 * Each backend can only have one AIO handle that has been "handed out" to
 * code, but not yet submitted or released. This restriction is necessary to
 * ensure that it is possible for code to wait for an unused handle by waiting
 * for in-flight IO to complete. There is a limited number of handles in each
 * backend, if multiple handles could be handed out without being submitted,
 * waiting for all in-flight IO to complete would not guarantee that handles
 * free up.
 *
 * It is cheap to acquire an IO handle, unless all handles are in use. In that
 * case this function waits for the oldest IO to complete. If that is not
 * desirable, use pgaio_io_acquire_nb().
 *
 * If a handle was acquired but then does not turn out to be needed,
 * e.g. because pgaio_io_acquire() is called before starting an IO in a
 * critical section, the handle needs to be released with pgaio_io_release().
 *
 *
 * To react to the completion of the IO as soon as it is known to have
 * completed, callbacks can be registered with pgaio_io_register_callbacks().
 *
 * To actually execute IO using the returned handle, the pgaio_io_prep_*()
 * family of functions is used. In many cases the pgaio_io_prep_*() call will
 * not be done directly by code that acquired the handle, but by lower level
 * code that gets passed the handle. E.g. if code in bufmgr.c wants to perform
 * AIO, it typically will pass the handle to smgr.c, which will pass it on to
 * md.c, on to fd.c, which then finally calls pgaio_io_prep_*().  This
 * forwarding allows the various layers to react to the IO's completion by
 * registering callbacks. These callbacks in turn can translate a lower
 * layer's result into a result understandable by a higher layer.
 *
 * During pgaio_io_prep_*() the IO is staged (i.e. prepared for execution but
 * not submitted to the kernel). Unless in batchmode
 * (c.f. pgaio_enter_batchmode()), the IO will also get submitted for
 * execution. Note that, whether in batchmode or not, the IO might even
 * complete before the functions return.
 *
 * After pgaio_io_prep_*() the AioHandle is "consumed" and may not be
 * referenced by the IO issuing code. To e.g. wait for IO, references to the
 * IO can be established with pgaio_io_get_wref() *before* pgaio_io_prep_*()
 * is called.  pgaio_wref_wait() can be used to wait for the IO to complete.
 *
 *
 * To know if the IO [partially] succeeded or failed, a PgAioReturn * can be
 * passed to pgaio_io_acquire(). Once the issuing backend has called
 * pgaio_wref_wait(), the PgAioReturn contains information about whether the
 * operation succeeded and details about the first failure, if any. The error
 * can be raised / logged with pgaio_result_report().
 *
 * The lifetime of the memory pointed to be *ret needs to be at least as long
 * as the passed in resowner. If the resowner releases resources before the IO
 * completes (typically due to an error), the reference to *ret will be
 * cleared. In case of resowner cleanup *ret will not be updated with the
 * results of the IO operation.
 */
PgAioHandle *
pgaio_io_acquire(struct ResourceOwnerData *resowner, PgAioReturn *ret)
{
	PgAioHandle *h;

	while (true)
	{
		h = pgaio_io_acquire_nb(resowner, ret);

		if (h != NULL)
			return h;

		/*
		 * Evidently all handles by this backend are in use. Just wait for
		 * some to complete.
		 */
		pgaio_io_wait_for_free();
	}
}

/*
 * Acquire an AioHandle, returning NULL if no handles are free.
 *
 * See pgaio_io_acquire(). The only difference is that this function will return
 * NULL if there are no idle handles, instead of blocking.
 */
PgAioHandle *
pgaio_io_acquire_nb(struct ResourceOwnerData *resowner, PgAioReturn *ret)
{
	if (pgaio_my_backend->num_staged_ios >= PGAIO_SUBMIT_BATCH_SIZE)
	{
		Assert(pgaio_my_backend->num_staged_ios == PGAIO_SUBMIT_BATCH_SIZE);
		pgaio_submit_staged();
	}

	if (pgaio_my_backend->handed_out_io)
		elog(ERROR, "API violation: Only one IO can be handed out");

	if (!dclist_is_empty(&pgaio_my_backend->idle_ios))
	{
		dlist_node *ion = dclist_pop_head_node(&pgaio_my_backend->idle_ios);
		PgAioHandle *ioh = dclist_container(PgAioHandle, node, ion);

		Assert(ioh->state == PGAIO_HS_IDLE);
		Assert(ioh->owner_procno == MyProcNumber);

		pgaio_io_update_state(ioh, PGAIO_HS_HANDED_OUT);
		pgaio_my_backend->handed_out_io = ioh;

		if (resowner)
			pgaio_io_resowner_register(ioh);

		if (ret)
		{
			ioh->report_return = ret;
			ret->result.status = PGAIO_RS_UNKNOWN;
		}

		return ioh;
	}

	return NULL;
}

/*
 * Release IO handle that turned out to not be required.
 *
 * See pgaio_io_acquire() for more details.
 */
void
pgaio_io_release(PgAioHandle *ioh)
{
	if (ioh == pgaio_my_backend->handed_out_io)
	{
		Assert(ioh->state == PGAIO_HS_HANDED_OUT);
		Assert(ioh->resowner);

		pgaio_my_backend->handed_out_io = NULL;
		pgaio_io_reclaim(ioh);
	}
	else
	{
		elog(ERROR, "release in unexpected state");
	}
}

/*
 * Release IO handle during resource owner cleanup.
 */
void
pgaio_io_release_resowner(dlist_node *ioh_node, bool on_error)
{
	PgAioHandle *ioh = dlist_container(PgAioHandle, resowner_node, ioh_node);

	Assert(ioh->resowner);

	ResourceOwnerForgetAioHandle(ioh->resowner, &ioh->resowner_node);
	ioh->resowner = NULL;

	switch (ioh->state)
	{
		case PGAIO_HS_IDLE:
			elog(ERROR, "unexpected");
			break;
		case PGAIO_HS_HANDED_OUT:
			Assert(ioh == pgaio_my_backend->handed_out_io || pgaio_my_backend->handed_out_io == NULL);

			if (ioh == pgaio_my_backend->handed_out_io)
			{
				pgaio_my_backend->handed_out_io = NULL;
				if (!on_error)
					elog(WARNING, "leaked AIO handle");
			}

			pgaio_io_reclaim(ioh);
			break;
		case PGAIO_HS_DEFINED:
		case PGAIO_HS_STAGED:
			if (!on_error)
				elog(WARNING, "AIO handle was not submitted");
			pgaio_submit_staged();
			break;
		case PGAIO_HS_SUBMITTED:
		case PGAIO_HS_COMPLETED_IO:
		case PGAIO_HS_COMPLETED_SHARED:
		case PGAIO_HS_COMPLETED_LOCAL:
			/* this is expected to happen */
			break;
	}

	/*
	 * Need to unregister the reporting of the IO's result, the memory it's
	 * referencing likely has gone away.
	 */
	if (ioh->report_return)
		ioh->report_return = NULL;
}

/*
 * Add a [set of] flags to the IO.
 *
 * Note that this combines flags with already set flags, rather than set flags
 * to explicitly the passed in parameters. This is to allow multiple callsites
 * to set flags.
 */
void
pgaio_io_set_flag(PgAioHandle *ioh, PgAioHandleFlags flag)
{
	Assert(ioh->state == PGAIO_HS_HANDED_OUT);

	ioh->flags |= flag;
}

/*
 * Returns an ID uniquely identifying the IO handle. This is only really
 * useful for logging, as handles are reused across multiple IOs.
 */
int
pgaio_io_get_id(PgAioHandle *ioh)
{
	Assert(ioh >= pgaio_ctl->io_handles &&
		   ioh < (pgaio_ctl->io_handles + pgaio_ctl->io_handle_count));
	return ioh - pgaio_ctl->io_handles;
}

/*
 * Return the ProcNumber for the process that can use an IO handle. The
 * mapping from IO handles to PGPROCs is static, therefore this even works
 * when the corresponding PGPROC is not in use.
 */
ProcNumber
pgaio_io_get_owner(PgAioHandle *ioh)
{
	return ioh->owner_procno;
}

/*
 * Return a wait reference for the IO. Only wait references can be used to
 * wait for an IOs completion, as handles themselves can be reused after
 * completion.  See also the comment above pgaio_io_acquire().
 */
void
pgaio_io_get_wref(PgAioHandle *ioh, PgAioWaitRef *iow)
{
	Assert(ioh->state == PGAIO_HS_HANDED_OUT ||
		   ioh->state == PGAIO_HS_DEFINED ||
		   ioh->state == PGAIO_HS_STAGED);
	Assert(ioh->generation != 0);

	iow->aio_index = ioh - pgaio_ctl->io_handles;
	iow->generation_upper = (uint32) (ioh->generation >> 32);
	iow->generation_lower = (uint32) ioh->generation;
}



/* --------------------------------------------------------------------------------
 * Internal Functions related to PgAioHandle
 * --------------------------------------------------------------------------------
 */

static inline void
pgaio_io_update_state(PgAioHandle *ioh, PgAioHandleState new_state)
{
	pgaio_debug_io(DEBUG5, ioh,
				   "updating state to %s",
				   pgaio_io_state_get_name(new_state));

	/*
	 * Ensure the changes signified by the new state are visible before the
	 * new state becomes visible.
	 */
	pg_write_barrier();

	ioh->state = new_state;
}

static void
pgaio_io_resowner_register(PgAioHandle *ioh)
{
	Assert(!ioh->resowner);
	Assert(CurrentResourceOwner);

	ResourceOwnerRememberAioHandle(CurrentResourceOwner, &ioh->resowner_node);
	ioh->resowner = CurrentResourceOwner;
}

/*
 * Stage IO for execution and, if appropriate, submit it immediately.
 *
 * Should only be called from pgaio_io_prep_*().
 */
void
pgaio_io_stage(PgAioHandle *ioh, PgAioOp op)
{
	bool		needs_synchronous;

	Assert(ioh->state == PGAIO_HS_HANDED_OUT);
	Assert(pgaio_my_backend->handed_out_io == ioh);
	Assert(pgaio_io_has_target(ioh));

	ioh->op = op;
	ioh->result = 0;

	pgaio_io_update_state(ioh, PGAIO_HS_DEFINED);

	/* allow a new IO to be staged */
	pgaio_my_backend->handed_out_io = NULL;

	pgaio_io_call_stage(ioh);

	pgaio_io_update_state(ioh, PGAIO_HS_STAGED);

	/*
	 * Synchronous execution has to be executed, well, synchronously, so check
	 * that first.
	 */
	needs_synchronous = pgaio_io_needs_synchronous_execution(ioh);

	pgaio_debug_io(DEBUG3, ioh,
				   "prepared (synchronous: %d, in_batch: %d)",
				   needs_synchronous, pgaio_my_backend->in_batchmode);

	if (!needs_synchronous)
	{
		pgaio_my_backend->staged_ios[pgaio_my_backend->num_staged_ios++] = ioh;
		Assert(pgaio_my_backend->num_staged_ios <= PGAIO_SUBMIT_BATCH_SIZE);

		/*
		 * Unless code explicitly opted into batching IOs, submit the IO
		 * immediately.
		 */
		if (!pgaio_my_backend->in_batchmode)
			pgaio_submit_staged();
	}
	else
	{
		pgaio_io_prepare_submit(ioh);
		pgaio_io_perform_synchronously(ioh);
	}
}

bool
pgaio_io_needs_synchronous_execution(PgAioHandle *ioh)
{
	/*
	 * If the caller said to execute the IO synchronously, do so.
	 *
	 * XXX: We could optimize the logic when to execute synchronously by first
	 * checking if there are other IOs in flight and only synchronously
	 * executing if not. Unclear whether that'll be sufficiently common to be
	 * worth worrying about.
	 */
	if (ioh->flags & PGAIO_HF_SYNCHRONOUS)
		return true;

	/* Check if the IO method requires synchronous execution of IO */
	if (pgaio_method_ops->needs_synchronous_execution)
		return pgaio_method_ops->needs_synchronous_execution(ioh);

	return false;
}

/*
 * Handle IO being processed by IO method.
 *
 * Should be called by IO methods / synchronous IO execution, just before the
 * IO is performed.
 */
void
pgaio_io_prepare_submit(PgAioHandle *ioh)
{
	pgaio_io_update_state(ioh, PGAIO_HS_SUBMITTED);

	dclist_push_tail(&pgaio_my_backend->in_flight_ios, &ioh->node);
}

/*
 * Handle IO getting completed by a method.
 *
 * Should be called by IO methods / synchronous IO execution, just after the
 * IO has been performed.
 *
 * Expects to be called in a critical section. We expect IOs to be usable for
 * WAL etc, which requires being able to execute completion callbacks in a
 * critical section.
 */
void
pgaio_io_process_completion(PgAioHandle *ioh, int result)
{
	Assert(ioh->state == PGAIO_HS_SUBMITTED);

	Assert(CritSectionCount > 0);

	ioh->result = result;

	pgaio_io_update_state(ioh, PGAIO_HS_COMPLETED_IO);

	pgaio_io_call_inj(ioh, "AIO_PROCESS_COMPLETION_BEFORE_SHARED");

	pgaio_io_call_complete_shared(ioh);

	pgaio_io_update_state(ioh, PGAIO_HS_COMPLETED_SHARED);

	/* condition variable broadcast ensures state is visible before wakeup */
	ConditionVariableBroadcast(&ioh->cv);

	/* contains call to pgaio_io_call_complete_local() */
	if (ioh->owner_procno == MyProcNumber)
		pgaio_io_reclaim(ioh);
}

/*
 * Has the IO completed and thus the IO handle been reused?
 *
 * This is useful when waiting for IO completion at a low level (e.g. in an IO
 * method's ->wait_one() callback).
 */
bool
pgaio_io_was_recycled(PgAioHandle *ioh, uint64 ref_generation, PgAioHandleState *state)
{
	*state = ioh->state;
	pg_read_barrier();

	return ioh->generation != ref_generation;
}

/*
 * Wait for IO to complete. External code should never use this, outside of
 * the AIO subsystem waits are only allowed via pgaio_wref_wait().
 */
static void
pgaio_io_wait(PgAioHandle *ioh, uint64 ref_generation)
{
	PgAioHandleState state;
	bool		am_owner;

	am_owner = ioh->owner_procno == MyProcNumber;

	if (pgaio_io_was_recycled(ioh, ref_generation, &state))
		return;

	if (am_owner)
	{
		if (state != PGAIO_HS_SUBMITTED
			&& state != PGAIO_HS_COMPLETED_IO
			&& state != PGAIO_HS_COMPLETED_SHARED
			&& state != PGAIO_HS_COMPLETED_LOCAL)
		{
			elog(PANIC, "waiting for own IO in wrong state: %d",
				 state);
		}
	}

	while (true)
	{
		if (pgaio_io_was_recycled(ioh, ref_generation, &state))
			return;

		switch (state)
		{
			case PGAIO_HS_IDLE:
			case PGAIO_HS_HANDED_OUT:
				elog(ERROR, "IO in wrong state: %d", state);
				break;

			case PGAIO_HS_SUBMITTED:

				/*
				 * If we need to wait via the IO method, do so now. Don't
				 * check via the IO method if the issuing backend is executing
				 * the IO synchronously.
				 */
				if (pgaio_method_ops->wait_one && !(ioh->flags & PGAIO_HF_SYNCHRONOUS))
				{
					pgaio_method_ops->wait_one(ioh, ref_generation);
					continue;
				}
				/* fallthrough */

				/* waiting for owner to submit */
			case PGAIO_HS_DEFINED:
			case PGAIO_HS_STAGED:
				/* waiting for reaper to complete */
				/* fallthrough */
			case PGAIO_HS_COMPLETED_IO:
				/* shouldn't be able to hit this otherwise */
				Assert(IsUnderPostmaster);
				/* ensure we're going to get woken up */
				ConditionVariablePrepareToSleep(&ioh->cv);

				while (!pgaio_io_was_recycled(ioh, ref_generation, &state))
				{
					if (state == PGAIO_HS_COMPLETED_SHARED ||
						state == PGAIO_HS_COMPLETED_LOCAL)
						break;
					ConditionVariableSleep(&ioh->cv, WAIT_EVENT_AIO_IO_COMPLETION);
				}

				ConditionVariableCancelSleep();
				break;

			case PGAIO_HS_COMPLETED_SHARED:
			case PGAIO_HS_COMPLETED_LOCAL:
				/* see above */
				if (am_owner)
					pgaio_io_reclaim(ioh);
				return;
		}
	}
}

/*
 * Make IO handle ready to be reused after IO has completed or after the
 * handle has been released without being used.
 */
static void
pgaio_io_reclaim(PgAioHandle *ioh)
{
	/* This is only ok if it's our IO */
	Assert(ioh->owner_procno == MyProcNumber);
	Assert(ioh->state != PGAIO_HS_IDLE);

	/*
	 * It's a bit ugly, but right now the easiest place to put the execution
	 * of shared completion callbacks is this function, as we need to execute
	 * local callbacks just before reclaiming at multiple callsites.
	 */
	if (ioh->state == PGAIO_HS_COMPLETED_SHARED)
	{
		pgaio_io_call_complete_local(ioh);
		pgaio_io_update_state(ioh, PGAIO_HS_COMPLETED_LOCAL);
	}

	pgaio_debug_io(DEBUG4, ioh,
				   "reclaiming: distilled_result: (status %s, id %u, error_data %d), raw_result: %d",
				   pgaio_result_status_string(ioh->distilled_result.status),
				   ioh->distilled_result.id,
				   ioh->distilled_result.error_data,
				   ioh->result);

	/* if the IO has been defined, we might need to do more work */
	if (ioh->state != PGAIO_HS_HANDED_OUT)
	{
		dclist_delete_from(&pgaio_my_backend->in_flight_ios, &ioh->node);

		if (ioh->report_return)
		{
			ioh->report_return->result = ioh->distilled_result;
			ioh->report_return->target_data = ioh->target_data;
		}
	}

	if (ioh->resowner)
	{
		ResourceOwnerForgetAioHandle(ioh->resowner, &ioh->resowner_node);
		ioh->resowner = NULL;
	}

	Assert(!ioh->resowner);

	ioh->op = PGAIO_OP_INVALID;
	ioh->target = PGAIO_TID_INVALID;
	ioh->flags = 0;
	ioh->num_callbacks = 0;
	ioh->handle_data_len = 0;
	ioh->report_return = NULL;
	ioh->result = 0;
	ioh->distilled_result.status = PGAIO_RS_UNKNOWN;

	/* XXX: the barrier is probably superfluous */
	pg_write_barrier();
	ioh->generation++;

	pgaio_io_update_state(ioh, PGAIO_HS_IDLE);

	/*
	 * We push the IO to the head of the idle IO list, that seems more cache
	 * efficient in cases where only a few IOs are used.
	 */
	dclist_push_head(&pgaio_my_backend->idle_ios, &ioh->node);
}

/*
 * Wait for an IO handle to become usable.
 *
 * This only really is useful for pgaio_io_acquire().
 */
static void
pgaio_io_wait_for_free(void)
{
	int			reclaimed = 0;

	pgaio_debug(DEBUG2, "waiting for self with %d pending",
				pgaio_my_backend->num_staged_ios);

	/*
	 * First check if any of our IOs actually have completed - when using
	 * worker, that'll often be the case. We could do so as part of the loop
	 * below, but that'd potentially lead us to wait for some IO submitted
	 * before.
	 */
	for (int i = 0; i < io_max_concurrency; i++)
	{
		PgAioHandle *ioh = &pgaio_ctl->io_handles[pgaio_my_backend->io_handle_off + i];

		if (ioh->state == PGAIO_HS_COMPLETED_SHARED)
		{
			pgaio_io_reclaim(ioh);
			reclaimed++;
		}
	}

	if (reclaimed > 0)
		return;

	/*
	 * If we have any unsubmitted IOs, submit them now. We'll start waiting in
	 * a second, so it's better they're in flight. This also addresses the
	 * edge-case that all IOs are unsubmitted.
	 */
	if (pgaio_my_backend->num_staged_ios > 0)
		pgaio_submit_staged();

	if (dclist_count(&pgaio_my_backend->in_flight_ios) == 0)
		elog(ERROR, "no free IOs despite no in-flight IOs");

	/*
	 * Wait for the oldest in-flight IO to complete.
	 *
	 * XXX: Reusing the general IO wait is suboptimal, we don't need to wait
	 * for that specific IO to complete, we just need *any* IO to complete.
	 */
	{
		PgAioHandle *ioh = dclist_head_element(PgAioHandle, node,
											   &pgaio_my_backend->in_flight_ios);

		switch (ioh->state)
		{
				/* should not be in in-flight list */
			case PGAIO_HS_IDLE:
			case PGAIO_HS_DEFINED:
			case PGAIO_HS_HANDED_OUT:
			case PGAIO_HS_STAGED:
			case PGAIO_HS_COMPLETED_LOCAL:
				elog(ERROR, "shouldn't get here with io:%d in state %d",
					 pgaio_io_get_id(ioh), ioh->state);
				break;

			case PGAIO_HS_COMPLETED_IO:
			case PGAIO_HS_SUBMITTED:
				pgaio_debug_io(DEBUG2, ioh,
							   "waiting for free io with %d in flight",
							   dclist_count(&pgaio_my_backend->in_flight_ios));

				/*
				 * In a more general case this would be racy, because the
				 * generation could increase after we read ioh->state above.
				 * But we are only looking at IOs by the current backend and
				 * the IO can only be recycled by this backend.
				 */
				pgaio_io_wait(ioh, ioh->generation);
				break;

			case PGAIO_HS_COMPLETED_SHARED:
				/* it's possible that another backend just finished this IO */
				pgaio_io_reclaim(ioh);
				break;
		}

		if (dclist_count(&pgaio_my_backend->idle_ios) == 0)
			elog(PANIC, "no idle IO after waiting for IO to terminate");
		return;
	}
}

/*
 * Internal - code outside of AIO should never need this and it'd be hard for
 * such code to be safe.
 */
static PgAioHandle *
pgaio_io_from_wref(PgAioWaitRef *iow, uint64 *ref_generation)
{
	PgAioHandle *ioh;

	Assert(iow->aio_index < pgaio_ctl->io_handle_count);

	ioh = &pgaio_ctl->io_handles[iow->aio_index];

	*ref_generation = ((uint64) iow->generation_upper) << 32 |
		iow->generation_lower;

	Assert(*ref_generation != 0);

	return ioh;
}

static const char *
pgaio_io_state_get_name(PgAioHandleState s)
{
#define PGAIO_HS_TOSTR_CASE(sym) case PGAIO_HS_##sym: return #sym
	switch (s)
	{
			PGAIO_HS_TOSTR_CASE(IDLE);
			PGAIO_HS_TOSTR_CASE(HANDED_OUT);
			PGAIO_HS_TOSTR_CASE(DEFINED);
			PGAIO_HS_TOSTR_CASE(STAGED);
			PGAIO_HS_TOSTR_CASE(SUBMITTED);
			PGAIO_HS_TOSTR_CASE(COMPLETED_IO);
			PGAIO_HS_TOSTR_CASE(COMPLETED_SHARED);
			PGAIO_HS_TOSTR_CASE(COMPLETED_LOCAL);
	}
#undef PGAIO_HS_TOSTR_CASE

	return NULL;				/* silence compiler */
}

const char *
pgaio_io_get_state_name(PgAioHandle *ioh)
{
	return pgaio_io_state_get_name(ioh->state);
}

const char *
pgaio_result_status_string(PgAioResultStatus rs)
{
	switch (rs)
	{
		case PGAIO_RS_UNKNOWN:
			return "UNKNOWN";
		case PGAIO_RS_OK:
			return "OK";
		case PGAIO_RS_PARTIAL:
			return "PARTIAL";
		case PGAIO_RS_ERROR:
			return "ERROR";
	}

	return NULL;				/* silence compiler */
}



/* --------------------------------------------------------------------------------
 * Functions primarily related to IO Wait References
 * --------------------------------------------------------------------------------
 */

/*
 * Mark a wait reference as invalid
 */
void
pgaio_wref_clear(PgAioWaitRef *iow)
{
	iow->aio_index = PG_UINT32_MAX;
}

/* Is the wait reference valid? */
bool
pgaio_wref_valid(PgAioWaitRef *iow)
{
	return iow->aio_index != PG_UINT32_MAX;
}

/*
 * Similar to pgaio_io_get_id(), just for wait references.
 */
int
pgaio_wref_get_id(PgAioWaitRef *iow)
{
	Assert(pgaio_wref_valid(iow));
	return iow->aio_index;
}

/*
 * Wait for the IO to have completed. Can be called in any process, not just
 * in the issuing backend.
 */
void
pgaio_wref_wait(PgAioWaitRef *iow)
{
	uint64		ref_generation;
	PgAioHandle *ioh;

	ioh = pgaio_io_from_wref(iow, &ref_generation);

	pgaio_io_wait(ioh, ref_generation);
}

/*
 * Check if the referenced IO completed, without blocking.
 */
bool
pgaio_wref_check_done(PgAioWaitRef *iow)
{
	uint64		ref_generation;
	PgAioHandleState state;
	bool		am_owner;
	PgAioHandle *ioh;

	ioh = pgaio_io_from_wref(iow, &ref_generation);

	if (pgaio_io_was_recycled(ioh, ref_generation, &state))
		return true;

	if (state == PGAIO_HS_IDLE)
		return true;

	am_owner = ioh->owner_procno == MyProcNumber;

	if (state == PGAIO_HS_COMPLETED_SHARED ||
		state == PGAIO_HS_COMPLETED_LOCAL)
	{
		if (am_owner)
			pgaio_io_reclaim(ioh);
		return true;
	}

	/*
	 * XXX: It likely would be worth checking in with the io method, to give
	 * the IO method a chance to check if there are completion events queued.
	 */

	return false;
}



/* --------------------------------------------------------------------------------
 * Actions on multiple IOs.
 * --------------------------------------------------------------------------------
 */

/*
 * Submit IOs in batches going forward.
 *
 * Submitting multiple IOs at once can be substantially faster than doing so
 * one-by-one. At the same time, submitting multiple IOs at once requires more
 * care to avoid deadlocks.
 *
 * Consider backend A staging an IO for buffer 1 and then trying to start IO
 * on buffer 2, while backend B does the inverse. If A submitted the IO before
 * moving on to buffer 2, this works just fine, B will wait for the IO to
 * complete. But if batching were used, each backend will wait for IO that has
 * not yet been submitted to complete, i.e. forever.
 *
 * End batch submission mode with pgaio_exit_batchmode().  (Throwing errors is
 * allowed; error recovery will end the batch.)
 *
 * To avoid deadlocks, code needs to ensure that it will not wait for another
 * backend while there is unsubmitted IO. E.g. by using conditional lock
 * acquisition when acquiring buffer locks. To check if there currently are
 * staged IOs, call pgaio_have_staged() and to submit all staged IOs call
 * pgaio_submit_staged().
 *
 * It is not allowed to enter batchmode while already in batchmode, it's
 * unlikely to ever be needed, as code needs to be explicitly aware of being
 * called in batchmode, to avoid the deadlock risks explained above.
 *
 * Note that IOs may get submitted before pgaio_exit_batchmode() is called,
 * e.g. because too many IOs have been staged or because pgaio_submit_staged()
 * was called.
 */
void
pgaio_enter_batchmode(void)
{
	if (pgaio_my_backend->in_batchmode)
		elog(ERROR, "starting batch while batch already in progress");
	pgaio_my_backend->in_batchmode = true;
}

/*
 * Stop submitting IOs in batches.
 */
void
pgaio_exit_batchmode(void)
{
	Assert(pgaio_my_backend->in_batchmode);

	pgaio_submit_staged();
	pgaio_my_backend->in_batchmode = false;
}

/*
 * Are there staged but unsubmitted IOs?
 *
 * See comment above pgaio_enter_batchmode() for why code may need to check if
 * there is IO in that state.
 */
bool
pgaio_have_staged(void)
{
	Assert(pgaio_my_backend->in_batchmode ||
		   pgaio_my_backend->num_staged_ios == 0);
	return pgaio_my_backend->num_staged_ios > 0;
}

/*
 * Submit all staged but not yet submitted IOs.
 *
 * Unless in batch mode, this never needs to be called, as IOs get submitted
 * as soon as possible. While in batchmode pgaio_submit_staged() can be called
 * before waiting on another backend, to avoid the risk of deadlocks. See
 * pgaio_enter_batchmode().
 */
void
pgaio_submit_staged(void)
{
	int			total_submitted = 0;
	int			did_submit;

	if (pgaio_my_backend->num_staged_ios == 0)
		return;


	START_CRIT_SECTION();

	did_submit = pgaio_method_ops->submit(pgaio_my_backend->num_staged_ios,
										  pgaio_my_backend->staged_ios);

	END_CRIT_SECTION();

	total_submitted += did_submit;

	Assert(total_submitted == did_submit);

	pgaio_my_backend->num_staged_ios = 0;

	pgaio_debug(DEBUG4,
				"aio: submitted %d IOs",
				total_submitted);
}



/* --------------------------------------------------------------------------------
 * Other
 * --------------------------------------------------------------------------------
 */


/*
 * Perform AIO related cleanup after an error.
 *
 * This should be called early in the error recovery paths, as later steps may
 * need to issue AIO (e.g. to record a transaction abort WAL record).
 */
void
pgaio_error_cleanup(void)
{
	/*
	 * It is possible that code errored out after pgaio_enter_batchmode() but
	 * before pgaio_exit_batchmode() was called. In that case we need to
	 * submit the IO now.
	 */
	if (pgaio_my_backend->in_batchmode)
	{
		pgaio_my_backend->in_batchmode = false;

		pgaio_submit_staged();
	}

	/*
	 * As we aren't in batchmode, there shouldn't be any unsubmitted IOs.
	 */
	Assert(pgaio_my_backend->num_staged_ios == 0);
}

/*
 * Perform AIO related checks at (sub-)transactional boundaries.
 *
 * This should be called late during (sub-)transactional commit/abort, after
 * all steps that might need to perform AIO, so that we can verify that the
 * AIO subsystem is in a valid state at the end of a transaction.
 */
void
AtEOXact_Aio(bool is_commit)
{
	/*
	 * We should never be in batch mode at transactional boundaries. In case
	 * an error was thrown while in batch mode, pgaio_error_cleanup() should
	 * have exited batchmode.
	 *
	 * In case we are in batchmode somehow, make sure to submit all staged
	 * IOs, other backends may need them to complete to continue.
	 */
	if (pgaio_my_backend->in_batchmode)
	{
		pgaio_error_cleanup();
		elog(WARNING, "open AIO batch at end of (sub-)transaction");
	}

	/*
	 * As we aren't in batchmode, there shouldn't be any unsubmitted IOs.
	 */
	Assert(pgaio_my_backend->num_staged_ios == 0);
}

/*
 * Need to submit staged but not yet submitted IOs using the fd, otherwise
 * the IO would end up targeting something bogus.
 */
void
pgaio_closing_fd(int fd)
{
	/*
	 * Might be called before AIO is initialized or in a subprocess that
	 * doesn't use AIO.
	 */
	if (!pgaio_my_backend)
		return;

	/*
	 * For now just submit all staged IOs - we could be more selective, but
	 * it's probably not worth it.
	 */
	pgaio_submit_staged();
}

/*
 * Registered as before_shmem_exit() callback in pgaio_init_backend()
 */
void
pgaio_shutdown(int code, Datum arg)
{
	Assert(pgaio_my_backend);
	Assert(!pgaio_my_backend->handed_out_io);

	/* first clean up resources as we would at a transaction boundary */
	AtEOXact_Aio(code == 0);

	/*
	 * Before exiting, make sure that all IOs are finished. That has two main
	 * purposes:
	 *
	 * - Some kernel-level AIO mechanisms don't deal well with the issuer of
	 * an AIO exiting before IO completed
	 *
	 * - It'd be confusing to see partially finished IOs in stats views etc
	 */
	while (!dclist_is_empty(&pgaio_my_backend->in_flight_ios))
	{
		PgAioHandle *ioh = dclist_head_element(PgAioHandle, node, &pgaio_my_backend->in_flight_ios);

		/* see comment in pgaio_io_wait_for_free() about raciness */
		pgaio_io_wait(ioh, ioh->generation);
	}

	pgaio_my_backend = NULL;
}

void
assign_io_method(int newval, void *extra)
{
	Assert(pgaio_method_ops_table[newval] != NULL);
	Assert(newval < lengthof(io_method_options));

	pgaio_method_ops = pgaio_method_ops_table[newval];
}

bool
check_io_max_concurrency(int *newval, void **extra, GucSource source)
{
	if (*newval == -1)
	{
		/*
		 * Auto-tuning will be applied later during startup, as auto-tuning
		 * depends on the value of various GUCs.
		 */
		return true;
	}
	else if (*newval == 0)
	{
		GUC_check_errdetail("Only -1 or values bigger than 0 are valid.");
		return false;
	}

	return true;
}



/* --------------------------------------------------------------------------------
 * Injection point support
 * --------------------------------------------------------------------------------
 */

#ifdef USE_INJECTION_POINTS

/*
 * Call injection point with support for pgaio_inj_io_get().
 */
void
pgaio_io_call_inj(PgAioHandle *ioh, const char *injection_point)
{
	pgaio_inj_cur_handle = ioh;

	PG_TRY();
	{
		InjectionPointCached(injection_point);
	}
	PG_FINALLY();
	{
		pgaio_inj_cur_handle = NULL;
	}
	PG_END_TRY();
}

/*
 * Return IO associated with injection point invocation. This is only needed
 * as injection points currently don't support arguments.
 */
PgAioHandle *
pgaio_inj_io_get(void)
{
	return pgaio_inj_cur_handle;
}

#endif
