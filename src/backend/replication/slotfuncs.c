/*-------------------------------------------------------------------------
 *
 * slotfuncs.c
 *	   Support functions for replication slots
 *
 * Copyright (c) 2012-2014, PostgreSQL Global Development Group
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
#include "replication/slot.h"
#include "replication/logical.h"
#include "replication/logicalfuncs.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

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
	ReplicationSlotCreate(NameStr(*name), false, RS_PERSISTENT);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);

	nulls[0] = false;
	nulls[1] = true;

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
	 * Acquire a logical decoding slot, this will check for conflicting
	 * names. Initially create it as ephemeral - that allows us to nicely
	 * handle errors during initialization because it'll get dropped if this
	 * transaction fails. We'll make it persistent at the end.
	 */
	ReplicationSlotCreate(NameStr(*name), true, RS_EPHEMERAL);

	/*
	 * Create logical decoding context, to build the initial snapshot.
	 */
	ctx = CreateInitDecodingContext(
									NameStr(*plugin), NIL,
									logical_read_local_xlog_page, NULL, NULL);

	/* build initial snapshot, might take a while */
	DecodingContextFindStartpoint(ctx);

	values[0] = CStringGetTextDatum(NameStr(MyReplicationSlot->data.name));
	values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);

	/* don't need the decoding context anymore */
	FreeDecodingContext(ctx);

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	/* ok, slot is now fully created, mark it as persistent */
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

	ReplicationSlotDrop(NameStr(*name));

	PG_RETURN_VOID();
}

/*
 * pg_get_replication_slots - SQL SRF showing active replication slots.
 */
Datum
pg_get_replication_slots(PG_FUNCTION_ARGS)
{
#define PG_GET_REPLICATION_SLOTS_COLS 8
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

	for (slotno = 0; slotno < max_replication_slots; slotno++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[slotno];
		Datum		values[PG_GET_REPLICATION_SLOTS_COLS];
		bool		nulls[PG_GET_REPLICATION_SLOTS_COLS];

		TransactionId xmin;
		TransactionId catalog_xmin;
		XLogRecPtr	restart_lsn;
		bool		active;
		Oid			database;
		NameData	slot_name;
		NameData	plugin;
		int			i;

		SpinLockAcquire(&slot->mutex);
		if (!slot->in_use)
		{
			SpinLockRelease(&slot->mutex);
			continue;
		}
		else
		{
			xmin = slot->data.xmin;
			catalog_xmin = slot->data.catalog_xmin;
			database = slot->data.database;
			restart_lsn = slot->data.restart_lsn;
			namecpy(&slot_name, &slot->data.name);
			namecpy(&plugin, &slot->data.plugin);

			active = slot->active;
		}
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

		values[i++] = BoolGetDatum(active);

		if (xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(xmin);
		else
			nulls[i++] = true;

		if (catalog_xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(catalog_xmin);
		else
			nulls[i++] = true;

		if (restart_lsn != InvalidTransactionId)
			values[i++] = LSNGetDatum(restart_lsn);
		else
			nulls[i++] = true;

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
