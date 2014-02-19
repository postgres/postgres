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
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "replication/slot.h"

Datum		pg_create_physical_replication_slot(PG_FUNCTION_ARGS);
Datum		pg_drop_replication_slot(PG_FUNCTION_ARGS);

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

	check_permissions();

	CheckSlotRequirements();

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* acquire replication slot, this will check for conflicting names*/
	ReplicationSlotCreate(NameStr(*name), false);

	values[0] = CStringGetTextDatum(NameStr(MyReplicationSlot->data.name));

	nulls[0] = false;
	nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

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
#define PG_STAT_GET_REPLICATION_SLOTS_COLS 6
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
		Datum		values[PG_STAT_GET_REPLICATION_SLOTS_COLS];
		bool		nulls[PG_STAT_GET_REPLICATION_SLOTS_COLS];

		TransactionId xmin;
		XLogRecPtr	restart_lsn;
		bool		active;
		Oid			database;
		const char *slot_name;
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
			database = slot->data.database;
			restart_lsn = slot->data.restart_lsn;
			slot_name = pstrdup(NameStr(slot->data.name));

			active = slot->active;
		}
		SpinLockRelease(&slot->mutex);

		memset(nulls, 0, sizeof(nulls));

		i = 0;
		values[i++] = CStringGetTextDatum(slot_name);
		if (database == InvalidOid)
			values[i++] = CStringGetTextDatum("physical");
		else
			values[i++] = CStringGetTextDatum("logical");
		values[i++] = database;
		values[i++] = BoolGetDatum(active);
		if (xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(xmin);
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
