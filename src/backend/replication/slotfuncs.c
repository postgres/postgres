/*-------------------------------------------------------------------------
 *
 * slotfuncs.c
 *	   Support functions for replication slots
 *
 * Copyright (c) 2012-2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/slotfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "access/htup_details.h"
#include "replication/decode.h"
#include "replication/slot.h"
#include "replication/logical.h"
#include "replication/logicalfuncs.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/pg_lsn.h"
#include "utils/resowner.h"

static void
check_permissions(void)
{
	if (!superuser() && !has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser or replication role to use replication slots"))));
}

/*
 * SQL function for creating a new physical (streaming replication)
 * replication slot.
 */
Datum
pg_create_physical_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		immediately_reserve = PG_GETARG_BOOL(1);
	bool		temporary = PG_GETARG_BOOL(2);
	Datum		values[2];
	bool		nulls[2];
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		result;

	Assert(!MyReplicationSlot);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	CheckSlotRequirements();

	/* acquire replication slot, this will check for conflicting names */
	ReplicationSlotCreate(NameStr(*name), false,
						  temporary ? RS_TEMPORARY : RS_PERSISTENT);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	if (immediately_reserve)
	{
		/* Reserve WAL as the user asked for it */
		ReplicationSlotReserveWal();

		/* Write this slot to disk */
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();

		values[1] = LSNGetDatum(MyReplicationSlot->data.restart_lsn);
		nulls[1] = false;
	}
	else
	{
		nulls[1] = true;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}


/*
 * SQL function for creating a new logical replication slot.
 */
Datum
pg_create_logical_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	Name		plugin = PG_GETARG_NAME(1);
	bool		temporary = PG_GETARG_BOOL(2);

	LogicalDecodingContext *ctx = NULL;

	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		result;
	Datum		values[2];
	bool		nulls[2];

	Assert(!MyReplicationSlot);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	CheckLogicalDecodingRequirements();

	/*
	 * Acquire a logical decoding slot, this will check for conflicting names.
	 * Initially create persistent slot as ephemeral - that allows us to
	 * nicely handle errors during initialization because it'll get dropped if
	 * this transaction fails. We'll make it persistent at the end. Temporary
	 * slots can be created as temporary from beginning as they get dropped on
	 * error as well.
	 */
	ReplicationSlotCreate(NameStr(*name), true,
						  temporary ? RS_TEMPORARY : RS_EPHEMERAL);

	/*
	 * Create logical decoding context, to build the initial snapshot.
	 */
	ctx = CreateInitDecodingContext(NameStr(*plugin), NIL,
									false,	/* do not build snapshot */
									logical_read_local_xlog_page, NULL, NULL,
									NULL);

	/* build initial snapshot, might take a while */
	DecodingContextFindStartpoint(ctx);

	values[0] = CStringGetTextDatum(NameStr(MyReplicationSlot->data.name));
	values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);

	/* don't need the decoding context anymore */
	FreeDecodingContext(ctx);

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	/* ok, slot is now fully created, mark it as persistent if needed */
	if (!temporary)
		ReplicationSlotPersist();
	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}


/*
 * SQL function for dropping a replication slot.
 */
Datum
pg_drop_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);

	check_permissions();

	CheckSlotRequirements();

	ReplicationSlotDrop(NameStr(*name), true);

	PG_RETURN_VOID();
}

/*
 * pg_get_replication_slots - SQL SRF showing active replication slots.
 */
Datum
pg_get_replication_slots(PG_FUNCTION_ARGS)
{
#define PG_GET_REPLICATION_SLOTS_COLS 11
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int			slotno;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We don't require any special permission to see this function's data
	 * because nothing should be sensitive. The most critical being the slot
	 * name, which shouldn't contain anything particularly sensitive.
	 */

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (slotno = 0; slotno < max_replication_slots; slotno++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[slotno];
		Datum		values[PG_GET_REPLICATION_SLOTS_COLS];
		bool		nulls[PG_GET_REPLICATION_SLOTS_COLS];

		ReplicationSlotPersistency persistency;
		TransactionId xmin;
		TransactionId catalog_xmin;
		XLogRecPtr	restart_lsn;
		XLogRecPtr	confirmed_flush_lsn;
		pid_t		active_pid;
		Oid			database;
		NameData	slot_name;
		NameData	plugin;
		int			i;

		if (!slot->in_use)
			continue;

		SpinLockAcquire(&slot->mutex);

		xmin = slot->data.xmin;
		catalog_xmin = slot->data.catalog_xmin;
		database = slot->data.database;
		restart_lsn = slot->data.restart_lsn;
		confirmed_flush_lsn = slot->data.confirmed_flush;
		namecpy(&slot_name, &slot->data.name);
		namecpy(&plugin, &slot->data.plugin);
		active_pid = slot->active_pid;
		persistency = slot->data.persistency;

		SpinLockRelease(&slot->mutex);

		memset(nulls, 0, sizeof(nulls));

		i = 0;
		values[i++] = NameGetDatum(&slot_name);

		if (database == InvalidOid)
			nulls[i++] = true;
		else
			values[i++] = NameGetDatum(&plugin);

		if (database == InvalidOid)
			values[i++] = CStringGetTextDatum("physical");
		else
			values[i++] = CStringGetTextDatum("logical");

		if (database == InvalidOid)
			nulls[i++] = true;
		else
			values[i++] = database;

		values[i++] = BoolGetDatum(persistency == RS_TEMPORARY);
		values[i++] = BoolGetDatum(active_pid != 0);

		if (active_pid != 0)
			values[i++] = Int32GetDatum(active_pid);
		else
			nulls[i++] = true;

		if (xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(xmin);
		else
			nulls[i++] = true;

		if (catalog_xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(catalog_xmin);
		else
			nulls[i++] = true;

		if (restart_lsn != InvalidXLogRecPtr)
			values[i++] = LSNGetDatum(restart_lsn);
		else
			nulls[i++] = true;

		if (confirmed_flush_lsn != InvalidXLogRecPtr)
			values[i++] = LSNGetDatum(confirmed_flush_lsn);
		else
			nulls[i++] = true;

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	LWLockRelease(ReplicationSlotControlLock);

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Helper function for advancing physical replication slot forward.
 */
static XLogRecPtr
pg_physical_replication_slot_advance(XLogRecPtr startlsn, XLogRecPtr moveto)
{
	XLogRecPtr	retlsn = InvalidXLogRecPtr;

	SpinLockAcquire(&MyReplicationSlot->mutex);
	if (MyReplicationSlot->data.restart_lsn < moveto)
	{
		MyReplicationSlot->data.restart_lsn = moveto;
		retlsn = moveto;
	}
	SpinLockRelease(&MyReplicationSlot->mutex);

	return retlsn;
}

/*
 * Helper function for advancing logical replication slot forward.
 */
static XLogRecPtr
pg_logical_replication_slot_advance(XLogRecPtr startlsn, XLogRecPtr moveto)
{
	LogicalDecodingContext *ctx;
	ResourceOwner	old_resowner = CurrentResourceOwner;
	XLogRecPtr		retlsn = InvalidXLogRecPtr;

	PG_TRY();
	{
		/* restart at slot's confirmed_flush */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,
									logical_read_local_xlog_page,
									NULL, NULL, NULL);

		CurrentResourceOwner = ResourceOwnerCreate(CurrentResourceOwner,
												   "logical decoding");

		/* invalidate non-timetravel entries */
		InvalidateSystemCaches();

		/* Decode until we run out of records */
		while ((startlsn != InvalidXLogRecPtr && startlsn < moveto) ||
			   (ctx->reader->EndRecPtr != InvalidXLogRecPtr && ctx->reader->EndRecPtr < moveto))
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, startlsn, &errm);
			if (errm)
				elog(ERROR, "%s", errm);

			/*
			 * Now that we've set up the xlog reader state, subsequent calls
			 * pass InvalidXLogRecPtr to say "continue from last record"
			 */
			startlsn = InvalidXLogRecPtr;

			/*
			 * The {begin_txn,change,commit_txn}_wrapper callbacks above will
			 * store the description into our tuplestore.
			 */
			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/* check limits */
			if (moveto <= ctx->reader->EndRecPtr)
				break;

			CHECK_FOR_INTERRUPTS();
		}

		CurrentResourceOwner = old_resowner;

		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr)
		{
			LogicalConfirmReceivedLocation(moveto);

			/*
			 * If only the confirmed_flush_lsn has changed the slot won't get
			 * marked as dirty by the above. Callers on the walsender
			 * interface are expected to keep track of their own progress and
			 * don't need it written out. But SQL-interface users cannot
			 * specify their own start positions and it's harder for them to
			 * keep track of their progress, so we should make more of an
			 * effort to save it for them.
			 *
			 * Dirty the slot so it's written out at the next checkpoint.
			 * We'll still lose its position on crash, as documented, but it's
			 * better than always losing the position even on clean restart.
			 */
			ReplicationSlotMarkDirty();
		}

		retlsn = MyReplicationSlot->data.confirmed_flush;

		/* free context, call shutdown callback */
		FreeDecodingContext(ctx);

		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* clear all timetravel entries */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return retlsn;
}

/*
 * SQL function for moving the position in a replication slot.
 */
Datum
pg_replication_slot_advance(PG_FUNCTION_ARGS)
{
	Name		slotname = PG_GETARG_NAME(0);
	XLogRecPtr	moveto = PG_GETARG_LSN(1);
	XLogRecPtr	endlsn;
	XLogRecPtr	startlsn;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2];
	HeapTuple	tuple;
	Datum		result;

	Assert(!MyReplicationSlot);

	check_permissions();

	if (XLogRecPtrIsInvalid(moveto))
		ereport(ERROR,
				(errmsg("invalid target wal lsn")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We can't move slot past what's been flushed/replayed so clamp the
	 * target possition accordingly.
	 */
	if (!RecoveryInProgress())
		moveto = Min(moveto, GetFlushRecPtr());
	else
		moveto = Min(moveto, GetXLogReplayRecPtr(&ThisTimeLineID));

	/* Acquire the slot so we "own" it */
	ReplicationSlotAcquire(NameStr(*slotname), true);

	startlsn = MyReplicationSlot->data.confirmed_flush;
	if (moveto < startlsn)
	{
		ReplicationSlotRelease();
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move slot to %X/%X, minimum is %X/%X",
						(uint32) (moveto >> 32), (uint32) moveto,
						(uint32) (MyReplicationSlot->data.confirmed_flush >> 32),
						(uint32) (MyReplicationSlot->data.confirmed_flush))));
	}

	if (OidIsValid(MyReplicationSlot->data.database))
		endlsn = pg_logical_replication_slot_advance(startlsn, moveto);
	else
		endlsn = pg_physical_replication_slot_advance(startlsn, moveto);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	/* Update the on disk state when lsn was updated. */
	if (XLogRecPtrIsInvalid(endlsn))
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
		ReplicationSlotSave();
	}

	ReplicationSlotRelease();

	/* Return the reached position. */
	values[1] = LSNGetDatum(endlsn);
	nulls[1] = false;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}
