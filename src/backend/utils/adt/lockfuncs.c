/*-------------------------------------------------------------------------
 *
 * lockfuncs.c
 *		Set-returning functions to view the state of locks within the DB.
 *
 * Copyright (c) 2002-2005, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		$PostgreSQL: pgsql/src/backend/utils/adt/lockfuncs.c,v 1.20 2005/10/15 02:49:28 momjian Exp $
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


/* This must match enum LockTagType! */
static const char *const LockTagTypeNames[] = {
	"relation",
	"extend",
	"page",
	"tuple",
	"transactionid",
	"object",
	"userlock"
};

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
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_locks view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(13, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "locktype",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "database",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "relation",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "page",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "tuple",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "transactionid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "classid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "objid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "objsubid",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "transaction",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "pid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "mode",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "granted",
						   BOOLOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the locking information that we will format and send
		 * out as a result set.
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
		LOCKMODE	mode = 0;
		const char *locktypename;
		char		tnbuf[32];
		Datum		values[13];
		char		nulls[13];
		HeapTuple	tuple;
		Datum		result;

		proclock = &(lockData->proclocks[mystatus->currIdx]);
		lock = &(lockData->locks[mystatus->currIdx]);
		proc = &(lockData->procs[mystatus->currIdx]);

		/*
		 * Look to see if there are any held lock modes in this PROCLOCK. If
		 * so, report, and destructively modify lockData so we don't report
		 * again.
		 */
		granted = false;
		if (proclock->holdMask)
		{
			for (mode = 0; mode < MAX_LOCKMODES; mode++)
			{
				if (proclock->holdMask & LOCKBIT_ON(mode))
				{
					granted = true;
					proclock->holdMask &= LOCKBIT_OFF(mode);
					break;
				}
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
				 * We are now done with this PROCLOCK, so advance pointer to
				 * continue with next one on next call.
				 */
				mystatus->currIdx++;
			}
			else
			{
				/*
				 * Okay, we've displayed all the locks associated with this
				 * PROCLOCK, proceed to the next one.
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

		if (lock->tag.locktag_type <= LOCKTAG_USERLOCK)
			locktypename = LockTagTypeNames[lock->tag.locktag_type];
		else
		{
			snprintf(tnbuf, sizeof(tnbuf), "unknown %d",
					 (int) lock->tag.locktag_type);
			locktypename = tnbuf;
		}
		values[0] = DirectFunctionCall1(textin,
										CStringGetDatum(locktypename));


		switch (lock->tag.locktag_type)
		{
			case LOCKTAG_RELATION:
			case LOCKTAG_RELATION_EXTEND:
				values[1] = ObjectIdGetDatum(lock->tag.locktag_field1);
				values[2] = ObjectIdGetDatum(lock->tag.locktag_field2);
				nulls[3] = 'n';
				nulls[4] = 'n';
				nulls[5] = 'n';
				nulls[6] = 'n';
				nulls[7] = 'n';
				nulls[8] = 'n';
				break;
			case LOCKTAG_PAGE:
				values[1] = ObjectIdGetDatum(lock->tag.locktag_field1);
				values[2] = ObjectIdGetDatum(lock->tag.locktag_field2);
				values[3] = UInt32GetDatum(lock->tag.locktag_field3);
				nulls[4] = 'n';
				nulls[5] = 'n';
				nulls[6] = 'n';
				nulls[7] = 'n';
				nulls[8] = 'n';
				break;
			case LOCKTAG_TUPLE:
				values[1] = ObjectIdGetDatum(lock->tag.locktag_field1);
				values[2] = ObjectIdGetDatum(lock->tag.locktag_field2);
				values[3] = UInt32GetDatum(lock->tag.locktag_field3);
				values[4] = UInt16GetDatum(lock->tag.locktag_field4);
				nulls[5] = 'n';
				nulls[6] = 'n';
				nulls[7] = 'n';
				nulls[8] = 'n';
				break;
			case LOCKTAG_TRANSACTION:
				values[5] = TransactionIdGetDatum(lock->tag.locktag_field1);
				nulls[1] = 'n';
				nulls[2] = 'n';
				nulls[3] = 'n';
				nulls[4] = 'n';
				nulls[6] = 'n';
				nulls[7] = 'n';
				nulls[8] = 'n';
				break;
			case LOCKTAG_OBJECT:
			case LOCKTAG_USERLOCK:
			default:			/* treat unknown locktags like OBJECT */
				values[1] = ObjectIdGetDatum(lock->tag.locktag_field1);
				values[6] = ObjectIdGetDatum(lock->tag.locktag_field2);
				values[7] = ObjectIdGetDatum(lock->tag.locktag_field3);
				values[8] = Int16GetDatum(lock->tag.locktag_field4);
				nulls[2] = 'n';
				nulls[3] = 'n';
				nulls[4] = 'n';
				nulls[5] = 'n';
				break;
		}

		values[9] = TransactionIdGetDatum(proc->xid);
		if (proc->pid != 0)
			values[10] = Int32GetDatum(proc->pid);
		else
			nulls[10] = 'n';
		values[11] = DirectFunctionCall1(textin,
									 CStringGetDatum(GetLockmodeName(mode)));
		values[12] = BoolGetDatum(granted);

		tuple = heap_formtuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}
