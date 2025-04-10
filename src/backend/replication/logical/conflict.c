/*-------------------------------------------------------------------------
 * conflict.c
 *	   Support routines for logging conflicts.
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
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
#include "access/tableam.h"
#include "executor/executor.h"
#include "pgstat.h"
#include "replication/conflict.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"

static const char *const ConflictTypeNames[] = {
	[CT_INSERT_EXISTS] = "insert_exists",
	[CT_UPDATE_ORIGIN_DIFFERS] = "update_origin_differs",
	[CT_UPDATE_EXISTS] = "update_exists",
	[CT_UPDATE_MISSING] = "update_missing",
	[CT_DELETE_ORIGIN_DIFFERS] = "delete_origin_differs",
	[CT_DELETE_MISSING] = "delete_missing",
	[CT_MULTIPLE_UNIQUE_CONFLICTS] = "multiple_unique_conflicts"
};

static int	errcode_apply_conflict(ConflictType type);
static void errdetail_apply_conflict(EState *estate,
									 ResultRelInfo *relinfo,
									 ConflictType type,
									 TupleTableSlot *searchslot,
									 TupleTableSlot *localslot,
									 TupleTableSlot *remoteslot,
									 Oid indexoid, TransactionId localxmin,
									 RepOriginId localorigin,
									 TimestampTz localts, StringInfo err_msg);
static char *build_tuple_value_details(EState *estate, ResultRelInfo *relinfo,
									   ConflictType type,
									   TupleTableSlot *searchslot,
									   TupleTableSlot *localslot,
									   TupleTableSlot *remoteslot,
									   Oid indexoid);
static char *build_index_value_desc(EState *estate, Relation localrel,
									TupleTableSlot *slot, Oid indexoid);

/*
 * Get the xmin and commit timestamp data (origin and timestamp) associated
 * with the provided local tuple.
 *
 * Return true if the commit timestamp data was found, false otherwise.
 */
bool
GetTupleTransactionInfo(TupleTableSlot *localslot, TransactionId *xmin,
						RepOriginId *localorigin, TimestampTz *localts)
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
		*localorigin = InvalidRepOriginId;
		*localts = 0;
		return false;
	}

	return TransactionIdGetCommitTsData(*xmin, localts, localorigin);
}

/*
 * This function is used to report a conflict while applying replication
 * changes.
 *
 * 'searchslot' should contain the tuple used to search the local tuple to be
 * updated or deleted.
 *
 * 'remoteslot' should contain the remote new tuple, if any.
 *
 * conflicttuples is a list of local tuples that caused the conflict and the
 * conflict related information. See ConflictTupleInfo.
 *
 * The caller must ensure that all the indexes passed in ConflictTupleInfo are
 * locked so that we can fetch and display the conflicting key values.
 */
void
ReportApplyConflict(EState *estate, ResultRelInfo *relinfo, int elevel,
					ConflictType type, TupleTableSlot *searchslot,
					TupleTableSlot *remoteslot, List *conflicttuples)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	StringInfoData err_detail;

	initStringInfo(&err_detail);

	/* Form errdetail message by combining conflicting tuples information. */
	foreach_ptr(ConflictTupleInfo, conflicttuple, conflicttuples)
		errdetail_apply_conflict(estate, relinfo, type, searchslot,
								 conflicttuple->slot, remoteslot,
								 conflicttuple->indexoid,
								 conflicttuple->xmin,
								 conflicttuple->origin,
								 conflicttuple->ts,
								 &err_detail);

	pgstat_report_subscription_conflict(MySubscription->oid, type);

	ereport(elevel,
			errcode_apply_conflict(type),
			errmsg("conflict detected on relation \"%s.%s\": conflict=%s",
				   get_namespace_name(RelationGetNamespace(localrel)),
				   RelationGetRelationName(localrel),
				   ConflictTypeNames[type]),
			errdetail_internal("%s", err_detail.data));
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
		case CT_DELETE_MISSING:
			return errcode(ERRCODE_T_R_SERIALIZATION_FAILURE);
	}

	Assert(false);
	return 0;					/* silence compiler warning */
}

/*
 * Add an errdetail() line showing conflict detail.
 *
 * The DETAIL line comprises of two parts:
 * 1. Explanation of the conflict type, including the origin and commit
 *    timestamp of the existing local tuple.
 * 2. Display of conflicting key, existing local tuple, remote new tuple, and
 *    replica identity columns, if any. The remote old tuple is excluded as its
 *    information is covered in the replica identity columns.
 */
static void
errdetail_apply_conflict(EState *estate, ResultRelInfo *relinfo,
						 ConflictType type, TupleTableSlot *searchslot,
						 TupleTableSlot *localslot, TupleTableSlot *remoteslot,
						 Oid indexoid, TransactionId localxmin,
						 RepOriginId localorigin, TimestampTz localts,
						 StringInfo err_msg)
{
	StringInfoData err_detail;
	char	   *val_desc;
	char	   *origin_name;

	initStringInfo(&err_detail);

	/* First, construct a detailed message describing the type of conflict */
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			Assert(OidIsValid(indexoid) &&
				   CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

			if (localts)
			{
				if (localorigin == InvalidRepOriginId)
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified locally in transaction %u at %s."),
									 get_rel_name(indexoid),
									 localxmin, timestamptz_to_str(localts));
				else if (replorigin_by_oid(localorigin, true, &origin_name))
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by origin \"%s\" in transaction %u at %s."),
									 get_rel_name(indexoid), origin_name,
									 localxmin, timestamptz_to_str(localts));

				/*
				 * The origin that modified this row has been removed. This
				 * can happen if the origin was created by a different apply
				 * worker and its associated subscription and origin were
				 * dropped after updating the row, or if the origin was
				 * manually dropped by the user.
				 */
				else
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by a non-existent origin in transaction %u at %s."),
									 get_rel_name(indexoid),
									 localxmin, timestamptz_to_str(localts));
			}
			else
				appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified in transaction %u."),
								 get_rel_name(indexoid), localxmin);

			break;

		case CT_UPDATE_ORIGIN_DIFFERS:
			if (localorigin == InvalidRepOriginId)
				appendStringInfo(&err_detail, _("Updating the row that was modified locally in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));
			else if (replorigin_by_oid(localorigin, true, &origin_name))
				appendStringInfo(&err_detail, _("Updating the row that was modified by a different origin \"%s\" in transaction %u at %s."),
								 origin_name, localxmin, timestamptz_to_str(localts));

			/* The origin that modified this row has been removed. */
			else
				appendStringInfo(&err_detail, _("Updating the row that was modified by a non-existent origin in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));

			break;

		case CT_UPDATE_MISSING:
			appendStringInfoString(&err_detail, _("Could not find the row to be updated."));
			break;

		case CT_DELETE_ORIGIN_DIFFERS:
			if (localorigin == InvalidRepOriginId)
				appendStringInfo(&err_detail, _("Deleting the row that was modified locally in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));
			else if (replorigin_by_oid(localorigin, true, &origin_name))
				appendStringInfo(&err_detail, _("Deleting the row that was modified by a different origin \"%s\" in transaction %u at %s."),
								 origin_name, localxmin, timestamptz_to_str(localts));

			/* The origin that modified this row has been removed. */
			else
				appendStringInfo(&err_detail, _("Deleting the row that was modified by a non-existent origin in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));

			break;

		case CT_DELETE_MISSING:
			appendStringInfoString(&err_detail, _("Could not find the row to be deleted."));
			break;
	}

	Assert(err_detail.len > 0);

	val_desc = build_tuple_value_details(estate, relinfo, type, searchslot,
										 localslot, remoteslot, indexoid);

	/*
	 * Next, append the key values, existing local tuple, remote tuple and
	 * replica identity columns after the message.
	 */
	if (val_desc)
		appendStringInfo(&err_detail, "\n%s", val_desc);

	/*
	 * Insert a blank line to visually separate the new detail line from the
	 * existing ones.
	 */
	if (err_msg->len > 0)
		appendStringInfoChar(err_msg, '\n');

	appendStringInfoString(err_msg, err_detail.data);
}

/*
 * Helper function to build the additional details for conflicting key,
 * existing local tuple, remote tuple, and replica identity columns.
 *
 * If the return value is NULL, it indicates that the current user lacks
 * permissions to view the columns involved.
 */
static char *
build_tuple_value_details(EState *estate, ResultRelInfo *relinfo,
						  ConflictType type,
						  TupleTableSlot *searchslot,
						  TupleTableSlot *localslot,
						  TupleTableSlot *remoteslot,
						  Oid indexoid)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	Oid			relid = RelationGetRelid(localrel);
	TupleDesc	tupdesc = RelationGetDescr(localrel);
	StringInfoData tuple_value;
	char	   *desc = NULL;

	Assert(searchslot || localslot || remoteslot);

	initStringInfo(&tuple_value);

	/*
	 * Report the conflicting key values in the case of a unique constraint
	 * violation.
	 */
	if (type == CT_INSERT_EXISTS || type == CT_UPDATE_EXISTS ||
		type == CT_MULTIPLE_UNIQUE_CONFLICTS)
	{
		Assert(OidIsValid(indexoid) && localslot);

		desc = build_index_value_desc(estate, localrel, localslot, indexoid);

		if (desc)
			appendStringInfo(&tuple_value, _("Key %s"), desc);
	}

	if (localslot)
	{
		/*
		 * The 'modifiedCols' only applies to the new tuple, hence we pass
		 * NULL for the existing local tuple.
		 */
		desc = ExecBuildSlotValueDescription(relid, localslot, tupdesc,
											 NULL, 64);

		if (desc)
		{
			if (tuple_value.len > 0)
			{
				appendStringInfoString(&tuple_value, "; ");
				appendStringInfo(&tuple_value, _("existing local tuple %s"),
								 desc);
			}
			else
			{
				appendStringInfo(&tuple_value, _("Existing local tuple %s"),
								 desc);
			}
		}
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
		desc = ExecBuildSlotValueDescription(relid, remoteslot, tupdesc,
											 modifiedCols, 64);

		if (desc)
		{
			if (tuple_value.len > 0)
			{
				appendStringInfoString(&tuple_value, "; ");
				appendStringInfo(&tuple_value, _("remote tuple %s"), desc);
			}
			else
			{
				appendStringInfo(&tuple_value, _("Remote tuple %s"), desc);
			}
		}
	}

	if (searchslot)
	{
		/*
		 * Note that while index other than replica identity may be used (see
		 * IsIndexUsableForReplicaIdentityFull for details) to find the tuple
		 * when applying update or delete, such an index scan may not result
		 * in a unique tuple and we still compare the complete tuple in such
		 * cases, thus such indexes are not used here.
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
			if (tuple_value.len > 0)
			{
				appendStringInfoString(&tuple_value, "; ");
				appendStringInfo(&tuple_value, OidIsValid(replica_index)
								 ? _("replica identity %s")
								 : _("replica identity full %s"), desc);
			}
			else
			{
				appendStringInfo(&tuple_value, OidIsValid(replica_index)
								 ? _("Replica identity %s")
								 : _("Replica identity full %s"), desc);
			}
		}
	}

	if (tuple_value.len == 0)
		return NULL;

	appendStringInfoChar(&tuple_value, '.');
	return tuple_value.data;
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
	TupleTableSlot *tableslot = slot;

	if (!tableslot)
		return NULL;

	Assert(CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

	indexDesc = index_open(indexoid, NoLock);

	/*
	 * If the slot is a virtual slot, copy it into a heap tuple slot as
	 * FormIndexDatum only works with heap tuple slots.
	 */
	if (TTS_IS_VIRTUAL(slot))
	{
		tableslot = table_slot_create(localrel, &estate->es_tupleTable);
		tableslot = ExecCopySlot(tableslot, slot);
	}

	/*
	 * Initialize ecxt_scantuple for potential use in FormIndexDatum when
	 * index expressions are present.
	 */
	GetPerTupleExprContext(estate)->ecxt_scantuple = tableslot;

	/*
	 * The values/nulls arrays passed to BuildIndexValueDescription should be
	 * the results of FormIndexDatum, which are the "raw" input to the index
	 * AM.
	 */
	FormIndexDatum(BuildIndexInfo(indexDesc), tableslot, estate, values, isnull);

	index_value = BuildIndexValueDescription(indexDesc, values, isnull);

	index_close(indexDesc, NoLock);

	return index_value;
}
