/*-------------------------------------------------------------------------
 *
 * lockfuncs.c
 *		Set-returning functions to view the state of locks within the DB.
 *
 * Copyright (c) 2002-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		$Header: /cvsroot/pgsql/src/backend/utils/adt/lockfuncs.c,v 1.11 2003/08/04 23:59:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/builtins.h"


/* Working status for pg_lock_status */
typedef struct
{
	LockData   *lockData;		/* state data from lmgr */
	int			currIdx;		/* current PROCLOCK index */
} PG_Lock_Status;

/*
 * pg_lock_status - produce a view with one row per held or awaited lock mode
 */
Datum
pg_lock_status(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	PG_Lock_Status *mystatus;
	LockData   *lockData;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_locks view in initdb.sh */
		tupdesc = CreateTemplateTupleDesc(6, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relation",
						   OIDOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "database",
						   OIDOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "transaction",
						   XIDOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "pid",
						   INT4OID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "mode",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "granted",
						   BOOLOID, -1, 0, false);

		funcctx->slot = TupleDescGetSlot(tupdesc);

		/*
		 * Collect all the locking information that we will format and
		 * send out as a result set.
		 */
		mystatus = (PG_Lock_Status *) palloc(sizeof(PG_Lock_Status));
		funcctx->user_fctx = (void *) mystatus;

		mystatus->lockData = GetLockStatusData();
		mystatus->currIdx = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	mystatus = (PG_Lock_Status *) funcctx->user_fctx;
	lockData = mystatus->lockData;

	while (mystatus->currIdx < lockData->nelements)
	{
		PROCLOCK   *proclock;
		LOCK	   *lock;
		PGPROC	   *proc;
		bool		granted;
		LOCKMODE	mode;
		Datum		values[6];
		char		nulls[6];
		HeapTuple	tuple;
		Datum		result;

		proclock = &(lockData->proclocks[mystatus->currIdx]);
		lock = &(lockData->locks[mystatus->currIdx]);
		proc = &(lockData->procs[mystatus->currIdx]);

		/*
		 * Look to see if there are any held lock modes in this PROCLOCK.
		 * If so, report, and destructively modify lockData so we don't
		 * report again.
		 */
		granted = false;
		for (mode = 0; mode < MAX_LOCKMODES; mode++)
		{
			if (proclock->holding[mode] > 0)
			{
				granted = true;
				proclock->holding[mode] = 0;
				break;
			}
		}

		/*
		 * If no (more) held modes to report, see if PROC is waiting for a
		 * lock on this lock.
		 */
		if (!granted)
		{
			if (proc->waitLock == (LOCK *) MAKE_PTR(proclock->tag.lock))
			{
				/* Yes, so report it with proper mode */
				mode = proc->waitLockMode;

				/*
				 * We are now done with this PROCLOCK, so advance pointer
				 * to continue with next one on next call.
				 */
				mystatus->currIdx++;
			}
			else
			{
				/*
				 * Okay, we've displayed all the locks associated with
				 * this PROCLOCK, proceed to the next one.
				 */
				mystatus->currIdx++;
				continue;
			}
		}

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));

		if (lock->tag.relId == XactLockTableId && lock->tag.dbId == 0)
		{
			/* Lock is for transaction ID */
			nulls[0] = 'n';
			nulls[1] = 'n';
			values[2] = TransactionIdGetDatum(lock->tag.objId.xid);
		}
		else
		{
			/* Lock is for a relation */
			values[0] = ObjectIdGetDatum(lock->tag.relId);
			values[1] = ObjectIdGetDatum(lock->tag.dbId);
			nulls[2] = 'n';

		}

		values[3] = Int32GetDatum(proc->pid);
		values[4] = DirectFunctionCall1(textin,
								 CStringGetDatum(GetLockmodeName(mode)));
		values[5] = BoolGetDatum(granted);

		tuple = heap_formtuple(funcctx->slot->ttc_tupleDescriptor,
							   values, nulls);
		result = TupleGetDatum(funcctx->slot, tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}
