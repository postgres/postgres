/*-------------------------------------------------------------------------
 * worker.c
 *	   PostgreSQL logical replication worker (apply)
 *
 * Copyright (c) 2016-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/worker.c
 *
 * NOTES
 *	  This file contains the worker which applies logical changes as they come
 *	  from remote logical replication stream.
 *
 *	  The main worker (apply) is started by logical replication worker
 *	  launcher for every enabled subscription in a database. It uses
 *	  walsender protocol to communicate with publisher.
 *
 *	  This module includes server facing code and shares libpqwalreceiver
 *	  module with walreceiver for providing the libpq specific functionality.
 *
 *
 * STREAMED TRANSACTIONS
 * ---------------------
 * Streamed transactions (large transactions exceeding a memory limit on the
 * upstream) are not applied immediately, but instead, the data is written
 * to temporary files and then applied at once when the final commit arrives.
 *
 * Unlike the regular (non-streamed) case, handling streamed transactions has
 * to handle aborts of both the toplevel transaction and subtransactions. This
 * is achieved by tracking offsets for subtransactions, which is then used
 * to truncate the file with serialized changes.
 *
 * The files are placed in tmp file directory by default, and the filenames
 * include both the XID of the toplevel transaction and OID of the
 * subscription. This is necessary so that different workers processing a
 * remote transaction with the same XID doesn't interfere.
 *
 * We use BufFiles instead of using normal temporary files because (a) the
 * BufFile infrastructure supports temporary files that exceed the OS file size
 * limit, (b) provides a way for automatic clean up on the error and (c) provides
 * a way to survive these files across local transactions and allow to open and
 * close at stream start and close. We decided to use SharedFileSet
 * infrastructure as without that it deletes the files on the closure of the
 * file and if we decide to keep stream files open across the start/stop stream
 * then it will consume a lot of memory (more than 8K for each BufFile and
 * there could be multiple such BufFiles as the subscriber could receive
 * multiple start/stop streams for different transactions before getting the
 * commit). Moreover, if we don't use SharedFileSet then we also need to invent
 * a new way to pass filenames to BufFile APIs so that we are allowed to open
 * the file we desired across multiple stream-open calls for the same
 * transaction.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeModifyTable.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/walwriter.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/logicalproto.h"
#include "replication/logicalrelation.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "rewrite/rewriteHandler.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/dynahash.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

#define NAPTIME_PER_CYCLE 1000	/* max sleep time between cycles (1s) */

typedef struct FlushPosition
{
	dlist_node	node;
	XLogRecPtr	local_end;
	XLogRecPtr	remote_end;
} FlushPosition;

static dlist_head lsn_mapping = DLIST_STATIC_INIT(lsn_mapping);

typedef struct SlotErrCallbackArg
{
	LogicalRepRelMapEntry *rel;
	int			local_attnum;
	int			remote_attnum;
} SlotErrCallbackArg;

/*
 * Stream xid hash entry. Whenever we see a new xid we create this entry in the
 * xidhash and along with it create the streaming file and store the fileset handle.
 * The subxact file is created iff there is any subxact info under this xid. This
 * entry is used on the subsequent streams for the xid to get the corresponding
 * fileset handles, so storing them in hash makes the search faster.
 */
typedef struct StreamXidHash
{
	TransactionId xid;			/* xid is the hash key and must be first */
	SharedFileSet *stream_fileset;	/* shared file set for stream data */
	SharedFileSet *subxact_fileset; /* shared file set for subxact info */
} StreamXidHash;

static MemoryContext ApplyMessageContext = NULL;
MemoryContext ApplyContext = NULL;

/* per stream context for streaming transactions */
static MemoryContext LogicalStreamingContext = NULL;

WalReceiverConn *wrconn = NULL;

Subscription *MySubscription = NULL;
bool		MySubscriptionValid = false;

bool		in_remote_transaction = false;
static XLogRecPtr remote_final_lsn = InvalidXLogRecPtr;

/* fields valid only when processing streamed transaction */
bool		in_streamed_transaction = false;

static TransactionId stream_xid = InvalidTransactionId;

/*
 * Hash table for storing the streaming xid information along with shared file
 * set for streaming and subxact files.
 */
static HTAB *xidhash = NULL;

/* BufFile handle of the current streaming file */
static BufFile *stream_fd = NULL;

typedef struct SubXactInfo
{
	TransactionId xid;			/* XID of the subxact */
	int			fileno;			/* file number in the buffile */
	off_t		offset;			/* offset in the file */
} SubXactInfo;

/* Sub-transaction data for the current streaming transaction */
typedef struct ApplySubXactData
{
	uint32		nsubxacts;		/* number of sub-transactions */
	uint32		nsubxacts_max;	/* current capacity of subxacts */
	TransactionId subxact_last; /* xid of the last sub-transaction */
	SubXactInfo *subxacts;		/* sub-xact offset in changes file */
} ApplySubXactData;

static ApplySubXactData subxact_data = {0, 0, InvalidTransactionId, NULL};

static inline void subxact_filename(char *path, Oid subid, TransactionId xid);
static inline void changes_filename(char *path, Oid subid, TransactionId xid);

/*
 * Information about subtransactions of a given toplevel transaction.
 */
static void subxact_info_write(Oid subid, TransactionId xid);
static void subxact_info_read(Oid subid, TransactionId xid);
static void subxact_info_add(TransactionId xid);
static inline void cleanup_subxact_info(void);

/*
 * Serialize and deserialize changes for a toplevel transaction.
 */
static void stream_cleanup_files(Oid subid, TransactionId xid);
static void stream_open_file(Oid subid, TransactionId xid, bool first);
static void stream_write_change(char action, StringInfo s);
static void stream_close_file(void);

static void send_feedback(XLogRecPtr recvpos, bool force, bool requestReply);

static void store_flush_position(XLogRecPtr remote_lsn);

static void maybe_reread_subscription(void);

/* prototype needed because of stream_commit */
static void apply_dispatch(StringInfo s);

static void apply_handle_commit_internal(StringInfo s,
										 LogicalRepCommitData *commit_data);
static void apply_handle_insert_internal(ResultRelInfo *relinfo,
										 EState *estate, TupleTableSlot *remoteslot);
static void apply_handle_update_internal(ResultRelInfo *relinfo,
										 EState *estate, TupleTableSlot *remoteslot,
										 LogicalRepTupleData *newtup,
										 LogicalRepRelMapEntry *relmapentry);
static void apply_handle_delete_internal(ResultRelInfo *relinfo, EState *estate,
										 TupleTableSlot *remoteslot,
										 LogicalRepRelation *remoterel);
static bool FindReplTupleInLocalRel(EState *estate, Relation localrel,
									LogicalRepRelation *remoterel,
									TupleTableSlot *remoteslot,
									TupleTableSlot **localslot);
static void apply_handle_tuple_routing(ResultRelInfo *relinfo,
									   EState *estate,
									   TupleTableSlot *remoteslot,
									   LogicalRepTupleData *newtup,
									   LogicalRepRelMapEntry *relmapentry,
									   CmdType operation);

/*
 * Should this worker apply changes for given relation.
 *
 * This is mainly needed for initial relation data sync as that runs in
 * separate worker process running in parallel and we need some way to skip
 * changes coming to the main apply worker during the sync of a table.
 *
 * Note we need to do smaller or equals comparison for SYNCDONE state because
 * it might hold position of end of initial slot consistent point WAL
 * record + 1 (ie start of next record) and next record can be COMMIT of
 * transaction we are now processing (which is what we set remote_final_lsn
 * to in apply_handle_begin).
 */
static bool
should_apply_changes_for_rel(LogicalRepRelMapEntry *rel)
{
	if (am_tablesync_worker())
		return MyLogicalRepWorker->relid == rel->localreloid;
	else
		return (rel->state == SUBREL_STATE_READY ||
				(rel->state == SUBREL_STATE_SYNCDONE &&
				 rel->statelsn <= remote_final_lsn));
}

/*
 * Make sure that we started local transaction.
 *
 * Also switches to ApplyMessageContext as necessary.
 */
static bool
ensure_transaction(void)
{
	if (IsTransactionState())
	{
		SetCurrentStatementStartTimestamp();

		if (CurrentMemoryContext != ApplyMessageContext)
			MemoryContextSwitchTo(ApplyMessageContext);

		return false;
	}

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();

	maybe_reread_subscription();

	MemoryContextSwitchTo(ApplyMessageContext);
	return true;
}

/*
 * Handle streamed transactions.
 *
 * If in streaming mode (receiving a block of streamed transaction), we
 * simply redirect it to a file for the proper toplevel transaction.
 *
 * Returns true for streamed transactions, false otherwise (regular mode).
 */
static bool
handle_streamed_transaction(LogicalRepMsgType action, StringInfo s)
{
	TransactionId xid;

	/* not in streaming mode */
	if (!in_streamed_transaction)
		return false;

	Assert(stream_fd != NULL);
	Assert(TransactionIdIsValid(stream_xid));

	/*
	 * We should have received XID of the subxact as the first part of the
	 * message, so extract it.
	 */
	xid = pq_getmsgint(s, 4);

	Assert(TransactionIdIsValid(xid));

	/* Add the new subxact to the array (unless already there). */
	subxact_info_add(xid);

	/* write the change to the current file */
	stream_write_change(action, s);

	return true;
}

/*
 * Executor state preparation for evaluation of constraint expressions,
 * indexes and triggers.
 *
 * This is based on similar code in copy.c
 */
static EState *
create_estate_for_relation(LogicalRepRelMapEntry *rel)
{
	EState	   *estate;
	RangeTblEntry *rte;

	estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel->localrel);
	rte->relkind = rel->localrel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;
	ExecInitRangeTable(estate, list_make1(rte));

	estate->es_output_cid = GetCurrentCommandId(true);

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	return estate;
}

/*
 * Executes default values for columns for which we can't map to remote
 * relation columns.
 *
 * This allows us to support tables which have more columns on the downstream
 * than on the upstream.
 */
static void
slot_fill_defaults(LogicalRepRelMapEntry *rel, EState *estate,
				   TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel->localrel);
	int			num_phys_attrs = desc->natts;
	int			i;
	int			attnum,
				num_defaults = 0;
	int		   *defmap;
	ExprState **defexprs;
	ExprContext *econtext;

	econtext = GetPerTupleExprContext(estate);

	/* We got all the data via replication, no need to evaluate anything. */
	if (num_phys_attrs == rel->remoterel.natts)
		return;

	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	Assert(rel->attrmap->maplen == num_phys_attrs);
	for (attnum = 0; attnum < num_phys_attrs; attnum++)
	{
		Expr	   *defexpr;

		if (TupleDescAttr(desc, attnum)->attisdropped || TupleDescAttr(desc, attnum)->attgenerated)
			continue;

		if (rel->attrmap->attnums[attnum] >= 0)
			continue;

		defexpr = (Expr *) build_column_default(rel->localrel, attnum + 1);

		if (defexpr != NULL)
		{
			/* Run the expression through planner */
			defexpr = expression_planner(defexpr);

			/* Initialize executable expression in copycontext */
			defexprs[num_defaults] = ExecInitExpr(defexpr, NULL);
			defmap[num_defaults] = attnum;
			num_defaults++;
		}

	}

	for (i = 0; i < num_defaults; i++)
		slot->tts_values[defmap[i]] =
			ExecEvalExpr(defexprs[i], econtext, &slot->tts_isnull[defmap[i]]);
}

/*
 * Error callback to give more context info about type conversion failure.
 */
static void
slot_store_error_callback(void *arg)
{
	SlotErrCallbackArg *errarg = (SlotErrCallbackArg *) arg;
	LogicalRepRelMapEntry *rel;
	char	   *remotetypname;
	Oid			remotetypoid,
				localtypoid;

	/* Nothing to do if remote attribute number is not set */
	if (errarg->remote_attnum < 0)
		return;

	rel = errarg->rel;
	remotetypoid = rel->remoterel.atttyps[errarg->remote_attnum];

	/* Fetch remote type name from the LogicalRepTypMap cache */
	remotetypname = logicalrep_typmap_gettypname(remotetypoid);

	/* Fetch local type OID from the local sys cache */
	localtypoid = get_atttype(rel->localreloid, errarg->local_attnum + 1);

	errcontext("processing remote data for replication target relation \"%s.%s\" column \"%s\", "
			   "remote type %s, local type %s",
			   rel->remoterel.nspname, rel->remoterel.relname,
			   rel->remoterel.attnames[errarg->remote_attnum],
			   remotetypname,
			   format_type_be(localtypoid));
}

/*
 * Store tuple data into slot.
 *
 * Incoming data can be either text or binary format.
 */
static void
slot_store_data(TupleTableSlot *slot, LogicalRepRelMapEntry *rel,
				LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;
	SlotErrCallbackArg errarg;
	ErrorContextCallback errcallback;

	ExecClearTuple(slot);

	/* Push callback + info on the error context stack */
	errarg.rel = rel;
	errarg.local_attnum = -1;
	errarg.remote_attnum = -1;
	errcallback.callback = slot_store_error_callback;
	errcallback.arg = (void *) &errarg;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Call the "in" function for each non-dropped, non-null attribute */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			Assert(remoteattnum < tupleData->ncols);

			errarg.local_attnum = i;
			errarg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * In some code paths we may be asked to re-parse the same
				 * tuple data.  Reset the StringInfo's cursor so that works.
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* Trouble if it didn't eat the whole buffer */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/*
				 * NULL value from remote.  (We don't expect to see
				 * LOGICALREP_COLUMN_UNCHANGED here, but if we do, treat it as
				 * NULL.)
				 */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			errarg.local_attnum = -1;
			errarg.remote_attnum = -1;
		}
		else
		{
			/*
			 * We assign NULL to dropped attributes and missing values
			 * (missing values should be later filled using
			 * slot_fill_defaults).
			 */
			slot->tts_values[i] = (Datum) 0;
			slot->tts_isnull[i] = true;
		}
	}

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	ExecStoreVirtualTuple(slot);
}

/*
 * Replace updated columns with data from the LogicalRepTupleData struct.
 * This is somewhat similar to heap_modify_tuple but also calls the type
 * input functions on the user data.
 *
 * "slot" is filled with a copy of the tuple in "srcslot", replacing
 * columns provided in "tupleData" and leaving others as-is.
 *
 * Caution: unreplaced pass-by-ref columns in "slot" will point into the
 * storage for "srcslot".  This is OK for current usage, but someday we may
 * need to materialize "slot" at the end to make it independent of "srcslot".
 */
static void
slot_modify_data(TupleTableSlot *slot, TupleTableSlot *srcslot,
				 LogicalRepRelMapEntry *rel,
				 LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;
	SlotErrCallbackArg errarg;
	ErrorContextCallback errcallback;

	/* We'll fill "slot" with a virtual tuple, so we must start with ... */
	ExecClearTuple(slot);

	/*
	 * Copy all the column data from srcslot, so that we'll have valid values
	 * for unreplaced columns.
	 */
	Assert(natts == srcslot->tts_tupleDescriptor->natts);
	slot_getallattrs(srcslot);
	memcpy(slot->tts_values, srcslot->tts_values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, srcslot->tts_isnull, natts * sizeof(bool));

	/* For error reporting, push callback + info on the error context stack */
	errarg.rel = rel;
	errarg.local_attnum = -1;
	errarg.remote_attnum = -1;
	errcallback.callback = slot_store_error_callback;
	errcallback.arg = (void *) &errarg;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Call the "in" function for each replaced attribute */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (remoteattnum < 0)
			continue;

		Assert(remoteattnum < tupleData->ncols);

		if (tupleData->colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			errarg.local_attnum = i;
			errarg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * In some code paths we may be asked to re-parse the same
				 * tuple data.  Reset the StringInfo's cursor so that works.
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* Trouble if it didn't eat the whole buffer */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/* must be LOGICALREP_COLUMN_NULL */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			errarg.local_attnum = -1;
			errarg.remote_attnum = -1;
		}
	}

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/* And finally, declare that "slot" contains a valid virtual tuple */
	ExecStoreVirtualTuple(slot);
}

/*
 * Handle BEGIN message.
 */
static void
apply_handle_begin(StringInfo s)
{
	LogicalRepBeginData begin_data;

	logicalrep_read_begin(s, &begin_data);

	remote_final_lsn = begin_data.final_lsn;

	in_remote_transaction = true;

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Handle COMMIT message.
 *
 * TODO, support tracking of multiple origins
 */
static void
apply_handle_commit(StringInfo s)
{
	LogicalRepCommitData commit_data;

	logicalrep_read_commit(s, &commit_data);

	Assert(commit_data.commit_lsn == remote_final_lsn);

	apply_handle_commit_internal(s, &commit_data);

	/* Process any tables that are being synchronized in parallel. */
	process_syncing_tables(commit_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
}

/*
 * Handle ORIGIN message.
 *
 * TODO, support tracking of multiple origins
 */
static void
apply_handle_origin(StringInfo s)
{
	/*
	 * ORIGIN message can only come inside streaming transaction or inside
	 * remote transaction and before any actual writes.
	 */
	if (!in_streamed_transaction &&
		(!in_remote_transaction ||
		 (IsTransactionState() && !am_tablesync_worker())))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("ORIGIN message sent out of order")));
}

/*
 * Handle STREAM START message.
 */
static void
apply_handle_stream_start(StringInfo s)
{
	bool		first_segment;
	HASHCTL		hash_ctl;

	Assert(!in_streamed_transaction);

	/*
	 * Start a transaction on stream start, this transaction will be committed
	 * on the stream stop unless it is a tablesync worker in which case it
	 * will be committed after processing all the messages. We need the
	 * transaction for handling the buffile, used for serializing the
	 * streaming data and subxact info.
	 */
	ensure_transaction();

	/* notify handle methods we're processing a remote transaction */
	in_streamed_transaction = true;

	/* extract XID of the top-level transaction */
	stream_xid = logicalrep_read_stream_start(s, &first_segment);

	/*
	 * Initialize the xidhash table if we haven't yet. This will be used for
	 * the entire duration of the apply worker so create it in permanent
	 * context.
	 */
	if (xidhash == NULL)
	{
		hash_ctl.keysize = sizeof(TransactionId);
		hash_ctl.entrysize = sizeof(StreamXidHash);
		hash_ctl.hcxt = ApplyContext;
		xidhash = hash_create("StreamXidHash", 1024, &hash_ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* open the spool file for this transaction */
	stream_open_file(MyLogicalRepWorker->subid, stream_xid, first_segment);

	/* if this is not the first segment, open existing subxact file */
	if (!first_segment)
		subxact_info_read(MyLogicalRepWorker->subid, stream_xid);

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Handle STREAM STOP message.
 */
static void
apply_handle_stream_stop(StringInfo s)
{
	Assert(in_streamed_transaction);

	/*
	 * Close the file with serialized changes, and serialize information about
	 * subxacts for the toplevel transaction.
	 */
	subxact_info_write(MyLogicalRepWorker->subid, stream_xid);
	stream_close_file();

	/* We must be in a valid transaction state */
	Assert(IsTransactionState());

	/* Commit the per-stream transaction */
	CommitTransactionCommand();

	in_streamed_transaction = false;

	/* Reset per-stream context */
	MemoryContextReset(LogicalStreamingContext);

	pgstat_report_activity(STATE_IDLE, NULL);
}

/*
 * Handle STREAM abort message.
 */
static void
apply_handle_stream_abort(StringInfo s)
{
	TransactionId xid;
	TransactionId subxid;

	Assert(!in_streamed_transaction);

	logicalrep_read_stream_abort(s, &xid, &subxid);

	/*
	 * If the two XIDs are the same, it's in fact abort of toplevel xact, so
	 * just delete the files with serialized info.
	 */
	if (xid == subxid)
		stream_cleanup_files(MyLogicalRepWorker->subid, xid);
	else
	{
		/*
		 * OK, so it's a subxact. We need to read the subxact file for the
		 * toplevel transaction, determine the offset tracked for the subxact,
		 * and truncate the file with changes. We also remove the subxacts
		 * with higher offsets (or rather higher XIDs).
		 *
		 * We intentionally scan the array from the tail, because we're likely
		 * aborting a change for the most recent subtransactions.
		 *
		 * We can't use the binary search here as subxact XIDs won't
		 * necessarily arrive in sorted order, consider the case where we have
		 * released the savepoint for multiple subtransactions and then
		 * performed rollback to savepoint for one of the earlier
		 * sub-transaction.
		 */

		int64		i;
		int64		subidx;
		BufFile    *fd;
		bool		found = false;
		char		path[MAXPGPATH];
		StreamXidHash *ent;

		subidx = -1;
		ensure_transaction();
		subxact_info_read(MyLogicalRepWorker->subid, xid);

		for (i = subxact_data.nsubxacts; i > 0; i--)
		{
			if (subxact_data.subxacts[i - 1].xid == subxid)
			{
				subidx = (i - 1);
				found = true;
				break;
			}
		}

		/*
		 * If it's an empty sub-transaction then we will not find the subxid
		 * here so just cleanup the subxact info and return.
		 */
		if (!found)
		{
			/* Cleanup the subxact info */
			cleanup_subxact_info();

			CommitTransactionCommand();
			return;
		}

		Assert((subidx >= 0) && (subidx < subxact_data.nsubxacts));

		ent = (StreamXidHash *) hash_search(xidhash,
											(void *) &xid,
											HASH_FIND,
											&found);
		Assert(found);

		/* open the changes file */
		changes_filename(path, MyLogicalRepWorker->subid, xid);
		fd = BufFileOpenShared(ent->stream_fileset, path, O_RDWR);

		/* OK, truncate the file at the right offset */
		BufFileTruncateShared(fd, subxact_data.subxacts[subidx].fileno,
							  subxact_data.subxacts[subidx].offset);
		BufFileClose(fd);

		/* discard the subxacts added later */
		subxact_data.nsubxacts = subidx;

		/* write the updated subxact list */
		subxact_info_write(MyLogicalRepWorker->subid, xid);

		CommitTransactionCommand();
	}
}

/*
 * Handle STREAM COMMIT message.
 */
static void
apply_handle_stream_commit(StringInfo s)
{
	TransactionId xid;
	StringInfoData s2;
	int			nchanges;
	char		path[MAXPGPATH];
	char	   *buffer = NULL;
	bool		found;
	LogicalRepCommitData commit_data;
	StreamXidHash *ent;
	MemoryContext oldcxt;
	BufFile    *fd;

	Assert(!in_streamed_transaction);

	xid = logicalrep_read_stream_commit(s, &commit_data);

	elog(DEBUG1, "received commit for streamed transaction %u", xid);

	ensure_transaction();

	/*
	 * Allocate file handle and memory required to process all the messages in
	 * TopTransactionContext to avoid them getting reset after each message is
	 * processed.
	 */
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	/* open the spool file for the committed transaction */
	changes_filename(path, MyLogicalRepWorker->subid, xid);
	elog(DEBUG1, "replaying changes from file \"%s\"", path);
	ent = (StreamXidHash *) hash_search(xidhash,
										(void *) &xid,
										HASH_FIND,
										&found);
	Assert(found);
	fd = BufFileOpenShared(ent->stream_fileset, path, O_RDONLY);

	buffer = palloc(BLCKSZ);
	initStringInfo(&s2);

	MemoryContextSwitchTo(oldcxt);

	remote_final_lsn = commit_data.commit_lsn;

	/*
	 * Make sure the handle apply_dispatch methods are aware we're in a remote
	 * transaction.
	 */
	in_remote_transaction = true;
	pgstat_report_activity(STATE_RUNNING, NULL);

	/*
	 * Read the entries one by one and pass them through the same logic as in
	 * apply_dispatch.
	 */
	nchanges = 0;
	while (true)
	{
		int			nbytes;
		int			len;

		CHECK_FOR_INTERRUPTS();

		/* read length of the on-disk record */
		nbytes = BufFileRead(fd, &len, sizeof(len));

		/* have we reached end of the file? */
		if (nbytes == 0)
			break;

		/* do we have a correct length? */
		if (nbytes != sizeof(len))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from streaming transaction's changes file \"%s\": %m",
							path)));

		Assert(len > 0);

		/* make sure we have sufficiently large buffer */
		buffer = repalloc(buffer, len);

		/* and finally read the data into the buffer */
		if (BufFileRead(fd, buffer, len) != len)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from streaming transaction's changes file \"%s\": %m",
							path)));

		/* copy the buffer to the stringinfo and call apply_dispatch */
		resetStringInfo(&s2);
		appendBinaryStringInfo(&s2, buffer, len);

		/* Ensure we are reading the data into our memory context. */
		oldcxt = MemoryContextSwitchTo(ApplyMessageContext);

		apply_dispatch(&s2);

		MemoryContextReset(ApplyMessageContext);

		MemoryContextSwitchTo(oldcxt);

		nchanges++;

		if (nchanges % 1000 == 0)
			elog(DEBUG1, "replayed %d changes from file '%s'",
				 nchanges, path);
	}

	BufFileClose(fd);

	pfree(buffer);
	pfree(s2.data);

	elog(DEBUG1, "replayed %d (all) changes from file \"%s\"",
		 nchanges, path);

	apply_handle_commit_internal(s, &commit_data);

	/* unlink the files with serialized changes and subxact info */
	stream_cleanup_files(MyLogicalRepWorker->subid, xid);

	/* Process any tables that are being synchronized in parallel. */
	process_syncing_tables(commit_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
}

/*
 * Helper function for apply_handle_commit and apply_handle_stream_commit.
 */
static void
apply_handle_commit_internal(StringInfo s, LogicalRepCommitData *commit_data)
{
	if (IsTransactionState())
	{
		/*
		 * Update origin state so we can restart streaming from correct
		 * position in case of crash.
		 */
		replorigin_session_origin_lsn = commit_data->end_lsn;
		replorigin_session_origin_timestamp = commit_data->committime;

		CommitTransactionCommand();
		pgstat_report_stat(false);

		store_flush_position(commit_data->end_lsn);
	}
	else
	{
		/* Process any invalidation messages that might have accumulated. */
		AcceptInvalidationMessages();
		maybe_reread_subscription();
	}

	in_remote_transaction = false;
}

/*
 * Handle RELATION message.
 *
 * Note we don't do validation against local schema here. The validation
 * against local schema is postponed until first change for given relation
 * comes as we only care about it when applying changes for it anyway and we
 * do less locking this way.
 */
static void
apply_handle_relation(StringInfo s)
{
	LogicalRepRelation *rel;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_RELATION, s))
		return;

	rel = logicalrep_read_rel(s);
	logicalrep_relmap_update(rel);
}

/*
 * Handle TYPE message.
 *
 * Note we don't do local mapping here, that's done when the type is
 * actually used.
 */
static void
apply_handle_type(StringInfo s)
{
	LogicalRepTyp typ;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_TYPE, s))
		return;

	logicalrep_read_typ(s, &typ);
	logicalrep_typmap_update(&typ);
}

/*
 * Get replica identity index or if it is not defined a primary key.
 *
 * If neither is defined, returns InvalidOid
 */
static Oid
GetRelationIdentityOrPK(Relation rel)
{
	Oid			idxoid;

	idxoid = RelationGetReplicaIndex(rel);

	if (!OidIsValid(idxoid))
		idxoid = RelationGetPrimaryKeyIndex(rel);

	return idxoid;
}

/*
 * Handle INSERT message.
 */

static void
apply_handle_insert(StringInfo s)
{
	ResultRelInfo *resultRelInfo;
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData newtup;
	LogicalRepRelId relid;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_INSERT, s))
		return;

	ensure_transaction();

	relid = logicalrep_read_insert(s, &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		return;
	}

	/* Initialize the executor state. */
	estate = create_estate_for_relation(rel);
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);
	resultRelInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(resultRelInfo, rel->localrel, 1, NULL, 0);

	/* Input functions may need an active snapshot, so get one */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Process and store remote tuple in the slot */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &newtup);
	slot_fill_defaults(rel, estate, remoteslot);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, insert the tuple into a partition. */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(resultRelInfo, estate,
								   remoteslot, NULL, rel, CMD_INSERT);
	else
		apply_handle_insert_internal(resultRelInfo, estate,
									 remoteslot);

	PopActiveSnapshot();

	/* Handle queued AFTER triggers. */
	AfterTriggerEndQuery(estate);

	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);

	logicalrep_rel_close(rel, NoLock);

	CommandCounterIncrement();
}

/* Workhorse for apply_handle_insert() */
static void
apply_handle_insert_internal(ResultRelInfo *relinfo,
							 EState *estate, TupleTableSlot *remoteslot)
{
	ExecOpenIndices(relinfo, false);

	/* Do the insert. */
	ExecSimpleRelationInsert(relinfo, estate, remoteslot);

	/* Cleanup. */
	ExecCloseIndices(relinfo);
}

/*
 * Check if the logical replication relation is updatable and throw
 * appropriate error if it isn't.
 */
static void
check_relation_updatable(LogicalRepRelMapEntry *rel)
{
	/* Updatable, no error. */
	if (rel->updatable)
		return;

	/*
	 * We are in error mode so it's fine this is somewhat slow. It's better to
	 * give user correct error.
	 */
	if (OidIsValid(GetRelationIdentityOrPK(rel->localrel)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publisher did not send replica identity column "
						"expected by the logical replication target relation \"%s.%s\"",
						rel->remoterel.nspname, rel->remoterel.relname)));
	}

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("logical replication target relation \"%s.%s\" has "
					"neither REPLICA IDENTITY index nor PRIMARY "
					"KEY and published relation does not have "
					"REPLICA IDENTITY FULL",
					rel->remoterel.nspname, rel->remoterel.relname)));
}

/*
 * Handle UPDATE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_update(StringInfo s)
{
	ResultRelInfo *resultRelInfo;
	LogicalRepRelMapEntry *rel;
	LogicalRepRelId relid;
	EState	   *estate;
	LogicalRepTupleData oldtup;
	LogicalRepTupleData newtup;
	bool		has_oldtup;
	TupleTableSlot *remoteslot;
	RangeTblEntry *target_rte;
	MemoryContext oldctx;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_UPDATE, s))
		return;

	ensure_transaction();

	relid = logicalrep_read_update(s, &has_oldtup, &oldtup,
								   &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		return;
	}

	/* Check if we can do the update. */
	check_relation_updatable(rel);

	/* Initialize the executor state. */
	estate = create_estate_for_relation(rel);
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);
	resultRelInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(resultRelInfo, rel->localrel, 1, NULL, 0);

	/*
	 * Populate updatedCols so that per-column triggers can fire, and so
	 * executor can correctly pass down indexUnchanged hint.  This could
	 * include more columns than were actually changed on the publisher
	 * because the logical replication protocol doesn't contain that
	 * information.  But it would for example exclude columns that only exist
	 * on the subscriber, since we are not touching those.
	 */
	target_rte = list_nth(estate->es_range_table, 0);
	for (int i = 0; i < remoteslot->tts_tupleDescriptor->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(remoteslot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			Assert(remoteattnum < newtup.ncols);
			if (newtup.colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
				target_rte->updatedCols =
					bms_add_member(target_rte->updatedCols,
								   i + 1 - FirstLowInvalidHeapAttributeNumber);
		}
	}

	/* Also populate extraUpdatedCols, in case we have generated columns */
	fill_extraUpdatedCols(target_rte, rel->localrel);

	PushActiveSnapshot(GetTransactionSnapshot());

	/* Build the search tuple. */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel,
					has_oldtup ? &oldtup : &newtup);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, apply update to correct partition. */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(resultRelInfo, estate,
								   remoteslot, &newtup, rel, CMD_UPDATE);
	else
		apply_handle_update_internal(resultRelInfo, estate,
									 remoteslot, &newtup, rel);

	PopActiveSnapshot();

	/* Handle queued AFTER triggers. */
	AfterTriggerEndQuery(estate);

	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);

	logicalrep_rel_close(rel, NoLock);

	CommandCounterIncrement();
}

/* Workhorse for apply_handle_update() */
static void
apply_handle_update_internal(ResultRelInfo *relinfo,
							 EState *estate, TupleTableSlot *remoteslot,
							 LogicalRepTupleData *newtup,
							 LogicalRepRelMapEntry *relmapentry)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	bool		found;
	MemoryContext oldctx;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(estate, localrel,
									&relmapentry->remoterel,
									remoteslot, &localslot);
	ExecClearTuple(remoteslot);

	/*
	 * Tuple found.
	 *
	 * Note this will fail if there are other conflicting unique indexes.
	 */
	if (found)
	{
		/* Process and store remote tuple in the slot */
		oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		slot_modify_data(remoteslot, localslot, relmapentry, newtup);
		MemoryContextSwitchTo(oldctx);

		EvalPlanQualSetSlot(&epqstate, remoteslot);

		/* Do the actual update. */
		ExecSimpleRelationUpdate(relinfo, estate, &epqstate, localslot,
								 remoteslot);
	}
	else
	{
		/*
		 * The tuple to be updated could not be found.
		 *
		 * TODO what to do here, change the log level to LOG perhaps?
		 */
		elog(DEBUG1,
			 "logical replication did not find row for update "
			 "in replication target relation \"%s\"",
			 RelationGetRelationName(localrel));
	}

	/* Cleanup. */
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * Handle DELETE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_delete(StringInfo s)
{
	ResultRelInfo *resultRelInfo;
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData oldtup;
	LogicalRepRelId relid;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_DELETE, s))
		return;

	ensure_transaction();

	relid = logicalrep_read_delete(s, &oldtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		return;
	}

	/* Check if we can do the delete. */
	check_relation_updatable(rel);

	/* Initialize the executor state. */
	estate = create_estate_for_relation(rel);
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);
	resultRelInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(resultRelInfo, rel->localrel, 1, NULL, 0);

	PushActiveSnapshot(GetTransactionSnapshot());

	/* Build the search tuple. */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &oldtup);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, apply delete to correct partition. */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(resultRelInfo, estate,
								   remoteslot, NULL, rel, CMD_DELETE);
	else
		apply_handle_delete_internal(resultRelInfo, estate,
									 remoteslot, &rel->remoterel);

	PopActiveSnapshot();

	/* Handle queued AFTER triggers. */
	AfterTriggerEndQuery(estate);

	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);

	logicalrep_rel_close(rel, NoLock);

	CommandCounterIncrement();
}

/* Workhorse for apply_handle_delete() */
static void
apply_handle_delete_internal(ResultRelInfo *relinfo, EState *estate,
							 TupleTableSlot *remoteslot,
							 LogicalRepRelation *remoterel)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	bool		found;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(estate, localrel, remoterel,
									remoteslot, &localslot);

	/* If found delete it. */
	if (found)
	{
		EvalPlanQualSetSlot(&epqstate, localslot);

		/* Do the actual delete. */
		ExecSimpleRelationDelete(relinfo, estate, &epqstate, localslot);
	}
	else
	{
		/* The tuple to be deleted could not be found. */
		elog(DEBUG1,
			 "logical replication did not find row for delete "
			 "in replication target relation \"%s\"",
			 RelationGetRelationName(localrel));
	}

	/* Cleanup. */
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * Try to find a tuple received from the publication side (in 'remoteslot') in
 * the corresponding local relation using either replica identity index,
 * primary key or if needed, sequential scan.
 *
 * Local tuple, if found, is returned in '*localslot'.
 */
static bool
FindReplTupleInLocalRel(EState *estate, Relation localrel,
						LogicalRepRelation *remoterel,
						TupleTableSlot *remoteslot,
						TupleTableSlot **localslot)
{
	Oid			idxoid;
	bool		found;

	*localslot = table_slot_create(localrel, &estate->es_tupleTable);

	idxoid = GetRelationIdentityOrPK(localrel);
	Assert(OidIsValid(idxoid) ||
		   (remoterel->replident == REPLICA_IDENTITY_FULL));

	if (OidIsValid(idxoid))
		found = RelationFindReplTupleByIndex(localrel, idxoid,
											 LockTupleExclusive,
											 remoteslot, *localslot);
	else
		found = RelationFindReplTupleSeq(localrel, LockTupleExclusive,
										 remoteslot, *localslot);

	return found;
}

/*
 * This handles insert, update, delete on a partitioned table.
 */
static void
apply_handle_tuple_routing(ResultRelInfo *relinfo,
						   EState *estate,
						   TupleTableSlot *remoteslot,
						   LogicalRepTupleData *newtup,
						   LogicalRepRelMapEntry *relmapentry,
						   CmdType operation)
{
	Relation	parentrel = relinfo->ri_RelationDesc;
	ModifyTableState *mtstate = NULL;
	PartitionTupleRouting *proute = NULL;
	ResultRelInfo *partrelinfo;
	Relation	partrel;
	TupleTableSlot *remoteslot_part;
	TupleConversionMap *map;
	MemoryContext oldctx;

	/* ModifyTableState is needed for ExecFindPartition(). */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = operation;
	mtstate->resultRelInfo = relinfo;
	proute = ExecSetupPartitionTupleRouting(estate, mtstate, parentrel);

	/*
	 * Find the partition to which the "search tuple" belongs.
	 */
	Assert(remoteslot != NULL);
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	partrelinfo = ExecFindPartition(mtstate, relinfo, proute,
									remoteslot, estate);
	Assert(partrelinfo != NULL);
	partrel = partrelinfo->ri_RelationDesc;

	/*
	 * To perform any of the operations below, the tuple must match the
	 * partition's rowtype. Convert if needed or just copy, using a dedicated
	 * slot to store the tuple in any case.
	 */
	remoteslot_part = partrelinfo->ri_PartitionTupleSlot;
	if (remoteslot_part == NULL)
		remoteslot_part = table_slot_create(partrel, &estate->es_tupleTable);
	map = partrelinfo->ri_RootToPartitionMap;
	if (map != NULL)
		remoteslot_part = execute_attr_map_slot(map->attrMap, remoteslot,
												remoteslot_part);
	else
	{
		remoteslot_part = ExecCopySlot(remoteslot_part, remoteslot);
		slot_getallattrs(remoteslot_part);
	}
	MemoryContextSwitchTo(oldctx);

	switch (operation)
	{
		case CMD_INSERT:
			apply_handle_insert_internal(partrelinfo, estate,
										 remoteslot_part);
			break;

		case CMD_DELETE:
			apply_handle_delete_internal(partrelinfo, estate,
										 remoteslot_part,
										 &relmapentry->remoterel);
			break;

		case CMD_UPDATE:

			/*
			 * For UPDATE, depending on whether or not the updated tuple
			 * satisfies the partition's constraint, perform a simple UPDATE
			 * of the partition or move the updated tuple into a different
			 * suitable partition.
			 */
			{
				AttrMap    *attrmap = map ? map->attrMap : NULL;
				LogicalRepRelMapEntry *part_entry;
				TupleTableSlot *localslot;
				ResultRelInfo *partrelinfo_new;
				bool		found;

				part_entry = logicalrep_partition_open(relmapentry, partrel,
													   attrmap);

				/* Get the matching local tuple from the partition. */
				found = FindReplTupleInLocalRel(estate, partrel,
												&part_entry->remoterel,
												remoteslot_part, &localslot);

				oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
				if (found)
				{
					/* Apply the update.  */
					slot_modify_data(remoteslot_part, localslot,
									 part_entry,
									 newtup);
					MemoryContextSwitchTo(oldctx);
				}
				else
				{
					/*
					 * The tuple to be updated could not be found.
					 *
					 * TODO what to do here, change the log level to LOG
					 * perhaps?
					 */
					elog(DEBUG1,
						 "logical replication did not find row for update "
						 "in replication target relation \"%s\"",
						 RelationGetRelationName(partrel));
				}

				/*
				 * Does the updated tuple still satisfy the current
				 * partition's constraint?
				 */
				if (!partrel->rd_rel->relispartition ||
					ExecPartitionCheck(partrelinfo, remoteslot_part, estate,
									   false))
				{
					/*
					 * Yes, so simply UPDATE the partition.  We don't call
					 * apply_handle_update_internal() here, which would
					 * normally do the following work, to avoid repeating some
					 * work already done above to find the local tuple in the
					 * partition.
					 */
					EPQState	epqstate;

					EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1);
					ExecOpenIndices(partrelinfo, false);

					EvalPlanQualSetSlot(&epqstate, remoteslot_part);
					ExecSimpleRelationUpdate(partrelinfo, estate, &epqstate,
											 localslot, remoteslot_part);
					ExecCloseIndices(partrelinfo);
					EvalPlanQualEnd(&epqstate);
				}
				else
				{
					/* Move the tuple into the new partition. */

					/*
					 * New partition will be found using tuple routing, which
					 * can only occur via the parent table.  We might need to
					 * convert the tuple to the parent's rowtype.  Note that
					 * this is the tuple found in the partition, not the
					 * original search tuple received by this function.
					 */
					if (map)
					{
						TupleConversionMap *PartitionToRootMap =
						convert_tuples_by_name(RelationGetDescr(partrel),
											   RelationGetDescr(parentrel));

						remoteslot =
							execute_attr_map_slot(PartitionToRootMap->attrMap,
												  remoteslot_part, remoteslot);
					}
					else
					{
						remoteslot = ExecCopySlot(remoteslot, remoteslot_part);
						slot_getallattrs(remoteslot);
					}


					/* Find the new partition. */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					partrelinfo_new = ExecFindPartition(mtstate, relinfo,
														proute, remoteslot,
														estate);
					MemoryContextSwitchTo(oldctx);
					Assert(partrelinfo_new != partrelinfo);

					/* DELETE old tuple found in the old partition. */
					apply_handle_delete_internal(partrelinfo, estate,
												 localslot,
												 &relmapentry->remoterel);

					/* INSERT new tuple into the new partition. */

					/*
					 * Convert the replacement tuple to match the destination
					 * partition rowtype.
					 */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					partrel = partrelinfo_new->ri_RelationDesc;
					remoteslot_part = partrelinfo_new->ri_PartitionTupleSlot;
					if (remoteslot_part == NULL)
						remoteslot_part = table_slot_create(partrel,
															&estate->es_tupleTable);
					map = partrelinfo_new->ri_RootToPartitionMap;
					if (map != NULL)
					{
						remoteslot_part = execute_attr_map_slot(map->attrMap,
																remoteslot,
																remoteslot_part);
					}
					else
					{
						remoteslot_part = ExecCopySlot(remoteslot_part,
													   remoteslot);
						slot_getallattrs(remoteslot);
					}
					MemoryContextSwitchTo(oldctx);
					apply_handle_insert_internal(partrelinfo_new, estate,
												 remoteslot_part);
				}
			}
			break;

		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) operation);
			break;
	}

	ExecCleanupTupleRouting(mtstate, proute);
}

/*
 * Handle TRUNCATE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_truncate(StringInfo s)
{
	bool		cascade = false;
	bool		restart_seqs = false;
	List	   *remote_relids = NIL;
	List	   *remote_rels = NIL;
	List	   *rels = NIL;
	List	   *part_rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *lc;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_TRUNCATE, s))
		return;

	ensure_transaction();

	remote_relids = logicalrep_read_truncate(s, &cascade, &restart_seqs);

	foreach(lc, remote_relids)
	{
		LogicalRepRelId relid = lfirst_oid(lc);
		LogicalRepRelMapEntry *rel;

		rel = logicalrep_rel_open(relid, RowExclusiveLock);
		if (!should_apply_changes_for_rel(rel))
		{
			/*
			 * The relation can't become interesting in the middle of the
			 * transaction so it's safe to unlock it.
			 */
			logicalrep_rel_close(rel, RowExclusiveLock);
			continue;
		}

		remote_rels = lappend(remote_rels, rel);
		rels = lappend(rels, rel->localrel);
		relids = lappend_oid(relids, rel->localreloid);
		if (RelationIsLogicallyLogged(rel->localrel))
			relids_logged = lappend_oid(relids_logged, rel->localreloid);

		/*
		 * Truncate partitions if we got a message to truncate a partitioned
		 * table.
		 */
		if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ListCell   *child;
			List	   *children = find_all_inheritors(rel->localreloid,
													   RowExclusiveLock,
													   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);
				Relation	childrel;

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors already got lock */
				childrel = table_open(childrelid, NoLock);

				/*
				 * Ignore temp tables of other backends.  See similar code in
				 * ExecuteTruncate().
				 */
				if (RELATION_IS_OTHER_TEMP(childrel))
				{
					table_close(childrel, RowExclusiveLock);
					continue;
				}

				rels = lappend(rels, childrel);
				part_rels = lappend(part_rels, childrel);
				relids = lappend_oid(relids, childrelid);
				/* Log this relation only if needed for logical decoding */
				if (RelationIsLogicallyLogged(childrel))
					relids_logged = lappend_oid(relids_logged, childrelid);
			}
		}
	}

	/*
	 * Even if we used CASCADE on the upstream primary we explicitly default
	 * to replaying changes without further cascading. This might be later
	 * changeable with a user specified option.
	 */
	ExecuteTruncateGuts(rels, relids, relids_logged, DROP_RESTRICT, restart_seqs);

	foreach(lc, remote_rels)
	{
		LogicalRepRelMapEntry *rel = lfirst(lc);

		logicalrep_rel_close(rel, NoLock);
	}
	foreach(lc, part_rels)
	{
		Relation	rel = lfirst(lc);

		table_close(rel, NoLock);
	}

	CommandCounterIncrement();
}


/*
 * Logical replication protocol message dispatcher.
 */
static void
apply_dispatch(StringInfo s)
{
	LogicalRepMsgType action = pq_getmsgbyte(s);

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			apply_handle_begin(s);
			return;

		case LOGICAL_REP_MSG_COMMIT:
			apply_handle_commit(s);
			return;

		case LOGICAL_REP_MSG_INSERT:
			apply_handle_insert(s);
			return;

		case LOGICAL_REP_MSG_UPDATE:
			apply_handle_update(s);
			return;

		case LOGICAL_REP_MSG_DELETE:
			apply_handle_delete(s);
			return;

		case LOGICAL_REP_MSG_TRUNCATE:
			apply_handle_truncate(s);
			return;

		case LOGICAL_REP_MSG_RELATION:
			apply_handle_relation(s);
			return;

		case LOGICAL_REP_MSG_TYPE:
			apply_handle_type(s);
			return;

		case LOGICAL_REP_MSG_ORIGIN:
			apply_handle_origin(s);
			return;

		case LOGICAL_REP_MSG_STREAM_START:
			apply_handle_stream_start(s);
			return;

		case LOGICAL_REP_MSG_STREAM_END:
			apply_handle_stream_stop(s);
			return;

		case LOGICAL_REP_MSG_STREAM_ABORT:
			apply_handle_stream_abort(s);
			return;

		case LOGICAL_REP_MSG_STREAM_COMMIT:
			apply_handle_stream_commit(s);
			return;
	}

	ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("invalid logical replication message type \"%c\"", action)));
}

/*
 * Figure out which write/flush positions to report to the walsender process.
 *
 * We can't simply report back the last LSN the walsender sent us because the
 * local transaction might not yet be flushed to disk locally. Instead we
 * build a list that associates local with remote LSNs for every commit. When
 * reporting back the flush position to the sender we iterate that list and
 * check which entries on it are already locally flushed. Those we can report
 * as having been flushed.
 *
 * The have_pending_txes is true if there are outstanding transactions that
 * need to be flushed.
 */
static void
get_flush_position(XLogRecPtr *write, XLogRecPtr *flush,
				   bool *have_pending_txes)
{
	dlist_mutable_iter iter;
	XLogRecPtr	local_flush = GetFlushRecPtr();

	*write = InvalidXLogRecPtr;
	*flush = InvalidXLogRecPtr;

	dlist_foreach_modify(iter, &lsn_mapping)
	{
		FlushPosition *pos =
		dlist_container(FlushPosition, node, iter.cur);

		*write = pos->remote_end;

		if (pos->local_end <= local_flush)
		{
			*flush = pos->remote_end;
			dlist_delete(iter.cur);
			pfree(pos);
		}
		else
		{
			/*
			 * Don't want to uselessly iterate over the rest of the list which
			 * could potentially be long. Instead get the last element and
			 * grab the write position from there.
			 */
			pos = dlist_tail_element(FlushPosition, node,
									 &lsn_mapping);
			*write = pos->remote_end;
			*have_pending_txes = true;
			return;
		}
	}

	*have_pending_txes = !dlist_is_empty(&lsn_mapping);
}

/*
 * Store current remote/local lsn pair in the tracking list.
 */
static void
store_flush_position(XLogRecPtr remote_lsn)
{
	FlushPosition *flushpos;

	/* Need to do this in permanent context */
	MemoryContextSwitchTo(ApplyContext);

	/* Track commit lsn  */
	flushpos = (FlushPosition *) palloc(sizeof(FlushPosition));
	flushpos->local_end = XactLastCommitEnd;
	flushpos->remote_end = remote_lsn;

	dlist_push_tail(&lsn_mapping, &flushpos->node);
	MemoryContextSwitchTo(ApplyMessageContext);
}


/* Update statistics of the worker. */
static void
UpdateWorkerStats(XLogRecPtr last_lsn, TimestampTz send_time, bool reply)
{
	MyLogicalRepWorker->last_lsn = last_lsn;
	MyLogicalRepWorker->last_send_time = send_time;
	MyLogicalRepWorker->last_recv_time = GetCurrentTimestamp();
	if (reply)
	{
		MyLogicalRepWorker->reply_lsn = last_lsn;
		MyLogicalRepWorker->reply_time = send_time;
	}
}

/*
 * Apply main loop.
 */
static void
LogicalRepApplyLoop(XLogRecPtr last_received)
{
	TimestampTz last_recv_timestamp = GetCurrentTimestamp();
	bool		ping_sent = false;
	TimeLineID	tli;

	/*
	 * Init the ApplyMessageContext which we clean up after each replication
	 * protocol message.
	 */
	ApplyMessageContext = AllocSetContextCreate(ApplyContext,
												"ApplyMessageContext",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * This memory context is used for per-stream data when the streaming mode
	 * is enabled. This context is reset on each stream stop.
	 */
	LogicalStreamingContext = AllocSetContextCreate(ApplyContext,
													"LogicalStreamingContext",
													ALLOCSET_DEFAULT_SIZES);

	/* mark as idle, before starting to loop */
	pgstat_report_activity(STATE_IDLE, NULL);

	/* This outer loop iterates once per wait. */
	for (;;)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			rc;
		int			len;
		char	   *buf = NULL;
		bool		endofstream = false;
		long		wait_time;

		CHECK_FOR_INTERRUPTS();

		MemoryContextSwitchTo(ApplyMessageContext);

		len = walrcv_receive(wrconn, &buf, &fd);

		if (len != 0)
		{
			/* Loop to process all available data (without blocking). */
			for (;;)
			{
				CHECK_FOR_INTERRUPTS();

				if (len == 0)
				{
					break;
				}
				else if (len < 0)
				{
					ereport(LOG,
							(errmsg("data stream from publisher has ended")));
					endofstream = true;
					break;
				}
				else
				{
					int			c;
					StringInfoData s;

					/* Reset timeout. */
					last_recv_timestamp = GetCurrentTimestamp();
					ping_sent = false;

					/* Ensure we are reading the data into our memory context. */
					MemoryContextSwitchTo(ApplyMessageContext);

					s.data = buf;
					s.len = len;
					s.cursor = 0;
					s.maxlen = -1;

					c = pq_getmsgbyte(&s);

					if (c == 'w')
					{
						XLogRecPtr	start_lsn;
						XLogRecPtr	end_lsn;
						TimestampTz send_time;

						start_lsn = pq_getmsgint64(&s);
						end_lsn = pq_getmsgint64(&s);
						send_time = pq_getmsgint64(&s);

						if (last_received < start_lsn)
							last_received = start_lsn;

						if (last_received < end_lsn)
							last_received = end_lsn;

						UpdateWorkerStats(last_received, send_time, false);

						apply_dispatch(&s);
					}
					else if (c == 'k')
					{
						XLogRecPtr	end_lsn;
						TimestampTz timestamp;
						bool		reply_requested;

						end_lsn = pq_getmsgint64(&s);
						timestamp = pq_getmsgint64(&s);
						reply_requested = pq_getmsgbyte(&s);

						if (last_received < end_lsn)
							last_received = end_lsn;

						send_feedback(last_received, reply_requested, false);
						UpdateWorkerStats(last_received, timestamp, true);
					}
					/* other message types are purposefully ignored */

					MemoryContextReset(ApplyMessageContext);
				}

				len = walrcv_receive(wrconn, &buf, &fd);
			}
		}

		/* confirm all writes so far */
		send_feedback(last_received, false, false);

		if (!in_remote_transaction && !in_streamed_transaction)
		{
			/*
			 * If we didn't get any transactions for a while there might be
			 * unconsumed invalidation messages in the queue, consume them
			 * now.
			 */
			AcceptInvalidationMessages();
			maybe_reread_subscription();

			/* Process any table synchronization changes. */
			process_syncing_tables(last_received);
		}

		/* Cleanup the memory. */
		MemoryContextResetAndDeleteChildren(ApplyMessageContext);
		MemoryContextSwitchTo(TopMemoryContext);

		/* Check if we need to exit the streaming loop. */
		if (endofstream)
			break;

		/*
		 * Wait for more data or latch.  If we have unflushed transactions,
		 * wake up after WalWriterDelay to see if they've been flushed yet (in
		 * which case we should send a feedback message).  Otherwise, there's
		 * no particular urgency about waking up unless we get data or a
		 * signal.
		 */
		if (!dlist_is_empty(&lsn_mapping))
			wait_time = WalWriterDelay;
		else
			wait_time = NAPTIME_PER_CYCLE;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							   fd, wait_time,
							   WAIT_EVENT_LOGICAL_APPLY_MAIN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (rc & WL_TIMEOUT)
		{
			/*
			 * We didn't receive anything new. If we haven't heard anything
			 * from the server for more than wal_receiver_timeout / 2, ping
			 * the server. Also, if it's been longer than
			 * wal_receiver_status_interval since the last update we sent,
			 * send a status update to the primary anyway, to report any
			 * progress in applying WAL.
			 */
			bool		requestReply = false;

			/*
			 * Check if time since last receive from primary has reached the
			 * configured limit.
			 */
			if (wal_receiver_timeout > 0)
			{
				TimestampTz now = GetCurrentTimestamp();
				TimestampTz timeout;

				timeout =
					TimestampTzPlusMilliseconds(last_recv_timestamp,
												wal_receiver_timeout);

				if (now >= timeout)
					ereport(ERROR,
							(errmsg("terminating logical replication worker due to timeout")));

				/* Check to see if it's time for a ping. */
				if (!ping_sent)
				{
					timeout = TimestampTzPlusMilliseconds(last_recv_timestamp,
														  (wal_receiver_timeout / 2));
					if (now >= timeout)
					{
						requestReply = true;
						ping_sent = true;
					}
				}
			}

			send_feedback(last_received, requestReply, requestReply);
		}
	}

	/* All done */
	walrcv_endstreaming(wrconn, &tli);
}

/*
 * Send a Standby Status Update message to server.
 *
 * 'recvpos' is the latest LSN we've received data to, force is set if we need
 * to send a response to avoid timeouts.
 */
static void
send_feedback(XLogRecPtr recvpos, bool force, bool requestReply)
{
	static StringInfo reply_message = NULL;
	static TimestampTz send_time = 0;

	static XLogRecPtr last_recvpos = InvalidXLogRecPtr;
	static XLogRecPtr last_writepos = InvalidXLogRecPtr;
	static XLogRecPtr last_flushpos = InvalidXLogRecPtr;

	XLogRecPtr	writepos;
	XLogRecPtr	flushpos;
	TimestampTz now;
	bool		have_pending_txes;

	/*
	 * If the user doesn't want status to be reported to the publisher, be
	 * sure to exit before doing anything at all.
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* It's legal to not pass a recvpos */
	if (recvpos < last_recvpos)
		recvpos = last_recvpos;

	get_flush_position(&writepos, &flushpos, &have_pending_txes);

	/*
	 * No outstanding transactions to flush, we can report the latest received
	 * position. This is important for synchronous replication.
	 */
	if (!have_pending_txes)
		flushpos = writepos = recvpos;

	if (writepos < last_writepos)
		writepos = last_writepos;

	if (flushpos < last_flushpos)
		flushpos = last_flushpos;

	now = GetCurrentTimestamp();

	/* if we've already reported everything we're good */
	if (!force &&
		writepos == last_writepos &&
		flushpos == last_flushpos &&
		!TimestampDifferenceExceeds(send_time, now,
									wal_receiver_status_interval * 1000))
		return;
	send_time = now;

	if (!reply_message)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(ApplyContext);

		reply_message = makeStringInfo();
		MemoryContextSwitchTo(oldctx);
	}
	else
		resetStringInfo(reply_message);

	pq_sendbyte(reply_message, 'r');
	pq_sendint64(reply_message, recvpos);	/* write */
	pq_sendint64(reply_message, flushpos);	/* flush */
	pq_sendint64(reply_message, writepos);	/* apply */
	pq_sendint64(reply_message, now);	/* sendTime */
	pq_sendbyte(reply_message, requestReply);	/* replyRequested */

	elog(DEBUG2, "sending feedback (force %d) to recv %X/%X, write %X/%X, flush %X/%X",
		 force,
		 LSN_FORMAT_ARGS(recvpos),
		 LSN_FORMAT_ARGS(writepos),
		 LSN_FORMAT_ARGS(flushpos));

	walrcv_send(wrconn, reply_message->data, reply_message->len);

	if (recvpos > last_recvpos)
		last_recvpos = recvpos;
	if (writepos > last_writepos)
		last_writepos = writepos;
	if (flushpos > last_flushpos)
		last_flushpos = flushpos;
}

/*
 * Reread subscription info if needed. Most changes will be exit.
 */
static void
maybe_reread_subscription(void)
{
	MemoryContext oldctx;
	Subscription *newsub;
	bool		started_tx = false;

	/* When cache state is valid there is nothing to do here. */
	if (MySubscriptionValid)
		return;

	/* This function might be called inside or outside of transaction. */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/* Ensure allocations in permanent context. */
	oldctx = MemoryContextSwitchTo(ApplyContext);

	newsub = GetSubscription(MyLogicalRepWorker->subid, true);

	/*
	 * Exit if the subscription was removed. This normally should not happen
	 * as the worker gets killed during DROP SUBSCRIPTION.
	 */
	if (!newsub)
	{
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" will "
						"stop because the subscription was removed",
						MySubscription->name)));

		proc_exit(0);
	}

	/*
	 * Exit if the subscription was disabled. This normally should not happen
	 * as the worker gets killed during ALTER SUBSCRIPTION ... DISABLE.
	 */
	if (!newsub->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" will "
						"stop because the subscription was disabled",
						MySubscription->name)));

		proc_exit(0);
	}

	/* !slotname should never happen when enabled is true. */
	Assert(newsub->slotname);

	/*
	 * Exit if any parameter that affects the remote connection was changed.
	 * The launcher will start a new worker.
	 */
	if (strcmp(newsub->conninfo, MySubscription->conninfo) != 0 ||
		strcmp(newsub->name, MySubscription->name) != 0 ||
		strcmp(newsub->slotname, MySubscription->slotname) != 0 ||
		newsub->binary != MySubscription->binary ||
		newsub->stream != MySubscription->stream ||
		!equal(newsub->publications, MySubscription->publications))
	{
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" will restart because of a parameter change",
						MySubscription->name)));

		proc_exit(0);
	}

	/* Check for other changes that should never happen too. */
	if (newsub->dbid != MySubscription->dbid)
	{
		elog(ERROR, "subscription %u changed unexpectedly",
			 MyLogicalRepWorker->subid);
	}

	/* Clean old subscription info and switch to new one. */
	FreeSubscription(MySubscription);
	MySubscription = newsub;

	MemoryContextSwitchTo(oldctx);

	/* Change synchronous commit according to the user's wishes */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	if (started_tx)
		CommitTransactionCommand();

	MySubscriptionValid = true;
}

/*
 * Callback from subscription syscache invalidation.
 */
static void
subscription_change_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	MySubscriptionValid = false;
}

/*
 * subxact_info_write
 *	  Store information about subxacts for a toplevel transaction.
 *
 * For each subxact we store offset of it's first change in the main file.
 * The file is always over-written as a whole.
 *
 * XXX We should only store subxacts that were not aborted yet.
 */
static void
subxact_info_write(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	bool		found;
	Size		len;
	StreamXidHash *ent;
	BufFile    *fd;

	Assert(TransactionIdIsValid(xid));

	/* find the xid entry in the xidhash */
	ent = (StreamXidHash *) hash_search(xidhash,
										(void *) &xid,
										HASH_FIND,
										&found);
	/* we must found the entry for its top transaction by this time */
	Assert(found);

	/*
	 * If there is no subtransaction then nothing to do, but if already have
	 * subxact file then delete that.
	 */
	if (subxact_data.nsubxacts == 0)
	{
		if (ent->subxact_fileset)
		{
			cleanup_subxact_info();
			SharedFileSetDeleteAll(ent->subxact_fileset);
			pfree(ent->subxact_fileset);
			ent->subxact_fileset = NULL;
		}
		return;
	}

	subxact_filename(path, subid, xid);

	/*
	 * Create the subxact file if it not already created, otherwise open the
	 * existing file.
	 */
	if (ent->subxact_fileset == NULL)
	{
		MemoryContext oldctx;

		/*
		 * We need to maintain shared fileset across multiple stream
		 * start/stop calls.  So, need to allocate it in a persistent context.
		 */
		oldctx = MemoryContextSwitchTo(ApplyContext);
		ent->subxact_fileset = palloc(sizeof(SharedFileSet));
		SharedFileSetInit(ent->subxact_fileset, NULL);
		MemoryContextSwitchTo(oldctx);

		fd = BufFileCreateShared(ent->subxact_fileset, path);
	}
	else
		fd = BufFileOpenShared(ent->subxact_fileset, path, O_RDWR);

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* Write the subxact count and subxact info */
	BufFileWrite(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));
	BufFileWrite(fd, subxact_data.subxacts, len);

	BufFileClose(fd);

	/* free the memory allocated for subxact info */
	cleanup_subxact_info();
}

/*
 * subxact_info_read
 *	  Restore information about subxacts of a streamed transaction.
 *
 * Read information about subxacts into the structure subxact_data that can be
 * used later.
 */
static void
subxact_info_read(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	bool		found;
	Size		len;
	BufFile    *fd;
	StreamXidHash *ent;
	MemoryContext oldctx;

	Assert(TransactionIdIsValid(xid));
	Assert(!subxact_data.subxacts);
	Assert(subxact_data.nsubxacts == 0);
	Assert(subxact_data.nsubxacts_max == 0);

	/* Find the stream xid entry in the xidhash */
	ent = (StreamXidHash *) hash_search(xidhash,
										(void *) &xid,
										HASH_FIND,
										&found);

	/*
	 * If subxact_fileset is not valid that mean we don't have any subxact
	 * info
	 */
	if (ent->subxact_fileset == NULL)
		return;

	subxact_filename(path, subid, xid);

	fd = BufFileOpenShared(ent->subxact_fileset, path, O_RDONLY);

	/* read number of subxact items */
	if (BufFileRead(fd, &subxact_data.nsubxacts,
					sizeof(subxact_data.nsubxacts)) !=
		sizeof(subxact_data.nsubxacts))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from streaming transaction's subxact file \"%s\": %m",
						path)));

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* we keep the maximum as a power of 2 */
	subxact_data.nsubxacts_max = 1 << my_log2(subxact_data.nsubxacts);

	/*
	 * Allocate subxact information in the logical streaming context. We need
	 * this information during the complete stream so that we can add the sub
	 * transaction info to this. On stream stop we will flush this information
	 * to the subxact file and reset the logical streaming context.
	 */
	oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
	subxact_data.subxacts = palloc(subxact_data.nsubxacts_max *
								   sizeof(SubXactInfo));
	MemoryContextSwitchTo(oldctx);

	if ((len > 0) && ((BufFileRead(fd, subxact_data.subxacts, len)) != len))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from streaming transaction's subxact file \"%s\": %m",
						path)));

	BufFileClose(fd);
}

/*
 * subxact_info_add
 *	  Add information about a subxact (offset in the main file).
 */
static void
subxact_info_add(TransactionId xid)
{
	SubXactInfo *subxacts = subxact_data.subxacts;
	int64		i;

	/* We must have a valid top level stream xid and a stream fd. */
	Assert(TransactionIdIsValid(stream_xid));
	Assert(stream_fd != NULL);

	/*
	 * If the XID matches the toplevel transaction, we don't want to add it.
	 */
	if (stream_xid == xid)
		return;

	/*
	 * In most cases we're checking the same subxact as we've already seen in
	 * the last call, so make sure to ignore it (this change comes later).
	 */
	if (subxact_data.subxact_last == xid)
		return;

	/* OK, remember we're processing this XID. */
	subxact_data.subxact_last = xid;

	/*
	 * Check if the transaction is already present in the array of subxact. We
	 * intentionally scan the array from the tail, because we're likely adding
	 * a change for the most recent subtransactions.
	 *
	 * XXX Can we rely on the subxact XIDs arriving in sorted order? That
	 * would allow us to use binary search here.
	 */
	for (i = subxact_data.nsubxacts; i > 0; i--)
	{
		/* found, so we're done */
		if (subxacts[i - 1].xid == xid)
			return;
	}

	/* This is a new subxact, so we need to add it to the array. */
	if (subxact_data.nsubxacts == 0)
	{
		MemoryContext oldctx;

		subxact_data.nsubxacts_max = 128;

		/*
		 * Allocate this memory for subxacts in per-stream context, see
		 * subxact_info_read.
		 */
		oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
		subxacts = palloc(subxact_data.nsubxacts_max * sizeof(SubXactInfo));
		MemoryContextSwitchTo(oldctx);
	}
	else if (subxact_data.nsubxacts == subxact_data.nsubxacts_max)
	{
		subxact_data.nsubxacts_max *= 2;
		subxacts = repalloc(subxacts,
							subxact_data.nsubxacts_max * sizeof(SubXactInfo));
	}

	subxacts[subxact_data.nsubxacts].xid = xid;

	/*
	 * Get the current offset of the stream file and store it as offset of
	 * this subxact.
	 */
	BufFileTell(stream_fd,
				&subxacts[subxact_data.nsubxacts].fileno,
				&subxacts[subxact_data.nsubxacts].offset);

	subxact_data.nsubxacts++;
	subxact_data.subxacts = subxacts;
}

/* format filename for file containing the info about subxacts */
static inline void
subxact_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.subxacts", subid, xid);
}

/* format filename for file containing serialized changes */
static inline void
changes_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.changes", subid, xid);
}

/*
 * stream_cleanup_files
 *	  Cleanup files for a subscription / toplevel transaction.
 *
 * Remove files with serialized changes and subxact info for a particular
 * toplevel transaction. Each subscription has a separate set of files.
 */
static void
stream_cleanup_files(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	StreamXidHash *ent;

	/* Remove the xid entry from the stream xid hash */
	ent = (StreamXidHash *) hash_search(xidhash,
										(void *) &xid,
										HASH_REMOVE,
										NULL);
	/* By this time we must have created the transaction entry */
	Assert(ent != NULL);

	/* Delete the change file and release the stream fileset memory */
	changes_filename(path, subid, xid);
	SharedFileSetDeleteAll(ent->stream_fileset);
	pfree(ent->stream_fileset);
	ent->stream_fileset = NULL;

	/* Delete the subxact file and release the memory, if it exist */
	if (ent->subxact_fileset)
	{
		subxact_filename(path, subid, xid);
		SharedFileSetDeleteAll(ent->subxact_fileset);
		pfree(ent->subxact_fileset);
		ent->subxact_fileset = NULL;
	}
}

/*
 * stream_open_file
 *	  Open a file that we'll use to serialize changes for a toplevel
 * transaction.
 *
 * Open a file for streamed changes from a toplevel transaction identified
 * by stream_xid (global variable). If it's the first chunk of streamed
 * changes for this transaction, initialize the shared fileset and create the
 * buffile, otherwise open the previously created file.
 *
 * This can only be called at the beginning of a "streaming" block, i.e.
 * between stream_start/stream_stop messages from the upstream.
 */
static void
stream_open_file(Oid subid, TransactionId xid, bool first_segment)
{
	char		path[MAXPGPATH];
	bool		found;
	MemoryContext oldcxt;
	StreamXidHash *ent;

	Assert(in_streamed_transaction);
	Assert(OidIsValid(subid));
	Assert(TransactionIdIsValid(xid));
	Assert(stream_fd == NULL);

	/* create or find the xid entry in the xidhash */
	ent = (StreamXidHash *) hash_search(xidhash,
										(void *) &xid,
										HASH_ENTER | HASH_FIND,
										&found);
	Assert(first_segment || found);
	changes_filename(path, subid, xid);
	elog(DEBUG1, "opening file \"%s\" for streamed changes", path);

	/*
	 * Create/open the buffiles under the logical streaming context so that we
	 * have those files until stream stop.
	 */
	oldcxt = MemoryContextSwitchTo(LogicalStreamingContext);

	/*
	 * If this is the first streamed segment, the file must not exist, so make
	 * sure we're the ones creating it. Otherwise just open the file for
	 * writing, in append mode.
	 */
	if (first_segment)
	{
		MemoryContext savectx;
		SharedFileSet *fileset;

		/*
		 * We need to maintain shared fileset across multiple stream
		 * start/stop calls. So, need to allocate it in a persistent context.
		 */
		savectx = MemoryContextSwitchTo(ApplyContext);
		fileset = palloc(sizeof(SharedFileSet));

		SharedFileSetInit(fileset, NULL);
		MemoryContextSwitchTo(savectx);

		stream_fd = BufFileCreateShared(fileset, path);

		/* Remember the fileset for the next stream of the same transaction */
		ent->xid = xid;
		ent->stream_fileset = fileset;
		ent->subxact_fileset = NULL;
	}
	else
	{
		/*
		 * Open the file and seek to the end of the file because we always
		 * append the changes file.
		 */
		stream_fd = BufFileOpenShared(ent->stream_fileset, path, O_RDWR);
		BufFileSeek(stream_fd, 0, 0, SEEK_END);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * stream_close_file
 *	  Close the currently open file with streamed changes.
 *
 * This can only be called at the end of a streaming block, i.e. at stream_stop
 * message from the upstream.
 */
static void
stream_close_file(void)
{
	Assert(in_streamed_transaction);
	Assert(TransactionIdIsValid(stream_xid));
	Assert(stream_fd != NULL);

	BufFileClose(stream_fd);

	stream_xid = InvalidTransactionId;
	stream_fd = NULL;
}

/*
 * stream_write_change
 *	  Serialize a change to a file for the current toplevel transaction.
 *
 * The change is serialized in a simple format, with length (not including
 * the length), action code (identifying the message type) and message
 * contents (without the subxact TransactionId value).
 */
static void
stream_write_change(char action, StringInfo s)
{
	int			len;

	Assert(in_streamed_transaction);
	Assert(TransactionIdIsValid(stream_xid));
	Assert(stream_fd != NULL);

	/* total on-disk size, including the action type character */
	len = (s->len - s->cursor) + sizeof(char);

	/* first write the size */
	BufFileWrite(stream_fd, &len, sizeof(len));

	/* then the action */
	BufFileWrite(stream_fd, &action, sizeof(action));

	/* and finally the remaining part of the buffer (after the XID) */
	len = (s->len - s->cursor);

	BufFileWrite(stream_fd, &s->data[s->cursor], len);
}

/*
 * Cleanup the memory for subxacts and reset the related variables.
 */
static inline void
cleanup_subxact_info()
{
	if (subxact_data.subxacts)
		pfree(subxact_data.subxacts);

	subxact_data.subxacts = NULL;
	subxact_data.subxact_last = InvalidTransactionId;
	subxact_data.nsubxacts = 0;
	subxact_data.nsubxacts_max = 0;
}

/* Logical Replication Apply worker entry point */
void
ApplyWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);
	MemoryContext oldctx;
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos;
	char	   *myslotname;
	WalRcvStreamOptions options;

	/* Attach to slot */
	logicalrep_worker_attach(worker_slot);

	/* Setup signal handling */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * We don't currently need any ResourceOwner in a walreceiver process, but
	 * if we did, we could call CreateAuxProcessResourceOwner here.
	 */

	/* Initialise stats to a sanish value */
	MyLogicalRepWorker->last_send_time = MyLogicalRepWorker->last_recv_time =
		MyLogicalRepWorker->reply_time = GetCurrentTimestamp();

	/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false);

	/* Run as replica session replication role. */
	SetConfigOption("session_replication_role", "replica",
					PGC_SUSET, PGC_S_OVERRIDE);

	/* Connect to our database. */
	BackgroundWorkerInitializeConnectionByOid(MyLogicalRepWorker->dbid,
											  MyLogicalRepWorker->userid,
											  0);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. pg_index.indexprs).
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/* Load the subscription into persistent memory context. */
	ApplyContext = AllocSetContextCreate(TopMemoryContext,
										 "ApplyContext",
										 ALLOCSET_DEFAULT_SIZES);
	StartTransactionCommand();
	oldctx = MemoryContextSwitchTo(ApplyContext);

	MySubscription = GetSubscription(MyLogicalRepWorker->subid, true);
	if (!MySubscription)
	{
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription %u will not "
						"start because the subscription was removed during startup",
						MyLogicalRepWorker->subid)));
		proc_exit(0);
	}

	MySubscriptionValid = true;
	MemoryContextSwitchTo(oldctx);

	if (!MySubscription->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" will not "
						"start because the subscription was disabled during startup",
						MySubscription->name)));

		proc_exit(0);
	}

	/* Setup synchronous commit according to the user's wishes */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/* Keep us informed about subscription changes. */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONOID,
								  subscription_change_cb,
								  (Datum) 0);

	if (am_tablesync_worker())
		ereport(LOG,
				(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has started",
						MySubscription->name, get_rel_name(MyLogicalRepWorker->relid))));
	else
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" has started",
						MySubscription->name)));

	CommitTransactionCommand();

	/* Connect to the origin and start the replication. */
	elog(DEBUG1, "connecting to publisher using connection string \"%s\"",
		 MySubscription->conninfo);

	if (am_tablesync_worker())
	{
		char	   *syncslotname;

		/* This is table synchronization worker, call initial sync. */
		syncslotname = LogicalRepSyncTableStart(&origin_startpos);

		/* allocate slot name in long-lived context */
		myslotname = MemoryContextStrdup(ApplyContext, syncslotname);

		pfree(syncslotname);
	}
	else
	{
		/* This is main apply worker */
		RepOriginId originid;
		TimeLineID	startpointTLI;
		char	   *err;

		myslotname = MySubscription->slotname;

		/*
		 * This shouldn't happen if the subscription is enabled, but guard
		 * against DDL bugs or manual catalog changes.  (libpqwalreceiver will
		 * crash if slot is NULL.)
		 */
		if (!myslotname)
			ereport(ERROR,
					(errmsg("subscription has no replication slot set")));

		/* Setup replication origin tracking. */
		StartTransactionCommand();
		snprintf(originname, sizeof(originname), "pg_%u", MySubscription->oid);
		originid = replorigin_by_name(originname, true);
		if (!OidIsValid(originid))
			originid = replorigin_create(originname);
		replorigin_session_setup(originid);
		replorigin_session_origin = originid;
		origin_startpos = replorigin_session_get_progress(false);
		CommitTransactionCommand();

		wrconn = walrcv_connect(MySubscription->conninfo, true, MySubscription->name,
								&err);
		if (wrconn == NULL)
			ereport(ERROR,
					(errmsg("could not connect to the publisher: %s", err)));

		/*
		 * We don't really use the output identify_system for anything but it
		 * does some initializations on the upstream so let's still call it.
		 */
		(void) walrcv_identify_system(wrconn, &startpointTLI);
	}

	/*
	 * Setup callback for syscache so that we know when something changes in
	 * the subscription relation state.
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONRELMAP,
								  invalidate_syncing_table_states,
								  (Datum) 0);

	/* Build logical replication streaming options. */
	options.logical = true;
	options.startpoint = origin_startpos;
	options.slotname = myslotname;
	options.proto.logical.proto_version =
		walrcv_server_version(wrconn) >= 140000 ?
		LOGICALREP_PROTO_STREAM_VERSION_NUM : LOGICALREP_PROTO_VERSION_NUM;
	options.proto.logical.publication_names = MySubscription->publications;
	options.proto.logical.binary = MySubscription->binary;
	options.proto.logical.streaming = MySubscription->stream;

	/* Start normal logical streaming replication. */
	walrcv_startstreaming(wrconn, &options);

	/* Run the main loop. */
	LogicalRepApplyLoop(origin_startpos);

	proc_exit(0);
}

/*
 * Is current process a logical replication worker?
 */
bool
IsLogicalWorker(void)
{
	return MyLogicalRepWorker != NULL;
}
