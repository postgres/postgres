/*
 * lockfuncs.c
 *		Set-returning functions to view the state of locks within the DB.
 * 
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		$Header: /cvsroot/pgsql/src/backend/utils/adt/lockfuncs.c,v 1.2 2002/08/27 04:00:28 momjian Exp $
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"

Datum pg_lock_status(PG_FUNCTION_ARGS);

static int next_lock(int locks[]);

Datum
pg_lock_status(PG_FUNCTION_ARGS)
{
	FuncCallContext		*funccxt;
	LockData			*lockData;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcxt;
		TupleDesc		tupdesc;

		funccxt = SRF_FIRSTCALL_INIT();
		tupdesc = CreateTemplateTupleDesc(5, WITHOUTOID);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relation",
						   OIDOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "database",
						   OIDOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "backendpid",
						   INT4OID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "mode",
						   TEXTOID, -1, 0, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "isgranted",
						   BOOLOID, -1, 0, false);

		funccxt->slot = TupleDescGetSlot(tupdesc);
		funccxt->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		oldcxt = MemoryContextSwitchTo(funccxt->fmctx);

		/*
		 * Preload all the locking information that we will eventually format
		 * and send out as a result set. This is palloc'ed, but since the
		 * MemoryContext is reset when the SRF finishes, we don't need to
		 * free it ourselves.
		 */
		funccxt->user_fctx = (LockData *) palloc(sizeof(LockData));

		GetLockStatusData(funccxt->user_fctx);

		MemoryContextSwitchTo(oldcxt);
	}

	funccxt	= SRF_PERCALL_SETUP();
	lockData = (LockData *) funccxt->user_fctx;

	while (lockData->currIdx < lockData->nelements)
	{
		PROCLOCK		 *holder;
		LOCK			 *lock;
		PGPROC			 *proc;
		HeapTuple		  tuple;
		Datum			  result;
		char			**values;
		LOCKMODE		  mode;
		int				  num_attrs;
		int				  i;
		int				  currIdx = lockData->currIdx;

		holder		= &(lockData->holders[currIdx]);
		lock		= &(lockData->locks[currIdx]);
		proc		= &(lockData->procs[currIdx]);
		num_attrs	= funccxt->attinmeta->tupdesc->natts;

		values = (char **) palloc(sizeof(*values) * num_attrs);

		for (i = 0; i < num_attrs; i++)
			values[i] = (char *) palloc(32);

		/* The OID of the locked relation */
		snprintf(values[0], 32, "%u", lock->tag.relId);
		/* The database the relation is in */
		snprintf(values[1], 32, "%u", lock->tag.dbId);
		/* The PID of the backend holding or waiting for the lock */
		snprintf(values[2], 32, "%d", proc->pid);

		/*
		 * We need to report both the locks held (i.e. successfully acquired)
		 * by this holder, as well as the locks upon which it is still
		 * waiting, if any. Since a single PROCLOCK struct may contain
		 * multiple locks, we may need to loop several times before we
		 * advance the array index and continue on.
		 */
		if (holder->nHolding > 0)
		{
			/* Already held locks */
			mode = next_lock(holder->holding);
			holder->holding[mode]--;
			holder->nHolding--;

			strcpy(values[4], "t");
		}
		else if (proc->waitLock != NULL)
		{
			/* Lock that is still being waited on */
			mode = proc->waitLockMode;
			proc->waitLock = NULL;
			proc->waitLockMode = NoLock;

			strcpy(values[4], "f");
		}
		else
		{
			/*
			 * Okay, we've displayed all the lock's belonging to this PROCLOCK,
			 * procede to the next one.
			 */
			lockData->currIdx++;
			continue;
		}

		strncpy(values[3], GetLockmodeName(mode), 32);

		tuple = BuildTupleFromCStrings(funccxt->attinmeta, values);
		result = TupleGetDatum(funccxt->slot, tuple);
		SRF_RETURN_NEXT(funccxt, result);
	}

	SRF_RETURN_DONE(funccxt);
}

static LOCKMODE
next_lock(int locks[])
{
	LOCKMODE i;

	for (i = 0; i < MAX_LOCKMODES; i++)
	{
		if (locks[i] != 0)
			return i;
	}

	/* No locks found: this should not occur */
	Assert(false);
	return -1;
}
