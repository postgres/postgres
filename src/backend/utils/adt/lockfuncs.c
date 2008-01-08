/*-------------------------------------------------------------------------
 *
 * lockfuncs.c
 *		Functions for SQL access to various lock-manager capabilities.
 *
 * Copyright (c) 2002-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		$PostgreSQL: pgsql/src/backend/utils/adt/lockfuncs.c,v 1.32 2008/01/08 23:18:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/builtins.h"


/* This must match enum LockTagType! */
static const char *const LockTagTypeNames[] = {
	"relation",
	"extend",
	"page",
	"tuple",
	"transactionid",
	"virtualxid",
	"object",
	"userlock",
	"advisory"
};

/* Working status for pg_lock_status */
typedef struct
{
	LockData   *lockData;		/* state data from lmgr */
	int			currIdx;		/* current PROCLOCK index */
} PG_Lock_Status;


/*
 * VXIDGetDatum - Construct a text representation of a VXID
 *
 * This is currently only used in pg_lock_status, so we put it here.
 */
static Datum
VXIDGetDatum(BackendId bid, LocalTransactionId lxid)
{
	/*
	 * The representation is "<bid>/<lxid>", decimal and unsigned decimal
	 * respectively.  Note that elog.c also knows how to format a vxid.
	 */
	char		vxidstr[32];

	snprintf(vxidstr, sizeof(vxidstr), "%d/%u", bid, lxid);

	return DirectFunctionCall1(textin, CStringGetDatum(vxidstr));
}


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
		tupdesc = CreateTemplateTupleDesc(14, false);
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
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "virtualxid",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "transactionid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "classid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "objid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "objsubid",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "virtualtransaction",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "pid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "mode",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 14, "granted",
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
		Datum		values[14];
		char		nulls[14];
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
			if (proc->waitLock == proclock->tag.myLock)
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

		if (lock->tag.locktag_type <= LOCKTAG_LAST_TYPE)
			locktypename = LockTagTypeNames[lock->tag.locktag_type];
		else
		{
			snprintf(tnbuf, sizeof(tnbuf), "unknown %d",
					 (int) lock->tag.locktag_type);
			locktypename = tnbuf;
		}
		values[0] = DirectFunctionCall1(textin,
										CStringGetDatum(locktypename));

		switch ((LockTagType) lock->tag.locktag_type)
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
				nulls[9] = 'n';
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
				nulls[9] = 'n';
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
				nulls[9] = 'n';
				break;
			case LOCKTAG_TRANSACTION:
				values[6] = TransactionIdGetDatum(lock->tag.locktag_field1);
				nulls[1] = 'n';
				nulls[2] = 'n';
				nulls[3] = 'n';
				nulls[4] = 'n';
				nulls[5] = 'n';
				nulls[7] = 'n';
				nulls[8] = 'n';
				nulls[9] = 'n';
				break;
			case LOCKTAG_VIRTUALTRANSACTION:
				values[5] = VXIDGetDatum(lock->tag.locktag_field1,
										 lock->tag.locktag_field2);
				nulls[1] = 'n';
				nulls[2] = 'n';
				nulls[3] = 'n';
				nulls[4] = 'n';
				nulls[6] = 'n';
				nulls[7] = 'n';
				nulls[8] = 'n';
				nulls[9] = 'n';
				break;
			case LOCKTAG_OBJECT:
			case LOCKTAG_USERLOCK:
			case LOCKTAG_ADVISORY:
			default:			/* treat unknown locktags like OBJECT */
				values[1] = ObjectIdGetDatum(lock->tag.locktag_field1);
				values[7] = ObjectIdGetDatum(lock->tag.locktag_field2);
				values[8] = ObjectIdGetDatum(lock->tag.locktag_field3);
				values[9] = Int16GetDatum(lock->tag.locktag_field4);
				nulls[2] = 'n';
				nulls[3] = 'n';
				nulls[4] = 'n';
				nulls[5] = 'n';
				nulls[6] = 'n';
				break;
		}

		values[10] = VXIDGetDatum(proc->backendId, proc->lxid);
		if (proc->pid != 0)
			values[11] = Int32GetDatum(proc->pid);
		else
			nulls[11] = 'n';
		values[12] = DirectFunctionCall1(textin,
					  CStringGetDatum(GetLockmodeName(LOCK_LOCKMETHOD(*lock),
													  mode)));
		values[13] = BoolGetDatum(granted);

		tuple = heap_formtuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * Functions for manipulating advisory locks
 *
 * We make use of the locktag fields as follows:
 *
 *	field1: MyDatabaseId ... ensures locks are local to each database
 *	field2: first of 2 int4 keys, or high-order half of an int8 key
 *	field3: second of 2 int4 keys, or low-order half of an int8 key
 *	field4: 1 if using an int8 key, 2 if using 2 int4 keys
 */
#define SET_LOCKTAG_INT64(tag, key64) \
	SET_LOCKTAG_ADVISORY(tag, \
						 MyDatabaseId, \
						 (uint32) ((key64) >> 32), \
						 (uint32) (key64), \
						 1)
#define SET_LOCKTAG_INT32(tag, key1, key2) \
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, key1, key2, 2)

/*
 * pg_advisory_lock(int8) - acquire exclusive lock on an int8 key
 */
Datum
pg_advisory_lock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;

	SET_LOCKTAG_INT64(tag, key);

	(void) LockAcquire(&tag, ExclusiveLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_lock_shared(int8) - acquire share lock on an int8 key
 */
Datum
pg_advisory_lock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;

	SET_LOCKTAG_INT64(tag, key);

	(void) LockAcquire(&tag, ShareLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_try_advisory_lock(int8) - acquire exclusive lock on an int8 key, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum
pg_try_advisory_lock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockAcquire(&tag, ExclusiveLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_lock_shared(int8) - acquire share lock on an int8 key, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum
pg_try_advisory_lock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockAcquire(&tag, ShareLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_advisory_unlock(int8) - release exclusive lock on an int8 key
 *
 * Returns true if successful, false if lock was not held
*/
Datum
pg_advisory_unlock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockRelease(&tag, ExclusiveLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_shared(int8) - release share lock on an int8 key
 *
 * Returns true if successful, false if lock was not held
 */
Datum
pg_advisory_unlock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockRelease(&tag, ShareLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_lock(int4, int4) - acquire exclusive lock on 2 int4 keys
 */
Datum
pg_advisory_lock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;

	SET_LOCKTAG_INT32(tag, key1, key2);

	(void) LockAcquire(&tag, ExclusiveLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_lock_shared(int4, int4) - acquire share lock on 2 int4 keys
 */
Datum
pg_advisory_lock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;

	SET_LOCKTAG_INT32(tag, key1, key2);

	(void) LockAcquire(&tag, ShareLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_try_advisory_lock(int4, int4) - acquire exclusive lock on 2 int4 keys, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum
pg_try_advisory_lock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockAcquire(&tag, ExclusiveLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_lock_shared(int4, int4) - acquire share lock on 2 int4 keys, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum
pg_try_advisory_lock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockAcquire(&tag, ShareLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_advisory_unlock(int4, int4) - release exclusive lock on 2 int4 keys
 *
 * Returns true if successful, false if lock was not held
*/
Datum
pg_advisory_unlock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockRelease(&tag, ExclusiveLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_shared(int4, int4) - release share lock on 2 int4 keys
 *
 * Returns true if successful, false if lock was not held
 */
Datum
pg_advisory_unlock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockRelease(&tag, ShareLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_all() - release all advisory locks
 */
Datum
pg_advisory_unlock_all(PG_FUNCTION_ARGS)
{
	LockReleaseAll(USER_LOCKMETHOD, true);

	PG_RETURN_VOID();
}
