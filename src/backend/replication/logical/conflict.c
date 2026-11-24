/*-------------------------------------------------------------------------
 * conflict.c
 *	   Support routines for logging conflicts.
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/conflict.c
 *
 * This file contains the code for logging conflicts on the subscriber during
 * logical replication.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/commit_ts.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_namespace.h"
#include "catalog/toasting.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "pgstat.h"
#include "replication/conflict.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"
#include "utils/xid8.h"

/*
 * String representations for the supported conflict logging destinations.
 */
const char *const ConflictLogDestNames[] = {
	[CONFLICT_LOG_DEST_LOG] = "log",
	[CONFLICT_LOG_DEST_TABLE] = "table",
	[CONFLICT_LOG_DEST_ALL] = "all"
};

StaticAssertDecl(lengthof(ConflictLogDestNames) == CONFLICT_LOG_DEST_ALL + 1,
				 "ConflictLogDestNames length mismatch");


/* Structure to hold metadata for one column of the conflict log table */
typedef struct ConflictLogColumnDef
{
	const char *attname;		/* Column name */
	Oid			atttypid;		/* Data type OID */
} ConflictLogColumnDef;

/*
 * Schema definition for conflict log tables.
 *
 * Defines the fixed schema of the per-subscription conflict log table created
 * in the pg_conflict namespace. Each entry specifies the column name and its
 * type OID; the table is created in this column order by
 * create_conflict_log_table().
 *
 * The replica_identity and local_conflicts columns are typed json rather than
 * jsonb on purpose: they hold an exact audit snapshot of the replica identity
 * key values and local conflict metadata, and json preserves the verbatim
 * representation whereas jsonb would normalize it. Indexing them (jsonb's main
 * advantage) wouldn't help anyway, as the conflict log is looked up by its
 * scalar columns (relid, conflict_type, commit timestamp) while these json
 * columns are per-conflict payload to inspect, not search keys.
 *
 * 'local_conflicts' is typed as an array of JSON objects (json[]), not a
 * single json object, so that a future conflict type needing to record
 * multiple local rows for one remote operation doesn't require a
 * backward-incompatible schema change.
 */
static const ConflictLogColumnDef ConflictLogSchema[] = {
	{.attname = "relid", .atttypid = OIDOID},
	{.attname = "schemaname", .atttypid = TEXTOID},
	{.attname = "relname", .atttypid = TEXTOID},
	{.attname = "conflict_type", .atttypid = TEXTOID},
	{.attname = "remote_xid", .atttypid = XIDOID},
	{.attname = "remote_commit_lsn", .atttypid = LSNOID},
	{.attname = "remote_commit_ts", .atttypid = TIMESTAMPTZOID},
	{.attname = "apply_origin", .atttypid = TEXTOID},
	{.attname = "replica_identity_full", .atttypid = BOOLOID},
	{.attname = "replica_identity", .atttypid = JSONOID},
	{.attname = "local_conflicts", .atttypid = JSONARRAYOID},
	{.attname = "has_omitted_values", .atttypid = BOOLOID}
};

#define NUM_CONFLICT_ATTRS ((AttrNumber) lengthof(ConflictLogSchema))

/*
 * Maximum size in bytes of a replica identity key value recorded in the
 * conflict log table. A larger value is replaced by a marker holding its
 * length. See build_index_key_json().
 *
 * The cap applies to the length of the type's text output before JSON escaping.
 * Since escape_json() expands each byte at most sixfold (to \uXXXX), and an
 * index has at most INDEX_MAX_KEYS (32) key columns, the worst-case JSON output
 * is 64kB * 6 * 32 = 12MB, comfortably inside the 1GB limit on a json value.
 * Ordinary keys are orders of magnitude below the cap, so in practice nothing
 * is ever omitted.
 */
#define CONFLICT_MAX_VALUE_SIZE (64 * 1024)

/*
 * Schema for the elements within the 'local_conflicts' JSON array.
 */
static const ConflictLogColumnDef LocalConflictSchema[] =
{
	{.attname = "xid", .atttypid = XID8OID},
	{.attname = "commit_ts", .atttypid = TIMESTAMPTZOID},
	{.attname = "origin", .atttypid = TEXTOID}
};

#define NUM_LOCAL_CONFLICT_ATTRS lengthof(LocalConflictSchema)

static const char *const ConflictTypeNames[] = {
	[CT_INSERT_EXISTS] = "insert_exists",
	[CT_UPDATE_ORIGIN_DIFFERS] = "update_origin_differs",
	[CT_UPDATE_EXISTS] = "update_exists",
	[CT_UPDATE_MISSING] = "update_missing",
	[CT_DELETE_ORIGIN_DIFFERS] = "delete_origin_differs",
	[CT_UPDATE_DELETED] = "update_deleted",
	[CT_DELETE_MISSING] = "delete_missing",
	[CT_MULTIPLE_UNIQUE_CONFLICTS] = "multiple_unique_conflicts"
};

static Relation get_conflictlog_dest_and_table(int elevel,
											   ConflictLogDest *log_dest);
static int	errcode_apply_conflict(ConflictType type);
static void errdetail_apply_conflict(EState *estate,
									 ResultRelInfo *relinfo,
									 ConflictType type,
									 TupleTableSlot *searchslot,
									 TupleTableSlot *localslot,
									 TupleTableSlot *remoteslot,
									 Oid indexoid, TransactionId localxmin,
									 ReplOriginId localorigin,
									 TimestampTz localts, StringInfo err_msg);
static void get_tuple_desc(EState *estate, ResultRelInfo *relinfo,
						   ConflictType type, char **key_desc,
						   TupleTableSlot *localslot, char **local_desc,
						   TupleTableSlot *remoteslot, char **remote_desc,
						   TupleTableSlot *searchslot, char **search_desc,
						   Oid indexoid);
static void build_index_datums_from_slot(EState *estate, Relation localrel,
										 TupleTableSlot *slot,
										 Relation indexDesc, Datum *values,
										 bool *isnull);
static char *build_index_value_desc(EState *estate, Relation localrel,
									TupleTableSlot *slot, Oid indexoid);
static Datum build_index_key_json(EState *estate,
								  Relation localrel,
								  Oid replica_index,
								  TupleTableSlot *slot,
								  bool *omitted);
static TupleDesc build_local_conflicts_tupledesc(void);
static Datum build_local_conflicts_json_array(List *conflicttuples);
static void insert_conflict_log_tuple(EState *estate, Relation rel,
									  Relation conflictlogrel,
									  ConflictType conflict_type,
									  Oid searchindexoid,
									  TupleTableSlot *searchslot,
									  List *conflicttuples);

/*
 * Builds the TupleDesc for the conflict log table.
 */
static TupleDesc
create_conflict_log_table_tupdesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(NUM_CONFLICT_ATTRS);

	for (int i = 0; i < NUM_CONFLICT_ATTRS; i++)
		TupleDescInitEntry(tupdesc, i + 1,
						   ConflictLogSchema[i].attname,
						   ConflictLogSchema[i].atttypid,
						   -1, 0);

	TupleDescFinalize(tupdesc);

	return tupdesc;
}

/*
 * Create a structured conflict log table for a subscription.
 *
 * The table is created within the system-managed 'pg_conflict' namespace to
 * prevent users from manually dropping or altering it.  This also prevents
 * accidental name collisions with user-created tables with the same name.
 *
 * The table name is generated automatically using the subscription's OID
 * (e.g., "pg_conflict_log_<subid>") to ensure uniqueness within the
 * cluster and to avoid collisions during subscription renames.
 */
Oid
create_conflict_log_table(Oid subid, char *subname, Oid subowner)
{
	TupleDesc	tupdesc;
	Oid			relid;
	char		relname[NAMEDATALEN];

	snprintf(relname, NAMEDATALEN, "pg_conflict_log_%u", subid);

	/* Build the tuple descriptor for the new table. */
	tupdesc = create_conflict_log_table_tupdesc();

	/* Create conflict log table. */
	relid = heap_create_with_catalog(relname,
									 PG_CONFLICT_NAMESPACE,
									 0, /* tablespace */
									 InvalidOid,	/* relid */
									 InvalidOid,	/* reltypeid */
									 InvalidOid,	/* reloftypeid */
									 subowner,
									 HEAP_TABLE_AM_OID,
									 tupdesc,
									 NIL,
									 RELKIND_RELATION,
									 RELPERSISTENCE_PERMANENT,
									 false, /* shared_relation */
									 false, /* mapped_relation */
									 ONCOMMIT_NOOP,
									 (Datum) 0, /* reloptions */
									 false, /* use_user_acl */
									 false, /* allow_system_table_mods */
									 true,	/* is_internal */
									 InvalidOid,	/* relrewrite */
									 NULL); /* typaddress */
	Assert(OidIsValid(relid));

	/* Release tuple descriptor memory. */
	FreeTupleDesc(tupdesc);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Create a TOAST table for the conflict log to support out-of-line
	 * storage of large json data.
	 */
	NewRelationCreateToastTable(relid, (Datum) 0);

	ereport(NOTICE,
			(errmsg("created conflict log table \"%s\" for subscription \"%s\"",
					get_qualified_objname(PG_CONFLICT_NAMESPACE, relname),
					subname)));

	return relid;
}

/*
 * Convert the string representation of a conflict logging destination to its
 * corresponding enum value.
 */
ConflictLogDest
GetConflictLogDest(const char *dest)
{
	/* NULL defaults to LOG. */
	if (dest == NULL || pg_strcasecmp(dest, "log") == 0)
		return CONFLICT_LOG_DEST_LOG;

	if (pg_strcasecmp(dest, "table") == 0)
		return CONFLICT_LOG_DEST_TABLE;

	if (pg_strcasecmp(dest, "all") == 0)
		return CONFLICT_LOG_DEST_ALL;

	/* Unrecognized string. */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized conflict_log_destination value: \"%s\"", dest),
			 errhint("Valid values are \"log\", \"table\", and \"all\".")));
}

/*
 * Get the xmin and commit timestamp data (origin and timestamp) associated
 * with the provided local row.
 *
 * Return true if the commit timestamp data was found, false otherwise.
 */
bool
GetTupleTransactionInfo(TupleTableSlot *localslot, TransactionId *xmin,
						ReplOriginId *localorigin, TimestampTz *localts)
{
	Datum		xminDatum;
	bool		isnull;

	xminDatum = slot_getsysattr(localslot, MinTransactionIdAttributeNumber,
								&isnull);
	*xmin = DatumGetTransactionId(xminDatum);
	Assert(!isnull);

	/*
	 * The commit timestamp data is not available if track_commit_timestamp is
	 * disabled.
	 */
	if (!track_commit_timestamp)
	{
		*localorigin = InvalidReplOriginId;
		*localts = 0;
		return false;
	}

	return TransactionIdGetCommitTsData(*xmin, localts, localorigin);
}

/*
 * This function is used to report a conflict while applying replication
 * changes.
 *
 * 'relmapentry' is the relation map entry that was used to search the local
 * row, or NULL for ERROR-level insert/update conflicts. If the search used
 * the replica identity or primary key index, the caller must have locked
 * it, which the search itself does, see FindReplTupleInLocalRel().
 *
 * 'searchslot' should contain the tuple used to search the local row to be
 * updated or deleted.
 *
 * 'remoteslot' should contain the remote new tuple, if any.
 *
 * conflicttuples is a list of local rows that caused the conflict and the
 * conflict related information. See ConflictTupleInfo.
 *
 * The caller must ensure that all the indexes passed in ConflictTupleInfo are
 * locked so that we can fetch and display the conflicting key values.
 */
void
ReportApplyConflict(EState *estate, ResultRelInfo *relinfo, int elevel,
					ConflictType type, LogicalRepRelMapEntry *relmapentry,
					TupleTableSlot *searchslot,
					TupleTableSlot *remoteslot, List *conflicttuples)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	ConflictLogDest dest;
	Oid			searchindexoid = InvalidOid;
	Relation	conflictlogrel;
	StringInfoData err_detail;

	pgstat_report_subscription_conflict(MySubscription->oid, type);

	/*
	 * Use the replica identity or primary key index that the search actually
	 * used, which the caller has locked, rather than looking it up again.
	 * Concurrent DDL could have changed which index that is since the search,
	 * see comments in FindReplTupleInLocalRel().
	 */
	if (relmapentry && relmapentry->idxisreplident)
		searchindexoid = relmapentry->localindexoid;

	conflictlogrel = get_conflictlog_dest_and_table(elevel, &dest);

	initStringInfo(&err_detail);

	/*
	 * Log the full details to the server log, unless the conflict goes only
	 * to the conflict log table, in which case just point to the table.
	 */
	if (CONFLICTS_LOGGED_TO_LOG(dest))
	{
		/* Form errdetail message by combining conflicting tuples information. */
		if (conflicttuples != NIL)
		{
			foreach_ptr(ConflictTupleInfo, conflicttuple, conflicttuples)
				errdetail_apply_conflict(estate, relinfo, type, searchslot,
										 conflicttuple->slot, remoteslot,
										 conflicttuple->indexoid,
										 XidFromFullTransactionId(conflicttuple->xmin),
										 conflicttuple->origin,
										 conflicttuple->ts,
										 &err_detail);
		}
		else
		{
			errdetail_apply_conflict(estate, relinfo, type, searchslot,
									 NULL, remoteslot,
									 InvalidOid,
									 InvalidTransactionId,
									 InvalidReplOriginId,
									 0,
									 &err_detail);
		}
	}
	else
	{
		Assert(conflictlogrel);

		appendStringInfo(&err_detail,
						 _("Conflict details are logged to the conflict log table \"%s.%s\"."),
						 get_namespace_name(RelationGetNamespace(conflictlogrel)),
						 RelationGetRelationName(conflictlogrel));
	}

	ereport(elevel,
			errcode_apply_conflict(type),
			errmsg("conflict detected on relation \"%s.%s\": conflict=%s",
				   get_namespace_name(RelationGetNamespace(localrel)),
				   RelationGetRelationName(localrel),
				   ConflictTypeNames[type]),
			errdetail_internal("%s", err_detail.data));

	/*
	 * Insert into the conflict log table for LOG-level conflicts if
	 * requested.
	 */
	if (conflictlogrel)
	{
		insert_conflict_log_tuple(estate,
								  relinfo->ri_RelationDesc,
								  conflictlogrel,
								  type,
								  searchindexoid,
								  searchslot,
								  conflicttuples);
		table_close(conflictlogrel, NoLock);
	}
}

/*
 * Find all unique indexes to check for a conflict and store them into
 * ResultRelInfo.
 */
void
InitConflictIndexes(ResultRelInfo *relInfo)
{
	List	   *uniqueIndexes = NIL;

	for (int i = 0; i < relInfo->ri_NumIndices; i++)
	{
		Relation	indexRelation = relInfo->ri_IndexRelationDescs[i];

		if (indexRelation == NULL)
			continue;

		/* Detect conflict only for unique indexes */
		if (!relInfo->ri_IndexRelationInfo[i]->ii_Unique)
			continue;

		/* Don't support conflict detection for deferrable index */
		if (!indexRelation->rd_index->indimmediate)
			continue;

		uniqueIndexes = lappend_oid(uniqueIndexes,
									RelationGetRelid(indexRelation));
	}

	relInfo->ri_onConflictArbiterIndexes = uniqueIndexes;
}

/*
 * get_conflictlog_dest_and_table
 *
 * Decides where a conflict of the given elevel is to be reported, based on
 * the subscription's conflict_log_destination. Sets *log_dest to the
 * effective destination and, if that includes the conflict log table, returns
 * the table opened for insertion. Otherwise returns NULL.
 *
 * Only LOG-level conflicts (i.e. resolved conflicts where the transaction
 * continues) are recorded in the conflict log table. ERROR-level conflicts
 * halt replication and abort the transaction, so they are always reported
 * exclusively to the server log.
 */
static Relation
get_conflictlog_dest_and_table(int elevel, ConflictLogDest *log_dest)
{
	Relation	conflictlogrel;
	Oid			conflictlogrelid;

	if (elevel >= ERROR)
	{
		*log_dest = CONFLICT_LOG_DEST_LOG;
		return NULL;
	}

	/* Convert the text log destination to the internal enum. */
	*log_dest = GetConflictLogDest(MySubscription->conflictlogdest);

	/* Quick exit if a conflict log table was not requested. */
	if (!CONFLICTS_LOGGED_TO_TABLE(*log_dest))
		return NULL;

	conflictlogrelid = MySubscription->conflictlogrelid;

	Assert(OidIsValid(conflictlogrelid));

	/*
	 * Use try_table_open() because the table may have been dropped
	 * concurrently by an ALTER SUBSCRIPTION that changed
	 * conflict_log_destination. In that case, fall back to the server log
	 * instead of failing.
	 */
	conflictlogrel = try_table_open(conflictlogrelid, RowExclusiveLock);

	if (!conflictlogrel)
		*log_dest = CONFLICT_LOG_DEST_LOG;

	return conflictlogrel;
}

/*
 * Add SQLSTATE error code to the current conflict report.
 */
static int
errcode_apply_conflict(ConflictType type)
{
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			return errcode(ERRCODE_UNIQUE_VIOLATION);
		case CT_UPDATE_ORIGIN_DIFFERS:
		case CT_UPDATE_MISSING:
		case CT_DELETE_ORIGIN_DIFFERS:
		case CT_UPDATE_DELETED:
		case CT_DELETE_MISSING:
			return errcode(ERRCODE_T_R_SERIALIZATION_FAILURE);
	}

	Assert(false);
	return 0;					/* silence compiler warning */
}

/*
 * Helper function to build the additional details for conflicting key,
 * local row, remote row, and replica identity columns.
 */
static void
append_tuple_value_detail(StringInfo buf, List *tuple_values)
{
	bool		first = true;

	Assert(buf != NULL && tuple_values != NIL);

	foreach_ptr(char, tuple_value, tuple_values)
	{
		/*
		 * Skip if the value is NULL. This means the current user does not
		 * have enough permissions to see all columns in the table. See
		 * get_tuple_desc().
		 */
		if (!tuple_value)
			continue;

		/* standard SQL punctuation, not translated */
		if (!first)
			appendStringInfoString(buf, ", ");

		appendStringInfoString(buf, tuple_value);
		first = false;
	}
}

/*
 * Add an errdetail() line showing conflict detail.
 *
 * The DETAIL line comprises of two parts:
 * 1. Explanation of the conflict type, including the origin and commit
 *    timestamp of the local row.
 * 2. Display of conflicting key, local row, remote new row, and replica
 *    identity columns, if any. The remote old row is excluded as its
 *    information is covered in the replica identity columns.
 */
static void
errdetail_apply_conflict(EState *estate, ResultRelInfo *relinfo,
						 ConflictType type, TupleTableSlot *searchslot,
						 TupleTableSlot *localslot, TupleTableSlot *remoteslot,
						 Oid indexoid, TransactionId localxmin,
						 ReplOriginId localorigin, TimestampTz localts,
						 StringInfo err_msg)
{
	StringInfoData err_detail;
	StringInfoData tuple_buf;
	char	   *origin_name;
	char	   *key_desc = NULL;
	char	   *local_desc = NULL;
	char	   *remote_desc = NULL;
	char	   *search_desc = NULL;

	/* Get key, replica identity, remote, and local value data */
	get_tuple_desc(estate, relinfo, type, &key_desc,
				   localslot, &local_desc,
				   remoteslot, &remote_desc,
				   searchslot, &search_desc,
				   indexoid);

	initStringInfo(&err_detail);
	initStringInfo(&tuple_buf);

	/* Construct a detailed message describing the type of conflict */
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			Assert(OidIsValid(indexoid) &&
				   CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

			if (err_msg->len == 0)
			{
				append_tuple_value_detail(&tuple_buf,
										  list_make2(remote_desc, search_desc));

				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Could not apply remote change: %s.\n"),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Could not apply remote change.\n"));


				resetStringInfo(&tuple_buf);
			}

			append_tuple_value_detail(&tuple_buf,
									  list_make2(key_desc, local_desc));

			if (localts)
			{
				if (localorigin == InvalidReplOriginId)
				{
					if (tuple_buf.len)
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified locally in transaction %u at %s: %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts),
										 tuple_buf.data);
					else
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified locally in transaction %u at %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts));
				}
				else if (replorigin_by_oid(localorigin, true, &origin_name))
				{
					if (tuple_buf.len)
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by origin \"%s\" in transaction %u at %s: %s."),
										 get_rel_name(indexoid), origin_name,
										 localxmin, timestamptz_to_str(localts),
										 tuple_buf.data);
					else
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by origin \"%s\" in transaction %u at %s."),
										 get_rel_name(indexoid), origin_name,
										 localxmin, timestamptz_to_str(localts));
				}

				/*
				 * The origin that modified this row has been removed. This
				 * can happen if the origin was created by a different apply
				 * worker and its associated subscription and origin were
				 * dropped after updating the row, or if the origin was
				 * manually dropped by the user.
				 */
				else
				{
					if (tuple_buf.len)
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by a non-existent origin in transaction %u at %s: %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts),
										 tuple_buf.data);
					else
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by a non-existent origin in transaction %u at %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts));
				}
			}
			else
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified in transaction %u: %s."),
									 get_rel_name(indexoid), localxmin,
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified in transaction %u."),
									 get_rel_name(indexoid), localxmin);
			}

			break;

		case CT_UPDATE_ORIGIN_DIFFERS:
			append_tuple_value_detail(&tuple_buf,
									  list_make3(local_desc, remote_desc,
												 search_desc));

			if (localorigin == InvalidReplOriginId)
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Updating the row that was modified locally in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Updating the row that was modified locally in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}
			else if (replorigin_by_oid(localorigin, true, &origin_name))
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Updating the row that was modified by a different origin \"%s\" in transaction %u at %s: %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Updating the row that was modified by a different origin \"%s\" in transaction %u at %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts));
			}

			/* The origin that modified this row has been removed. */
			else
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Updating the row that was modified by a non-existent origin in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Updating the row that was modified by a non-existent origin in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}

			break;

		case CT_UPDATE_DELETED:
			append_tuple_value_detail(&tuple_buf,
									  list_make2(remote_desc, search_desc));

			if (tuple_buf.len)
				appendStringInfo(&err_detail, _("Could not find the row to be updated: %s.\n"),
								 tuple_buf.data);
			else
				appendStringInfo(&err_detail, _("Could not find the row to be updated.\n"));

			if (localts)
			{
				if (localorigin == InvalidReplOriginId)
					appendStringInfo(&err_detail, _("The row to be updated was deleted locally in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
				else if (replorigin_by_oid(localorigin, true, &origin_name))
					appendStringInfo(&err_detail, _("The row to be updated was deleted by a different origin \"%s\" in transaction %u at %s."),
									 origin_name, localxmin, timestamptz_to_str(localts));

				/* The origin that modified this row has been removed. */
				else
					appendStringInfo(&err_detail, _("The row to be updated was deleted by a non-existent origin in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}
			else
				appendStringInfoString(&err_detail, _("The row to be updated was deleted."));

			break;

		case CT_UPDATE_MISSING:
			append_tuple_value_detail(&tuple_buf,
									  list_make2(remote_desc, search_desc));

			if (tuple_buf.len)
				appendStringInfo(&err_detail, _("Could not find the row to be updated: %s."),
								 tuple_buf.data);
			else
				appendStringInfo(&err_detail, _("Could not find the row to be updated."));

			break;

		case CT_DELETE_ORIGIN_DIFFERS:
			append_tuple_value_detail(&tuple_buf,
									  list_make3(local_desc, remote_desc,
												 search_desc));

			if (localorigin == InvalidReplOriginId)
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Deleting the row that was modified locally in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Deleting the row that was modified locally in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}
			else if (replorigin_by_oid(localorigin, true, &origin_name))
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a different origin \"%s\" in transaction %u at %s: %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a different origin \"%s\" in transaction %u at %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts));
			}

			/* The origin that modified this row has been removed. */
			else
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a non-existent origin in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a non-existent origin in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}

			break;

		case CT_DELETE_MISSING:
			append_tuple_value_detail(&tuple_buf,
									  list_make1(search_desc));

			if (tuple_buf.len)
				appendStringInfo(&err_detail, _("Could not find the row to be deleted: %s."),
								 tuple_buf.data);
			else
				appendStringInfo(&err_detail, _("Could not find the row to be deleted."));

			break;
	}

	Assert(err_detail.len > 0);

	/*
	 * Insert a blank line to visually separate the new detail line from the
	 * existing ones.
	 */
	if (err_msg->len > 0)
		appendStringInfoChar(err_msg, '\n');

	appendStringInfoString(err_msg, err_detail.data);
}

/*
 * Extract conflicting key, local row, remote row, and replica identity
 * columns. Results are set at xxx_desc.
 *
 * If the output is NULL, it indicates that the current user lacks permissions
 * to view the columns involved.
 */
static void
get_tuple_desc(EState *estate, ResultRelInfo *relinfo, ConflictType type,
			   char **key_desc,
			   TupleTableSlot *localslot, char **local_desc,
			   TupleTableSlot *remoteslot, char **remote_desc,
			   TupleTableSlot *searchslot, char **search_desc,
			   Oid indexoid)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	Oid			relid = RelationGetRelid(localrel);
	TupleDesc	tupdesc = RelationGetDescr(localrel);
	char	   *desc = NULL;

	Assert((localslot && local_desc) || (remoteslot && remote_desc) ||
		   (searchslot && search_desc));

	/*
	 * Report the conflicting key values in the case of a unique constraint
	 * violation.
	 */
	if (type == CT_INSERT_EXISTS || type == CT_UPDATE_EXISTS ||
		type == CT_MULTIPLE_UNIQUE_CONFLICTS)
	{
		Assert(OidIsValid(indexoid) && localslot);

		desc = build_index_value_desc(estate, localrel, localslot,
									  indexoid);

		if (desc)
			*key_desc = psprintf(_("key %s"), desc);
	}

	if (localslot)
	{
		/*
		 * The 'modifiedCols' only applies to the new tuple, hence we pass
		 * NULL for the local row.
		 */
		desc = ExecBuildSlotValueDescription(relid, localslot, tupdesc,
											 NULL, 64);

		if (desc)
			*local_desc = psprintf(_("local row %s"), desc);
	}

	if (remoteslot)
	{
		Bitmapset  *modifiedCols;

		/*
		 * Although logical replication doesn't maintain the bitmap for the
		 * columns being inserted, we still use it to create 'modifiedCols'
		 * for consistency with other calls to ExecBuildSlotValueDescription.
		 *
		 * Note that generated columns are formed locally on the subscriber.
		 */
		modifiedCols = bms_union(ExecGetInsertedCols(relinfo, estate),
								 ExecGetUpdatedCols(relinfo, estate));
		desc = ExecBuildSlotValueDescription(relid, remoteslot,
											 tupdesc, modifiedCols,
											 64);

		if (desc)
			*remote_desc = psprintf(_("remote row %s"), desc);
	}

	if (searchslot)
	{
		/*
		 * Note that while index other than replica identity may be used (see
		 * IsIndexUsableForReplicaIdentityFull for details) to find the tuple
		 * when applying update or delete, such an index scan may not result
		 * in a unique tuple and we still compare the complete tuple in such
		 * cases, thus such indexes are not used here.
		 *
		 * XXX This can disagree with the index the apply worker searched by,
		 * see FindReplTupleInLocalRel(). It may not even be one that
		 * ExecOpenIndices() locked.
		 */
		Oid			replica_index = GetRelationIdentityOrPK(localrel);

		Assert(type != CT_INSERT_EXISTS);

		/*
		 * If the table has a valid replica identity index, build the index
		 * key value string. Otherwise, construct the full tuple value for
		 * REPLICA IDENTITY FULL cases.
		 */
		if (OidIsValid(replica_index))
			desc = build_index_value_desc(estate, localrel, searchslot, replica_index);
		else
			desc = ExecBuildSlotValueDescription(relid, searchslot, tupdesc, NULL, 64);

		if (desc)
		{
			if (OidIsValid(replica_index))
				*search_desc = psprintf(_("replica identity %s"), desc);
			else
				*search_desc = psprintf(_("replica identity full %s"), desc);
		}
	}
}

/*
 * Helper function to extract the "raw" index key Datums and their null flags
 * from a TupleTableSlot, given an already open index descriptor.
 */
static void
build_index_datums_from_slot(EState *estate, Relation localrel,
							 TupleTableSlot *slot,
							 Relation indexDesc, Datum *values,
							 bool *isnull)
{
	TupleTableSlot *tableslot = slot;

	/*
	 * FormIndexDatum needs an EState whose per-tuple econtext's scantuple
	 * points to the slot holding the tuple, to evaluate any index
	 * expressions. It also requires a heap tuple slot, so copy a virtual slot
	 * into one, created in the EState's tuple table so that it is freed along
	 * with it.
	 */
	if (TTS_IS_VIRTUAL(slot))
	{
		tableslot = table_slot_create(localrel, &estate->es_tupleTable);
		tableslot = ExecCopySlot(tableslot, slot);
	}

	GetPerTupleExprContext(estate)->ecxt_scantuple = tableslot;

	/* Form the index datums */
	FormIndexDatum(BuildIndexInfo(indexDesc), tableslot, estate, values,
				   isnull);
}

/*
 * Helper functions to construct a string describing the contents of an index
 * entry. See BuildIndexValueDescription for details.
 *
 * The caller must ensure that the index with the OID 'indexoid' is locked so
 * that we can fetch and display the conflicting key value.
 */
static char *
build_index_value_desc(EState *estate, Relation localrel, TupleTableSlot *slot,
					   Oid indexoid)
{
	char	   *index_value;
	Relation	indexDesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	if (!slot)
		return NULL;

	Assert(CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

	indexDesc = index_open(indexoid, NoLock);

	build_index_datums_from_slot(estate, localrel, slot, indexDesc, values,
								 isnull);

	index_value = BuildIndexValueDescription(indexDesc, values, isnull);

	index_close(indexDesc, NoLock);

	return index_value;
}

/*
 * build_index_key_json
 *
 * Fetch replica identity key from the tuple table slot and convert into a
 * JSON datum.
 *
 * The caller must ensure that the index with the OID 'indexid' is locked so
 * that we can safely fetch and construct the replica identity key values.
 * See comments atop ReportApplyConflict() for how this holds.
 */
static Datum
build_index_key_json(EState *estate, Relation localrel,
					 Oid indexid, TupleTableSlot *slot,
					 bool *omitted)
{
	Relation	indexDesc;
	TupleDesc	reltupdesc = RelationGetDescr(localrel);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	StringInfoData result;
	int			indnkeyatts;
	Datum		datum;

	Assert(slot != NULL);

	Assert(CheckRelationOidLockedByMe(indexid, RowExclusiveLock, true));

	indexDesc = index_open(indexid, NoLock);

	build_index_datums_from_slot(estate, localrel, slot, indexDesc, values,
								 isnull);

	/*
	 * Build the JSON object here rather than passing the values to
	 * row_to_json(). For a type at or above FirstNormalObjectId,
	 * json_categorize_type() would use a user-defined cast to json if one
	 * exists. Nothing else in apply calls that cast, so its output is
	 * unrelated to the size of the value we received, and a small key could
	 * produce a json value over the 1GB limit and fail the apply worker.
	 * Rendering each value with its type's output function keeps us to the
	 * representation the publisher sent (when the column types and output
	 * settings match, see below), and matches what the server log does in
	 * BuildIndexValueDescription().
	 *
	 * Only the key attributes are recorded. FormIndexDatum() above also fills
	 * in any non-key (INCLUDE) columns, but those are not part of the replica
	 * identity.
	 */
	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(indexDesc);

	initStringInfo(&result);
	appendStringInfoChar(&result, '{');

	for (int i = 0; i < indnkeyatts; i++)
	{
		Form_pg_attribute att;
		AttrNumber	attnum;

		/*
		 * Take the column name from the table, not the index descriptor. The
		 * index keeps the name it had at creation, which goes stale after a
		 * column rename and can be wrong for repeated columns such as UNIQUE
		 * (a, a). Using the table's names keeps these consistent with the
		 * server log.
		 *
		 * Replica identity and primary key indexes have no expression
		 * columns, so every key maps to a table column.
		 */
		attnum = indexDesc->rd_index->indkey.values[i];
		Assert(attnum > 0);

		att = TupleDescAttr(reltupdesc, attnum - 1);

		if (i > 0)
			appendStringInfoChar(&result, ',');

		escape_json(&result, NameStr(att->attname));
		appendStringInfoChar(&result, ':');

		if (isnull[i])
			appendStringInfoString(&result, "null");
		else
		{
			Oid			outfuncoid;
			bool		typisvarlena;
			char	   *outputstr;
			Size		outputlen;

			/*
			 * See BuildIndexValueDescription() for why we used rd_opcintype
			 * instead of index's tupleDesc.
			 */
			getTypeOutputInfo(indexDesc->rd_opcintype[i], &outfuncoid, &typisvarlena);

			/*
			 * XXX The cap below is checked only after the value is rendered.
			 * The publisher rendered the same value to send it, which bounds
			 * its length, unless the column types differ, subscription option
			 * binary = true, or the apply worker's bytea_output or
			 * quote_all_identifiers differs. Then the render can exceed 1GB
			 * and error out the apply worker.
			 */
			outputstr = OidOutputFunctionCall(outfuncoid, values[i]);
			outputlen = strlen(outputstr);

			/*
			 * Don't record a value larger than CONFLICT_MAX_VALUE_SIZE.
			 * Instead, put a marker recording its length in its place.
			 *
			 * XXX The marker deliberately keeps no prefix of the value. For
			 * arrays, composites and similar types the leading bytes are
			 * mostly punctuation and say little about the value.
			 */
			if (outputlen > CONFLICT_MAX_VALUE_SIZE)
			{
				/* A marker is an object, so it is appended unescaped. */
				appendStringInfo(&result,
								 "{\"omitted\":true,\"length\":" UINT64_FORMAT "}",
								 (uint64) outputlen);
				*omitted = true;
				pfree(outputstr);
				continue;
			}

			escape_json(&result, outputstr);
			pfree(outputstr);
		}
	}

	appendStringInfoChar(&result, '}');

	index_close(indexDesc, NoLock);

	datum = PointerGetDatum(cstring_to_text_with_len(result.data, result.len));
	pfree(result.data);

	return datum;
}

/*
 * build_local_conflicts_tupledesc
 *
 * Build and bless a tuple descriptor for the elements of the local_conflicts
 * array based on the predefined LocalConflictSchema.
 */
static TupleDesc
build_local_conflicts_tupledesc(void)
{
	static TupleDesc cached_tupdesc = NULL;

	if (cached_tupdesc == NULL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		cached_tupdesc = CreateTemplateTupleDesc(NUM_LOCAL_CONFLICT_ATTRS);

		for (int i = 0; i < NUM_LOCAL_CONFLICT_ATTRS; i++)
			TupleDescInitEntry(cached_tupdesc,
							   (AttrNumber) (i + 1),
							   LocalConflictSchema[i].attname,
							   LocalConflictSchema[i].atttypid,
							   -1, 0);

		TupleDescFinalize(cached_tupdesc);

		/*
		 * Bless once so it can be used as a RECORD type (e.g. for row_to_json
		 * or other record-based operations).
		 */
		BlessTupleDesc(cached_tupdesc);

		MemoryContextSwitchTo(oldcxt);
	}

	return cached_tupdesc;
}

/*
 * Builds the local conflicts JSON array column from the list of
 * ConflictTupleInfo objects.
 *
 * Example output structure:
 * [ { "xid": "1001", "commit_ts": "...", "origin": "..." }, ... ]
 */
static Datum
build_local_conflicts_json_array(List *conflicttuples)
{
	Datum	   *json_datum_array;
	Datum		json_array_datum;
	int			num_conflicts;
	int			i = 0;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	TupleDesc	tupdesc;

	Assert(conflicttuples != NIL);

	/* Build local conflicts tuple descriptor. */
	tupdesc = build_local_conflicts_tupledesc();

	num_conflicts = list_length(conflicttuples);
	json_datum_array = palloc_array(Datum, num_conflicts);

	/* Process local conflict tuple list and prepare an array of JSON. */
	foreach_ptr(ConflictTupleInfo, conflicttuple, conflicttuples)
	{
		Datum		values[NUM_LOCAL_CONFLICT_ATTRS] = {0};
		bool		nulls[NUM_LOCAL_CONFLICT_ATTRS] = {0};
		char	   *origin_name = NULL;
		HeapTuple	tuple;
		Datum		datum;
		Datum		json_datum;
		int			attno;

		attno = 0;
		if (FullTransactionIdIsValid(conflicttuple->xmin))
			values[attno++] = FullTransactionIdGetDatum(conflicttuple->xmin);
		else
			nulls[attno++] = true;

		if (conflicttuple->ts)
			values[attno++] = TimestampTzGetDatum(conflicttuple->ts);
		else
			nulls[attno++] = true;

		if (conflicttuple->origin != InvalidReplOriginId)
			replorigin_by_oid(conflicttuple->origin, true, &origin_name);

		/*
		 * Set NULL if origin name for the tuple is InvalidReplOriginId or not
		 * found.
		 */
		if (origin_name != NULL)
			values[attno++] = CStringGetTextDatum(origin_name);
		else
			nulls[attno++] = true;

		Assert(attno == NUM_LOCAL_CONFLICT_ATTRS);

		tuple = heap_form_tuple(tupdesc, values, nulls);

		datum = heap_copy_tuple_as_datum(tuple, tupdesc);

		/*
		 * Build the higher level JSON datum in format described in function
		 * header.
		 *
		 * Unlike build_index_key_json(), using row_to_json() is safe here
		 * because the object's fields are all built-in types (xid8,
		 * timestamptz, text) below FirstNormalObjectId, so
		 * json_categorize_type() never invokes a user CREATE CAST (t AS
		 * json), and the values are small and bounded.
		 */
		json_datum = DirectFunctionCall1(row_to_json, datum);

		/* Done with the temporary tuple. */
		heap_freetuple(tuple);

		/* Add to the array element. */
		json_datum_array[i++] = json_datum;
	}

	Assert(i == num_conflicts);

	/* Construct the JSON array Datum. */
	get_typlenbyvalalign(JSONOID, &typlen, &typbyval, &typalign);
	json_array_datum = PointerGetDatum(construct_array(json_datum_array,
													   num_conflicts,
													   JSONOID,
													   typlen,
													   typbyval,
													   typalign));
	pfree(json_datum_array);

	return json_array_datum;
}

/*
 * insert_conflict_log_tuple
 *
 * Prepares and inserts a tuple detailing a conflict encountered during
 * logical replication into the conflict log table.
 */
static void
insert_conflict_log_tuple(EState *estate, Relation rel,
						  Relation conflictlogrel,
						  ConflictType conflict_type,
						  Oid searchindexoid,
						  TupleTableSlot *searchslot,
						  List *conflicttuples)
{
	Datum		values[NUM_CONFLICT_ATTRS] = {0};
	bool		nulls[NUM_CONFLICT_ATTRS] = {0};
	int			attno;
	char	   *apply_origin = NULL;
	TransactionId remote_xid;
	XLogRecPtr	remote_final_lsn;
	TimestampTz remote_commit_ts;
	bool		omitted = false;
	Oid			replica_index;
	HeapTuple	tuple;

	Assert(conflictlogrel != NULL);

	/* Fetch the information of the remote transaction being applied. */
	GetRemoteTransactionInfoForConflict(&remote_xid, &remote_final_lsn,
										&remote_commit_ts);

	/* Populate the values and nulls arrays. */
	attno = 0;
	values[attno++] = ObjectIdGetDatum(RelationGetRelid(rel));

	values[attno++] =
		CStringGetTextDatum(get_namespace_name(RelationGetNamespace(rel)));

	values[attno++] = CStringGetTextDatum(RelationGetRelationName(rel));

	values[attno++] = CStringGetTextDatum(ConflictTypeNames[conflict_type]);

	/*
	 * The remote xid is always valid here: a conflict is only detected while
	 * applying a row change, which is preceded by a BEGIN, BEGIN PREPARE or
	 * STREAM START that recorded a valid top-level xid.
	 */
	Assert(TransactionIdIsValid(remote_xid));
	values[attno++] = TransactionIdGetDatum(remote_xid);

	if (XLogRecPtrIsValid(remote_final_lsn))
		values[attno++] = LSNGetDatum(remote_final_lsn);
	else
		nulls[attno++] = true;

	if (remote_commit_ts != 0)
		values[attno++] = TimestampTzGetDatum(remote_commit_ts);
	else
		nulls[attno++] = true;

	if (replorigin_xact_state.origin != InvalidReplOriginId)
		replorigin_by_oid(replorigin_xact_state.origin, true, &apply_origin);

	if (apply_origin != NULL)
		values[attno++] = CStringGetTextDatum(apply_origin);
	else
		nulls[attno++] = true;

	/*
	 * All conflicts logged to the table are LOG-level update or delete
	 * conflicts, which always have a searchslot. Insert conflicts, which
	 * don't, are ERROR-level and never reach here.
	 */
	Assert(!TupIsNull(searchslot));

	replica_index = searchindexoid;

	/*
	 * If the search used a replica identity index, build the index JSON datum
	 * from key value. Otherwise, in REPLICA IDENTITY FULL cases, set
	 * replica_identity_full to true and leave replica_identity NULL to avoid
	 * serializing full tuples that could exceed memory allocation limits.
	 */
	if (OidIsValid(replica_index))
	{
		values[attno++] = BoolGetDatum(false);
		values[attno++] = build_index_key_json(estate, rel,
											   replica_index,
											   searchslot,
											   &omitted);
	}
	else
	{
		values[attno++] = BoolGetDatum(true);
		nulls[attno++] = true;
	}

	/*
	 * In update_missing and delete_missing conflicts, there are no local
	 * conflicting rows, so set local_conflicts to NULL.
	 */
	if (conflicttuples != NIL)
		values[attno++] = build_local_conflicts_json_array(conflicttuples);
	else
		nulls[attno++] = true;

	values[attno] = BoolGetDatum(omitted);

	Assert(attno + 1 == NUM_CONFLICT_ATTRS);

	/*
	 * Conflict log tables are always created with the heap access method (see
	 * create_conflict_log_table()) and cannot be altered, so we can call
	 * heap_insert() directly instead of creating a TupleTableSlot for
	 * table_tuple_insert().
	 *
	 * The conflict log table itself is never logically decoded, but its TOAST
	 * table would be. heap_insert() passes HEAP_INSERT_NO_LOGICAL on to the
	 * TOAST inserts, so toasted values are not decoded either.
	 */
	tuple = heap_form_tuple(RelationGetDescr(conflictlogrel), values, nulls);

	heap_insert(conflictlogrel, tuple,
				GetCurrentCommandId(true), HEAP_INSERT_NO_LOGICAL, NULL);

	heap_freetuple(tuple);
}
