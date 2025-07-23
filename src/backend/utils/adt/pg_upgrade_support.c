/*
 *	pg_upgrade_support.c
 *
 *	server-side functions to set backend global variables
 *	to control oid and relfilenumber assignment, and do other special
 *	hacks needed for pg_upgrade.
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/backend/utils/adt/pg_upgrade_support.c
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/binary_upgrade.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "replication/logical.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"


#define CHECK_IS_BINARY_UPGRADE									\
do {															\
	if (!IsBinaryUpgrade)										\
		ereport(ERROR,											\
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),	\
				 errmsg("function can only be called when server is in binary upgrade mode"))); \
} while (0)

Datum
binary_upgrade_set_next_pg_tablespace_oid(PG_FUNCTION_ARGS)
{
	Oid			tbspoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_pg_tablespace_oid = tbspoid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_pg_type_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_pg_type_oid = typoid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_array_pg_type_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_array_pg_type_oid = typoid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_multirange_pg_type_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_mrng_pg_type_oid = typoid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_multirange_array_pg_type_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_mrng_array_pg_type_oid = typoid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_heap_pg_class_oid(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_heap_pg_class_oid = reloid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_heap_relfilenode(PG_FUNCTION_ARGS)
{
	RelFileNumber relfilenumber = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_heap_pg_class_relfilenumber = relfilenumber;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_index_pg_class_oid(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_index_pg_class_oid = reloid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_index_relfilenode(PG_FUNCTION_ARGS)
{
	RelFileNumber relfilenumber = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_index_pg_class_relfilenumber = relfilenumber;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_toast_pg_class_oid(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_toast_pg_class_oid = reloid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_toast_relfilenode(PG_FUNCTION_ARGS)
{
	RelFileNumber relfilenumber = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_toast_pg_class_relfilenumber = relfilenumber;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_pg_enum_oid(PG_FUNCTION_ARGS)
{
	Oid			enumoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_pg_enum_oid = enumoid;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_next_pg_authid_oid(PG_FUNCTION_ARGS)
{
	Oid			authoid = PG_GETARG_OID(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_next_pg_authid_oid = authoid;
	PG_RETURN_VOID();
}

Datum
binary_upgrade_create_empty_extension(PG_FUNCTION_ARGS)
{
	text	   *extName;
	text	   *schemaName;
	bool		relocatable;
	text	   *extVersion;
	Datum		extConfig;
	Datum		extCondition;
	List	   *requiredExtensions;

	CHECK_IS_BINARY_UPGRADE;

	/* We must check these things before dereferencing the arguments */
	if (PG_ARGISNULL(0) ||
		PG_ARGISNULL(1) ||
		PG_ARGISNULL(2) ||
		PG_ARGISNULL(3))
		elog(ERROR, "null argument to binary_upgrade_create_empty_extension is not allowed");

	extName = PG_GETARG_TEXT_PP(0);
	schemaName = PG_GETARG_TEXT_PP(1);
	relocatable = PG_GETARG_BOOL(2);
	extVersion = PG_GETARG_TEXT_PP(3);

	if (PG_ARGISNULL(4))
		extConfig = PointerGetDatum(NULL);
	else
		extConfig = PG_GETARG_DATUM(4);

	if (PG_ARGISNULL(5))
		extCondition = PointerGetDatum(NULL);
	else
		extCondition = PG_GETARG_DATUM(5);

	requiredExtensions = NIL;
	if (!PG_ARGISNULL(6))
	{
		ArrayType  *textArray = PG_GETARG_ARRAYTYPE_P(6);
		Datum	   *textDatums;
		int			ndatums;
		int			i;

		deconstruct_array_builtin(textArray, TEXTOID, &textDatums, NULL, &ndatums);
		for (i = 0; i < ndatums; i++)
		{
			char	   *extName = TextDatumGetCString(textDatums[i]);
			Oid			extOid = get_extension_oid(extName, false);

			requiredExtensions = lappend_oid(requiredExtensions, extOid);
		}
	}

	InsertExtensionTuple(text_to_cstring(extName),
						 GetUserId(),
						 get_namespace_oid(text_to_cstring(schemaName), false),
						 relocatable,
						 text_to_cstring(extVersion),
						 extConfig,
						 extCondition,
						 requiredExtensions);

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_record_init_privs(PG_FUNCTION_ARGS)
{
	bool		record_init_privs = PG_GETARG_BOOL(0);

	CHECK_IS_BINARY_UPGRADE;
	binary_upgrade_record_init_privs = record_init_privs;

	PG_RETURN_VOID();
}

Datum
binary_upgrade_set_missing_value(PG_FUNCTION_ARGS)
{
	Oid			table_id = PG_GETARG_OID(0);
	text	   *attname = PG_GETARG_TEXT_P(1);
	text	   *value = PG_GETARG_TEXT_P(2);
	char	   *cattname = text_to_cstring(attname);
	char	   *cvalue = text_to_cstring(value);

	CHECK_IS_BINARY_UPGRADE;
	SetAttrMissing(table_id, cattname, cvalue);

	PG_RETURN_VOID();
}

/*
 * Verify the given slot has already consumed all the WAL changes.
 *
 * Returns true if there are no decodable WAL records after the
 * confirmed_flush_lsn. Otherwise false.
 *
 * This is a special purpose function to ensure that the given slot can be
 * upgraded without data loss.
 */
Datum
binary_upgrade_logical_slot_has_caught_up(PG_FUNCTION_ARGS)
{
	Name		slot_name;
	XLogRecPtr	end_of_wal;
	bool		found_pending_wal;

	CHECK_IS_BINARY_UPGRADE;

	/*
	 * Binary upgrades only allowed super-user connections so we must have
	 * permission to use replication slots.
	 */
	Assert(has_rolreplication(GetUserId()));

	slot_name = PG_GETARG_NAME(0);

	/* Acquire the given slot */
	ReplicationSlotAcquire(NameStr(*slot_name), true, true);

	Assert(SlotIsLogical(MyReplicationSlot));

	/* Slots must be valid as otherwise we won't be able to scan the WAL */
	Assert(MyReplicationSlot->data.invalidated == RS_INVAL_NONE);

	end_of_wal = GetFlushRecPtr(NULL);
	found_pending_wal = LogicalReplicationSlotHasPendingWal(end_of_wal);

	/* Clean up */
	ReplicationSlotRelease();

	PG_RETURN_BOOL(!found_pending_wal);
}

/*
 * binary_upgrade_add_sub_rel_state
 *
 * Add the relation with the specified relation state to pg_subscription_rel
 * catalog.
 */
Datum
binary_upgrade_add_sub_rel_state(PG_FUNCTION_ARGS)
{
	Relation	subrel;
	Relation	rel;
	Oid			subid;
	char	   *subname;
	Oid			relid;
	char		relstate;
	XLogRecPtr	sublsn;

	CHECK_IS_BINARY_UPGRADE;

	/* We must check these things before dereferencing the arguments */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		elog(ERROR, "null argument to binary_upgrade_add_sub_rel_state is not allowed");

	subname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	relid = PG_GETARG_OID(1);
	relstate = PG_GETARG_CHAR(2);
	sublsn = PG_ARGISNULL(3) ? InvalidXLogRecPtr : PG_GETARG_LSN(3);

	subrel = table_open(SubscriptionRelationId, RowExclusiveLock);
	subid = get_subscription_oid(subname, false);
	rel = relation_open(relid, AccessShareLock);

	/*
	 * Since there are no concurrent ALTER/DROP SUBSCRIPTION commands during
	 * the upgrade process, and the apply worker (which builds cache based on
	 * the subscription catalog) is not running, the locks can be released
	 * immediately.
	 */
	AddSubscriptionRelState(subid, relid, relstate, sublsn, false);
	relation_close(rel, AccessShareLock);
	table_close(subrel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * binary_upgrade_replorigin_advance
 *
 * Update the remote_lsn for the subscriber's replication origin.
 */
Datum
binary_upgrade_replorigin_advance(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Oid			subid;
	char	   *subname;
	char		originname[NAMEDATALEN];
	RepOriginId node;
	XLogRecPtr	remote_commit;

	CHECK_IS_BINARY_UPGRADE;

	/*
	 * We must ensure a non-NULL subscription name before dereferencing the
	 * arguments.
	 */
	if (PG_ARGISNULL(0))
		elog(ERROR, "null argument to binary_upgrade_replorigin_advance is not allowed");

	subname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	remote_commit = PG_ARGISNULL(1) ? InvalidXLogRecPtr : PG_GETARG_LSN(1);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);
	subid = get_subscription_oid(subname, false);

	ReplicationOriginNameForLogicalRep(subid, InvalidOid, originname, sizeof(originname));

	/* Lock to prevent the replication origin from vanishing */
	LockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);
	node = replorigin_by_name(originname, false);

	/*
	 * The server will be stopped after setting up the objects in the new
	 * cluster and the origins will be flushed during the shutdown checkpoint.
	 * This will ensure that the latest LSN values for origin will be
	 * available after the upgrade.
	 */
	replorigin_advance(node, remote_commit, InvalidXLogRecPtr,
					   false /* backward */ ,
					   false /* WAL log */ );

	UnlockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * binary_upgrade_create_conflict_detection_slot
 *
 * Create a replication slot to retain information necessary for conflict
 * detection such as dead tuples, commit timestamps, and origins.
 */
Datum
binary_upgrade_create_conflict_detection_slot(PG_FUNCTION_ARGS)
{
	CHECK_IS_BINARY_UPGRADE;

	CreateConflictDetectionSlot();

	ReplicationSlotRelease();

	PG_RETURN_VOID();
}
