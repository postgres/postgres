/*-------------------------------------------------------------------------
 *
 * slotfuncs.c
 *	   Support functions for replication slots
 *
 * Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/slotfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog_internal.h"
#include "funcapi.h"
#include "miscadmin.h"
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
 * Helper function for creating a new physical replication slot with
 * given arguments. Note that this function doesn't release the created
 * slot.
 *
 * If restart_lsn is a valid value, we use it without WAL reservation
 * routine. So the caller must guarantee that WAL is available.
 */
static void
create_physical_replication_slot(char *name, bool immediately_reserve,
								 bool temporary, XLogRecPtr restart_lsn)
{
	Assert(!MyReplicationSlot);

	/* acquire replication slot, this will check for conflicting names */
	ReplicationSlotCreate(name, false,
						  temporary ? RS_TEMPORARY : RS_PERSISTENT);

	if (immediately_reserve)
	{
		/* Reserve WAL as the user asked for it */
		if (XLogRecPtrIsInvalid(restart_lsn))
			ReplicationSlotReserveWal();
		else
			MyReplicationSlot->data.restart_lsn = restart_lsn;

		/* Write this slot to disk */
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}
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

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	CheckSlotRequirements();

	create_physical_replication_slot(NameStr(*name),
									 immediately_reserve,
									 temporary,
									 InvalidXLogRecPtr);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	if (immediately_reserve)
	{
		values[1] = LSNGetDatum(MyReplicationSlot->data.restart_lsn);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}


/*
 * Helper function for creating a new logical replication slot with
 * given arguments. Note that this function doesn't release the created
 * slot.
 */
static void
create_logical_replication_slot(char *name, char *plugin,
								bool temporary, XLogRecPtr restart_lsn)
{
	LogicalDecodingContext *ctx = NULL;

	Assert(!MyReplicationSlot);

	/*
	 * Acquire a logical decoding slot, this will check for conflicting names.
	 * Initially create persistent slot as ephemeral - that allows us to
	 * nicely handle errors during initialization because it'll get dropped if
	 * this transaction fails. We'll make it persistent at the end. Temporary
	 * slots can be created as temporary from beginning as they get dropped on
	 * error as well.
	 */
	ReplicationSlotCreate(name, true,
						  temporary ? RS_TEMPORARY : RS_EPHEMERAL);

	/*
	 * Create logical decoding context, to build the initial snapshot.
	 */
	ctx = CreateInitDecodingContext(plugin, NIL,
									false,	/* do not build snapshot */
									restart_lsn,
									logical_read_local_xlog_page, NULL, NULL,
									NULL);

	/* build initial snapshot, might take a while */
	DecodingContextFindStartpoint(ctx);

	/* don't need the decoding context anymore */
	FreeDecodingContext(ctx);
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
	Datum		result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[2];
	bool		nulls[2];

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	CheckLogicalDecodingRequirements();

	create_logical_replication_slot(NameStr(*name),
									NameStr(*plugin),
									temporary,
									InvalidXLogRecPtr);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);

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
 * Helper function for advancing our physical replication slot forward.
 *
 * The LSN position to move to is compared simply to the slot's restart_lsn,
 * knowing that any position older than that would be removed by successive
 * checkpoints.
 */
static XLogRecPtr
pg_physical_replication_slot_advance(XLogRecPtr moveto)
{
	XLogRecPtr	startlsn = MyReplicationSlot->data.restart_lsn;
	XLogRecPtr	retlsn = startlsn;

	if (startlsn < moveto)
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->data.restart_lsn = moveto;
		SpinLockRelease(&MyReplicationSlot->mutex);
		retlsn = moveto;
	}

	return retlsn;
}

/*
 * Helper function for advancing our logical replication slot forward.
 *
 * The slot's restart_lsn is used as start point for reading records,
 * while confirmed_lsn is used as base point for the decoding context.
 *
 * We cannot just do LogicalConfirmReceivedLocation to update confirmed_flush,
 * because we need to digest WAL to advance restart_lsn allowing to recycle
 * WAL and removal of old catalog tuples.  As decoding is done in fast_forward
 * mode, no changes are generated anyway.
 */
static XLogRecPtr
pg_logical_replication_slot_advance(XLogRecPtr moveto)
{
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	XLogRecPtr	startlsn;
	XLogRecPtr	retlsn;

	PG_TRY();
	{
		/*
		 * Create our decoding context in fast_forward mode, passing start_lsn
		 * as InvalidXLogRecPtr, so that we start processing from my slot's
		 * confirmed_flush.
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,	/* fast_forward */
									logical_read_local_xlog_page,
									NULL, NULL, NULL);

		/*
		 * Start reading at the slot's restart_lsn, which we know to point to
		 * a valid record.
		 */
		startlsn = MyReplicationSlot->data.restart_lsn;

		/* Initialize our return value in case we don't do anything */
		retlsn = MyReplicationSlot->data.confirmed_flush;

		/* invalidate non-timetravel entries */
		InvalidateSystemCaches();

		/* Decode at least one record, until we run out of records */
		while ((!XLogRecPtrIsInvalid(startlsn) &&
				startlsn < moveto) ||
			   (!XLogRecPtrIsInvalid(ctx->reader->EndRecPtr) &&
				ctx->reader->EndRecPtr < moveto))
		{
			char	   *errm = NULL;
			XLogRecord *record;

			/*
			 * Read records.  No changes are generated in fast_forward mode,
			 * but snapbuilder/slot statuses are updated properly.
			 */
			record = XLogReadRecord(ctx->reader, startlsn, &errm);
			if (errm)
				elog(ERROR, "%s", errm);

			/* Read sequentially from now on */
			startlsn = InvalidXLogRecPtr;

			/*
			 * Process the record.  Storage-level changes are ignored in
			 * fast_forward mode, but other modules (such as snapbuilder)
			 * might still have critical updates to do.
			 */
			if (record)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/* Stop once the requested target has been reached */
			if (moveto <= ctx->reader->EndRecPtr)
				break;

			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * Logical decoding could have clobbered CurrentResourceOwner during
		 * transaction management, so restore the executor's value.  (This is
		 * a kluge, but it's not worth cleaning up right now.)
		 */
		CurrentResourceOwner = old_resowner;

		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr)
		{
			LogicalConfirmReceivedLocation(moveto);

			/*
			 * If only the confirmed_flush LSN has changed the slot won't get
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
	XLogRecPtr	minlsn;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2];
	HeapTuple	tuple;
	Datum		result;

	Assert(!MyReplicationSlot);

	check_permissions();

	if (XLogRecPtrIsInvalid(moveto))
		ereport(ERROR,
				(errmsg("invalid target WAL LSN")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We can't move slot past what's been flushed/replayed so clamp the
	 * target position accordingly.
	 */
	if (!RecoveryInProgress())
		moveto = Min(moveto, GetFlushRecPtr());
	else
		moveto = Min(moveto, GetXLogReplayRecPtr(&ThisTimeLineID));

	/* Acquire the slot so we "own" it */
	ReplicationSlotAcquire(NameStr(*slotname), true);

	/* A slot whose restart_lsn has never been reserved cannot be advanced */
	if (XLogRecPtrIsInvalid(MyReplicationSlot->data.restart_lsn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot advance replication slot that has not previously reserved WAL")));

	/*
	 * Check if the slot is not moving backwards.  Physical slots rely simply
	 * on restart_lsn as a minimum point, while logical slots have confirmed
	 * consumption up to confirmed_lsn, meaning that in both cases data older
	 * than that is not available anymore.
	 */
	if (OidIsValid(MyReplicationSlot->data.database))
		minlsn = MyReplicationSlot->data.confirmed_flush;
	else
		minlsn = MyReplicationSlot->data.restart_lsn;

	if (moveto < minlsn)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot advance replication slot to %X/%X, minimum is %X/%X",
						(uint32) (moveto >> 32), (uint32) moveto,
						(uint32) (minlsn >> 32), (uint32) minlsn)));

	/* Do the actual slot update, depending on the slot type */
	if (OidIsValid(MyReplicationSlot->data.database))
		endlsn = pg_logical_replication_slot_advance(moveto);
	else
		endlsn = pg_physical_replication_slot_advance(moveto);

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

/*
 * Helper function of copying a replication slot.
 */
static Datum
copy_replication_slot(FunctionCallInfo fcinfo, bool logical_slot)
{
	Name		src_name = PG_GETARG_NAME(0);
	Name		dst_name = PG_GETARG_NAME(1);
	ReplicationSlot *src = NULL;
	XLogRecPtr	src_restart_lsn;
	bool		src_islogical;
	bool		temporary;
	char	   *plugin;
	Datum		values[2];
	bool		nulls[2];
	Datum		result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	check_permissions();

	if (logical_slot)
		CheckLogicalDecodingRequirements();
	else
		CheckSlotRequirements();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/*
	 * We need to prevent the source slot's reserved WAL from being removed,
	 * but we don't want to lock that slot for very long, and it can advance
	 * in the meantime.  So obtain the source slot's data, and create a new
	 * slot using its restart_lsn.  Afterwards we lock the source slot again
	 * and verify that the data we copied (name, type) has not changed
	 * incompatibly.  No inconvenient WAL removal can occur once the new slot
	 * is created -- but since WAL removal could have occurred before we
	 * managed to create the new slot, we advance the new slot's restart_lsn
	 * to the source slot's updated restart_lsn the second time we lock it.
	 */
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(NameStr(s->data.name), NameStr(*src_name)) == 0)
		{
			SpinLockAcquire(&s->mutex);
			src_islogical = SlotIsLogical(s);
			src_restart_lsn = s->data.restart_lsn;
			temporary = s->data.persistency == RS_TEMPORARY;
			plugin = logical_slot ? pstrdup(NameStr(s->data.plugin)) : NULL;
			SpinLockRelease(&s->mutex);

			src = s;
			break;
		}
	}

	LWLockRelease(ReplicationSlotControlLock);

	if (src == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist", NameStr(*src_name))));

	/* Check type of replication slot */
	if (src_islogical != logical_slot)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 src_islogical ?
				 errmsg("cannot copy physical replication slot \"%s\" as a logical replication slot",
						NameStr(*src_name)) :
				 errmsg("cannot copy logical replication slot \"%s\" as a physical replication slot",
						NameStr(*src_name))));

	/* Copying non-reserved slot doesn't make sense */
	if (XLogRecPtrIsInvalid(src_restart_lsn))
	{
		Assert(!logical_slot);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 (errmsg("cannot copy a replication slot that doesn't reserve WAL"))));
	}

	/* Overwrite params from optional arguments */
	if (PG_NARGS() >= 3)
		temporary = PG_GETARG_BOOL(2);
	if (PG_NARGS() >= 4)
	{
		Assert(logical_slot);
		plugin = NameStr(*(PG_GETARG_NAME(3)));
	}

	/* Create new slot and acquire it */
	if (logical_slot)
		create_logical_replication_slot(NameStr(*dst_name),
										plugin,
										temporary,
										src_restart_lsn);
	else
		create_physical_replication_slot(NameStr(*dst_name),
										 true,
										 temporary,
										 src_restart_lsn);

	/*
	 * Update the destination slot to current values of the source slot;
	 * recheck that the source slot is still the one we saw previously.
	 */
	{
		TransactionId copy_effective_xmin;
		TransactionId copy_effective_catalog_xmin;
		TransactionId copy_xmin;
		TransactionId copy_catalog_xmin;
		XLogRecPtr	copy_restart_lsn;
		bool		copy_islogical;
		char	   *copy_name;

		/* Copy data of source slot again */
		SpinLockAcquire(&src->mutex);
		copy_effective_xmin = src->effective_xmin;
		copy_effective_catalog_xmin = src->effective_catalog_xmin;

		copy_xmin = src->data.xmin;
		copy_catalog_xmin = src->data.catalog_xmin;
		copy_restart_lsn = src->data.restart_lsn;

		/* for existence check */
		copy_name = pstrdup(NameStr(src->data.name));
		copy_islogical = SlotIsLogical(src);
		SpinLockRelease(&src->mutex);

		/*
		 * Check if the source slot still exists and is valid. We regard it as
		 * invalid if the type of replication slot or name has been changed,
		 * or the restart_lsn either is invalid or has gone backward. (The
		 * restart_lsn could go backwards if the source slot is dropped and
		 * copied from an older slot during installation.)
		 *
		 * Since erroring out will release and drop the destination slot we
		 * don't need to release it here.
		 */
		if (copy_restart_lsn < src_restart_lsn ||
			src_islogical != copy_islogical ||
			strcmp(copy_name, NameStr(*src_name)) != 0)
			ereport(ERROR,
					(errmsg("could not copy replication slot \"%s\"",
							NameStr(*src_name)),
					 errdetail("The source replication slot was modified incompatibly during the copy operation.")));

		/* Install copied values again */
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->effective_xmin = copy_effective_xmin;
		MyReplicationSlot->effective_catalog_xmin = copy_effective_catalog_xmin;

		MyReplicationSlot->data.xmin = copy_xmin;
		MyReplicationSlot->data.catalog_xmin = copy_catalog_xmin;
		MyReplicationSlot->data.restart_lsn = copy_restart_lsn;
		SpinLockRelease(&MyReplicationSlot->mutex);

		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
		ReplicationSlotSave();

#ifdef USE_ASSERT_CHECKING
		/* Check that the restart_lsn is available */
		{
			XLogSegNo	segno;

			XLByteToSeg(copy_restart_lsn, segno, wal_segment_size);
			Assert(XLogGetLastRemovedSegno() < segno);
		}
#endif
	}

	/* target slot fully created, mark as persistent if needed */
	if (logical_slot && !temporary)
		ReplicationSlotPersist();

	/* All done.  Set up the return values */
	values[0] = NameGetDatum(dst_name);
	nulls[0] = false;
	if (!XLogRecPtrIsInvalid(MyReplicationSlot->data.confirmed_flush))
	{
		values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}

/* The wrappers below are all to appease opr_sanity */
Datum
pg_copy_logical_replication_slot_a(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, true);
}

Datum
pg_copy_logical_replication_slot_b(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, true);
}

Datum
pg_copy_logical_replication_slot_c(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, true);
}

Datum
pg_copy_physical_replication_slot_a(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, false);
}

Datum
pg_copy_physical_replication_slot_b(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, false);
}
