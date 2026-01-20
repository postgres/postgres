/*-------------------------------------------------------------------------
 * sequencesync.c
 *	  PostgreSQL logical replication: sequence synchronization
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/sequencesync.c
 *
 * NOTES
 *	  This file contains code for sequence synchronization for
 *	  logical replication.
 *
 * Sequences requiring synchronization are tracked in the pg_subscription_rel
 * catalog.
 *
 * Sequences to be synchronized will be added with state INIT when either of
 * the following commands is executed:
 * CREATE SUBSCRIPTION
 * ALTER SUBSCRIPTION ... REFRESH PUBLICATION
 *
 * Executing the following command resets all sequences in the subscription to
 * state INIT, triggering re-synchronization:
 * ALTER SUBSCRIPTION ... REFRESH SEQUENCES
 *
 * The apply worker periodically scans pg_subscription_rel for sequences in
 * INIT state. When such sequences are found, it spawns a sequencesync worker
 * to handle synchronization.
 *
 * A single sequencesync worker is responsible for synchronizing all sequences.
 * It begins by retrieving the list of sequences that are flagged for
 * synchronization, i.e., those in the INIT state. These sequences are then
 * processed in batches, allowing multiple entries to be synchronized within a
 * single transaction. The worker fetches the current sequence values and page
 * LSNs from the remote publisher, updates the corresponding sequences on the
 * local subscriber, and finally marks each sequence as READY upon successful
 * synchronization.
 *
 * Sequence state transitions follow this pattern:
 *   INIT -> READY
 *
 * To avoid creating too many transactions, up to MAX_SEQUENCES_SYNC_PER_BATCH
 * sequences are synchronized per transaction. The locks on the sequence
 * relation will be periodically released at each transaction commit.
 *
 * XXX: We didn't choose launcher process to maintain the launch of sequencesync
 * worker as it didn't have database connection to access the sequences from the
 * pg_subscription_rel system catalog that need to be synchronized.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_subscription_rel.h"
#include "commands/sequence.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/logicalworker.h"
#include "replication/worker_internal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/syscache.h"
#include "utils/usercontext.h"

#define REMOTE_SEQ_COL_COUNT 10

typedef enum CopySeqResult
{
	COPYSEQ_SUCCESS,
	COPYSEQ_MISMATCH,
	COPYSEQ_INSUFFICIENT_PERM,
	COPYSEQ_SKIPPED
} CopySeqResult;

static List *seqinfos = NIL;

/*
 * Apply worker determines if sequence synchronization is needed.
 *
 * Start a sequencesync worker if one is not already running. The active
 * sequencesync worker will handle all pending sequence synchronization. If any
 * sequences remain unsynchronized after it exits, a new worker can be started
 * in the next iteration.
 */
void
ProcessSequencesForSync(void)
{
	LogicalRepWorker *sequencesync_worker;
	int			nsyncworkers;
	bool		has_pending_sequences;
	bool		started_tx;

	FetchRelationStates(NULL, &has_pending_sequences, &started_tx);

	if (started_tx)
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	if (!has_pending_sequences)
		return;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	/* Check if there is a sequencesync worker already running? */
	sequencesync_worker = logicalrep_worker_find(WORKERTYPE_SEQUENCESYNC,
												 MyLogicalRepWorker->subid,
												 InvalidOid, true);
	if (sequencesync_worker)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return;
	}

	/*
	 * Count running sync workers for this subscription, while we have the
	 * lock.
	 */
	nsyncworkers = logicalrep_sync_worker_count(MyLogicalRepWorker->subid);
	LWLockRelease(LogicalRepWorkerLock);

	/*
	 * It is okay to read/update last_seqsync_start_time here in apply worker
	 * as we have already ensured that sync worker doesn't exist.
	 */
	launch_sync_worker(WORKERTYPE_SEQUENCESYNC, nsyncworkers, InvalidOid,
					   &MyLogicalRepWorker->last_seqsync_start_time);
}

/*
 * get_sequences_string
 *
 * Build a comma-separated string of schema-qualified sequence names
 * for the given list of sequence indexes.
 */
static void
get_sequences_string(List *seqindexes, StringInfo buf)
{
	resetStringInfo(buf);
	foreach_int(seqidx, seqindexes)
	{
		LogicalRepSequenceInfo *seqinfo =
			(LogicalRepSequenceInfo *) list_nth(seqinfos, seqidx);

		if (buf->len > 0)
			appendStringInfoString(buf, ", ");

		appendStringInfo(buf, "\"%s.%s\"", seqinfo->nspname, seqinfo->seqname);
	}
}

/*
 * report_sequence_errors
 *
 * Report discrepancies found during sequence synchronization between
 * the publisher and subscriber. Emits warnings for:
 * a) mismatched definitions or concurrent rename
 * b) insufficient privileges
 * c) missing sequences on the subscriber
 * Then raises an ERROR to indicate synchronization failure.
 */
static void
report_sequence_errors(List *mismatched_seqs_idx, List *insuffperm_seqs_idx,
					   List *missing_seqs_idx)
{
	StringInfo	seqstr;

	/* Quick exit if there are no errors to report */
	if (!mismatched_seqs_idx && !insuffperm_seqs_idx && !missing_seqs_idx)
		return;

	seqstr = makeStringInfo();

	if (mismatched_seqs_idx)
	{
		get_sequences_string(mismatched_seqs_idx, seqstr);
		ereport(WARNING,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("mismatched or renamed sequence on subscriber (%s)",
							  "mismatched or renamed sequences on subscriber (%s)",
							  list_length(mismatched_seqs_idx),
							  seqstr->data));
	}

	if (insuffperm_seqs_idx)
	{
		get_sequences_string(insuffperm_seqs_idx, seqstr);
		ereport(WARNING,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("insufficient privileges on sequence (%s)",
							  "insufficient privileges on sequences (%s)",
							  list_length(insuffperm_seqs_idx),
							  seqstr->data));
	}

	if (missing_seqs_idx)
	{
		get_sequences_string(missing_seqs_idx, seqstr);
		ereport(WARNING,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("missing sequence on publisher (%s)",
							  "missing sequences on publisher (%s)",
							  list_length(missing_seqs_idx),
							  seqstr->data));
	}

	ereport(ERROR,
			errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			errmsg("logical replication sequence synchronization failed for subscription \"%s\"",
				   MySubscription->name));
}

/*
 * get_and_validate_seq_info
 *
 * Extracts remote sequence information from the tuple slot received from the
 * publisher, and validates it against the corresponding local sequence
 * definition.
 */
static CopySeqResult
get_and_validate_seq_info(TupleTableSlot *slot, Relation *sequence_rel,
						  LogicalRepSequenceInfo **seqinfo, int *seqidx)
{
	bool		isnull;
	int			col = 0;
	Datum		datum;
	Oid			remote_typid;
	int64		remote_start;
	int64		remote_increment;
	int64		remote_min;
	int64		remote_max;
	bool		remote_cycle;
	CopySeqResult result = COPYSEQ_SUCCESS;
	HeapTuple	tup;
	Form_pg_sequence local_seq;
	LogicalRepSequenceInfo *seqinfo_local;

	*seqidx = DatumGetInt32(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	/* Identify the corresponding local sequence for the given index. */
	*seqinfo = seqinfo_local =
		(LogicalRepSequenceInfo *) list_nth(seqinfos, *seqidx);

	/*
	 * last_value can be NULL if the sequence was dropped concurrently (see
	 * pg_get_sequence_data()).
	 */
	datum = slot_getattr(slot, ++col, &isnull);
	if (isnull)
		return COPYSEQ_SKIPPED;
	seqinfo_local->last_value = DatumGetInt64(datum);

	seqinfo_local->is_called = DatumGetBool(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	seqinfo_local->page_lsn = DatumGetLSN(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	remote_typid = DatumGetObjectId(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	remote_start = DatumGetInt64(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	remote_increment = DatumGetInt64(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	remote_min = DatumGetInt64(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	remote_max = DatumGetInt64(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	remote_cycle = DatumGetBool(slot_getattr(slot, ++col, &isnull));
	Assert(!isnull);

	/* Sanity check */
	Assert(col == REMOTE_SEQ_COL_COUNT);

	seqinfo_local->found_on_pub = true;

	*sequence_rel = try_table_open(seqinfo_local->localrelid, RowExclusiveLock);

	/* Sequence was concurrently dropped? */
	if (!*sequence_rel)
		return COPYSEQ_SKIPPED;

	tup = SearchSysCache1(SEQRELID, ObjectIdGetDatum(seqinfo_local->localrelid));

	/* Sequence was concurrently dropped? */
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for sequence %u",
			 seqinfo_local->localrelid);

	local_seq = (Form_pg_sequence) GETSTRUCT(tup);

	/* Sequence parameters for remote/local are the same? */
	if (local_seq->seqtypid != remote_typid ||
		local_seq->seqstart != remote_start ||
		local_seq->seqincrement != remote_increment ||
		local_seq->seqmin != remote_min ||
		local_seq->seqmax != remote_max ||
		local_seq->seqcycle != remote_cycle)
		result = COPYSEQ_MISMATCH;

	/* Sequence was concurrently renamed? */
	if (strcmp(seqinfo_local->nspname,
			   get_namespace_name(RelationGetNamespace(*sequence_rel))) ||
		strcmp(seqinfo_local->seqname, RelationGetRelationName(*sequence_rel)))
		result = COPYSEQ_MISMATCH;

	ReleaseSysCache(tup);
	return result;
}

/*
 * Apply remote sequence state to local sequence and mark it as
 * synchronized (READY).
 */
static CopySeqResult
copy_sequence(LogicalRepSequenceInfo *seqinfo, Oid seqowner)
{
	UserContext ucxt;
	AclResult	aclresult;
	bool		run_as_owner = MySubscription->runasowner;
	Oid			seqoid = seqinfo->localrelid;

	/*
	 * If the user did not opt to run as the owner of the subscription
	 * ('run_as_owner'), then copy the sequence as the owner of the sequence.
	 */
	if (!run_as_owner)
		SwitchToUntrustedUser(seqowner, &ucxt);

	aclresult = pg_class_aclcheck(seqoid, GetUserId(), ACL_UPDATE);

	if (aclresult != ACLCHECK_OK)
	{
		if (!run_as_owner)
			RestoreUserContext(&ucxt);

		return COPYSEQ_INSUFFICIENT_PERM;
	}

	/*
	 * The log counter (log_cnt) tracks how many sequence values are still
	 * unused locally. It is only relevant to the local node and managed
	 * internally by nextval() when allocating new ranges. Since log_cnt does
	 * not affect the visible sequence state (like last_value or is_called)
	 * and is only used for local caching, it need not be copied to the
	 * subscriber during synchronization.
	 */
	SetSequence(seqoid, seqinfo->last_value, seqinfo->is_called);

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	/*
	 * Record the remote sequence's LSN in pg_subscription_rel and mark the
	 * sequence as READY.
	 */
	UpdateSubscriptionRelState(MySubscription->oid, seqoid, SUBREL_STATE_READY,
							   seqinfo->page_lsn, false);

	return COPYSEQ_SUCCESS;
}

/*
 * Copy existing data of sequences from the publisher.
 */
static void
copy_sequences(WalReceiverConn *conn)
{
	int			cur_batch_base_index = 0;
	int			n_seqinfos = list_length(seqinfos);
	List	   *mismatched_seqs_idx = NIL;
	List	   *missing_seqs_idx = NIL;
	List	   *insuffperm_seqs_idx = NIL;
	StringInfo	seqstr = makeStringInfo();
	StringInfo	cmd = makeStringInfo();
	MemoryContext oldctx;

#define MAX_SEQUENCES_SYNC_PER_BATCH 100

	elog(DEBUG1,
		 "logical replication sequence synchronization for subscription \"%s\" - total unsynchronized: %d",
		 MySubscription->name, n_seqinfos);

	while (cur_batch_base_index < n_seqinfos)
	{
		Oid			seqRow[REMOTE_SEQ_COL_COUNT] = {INT8OID, INT8OID,
		BOOLOID, LSNOID, OIDOID, INT8OID, INT8OID, INT8OID, INT8OID, BOOLOID};
		int			batch_size = 0;
		int			batch_succeeded_count = 0;
		int			batch_mismatched_count = 0;
		int			batch_skipped_count = 0;
		int			batch_insuffperm_count = 0;
		int			batch_missing_count;
		Relation	sequence_rel = NULL;

		WalRcvExecResult *res;
		TupleTableSlot *slot;

		StartTransactionCommand();

		for (int idx = cur_batch_base_index; idx < n_seqinfos; idx++)
		{
			char	   *nspname_literal;
			char	   *seqname_literal;

			LogicalRepSequenceInfo *seqinfo =
				(LogicalRepSequenceInfo *) list_nth(seqinfos, idx);

			if (seqstr->len > 0)
				appendStringInfoString(seqstr, ", ");

			nspname_literal = quote_literal_cstr(seqinfo->nspname);
			seqname_literal = quote_literal_cstr(seqinfo->seqname);

			appendStringInfo(seqstr, "(%s, %s, %d)",
							 nspname_literal, seqname_literal, idx);

			if (++batch_size == MAX_SEQUENCES_SYNC_PER_BATCH)
				break;
		}

		/*
		 * We deliberately avoid acquiring a local lock on the sequence before
		 * querying the publisher to prevent potential distributed deadlocks
		 * in bi-directional replication setups.
		 *
		 * Example scenario:
		 *
		 * - On each node, a background worker acquires a lock on a sequence
		 * as part of a sync operation.
		 *
		 * - Concurrently, a user transaction attempts to alter the same
		 * sequence, waiting on the background worker's lock.
		 *
		 * - Meanwhile, a query from the other node tries to access metadata
		 * that depends on the completion of the alter operation.
		 *
		 * - This creates a circular wait across nodes:
		 *
		 * Node-1: Query -> waits on Alter -> waits on Sync Worker
		 *
		 * Node-2: Query -> waits on Alter -> waits on Sync Worker
		 *
		 * Since each node only sees part of the wait graph, the deadlock may
		 * go undetected, leading to indefinite blocking.
		 *
		 * Note: Each entry in VALUES includes an index 'seqidx' that
		 * represents the sequence's position in the local 'seqinfos' list.
		 * This index is propagated to the query results and later used to
		 * directly map the fetched publisher sequence rows back to their
		 * corresponding local entries without relying on result order or name
		 * matching.
		 */
		appendStringInfo(cmd,
						 "SELECT s.seqidx, ps.*, seq.seqtypid,\n"
						 "       seq.seqstart, seq.seqincrement, seq.seqmin,\n"
						 "       seq.seqmax, seq.seqcycle\n"
						 "FROM ( VALUES %s ) AS s (schname, seqname, seqidx)\n"
						 "JOIN pg_namespace n ON n.nspname = s.schname\n"
						 "JOIN pg_class c ON c.relnamespace = n.oid AND c.relname = s.seqname\n"
						 "JOIN pg_sequence seq ON seq.seqrelid = c.oid\n"
						 "JOIN LATERAL pg_get_sequence_data(seq.seqrelid) AS ps ON true\n",
						 seqstr->data);

		res = walrcv_exec(conn, cmd->data, lengthof(seqRow), seqRow);
		if (res->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not fetch sequence information from the publisher: %s",
						   res->err));

		slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
		while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		{
			CopySeqResult sync_status;
			LogicalRepSequenceInfo *seqinfo;
			int			seqidx;

			CHECK_FOR_INTERRUPTS();

			if (ConfigReloadPending)
			{
				ConfigReloadPending = false;
				ProcessConfigFile(PGC_SIGHUP);
			}

			sync_status = get_and_validate_seq_info(slot, &sequence_rel,
													&seqinfo, &seqidx);
			if (sync_status == COPYSEQ_SUCCESS)
				sync_status = copy_sequence(seqinfo,
											sequence_rel->rd_rel->relowner);

			switch (sync_status)
			{
				case COPYSEQ_SUCCESS:
					elog(DEBUG1,
						 "logical replication synchronization for subscription \"%s\", sequence \"%s.%s\" has finished",
						 MySubscription->name, seqinfo->nspname,
						 seqinfo->seqname);
					batch_succeeded_count++;
					break;
				case COPYSEQ_MISMATCH:

					/*
					 * Remember mismatched sequences in a long-lived memory
					 * context since these will be used after the transaction
					 * is committed.
					 */
					oldctx = MemoryContextSwitchTo(ApplyContext);
					mismatched_seqs_idx = lappend_int(mismatched_seqs_idx,
													  seqidx);
					MemoryContextSwitchTo(oldctx);
					batch_mismatched_count++;
					break;
				case COPYSEQ_INSUFFICIENT_PERM:

					/*
					 * Remember sequences with insufficient privileges in a
					 * long-lived memory context since these will be used
					 * after the transaction is committed.
					 */
					oldctx = MemoryContextSwitchTo(ApplyContext);
					insuffperm_seqs_idx = lappend_int(insuffperm_seqs_idx,
													  seqidx);
					MemoryContextSwitchTo(oldctx);
					batch_insuffperm_count++;
					break;
				case COPYSEQ_SKIPPED:

					/*
					 * Concurrent removal of a sequence on the subscriber is
					 * treated as success, since the only viable action is to
					 * skip the corresponding sequence data. Missing sequences
					 * on the publisher are treated as ERROR.
					 */
					if (seqinfo->found_on_pub)
					{
						ereport(LOG,
								errmsg("skip synchronization of sequence \"%s.%s\" because it has been dropped concurrently",
									   seqinfo->nspname,
									   seqinfo->seqname));
						batch_skipped_count++;
					}
					break;
			}

			if (sequence_rel)
				table_close(sequence_rel, NoLock);
		}

		ExecDropSingleTupleTableSlot(slot);
		walrcv_clear_result(res);
		resetStringInfo(seqstr);
		resetStringInfo(cmd);

		batch_missing_count = batch_size - (batch_succeeded_count +
											batch_mismatched_count +
											batch_insuffperm_count +
											batch_skipped_count);

		elog(DEBUG1,
			 "logical replication sequence synchronization for subscription \"%s\" - batch #%d = %d attempted, %d succeeded, %d mismatched, %d insufficient permission, %d missing from publisher, %d skipped",
			 MySubscription->name,
			 (cur_batch_base_index / MAX_SEQUENCES_SYNC_PER_BATCH) + 1,
			 batch_size, batch_succeeded_count, batch_mismatched_count,
			 batch_insuffperm_count, batch_missing_count, batch_skipped_count);

		/* Commit this batch, and prepare for next batch */
		CommitTransactionCommand();

		if (batch_missing_count)
		{
			for (int idx = cur_batch_base_index; idx < cur_batch_base_index + batch_size; idx++)
			{
				LogicalRepSequenceInfo *seqinfo =
					(LogicalRepSequenceInfo *) list_nth(seqinfos, idx);

				/* If the sequence was not found on publisher, record it */
				if (!seqinfo->found_on_pub)
					missing_seqs_idx = lappend_int(missing_seqs_idx, idx);
			}
		}

		/*
		 * cur_batch_base_index is not incremented sequentially because some
		 * sequences may be missing, and the number of fetched rows may not
		 * match the batch size.
		 */
		cur_batch_base_index += batch_size;
	}

	/* Report mismatches, permission issues, or missing sequences */
	report_sequence_errors(mismatched_seqs_idx, insuffperm_seqs_idx,
						   missing_seqs_idx);
}

/*
 * Identifies sequences that require synchronization and initiates the
 * synchronization process.
 */
static void
LogicalRepSyncSequences(void)
{
	char	   *err;
	bool		must_use_password;
	Relation	rel;
	HeapTuple	tup;
	ScanKeyData skey[2];
	SysScanDesc scan;
	Oid			subid = MyLogicalRepWorker->subid;
	StringInfoData app_name;

	StartTransactionCommand();

	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_subscription_rel_srsubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(subid));

	ScanKeyInit(&skey[1],
				Anum_pg_subscription_rel_srsubstate,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(SUBREL_STATE_INIT));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 2, skey);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_subscription_rel subrel;
		LogicalRepSequenceInfo *seq;
		Relation	sequence_rel;
		MemoryContext oldctx;

		CHECK_FOR_INTERRUPTS();

		subrel = (Form_pg_subscription_rel) GETSTRUCT(tup);

		sequence_rel = try_table_open(subrel->srrelid, RowExclusiveLock);

		/* Skip if sequence was dropped concurrently */
		if (!sequence_rel)
			continue;

		/* Skip if the relation is not a sequence */
		if (sequence_rel->rd_rel->relkind != RELKIND_SEQUENCE)
		{
			table_close(sequence_rel, NoLock);
			continue;
		}

		/*
		 * Worker needs to process sequences across transaction boundary, so
		 * allocate them under long-lived context.
		 */
		oldctx = MemoryContextSwitchTo(ApplyContext);

		seq = palloc0_object(LogicalRepSequenceInfo);
		seq->localrelid = subrel->srrelid;
		seq->nspname = get_namespace_name(RelationGetNamespace(sequence_rel));
		seq->seqname = pstrdup(RelationGetRelationName(sequence_rel));
		seqinfos = lappend(seqinfos, seq);

		MemoryContextSwitchTo(oldctx);

		table_close(sequence_rel, NoLock);
	}

	/* Cleanup */
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	/*
	 * Exit early if no catalog entries found, likely due to concurrent drops.
	 */
	if (!seqinfos)
		return;

	/* Is the use of a password mandatory? */
	must_use_password = MySubscription->passwordrequired &&
		!MySubscription->ownersuperuser;

	initStringInfo(&app_name);
	appendStringInfo(&app_name, "pg_%u_sequence_sync_" UINT64_FORMAT,
					 MySubscription->oid, GetSystemIdentifier());

	/*
	 * Establish the connection to the publisher for sequence synchronization.
	 */
	LogRepWorkerWalRcvConn =
		walrcv_connect(MySubscription->conninfo, true, true,
					   must_use_password,
					   app_name.data, &err);
	if (LogRepWorkerWalRcvConn == NULL)
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("sequencesync worker for subscription \"%s\" could not connect to the publisher: %s",
					   MySubscription->name, err));

	pfree(app_name.data);

	copy_sequences(LogRepWorkerWalRcvConn);
}

/*
 * Execute the initial sync with error handling. Disable the subscription,
 * if required.
 *
 * Note that we don't handle FATAL errors which are probably because of system
 * resource error and are not repeatable.
 */
static void
start_sequence_sync(void)
{
	Assert(am_sequencesync_worker());

	PG_TRY();
	{
		/* Call initial sync. */
		LogicalRepSyncSequences();
	}
	PG_CATCH();
	{
		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * Report the worker failed during sequence synchronization. Abort
			 * the current transaction so that the stats message is sent in an
			 * idle state.
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid,
											 WORKERTYPE_SEQUENCESYNC);

			PG_RE_THROW();
		}
	}
	PG_END_TRY();
}

/* Logical Replication sequencesync worker entry point */
void
SequenceSyncWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);

	SetupApplyOrSyncWorker(worker_slot);

	start_sequence_sync();

	FinishSyncWorker();
}
