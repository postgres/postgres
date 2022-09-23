/*-------------------------------------------------------------------------
 *
 * pgoutput.c
 *		Logical Replication output plugin
 *
 * Copyright (c) 2012-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/replication/pgoutput/pgoutput.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupconvert.h"
#include "catalog/partition.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "replication/logical.h"
#include "replication/logicalproto.h"
#include "replication/origin.h"
#include "replication/pgoutput.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

static void pgoutput_startup(LogicalDecodingContext *ctx,
							 OutputPluginOptions *opt, bool is_init);
static void pgoutput_shutdown(LogicalDecodingContext *ctx);
static void pgoutput_begin_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn);
static void pgoutput_commit_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_change(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn, Relation rel,
							ReorderBufferChange *change);
static void pgoutput_truncate(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, int nrelations, Relation relations[],
							  ReorderBufferChange *change);
static void pgoutput_message(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn, XLogRecPtr message_lsn,
							 bool transactional, const char *prefix,
							 Size sz, const char *message);
static bool pgoutput_origin_filter(LogicalDecodingContext *ctx,
								   RepOriginId origin_id);
static void pgoutput_begin_prepare_txn(LogicalDecodingContext *ctx,
									   ReorderBufferTXN *txn);
static void pgoutput_prepare_txn(LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn, XLogRecPtr prepare_lsn);
static void pgoutput_commit_prepared_txn(LogicalDecodingContext *ctx,
										 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_rollback_prepared_txn(LogicalDecodingContext *ctx,
										   ReorderBufferTXN *txn,
										   XLogRecPtr prepare_end_lsn,
										   TimestampTz prepare_time);
static void pgoutput_stream_start(struct LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn);
static void pgoutput_stream_stop(struct LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn);
static void pgoutput_stream_abort(struct LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn,
								  XLogRecPtr abort_lsn);
static void pgoutput_stream_commit(struct LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn,
								   XLogRecPtr commit_lsn);
static void pgoutput_stream_prepare_txn(LogicalDecodingContext *ctx,
										ReorderBufferTXN *txn, XLogRecPtr prepare_lsn);

static bool publications_valid;
static bool in_streaming;

static List *LoadPublications(List *pubnames);
static void publication_invalidation_cb(Datum arg, int cacheid,
										uint32 hashvalue);
static void send_relation_and_attrs(Relation relation, TransactionId xid,
									LogicalDecodingContext *ctx,
									Bitmapset *columns);
static void send_repl_origin(LogicalDecodingContext *ctx,
							 RepOriginId origin_id, XLogRecPtr origin_lsn,
							 bool send_origin);
static void update_replication_progress(LogicalDecodingContext *ctx,
										bool skipped_xact);

/*
 * Only 3 publication actions are used for row filtering ("insert", "update",
 * "delete"). See RelationSyncEntry.exprstate[].
 */
enum RowFilterPubAction
{
	PUBACTION_INSERT,
	PUBACTION_UPDATE,
	PUBACTION_DELETE
};

#define NUM_ROWFILTER_PUBACTIONS (PUBACTION_DELETE+1)

/*
 * Entry in the map used to remember which relation schemas we sent.
 *
 * The schema_sent flag determines if the current schema record for the
 * relation (and for its ancestor if publish_as_relid is set) was already
 * sent to the subscriber (in which case we don't need to send it again).
 *
 * The schema cache on downstream is however updated only at commit time,
 * and with streamed transactions the commit order may be different from
 * the order the transactions are sent in. Also, the (sub) transactions
 * might get aborted so we need to send the schema for each (sub) transaction
 * so that we don't lose the schema information on abort. For handling this,
 * we maintain the list of xids (streamed_txns) for those we have already sent
 * the schema.
 *
 * For partitions, 'pubactions' considers not only the table's own
 * publications, but also those of all of its ancestors.
 */
typedef struct RelationSyncEntry
{
	Oid			relid;			/* relation oid */

	bool		replicate_valid;	/* overall validity flag for entry */

	bool		schema_sent;
	List	   *streamed_txns;	/* streamed toplevel transactions with this
								 * schema */

	/* are we publishing this rel? */
	PublicationActions pubactions;

	/*
	 * ExprState array for row filter. Different publication actions don't
	 * allow multiple expressions to always be combined into one, because
	 * updates or deletes restrict the column in expression to be part of the
	 * replica identity index whereas inserts do not have this restriction, so
	 * there is one ExprState per publication action.
	 */
	ExprState  *exprstate[NUM_ROWFILTER_PUBACTIONS];
	EState	   *estate;			/* executor state used for row filter */
	TupleTableSlot *new_slot;	/* slot for storing new tuple */
	TupleTableSlot *old_slot;	/* slot for storing old tuple */

	/*
	 * OID of the relation to publish changes as.  For a partition, this may
	 * be set to one of its ancestors whose schema will be used when
	 * replicating changes, if publish_via_partition_root is set for the
	 * publication.
	 */
	Oid			publish_as_relid;

	/*
	 * Map used when replicating using an ancestor's schema to convert tuples
	 * from partition's type to the ancestor's; NULL if publish_as_relid is
	 * same as 'relid' or if unnecessary due to partition and the ancestor
	 * having identical TupleDesc.
	 */
	AttrMap    *attrmap;

	/*
	 * Columns included in the publication, or NULL if all columns are
	 * included implicitly.  Note that the attnums in this bitmap are not
	 * shifted by FirstLowInvalidHeapAttributeNumber.
	 */
	Bitmapset  *columns;

	/*
	 * Private context to store additional data for this entry - state for the
	 * row filter expressions, column list, etc.
	 */
	MemoryContext entry_cxt;
} RelationSyncEntry;

/*
 * Maintain a per-transaction level variable to track whether the transaction
 * has sent BEGIN. BEGIN is only sent when the first change in a transaction
 * is processed. This makes it possible to skip sending a pair of BEGIN/COMMIT
 * messages for empty transactions which saves network bandwidth.
 *
 * This optimization is not used for prepared transactions because if the
 * WALSender restarts after prepare of a transaction and before commit prepared
 * of the same transaction then we won't be able to figure out if we have
 * skipped sending BEGIN/PREPARE of a transaction as it was empty. This is
 * because we would have lost the in-memory txndata information that was
 * present prior to the restart. This will result in sending a spurious
 * COMMIT PREPARED without a corresponding prepared transaction at the
 * downstream which would lead to an error when it tries to process it.
 *
 * XXX We could achieve this optimization by changing protocol to send
 * additional information so that downstream can detect that the corresponding
 * prepare has not been sent. However, adding such a check for every
 * transaction in the downstream could be costly so we might want to do it
 * optionally.
 *
 * We also don't have this optimization for streamed transactions because
 * they can contain prepared transactions.
 */
typedef struct PGOutputTxnData
{
	bool		sent_begin_txn; /* flag indicating whether BEGIN has been sent */
} PGOutputTxnData;

/* Map used to remember which relation schemas we sent. */
static HTAB *RelationSyncCache = NULL;

static void init_rel_sync_cache(MemoryContext decoding_context);
static void cleanup_rel_sync_cache(TransactionId xid, bool is_commit);
static RelationSyncEntry *get_rel_sync_entry(PGOutputData *data,
											 Relation relation);
static void rel_sync_cache_relation_cb(Datum arg, Oid relid);
static void rel_sync_cache_publication_cb(Datum arg, int cacheid,
										  uint32 hashvalue);
static void set_schema_sent_in_streamed_txn(RelationSyncEntry *entry,
											TransactionId xid);
static bool get_schema_sent_in_streamed_txn(RelationSyncEntry *entry,
											TransactionId xid);
static void init_tuple_slot(PGOutputData *data, Relation relation,
							RelationSyncEntry *entry);

/* row filter routines */
static EState *create_estate_for_relation(Relation rel);
static void pgoutput_row_filter_init(PGOutputData *data,
									 List *publications,
									 RelationSyncEntry *entry);
static bool pgoutput_row_filter_exec_expr(ExprState *state,
										  ExprContext *econtext);
static bool pgoutput_row_filter(Relation relation, TupleTableSlot *old_slot,
								TupleTableSlot **new_slot_ptr,
								RelationSyncEntry *entry,
								ReorderBufferChangeType *action);

/* column list routines */
static void pgoutput_column_list_init(PGOutputData *data,
									  List *publications,
									  RelationSyncEntry *entry);

/*
 * Specify output plugin callbacks
 */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

	cb->startup_cb = pgoutput_startup;
	cb->begin_cb = pgoutput_begin_txn;
	cb->change_cb = pgoutput_change;
	cb->truncate_cb = pgoutput_truncate;
	cb->message_cb = pgoutput_message;
	cb->commit_cb = pgoutput_commit_txn;

	cb->begin_prepare_cb = pgoutput_begin_prepare_txn;
	cb->prepare_cb = pgoutput_prepare_txn;
	cb->commit_prepared_cb = pgoutput_commit_prepared_txn;
	cb->rollback_prepared_cb = pgoutput_rollback_prepared_txn;
	cb->filter_by_origin_cb = pgoutput_origin_filter;
	cb->shutdown_cb = pgoutput_shutdown;

	/* transaction streaming */
	cb->stream_start_cb = pgoutput_stream_start;
	cb->stream_stop_cb = pgoutput_stream_stop;
	cb->stream_abort_cb = pgoutput_stream_abort;
	cb->stream_commit_cb = pgoutput_stream_commit;
	cb->stream_change_cb = pgoutput_change;
	cb->stream_message_cb = pgoutput_message;
	cb->stream_truncate_cb = pgoutput_truncate;
	/* transaction streaming - two-phase commit */
	cb->stream_prepare_cb = pgoutput_stream_prepare_txn;
}

static void
parse_output_parameters(List *options, PGOutputData *data)
{
	ListCell   *lc;
	bool		protocol_version_given = false;
	bool		publication_names_given = false;
	bool		binary_option_given = false;
	bool		messages_option_given = false;
	bool		streaming_given = false;
	bool		two_phase_option_given = false;

	data->binary = false;
	data->streaming = false;
	data->messages = false;
	data->two_phase = false;

	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		Assert(defel->arg == NULL || IsA(defel->arg, String));

		/* Check each param, whether or not we recognize it */
		if (strcmp(defel->defname, "proto_version") == 0)
		{
			unsigned long parsed;
			char	   *endptr;

			if (protocol_version_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			protocol_version_given = true;

			errno = 0;
			parsed = strtoul(strVal(defel->arg), &endptr, 10);
			if (errno != 0 || *endptr != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid proto_version")));

			if (parsed > PG_UINT32_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("proto_version \"%s\" out of range",
								strVal(defel->arg))));

			data->protocol_version = (uint32) parsed;
		}
		else if (strcmp(defel->defname, "publication_names") == 0)
		{
			if (publication_names_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			publication_names_given = true;

			if (!SplitIdentifierString(strVal(defel->arg), ',',
									   &data->publication_names))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("invalid publication_names syntax")));
		}
		else if (strcmp(defel->defname, "binary") == 0)
		{
			if (binary_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			binary_option_given = true;

			data->binary = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "messages") == 0)
		{
			if (messages_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			messages_option_given = true;

			data->messages = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "streaming") == 0)
		{
			if (streaming_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			streaming_given = true;

			data->streaming = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_option_given = true;

			data->two_phase = defGetBoolean(defel);
		}
		else
			elog(ERROR, "unrecognized pgoutput option: %s", defel->defname);
	}
}

/*
 * Initialize this plugin
 */
static void
pgoutput_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				 bool is_init)
{
	PGOutputData *data = palloc0(sizeof(PGOutputData));

	/* Create our memory context for private allocations. */
	data->context = AllocSetContextCreate(ctx->context,
										  "logical replication output context",
										  ALLOCSET_DEFAULT_SIZES);

	data->cachectx = AllocSetContextCreate(ctx->context,
										   "logical replication cache context",
										   ALLOCSET_DEFAULT_SIZES);

	ctx->output_plugin_private = data;

	/* This plugin uses binary protocol. */
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

	/*
	 * This is replication start and not slot initialization.
	 *
	 * Parse and validate options passed by the client.
	 */
	if (!is_init)
	{
		/* Parse the params and ERROR if we see any we don't recognize */
		parse_output_parameters(ctx->output_plugin_options, data);

		/* Check if we support requested protocol */
		if (data->protocol_version > LOGICALREP_PROTO_MAX_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but we only support protocol %d or lower",
							data->protocol_version, LOGICALREP_PROTO_MAX_VERSION_NUM)));

		if (data->protocol_version < LOGICALREP_PROTO_MIN_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but we only support protocol %d or higher",
							data->protocol_version, LOGICALREP_PROTO_MIN_VERSION_NUM)));

		if (list_length(data->publication_names) < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("publication_names parameter missing")));

		/*
		 * Decide whether to enable streaming. It is disabled by default, in
		 * which case we just update the flag in decoding context. Otherwise
		 * we only allow it with sufficient version of the protocol, and when
		 * the output plugin supports it.
		 */
		if (!data->streaming)
			ctx->streaming = false;
		else if (data->protocol_version < LOGICALREP_PROTO_STREAM_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support streaming, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_STREAM_VERSION_NUM)));
		else if (!ctx->streaming)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("streaming requested, but not supported by output plugin")));

		/* Also remember we're currently not streaming any transaction. */
		in_streaming = false;

		/*
		 * Here, we just check whether the two-phase option is passed by
		 * plugin and decide whether to enable it at later point of time. It
		 * remains enabled if the previous start-up has done so. But we only
		 * allow the option to be passed in with sufficient version of the
		 * protocol, and when the output plugin supports it.
		 */
		if (!data->two_phase)
			ctx->twophase_opt_given = false;
		else if (data->protocol_version < LOGICALREP_PROTO_TWOPHASE_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support two-phase commit, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_TWOPHASE_VERSION_NUM)));
		else if (!ctx->twophase)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("two-phase commit requested, but not supported by output plugin")));
		else
			ctx->twophase_opt_given = true;

		/* Init publication state. */
		data->publications = NIL;
		publications_valid = false;
		CacheRegisterSyscacheCallback(PUBLICATIONOID,
									  publication_invalidation_cb,
									  (Datum) 0);

		/* Initialize relation schema cache. */
		init_rel_sync_cache(CacheMemoryContext);
	}
	else
	{
		/*
		 * Disable the streaming and prepared transactions during the slot
		 * initialization mode.
		 */
		ctx->streaming = false;
		ctx->twophase = false;
	}
}

/*
 * BEGIN callback.
 *
 * Don't send the BEGIN message here instead postpone it until the first
 * change. In logical replication, a common scenario is to replicate a set of
 * tables (instead of all tables) and transactions whose changes were on
 * the table(s) that are not published will produce empty transactions. These
 * empty transactions will send BEGIN and COMMIT messages to subscribers,
 * using bandwidth on something with little/no use for logical replication.
 */
static void
pgoutput_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	PGOutputTxnData *txndata = MemoryContextAllocZero(ctx->context,
													  sizeof(PGOutputTxnData));

	txn->output_plugin_private = txndata;
}

/*
 * Send BEGIN.
 *
 * This is called while processing the first change of the transaction.
 */
static void
pgoutput_send_begin(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;

	Assert(txndata);
	Assert(!txndata->sent_begin_txn);

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin(ctx->out, txn);
	txndata->sent_begin_txn = true;

	send_repl_origin(ctx, txn->origin_id, txn->origin_lsn,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT callback
 */
static void
pgoutput_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					XLogRecPtr commit_lsn)
{
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	bool		sent_begin_txn;

	Assert(txndata);

	/*
	 * We don't need to send the commit message unless some relevant change
	 * from this transaction has been sent to the downstream.
	 */
	sent_begin_txn = txndata->sent_begin_txn;
	update_replication_progress(ctx, !sent_begin_txn);
	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (!sent_begin_txn)
	{
		elog(DEBUG1, "skipped replication of an empty transaction with XID: %u", txn->xid);
		return;
	}

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * BEGIN PREPARE callback
 */
static void
pgoutput_begin_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin_prepare(ctx->out, txn);

	send_repl_origin(ctx, txn->origin_id, txn->origin_lsn,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);
}

/*
 * PREPARE callback
 */
static void
pgoutput_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr prepare_lsn)
{
	update_replication_progress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_prepare(ctx->out, txn, prepare_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT PREPARED callback
 */
static void
pgoutput_commit_prepared_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
							 XLogRecPtr commit_lsn)
{
	update_replication_progress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit_prepared(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * ROLLBACK PREPARED callback
 */
static void
pgoutput_rollback_prepared_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn,
							   XLogRecPtr prepare_end_lsn,
							   TimestampTz prepare_time)
{
	update_replication_progress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_rollback_prepared(ctx->out, txn, prepare_end_lsn,
									   prepare_time);
	OutputPluginWrite(ctx, true);
}

/*
 * Write the current schema of the relation and its ancestor (if any) if not
 * done yet.
 */
static void
maybe_send_schema(LogicalDecodingContext *ctx,
				  ReorderBufferChange *change,
				  Relation relation, RelationSyncEntry *relentry)
{
	bool		schema_sent;
	TransactionId xid = InvalidTransactionId;
	TransactionId topxid = InvalidTransactionId;

	/*
	 * Remember XID of the (sub)transaction for the change. We don't care if
	 * it's top-level transaction or not (we have already sent that XID in
	 * start of the current streaming block).
	 *
	 * If we're not in a streaming block, just use InvalidTransactionId and
	 * the write methods will not include it.
	 */
	if (in_streaming)
		xid = change->txn->xid;

	if (change->txn->toptxn)
		topxid = change->txn->toptxn->xid;
	else
		topxid = xid;

	/*
	 * Do we need to send the schema? We do track streamed transactions
	 * separately, because those may be applied later (and the regular
	 * transactions won't see their effects until then) and in an order that
	 * we don't know at this point.
	 *
	 * XXX There is a scope of optimization here. Currently, we always send
	 * the schema first time in a streaming transaction but we can probably
	 * avoid that by checking 'relentry->schema_sent' flag. However, before
	 * doing that we need to study its impact on the case where we have a mix
	 * of streaming and non-streaming transactions.
	 */
	if (in_streaming)
		schema_sent = get_schema_sent_in_streamed_txn(relentry, topxid);
	else
		schema_sent = relentry->schema_sent;

	/* Nothing to do if we already sent the schema. */
	if (schema_sent)
		return;

	/*
	 * Send the schema.  If the changes will be published using an ancestor's
	 * schema, not the relation's own, send that ancestor's schema before
	 * sending relation's own (XXX - maybe sending only the former suffices?).
	 */
	if (relentry->publish_as_relid != RelationGetRelid(relation))
	{
		Relation	ancestor = RelationIdGetRelation(relentry->publish_as_relid);

		send_relation_and_attrs(ancestor, xid, ctx, relentry->columns);
		RelationClose(ancestor);
	}

	send_relation_and_attrs(relation, xid, ctx, relentry->columns);

	if (in_streaming)
		set_schema_sent_in_streamed_txn(relentry, topxid);
	else
		relentry->schema_sent = true;
}

/*
 * Sends a relation
 */
static void
send_relation_and_attrs(Relation relation, TransactionId xid,
						LogicalDecodingContext *ctx,
						Bitmapset *columns)
{
	TupleDesc	desc = RelationGetDescr(relation);
	int			i;

	/*
	 * Write out type info if needed.  We do that only for user-created types.
	 * We use FirstGenbkiObjectId as the cutoff, so that we only consider
	 * objects with hand-assigned OIDs to be "built in", not for instance any
	 * function or type defined in the information_schema. This is important
	 * because only hand-assigned OIDs can be expected to remain stable across
	 * major versions.
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || att->attgenerated)
			continue;

		if (att->atttypid < FirstGenbkiObjectId)
			continue;

		/* Skip this attribute if it's not present in the column list */
		if (columns != NULL && !bms_is_member(att->attnum, columns))
			continue;

		OutputPluginPrepareWrite(ctx, false);
		logicalrep_write_typ(ctx->out, xid, att->atttypid);
		OutputPluginWrite(ctx, false);
	}

	OutputPluginPrepareWrite(ctx, false);
	logicalrep_write_rel(ctx->out, xid, relation, columns);
	OutputPluginWrite(ctx, false);
}

/*
 * Executor state preparation for evaluation of row filter expressions for the
 * specified relation.
 */
static EState *
create_estate_for_relation(Relation rel)
{
	EState	   *estate;
	RangeTblEntry *rte;

	estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;
	ExecInitRangeTable(estate, list_make1(rte));

	estate->es_output_cid = GetCurrentCommandId(false);

	return estate;
}

/*
 * Evaluates row filter.
 *
 * If the row filter evaluates to NULL, it is taken as false i.e. the change
 * isn't replicated.
 */
static bool
pgoutput_row_filter_exec_expr(ExprState *state, ExprContext *econtext)
{
	Datum		ret;
	bool		isnull;

	Assert(state != NULL);

	ret = ExecEvalExprSwitchContext(state, econtext, &isnull);

	elog(DEBUG3, "row filter evaluates to %s (isnull: %s)",
		 isnull ? "false" : DatumGetBool(ret) ? "true" : "false",
		 isnull ? "true" : "false");

	if (isnull)
		return false;

	return DatumGetBool(ret);
}

/*
 * Make sure the per-entry memory context exists.
 */
static void
pgoutput_ensure_entry_cxt(PGOutputData *data, RelationSyncEntry *entry)
{
	Relation	relation;

	/* The context may already exist, in which case bail out. */
	if (entry->entry_cxt)
		return;

	relation = RelationIdGetRelation(entry->publish_as_relid);

	entry->entry_cxt = AllocSetContextCreate(data->cachectx,
											 "entry private context",
											 ALLOCSET_SMALL_SIZES);

	MemoryContextCopyAndSetIdentifier(entry->entry_cxt,
									  RelationGetRelationName(relation));
}

/*
 * Initialize the row filter.
 */
static void
pgoutput_row_filter_init(PGOutputData *data, List *publications,
						 RelationSyncEntry *entry)
{
	ListCell   *lc;
	List	   *rfnodes[] = {NIL, NIL, NIL};	/* One per pubaction */
	bool		no_filter[] = {false, false, false};	/* One per pubaction */
	MemoryContext oldctx;
	int			idx;
	bool		has_filter = true;
	Oid			schemaid = get_rel_namespace(entry->publish_as_relid);

	/*
	 * Find if there are any row filters for this relation. If there are, then
	 * prepare the necessary ExprState and cache it in entry->exprstate. To
	 * build an expression state, we need to ensure the following:
	 *
	 * All the given publication-table mappings must be checked.
	 *
	 * Multiple publications might have multiple row filters for this
	 * relation. Since row filter usage depends on the DML operation, there
	 * are multiple lists (one for each operation) to which row filters will
	 * be appended.
	 *
	 * FOR ALL TABLES and FOR TABLES IN SCHEMA implies "don't use row
	 * filter expression" so it takes precedence.
	 */
	foreach(lc, publications)
	{
		Publication *pub = lfirst(lc);
		HeapTuple	rftuple = NULL;
		Datum		rfdatum = 0;
		bool		pub_no_filter = true;

		/*
		 * If the publication is FOR ALL TABLES, or the publication includes a
		 * FOR TABLES IN SCHEMA where the table belongs to the referred
		 * schema, then it is treated the same as if there are no row filters
		 * (even if other publications have a row filter).
		 */
		if (!pub->alltables &&
			!SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
								   ObjectIdGetDatum(schemaid),
								   ObjectIdGetDatum(pub->oid)))
		{
			/*
			 * Check for the presence of a row filter in this publication.
			 */
			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(entry->publish_as_relid),
									  ObjectIdGetDatum(pub->oid));

			if (HeapTupleIsValid(rftuple))
			{
				/* Null indicates no filter. */
				rfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
										  Anum_pg_publication_rel_prqual,
										  &pub_no_filter);
			}
		}

		if (pub_no_filter)
		{
			if (rftuple)
				ReleaseSysCache(rftuple);

			no_filter[PUBACTION_INSERT] |= pub->pubactions.pubinsert;
			no_filter[PUBACTION_UPDATE] |= pub->pubactions.pubupdate;
			no_filter[PUBACTION_DELETE] |= pub->pubactions.pubdelete;

			/*
			 * Quick exit if all the DML actions are publicized via this
			 * publication.
			 */
			if (no_filter[PUBACTION_INSERT] &&
				no_filter[PUBACTION_UPDATE] &&
				no_filter[PUBACTION_DELETE])
			{
				has_filter = false;
				break;
			}

			/* No additional work for this publication. Next one. */
			continue;
		}

		/* Form the per pubaction row filter lists. */
		if (pub->pubactions.pubinsert && !no_filter[PUBACTION_INSERT])
			rfnodes[PUBACTION_INSERT] = lappend(rfnodes[PUBACTION_INSERT],
												TextDatumGetCString(rfdatum));
		if (pub->pubactions.pubupdate && !no_filter[PUBACTION_UPDATE])
			rfnodes[PUBACTION_UPDATE] = lappend(rfnodes[PUBACTION_UPDATE],
												TextDatumGetCString(rfdatum));
		if (pub->pubactions.pubdelete && !no_filter[PUBACTION_DELETE])
			rfnodes[PUBACTION_DELETE] = lappend(rfnodes[PUBACTION_DELETE],
												TextDatumGetCString(rfdatum));

		ReleaseSysCache(rftuple);
	}							/* loop all subscribed publications */

	/* Clean the row filter */
	for (idx = 0; idx < NUM_ROWFILTER_PUBACTIONS; idx++)
	{
		if (no_filter[idx])
		{
			list_free_deep(rfnodes[idx]);
			rfnodes[idx] = NIL;
		}
	}

	if (has_filter)
	{
		Relation	relation = RelationIdGetRelation(entry->publish_as_relid);

		pgoutput_ensure_entry_cxt(data, entry);

		/*
		 * Now all the filters for all pubactions are known. Combine them when
		 * their pubactions are the same.
		 */
		oldctx = MemoryContextSwitchTo(entry->entry_cxt);
		entry->estate = create_estate_for_relation(relation);
		for (idx = 0; idx < NUM_ROWFILTER_PUBACTIONS; idx++)
		{
			List	   *filters = NIL;
			Expr	   *rfnode;

			if (rfnodes[idx] == NIL)
				continue;

			foreach(lc, rfnodes[idx])
				filters = lappend(filters, stringToNode((char *) lfirst(lc)));

			/* combine the row filter and cache the ExprState */
			rfnode = make_orclause(filters);
			entry->exprstate[idx] = ExecPrepareExpr(rfnode, entry->estate);
		}						/* for each pubaction */
		MemoryContextSwitchTo(oldctx);

		RelationClose(relation);
	}
}

/*
 * Initialize the column list.
 */
static void
pgoutput_column_list_init(PGOutputData *data, List *publications,
						  RelationSyncEntry *entry)
{
	ListCell   *lc;
	bool		first = true;
	Relation	relation = RelationIdGetRelation(entry->publish_as_relid);

	/*
	 * Find if there are any column lists for this relation. If there are,
	 * build a bitmap using the column lists.
	 *
	 * Multiple publications might have multiple column lists for this
	 * relation.
	 *
	 * Note that we don't support the case where the column list is different
	 * for the same table when combining publications. See comments atop
	 * fetch_table_list. But one can later change the publication so we still
	 * need to check all the given publication-table mappings and report an
	 * error if any publications have a different column list.
	 *
	 * FOR ALL TABLES and FOR TABLES IN SCHEMA imply "don't use column list".
	 */
	foreach(lc, publications)
	{
		Publication *pub = lfirst(lc);
		HeapTuple	cftuple = NULL;
		Datum		cfdatum = 0;
		Bitmapset  *cols = NULL;

		/*
		 * If the publication is FOR ALL TABLES then it is treated the same as
		 * if there are no column lists (even if other publications have a
		 * list).
		 */
		if (!pub->alltables)
		{
			bool		pub_no_list = true;

			/*
			 * Check for the presence of a column list in this publication.
			 *
			 * Note: If we find no pg_publication_rel row, it's a publication
			 * defined for a whole schema, so it can't have a column list,
			 * just like a FOR ALL TABLES publication.
			 */
			cftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(entry->publish_as_relid),
									  ObjectIdGetDatum(pub->oid));

			if (HeapTupleIsValid(cftuple))
			{
				/* Lookup the column list attribute. */
				cfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, cftuple,
										  Anum_pg_publication_rel_prattrs,
										  &pub_no_list);

				/* Build the column list bitmap in the per-entry context. */
				if (!pub_no_list)	/* when not null */
				{
					pgoutput_ensure_entry_cxt(data, entry);

					cols = pub_collist_to_bitmapset(cols, cfdatum,
													entry->entry_cxt);

					/*
					 * If column list includes all the columns of the table,
					 * set it to NULL.
					 */
					if (bms_num_members(cols) == RelationGetNumberOfAttributes(relation))
					{
						bms_free(cols);
						cols = NULL;
					}
				}

				ReleaseSysCache(cftuple);
			}
		}

		if (first)
		{
			entry->columns = cols;
			first = false;
		}
		else if (!bms_equal(entry->columns, cols))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   get_namespace_name(RelationGetNamespace(relation)),
						   RelationGetRelationName(relation)));
	}							/* loop all subscribed publications */

	RelationClose(relation);
}

/*
 * Initialize the slot for storing new and old tuples, and build the map that
 * will be used to convert the relation's tuples into the ancestor's format.
 */
static void
init_tuple_slot(PGOutputData *data, Relation relation,
				RelationSyncEntry *entry)
{
	MemoryContext oldctx;
	TupleDesc	oldtupdesc;
	TupleDesc	newtupdesc;

	oldctx = MemoryContextSwitchTo(data->cachectx);

	/*
	 * Create tuple table slots. Create a copy of the TupleDesc as it needs to
	 * live as long as the cache remains.
	 */
	oldtupdesc = CreateTupleDescCopy(RelationGetDescr(relation));
	newtupdesc = CreateTupleDescCopy(RelationGetDescr(relation));

	entry->old_slot = MakeSingleTupleTableSlot(oldtupdesc, &TTSOpsHeapTuple);
	entry->new_slot = MakeSingleTupleTableSlot(newtupdesc, &TTSOpsHeapTuple);

	MemoryContextSwitchTo(oldctx);

	/*
	 * Cache the map that will be used to convert the relation's tuples into
	 * the ancestor's format, if needed.
	 */
	if (entry->publish_as_relid != RelationGetRelid(relation))
	{
		Relation	ancestor = RelationIdGetRelation(entry->publish_as_relid);
		TupleDesc	indesc = RelationGetDescr(relation);
		TupleDesc	outdesc = RelationGetDescr(ancestor);

		/* Map must live as long as the session does. */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);

		entry->attrmap = build_attrmap_by_name_if_req(indesc, outdesc);

		MemoryContextSwitchTo(oldctx);
		RelationClose(ancestor);
	}
}

/*
 * Change is checked against the row filter if any.
 *
 * Returns true if the change is to be replicated, else false.
 *
 * For inserts, evaluate the row filter for new tuple.
 * For deletes, evaluate the row filter for old tuple.
 * For updates, evaluate the row filter for old and new tuple.
 *
 * For updates, if both evaluations are true, we allow sending the UPDATE and
 * if both the evaluations are false, it doesn't replicate the UPDATE. Now, if
 * only one of the tuples matches the row filter expression, we transform
 * UPDATE to DELETE or INSERT to avoid any data inconsistency based on the
 * following rules:
 *
 * Case 1: old-row (no match)    new-row (no match)  -> (drop change)
 * Case 2: old-row (no match)    new row (match)     -> INSERT
 * Case 3: old-row (match)       new-row (no match)  -> DELETE
 * Case 4: old-row (match)       new row (match)     -> UPDATE
 *
 * The new action is updated in the action parameter.
 *
 * The new slot could be updated when transforming the UPDATE into INSERT,
 * because the original new tuple might not have column values from the replica
 * identity.
 *
 * Examples:
 * Let's say the old tuple satisfies the row filter but the new tuple doesn't.
 * Since the old tuple satisfies, the initial table synchronization copied this
 * row (or another method was used to guarantee that there is data
 * consistency).  However, after the UPDATE the new tuple doesn't satisfy the
 * row filter, so from a data consistency perspective, that row should be
 * removed on the subscriber. The UPDATE should be transformed into a DELETE
 * statement and be sent to the subscriber. Keeping this row on the subscriber
 * is undesirable because it doesn't reflect what was defined in the row filter
 * expression on the publisher. This row on the subscriber would likely not be
 * modified by replication again. If someone inserted a new row with the same
 * old identifier, replication could stop due to a constraint violation.
 *
 * Let's say the old tuple doesn't match the row filter but the new tuple does.
 * Since the old tuple doesn't satisfy, the initial table synchronization
 * probably didn't copy this row. However, after the UPDATE the new tuple does
 * satisfy the row filter, so from a data consistency perspective, that row
 * should be inserted on the subscriber. Otherwise, subsequent UPDATE or DELETE
 * statements have no effect (it matches no row -- see
 * apply_handle_update_internal()). So, the UPDATE should be transformed into a
 * INSERT statement and be sent to the subscriber. However, this might surprise
 * someone who expects the data set to satisfy the row filter expression on the
 * provider.
 */
static bool
pgoutput_row_filter(Relation relation, TupleTableSlot *old_slot,
					TupleTableSlot **new_slot_ptr, RelationSyncEntry *entry,
					ReorderBufferChangeType *action)
{
	TupleDesc	desc;
	int			i;
	bool		old_matched,
				new_matched,
				result;
	TupleTableSlot *tmp_new_slot;
	TupleTableSlot *new_slot = *new_slot_ptr;
	ExprContext *ecxt;
	ExprState  *filter_exprstate;

	/*
	 * We need this map to avoid relying on ReorderBufferChangeType enums
	 * having specific values.
	 */
	static const int map_changetype_pubaction[] = {
		[REORDER_BUFFER_CHANGE_INSERT] = PUBACTION_INSERT,
		[REORDER_BUFFER_CHANGE_UPDATE] = PUBACTION_UPDATE,
		[REORDER_BUFFER_CHANGE_DELETE] = PUBACTION_DELETE
	};

	Assert(*action == REORDER_BUFFER_CHANGE_INSERT ||
		   *action == REORDER_BUFFER_CHANGE_UPDATE ||
		   *action == REORDER_BUFFER_CHANGE_DELETE);

	Assert(new_slot || old_slot);

	/* Get the corresponding row filter */
	filter_exprstate = entry->exprstate[map_changetype_pubaction[*action]];

	/* Bail out if there is no row filter */
	if (!filter_exprstate)
		return true;

	elog(DEBUG3, "table \"%s.%s\" has row filter",
		 get_namespace_name(RelationGetNamespace(relation)),
		 RelationGetRelationName(relation));

	ResetPerTupleExprContext(entry->estate);

	ecxt = GetPerTupleExprContext(entry->estate);

	/*
	 * For the following occasions where there is only one tuple, we can
	 * evaluate the row filter for that tuple and return.
	 *
	 * For inserts, we only have the new tuple.
	 *
	 * For updates, we can have only a new tuple when none of the replica
	 * identity columns changed and none of those columns have external data
	 * but we still need to evaluate the row filter for the new tuple as the
	 * existing values of those columns might not match the filter. Also,
	 * users can use constant expressions in the row filter, so we anyway need
	 * to evaluate it for the new tuple.
	 *
	 * For deletes, we only have the old tuple.
	 */
	if (!new_slot || !old_slot)
	{
		ecxt->ecxt_scantuple = new_slot ? new_slot : old_slot;
		result = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

		return result;
	}

	/*
	 * Both the old and new tuples must be valid only for updates and need to
	 * be checked against the row filter.
	 */
	Assert(map_changetype_pubaction[*action] == PUBACTION_UPDATE);

	slot_getallattrs(new_slot);
	slot_getallattrs(old_slot);

	tmp_new_slot = NULL;
	desc = RelationGetDescr(relation);

	/*
	 * The new tuple might not have all the replica identity columns, in which
	 * case it needs to be copied over from the old tuple.
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		/*
		 * if the column in the new tuple or old tuple is null, nothing to do
		 */
		if (new_slot->tts_isnull[i] || old_slot->tts_isnull[i])
			continue;

		/*
		 * Unchanged toasted replica identity columns are only logged in the
		 * old tuple. Copy this over to the new tuple. The changed (or WAL
		 * Logged) toast values are always assembled in memory and set as
		 * VARTAG_INDIRECT. See ReorderBufferToastReplace.
		 */
		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_ONDISK(new_slot->tts_values[i]) &&
			!VARATT_IS_EXTERNAL_ONDISK(old_slot->tts_values[i]))
		{
			if (!tmp_new_slot)
			{
				tmp_new_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
				ExecClearTuple(tmp_new_slot);

				memcpy(tmp_new_slot->tts_values, new_slot->tts_values,
					   desc->natts * sizeof(Datum));
				memcpy(tmp_new_slot->tts_isnull, new_slot->tts_isnull,
					   desc->natts * sizeof(bool));
			}

			tmp_new_slot->tts_values[i] = old_slot->tts_values[i];
			tmp_new_slot->tts_isnull[i] = old_slot->tts_isnull[i];
		}
	}

	ecxt->ecxt_scantuple = old_slot;
	old_matched = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

	if (tmp_new_slot)
	{
		ExecStoreVirtualTuple(tmp_new_slot);
		ecxt->ecxt_scantuple = tmp_new_slot;
	}
	else
		ecxt->ecxt_scantuple = new_slot;

	new_matched = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

	/*
	 * Case 1: if both tuples don't match the row filter, bailout. Send
	 * nothing.
	 */
	if (!old_matched && !new_matched)
		return false;

	/*
	 * Case 2: if the old tuple doesn't satisfy the row filter but the new
	 * tuple does, transform the UPDATE into INSERT.
	 *
	 * Use the newly transformed tuple that must contain the column values for
	 * all the replica identity columns. This is required to ensure that the
	 * while inserting the tuple in the downstream node, we have all the
	 * required column values.
	 */
	if (!old_matched && new_matched)
	{
		*action = REORDER_BUFFER_CHANGE_INSERT;

		if (tmp_new_slot)
			*new_slot_ptr = tmp_new_slot;
	}

	/*
	 * Case 3: if the old tuple satisfies the row filter but the new tuple
	 * doesn't, transform the UPDATE into DELETE.
	 *
	 * This transformation does not require another tuple. The Old tuple will
	 * be used for DELETE.
	 */
	else if (old_matched && !new_matched)
		*action = REORDER_BUFFER_CHANGE_DELETE;

	/*
	 * Case 4: if both tuples match the row filter, transformation isn't
	 * required. (*action is default UPDATE).
	 */

	return true;
}

/*
 * Sends the decoded DML over wire.
 *
 * This is called both in streaming and non-streaming modes.
 */
static void
pgoutput_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				Relation relation, ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	TransactionId xid = InvalidTransactionId;
	Relation	ancestor = NULL;
	Relation	targetrel = relation;
	ReorderBufferChangeType action = change->action;
	TupleTableSlot *old_slot = NULL;
	TupleTableSlot *new_slot = NULL;

	update_replication_progress(ctx, false);

	if (!is_publishable_relation(relation))
		return;

	/*
	 * Remember the xid for the change in streaming mode. We need to send xid
	 * with each change in the streaming mode so that subscriber can make
	 * their association and on aborts, it can discard the corresponding
	 * changes.
	 */
	if (in_streaming)
		xid = change->txn->xid;

	relentry = get_rel_sync_entry(data, relation);

	/* First check the table filter */
	switch (action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (!relentry->pubactions.pubinsert)
				return;
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			if (!relentry->pubactions.pubupdate)
				return;
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			if (!relentry->pubactions.pubdelete)
				return;
			break;
		default:
			Assert(false);
	}

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	/* Send the data */
	switch (action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			new_slot = relentry->new_slot;
			ExecStoreHeapTuple(&change->data.tp.newtuple->tuple,
							   new_slot, false);

			/* Switch relation if publishing via root. */
			if (relentry->publish_as_relid != RelationGetRelid(relation))
			{
				Assert(relation->rd_rel->relispartition);
				ancestor = RelationIdGetRelation(relentry->publish_as_relid);
				targetrel = ancestor;
				/* Convert tuple if needed. */
				if (relentry->attrmap)
				{
					TupleDesc	tupdesc = RelationGetDescr(targetrel);

					new_slot = execute_attr_map_slot(relentry->attrmap,
													 new_slot,
													 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
				}
			}

			/* Check row filter */
			if (!pgoutput_row_filter(targetrel, NULL, &new_slot, relentry,
									 &action))
				break;

			/*
			 * Send BEGIN if we haven't yet.
			 *
			 * We send the BEGIN message after ensuring that we will actually
			 * send the change. This avoids sending a pair of BEGIN/COMMIT
			 * messages for empty transactions.
			 */
			if (txndata && !txndata->sent_begin_txn)
				pgoutput_send_begin(ctx, txn);

			/*
			 * Schema should be sent using the original relation because it
			 * also sends the ancestor's relation.
			 */
			maybe_send_schema(ctx, change, relation, relentry);

			OutputPluginPrepareWrite(ctx, true);
			logicalrep_write_insert(ctx->out, xid, targetrel, new_slot,
									data->binary, relentry->columns);
			OutputPluginWrite(ctx, true);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			if (change->data.tp.oldtuple)
			{
				old_slot = relentry->old_slot;
				ExecStoreHeapTuple(&change->data.tp.oldtuple->tuple,
								   old_slot, false);
			}

			new_slot = relentry->new_slot;
			ExecStoreHeapTuple(&change->data.tp.newtuple->tuple,
							   new_slot, false);

			/* Switch relation if publishing via root. */
			if (relentry->publish_as_relid != RelationGetRelid(relation))
			{
				Assert(relation->rd_rel->relispartition);
				ancestor = RelationIdGetRelation(relentry->publish_as_relid);
				targetrel = ancestor;
				/* Convert tuples if needed. */
				if (relentry->attrmap)
				{
					TupleDesc	tupdesc = RelationGetDescr(targetrel);

					if (old_slot)
						old_slot = execute_attr_map_slot(relentry->attrmap,
														 old_slot,
														 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));

					new_slot = execute_attr_map_slot(relentry->attrmap,
													 new_slot,
													 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
				}
			}

			/* Check row filter */
			if (!pgoutput_row_filter(targetrel, old_slot, &new_slot,
									 relentry, &action))
				break;

			/* Send BEGIN if we haven't yet */
			if (txndata && !txndata->sent_begin_txn)
				pgoutput_send_begin(ctx, txn);

			maybe_send_schema(ctx, change, relation, relentry);

			OutputPluginPrepareWrite(ctx, true);

			/*
			 * Updates could be transformed to inserts or deletes based on the
			 * results of the row filter for old and new tuple.
			 */
			switch (action)
			{
				case REORDER_BUFFER_CHANGE_INSERT:
					logicalrep_write_insert(ctx->out, xid, targetrel,
											new_slot, data->binary,
											relentry->columns);
					break;
				case REORDER_BUFFER_CHANGE_UPDATE:
					logicalrep_write_update(ctx->out, xid, targetrel,
											old_slot, new_slot, data->binary,
											relentry->columns);
					break;
				case REORDER_BUFFER_CHANGE_DELETE:
					logicalrep_write_delete(ctx->out, xid, targetrel,
											old_slot, data->binary);
					break;
				default:
					Assert(false);
			}

			OutputPluginWrite(ctx, true);
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			if (change->data.tp.oldtuple)
			{
				old_slot = relentry->old_slot;

				ExecStoreHeapTuple(&change->data.tp.oldtuple->tuple,
								   old_slot, false);

				/* Switch relation if publishing via root. */
				if (relentry->publish_as_relid != RelationGetRelid(relation))
				{
					Assert(relation->rd_rel->relispartition);
					ancestor = RelationIdGetRelation(relentry->publish_as_relid);
					targetrel = ancestor;
					/* Convert tuple if needed. */
					if (relentry->attrmap)
					{
						TupleDesc	tupdesc = RelationGetDescr(targetrel);

						old_slot = execute_attr_map_slot(relentry->attrmap,
														 old_slot,
														 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
					}
				}

				/* Check row filter */
				if (!pgoutput_row_filter(targetrel, old_slot, &new_slot,
										 relentry, &action))
					break;

				/* Send BEGIN if we haven't yet */
				if (txndata && !txndata->sent_begin_txn)
					pgoutput_send_begin(ctx, txn);

				maybe_send_schema(ctx, change, relation, relentry);

				OutputPluginPrepareWrite(ctx, true);
				logicalrep_write_delete(ctx->out, xid, targetrel,
										old_slot, data->binary);
				OutputPluginWrite(ctx, true);
			}
			else
				elog(DEBUG1, "didn't send DELETE change because of missing oldtuple");
			break;
		default:
			Assert(false);
	}

	if (RelationIsValid(ancestor))
	{
		RelationClose(ancestor);
		ancestor = NULL;
	}

	/* Cleanup */
	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

static void
pgoutput_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				  int nrelations, Relation relations[], ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	int			i;
	int			nrelids;
	Oid		   *relids;
	TransactionId xid = InvalidTransactionId;

	update_replication_progress(ctx, false);

	/* Remember the xid for the change in streaming mode. See pgoutput_change. */
	if (in_streaming)
		xid = change->txn->xid;

	old = MemoryContextSwitchTo(data->context);

	relids = palloc0(nrelations * sizeof(Oid));
	nrelids = 0;

	for (i = 0; i < nrelations; i++)
	{
		Relation	relation = relations[i];
		Oid			relid = RelationGetRelid(relation);

		if (!is_publishable_relation(relation))
			continue;

		relentry = get_rel_sync_entry(data, relation);

		if (!relentry->pubactions.pubtruncate)
			continue;

		/*
		 * Don't send partitions if the publication wants to send only the
		 * root tables through it.
		 */
		if (relation->rd_rel->relispartition &&
			relentry->publish_as_relid != relid)
			continue;

		relids[nrelids++] = relid;

		/* Send BEGIN if we haven't yet */
		if (txndata && !txndata->sent_begin_txn)
			pgoutput_send_begin(ctx, txn);

		maybe_send_schema(ctx, change, relation, relentry);
	}

	if (nrelids > 0)
	{
		OutputPluginPrepareWrite(ctx, true);
		logicalrep_write_truncate(ctx->out,
								  xid,
								  nrelids,
								  relids,
								  change->data.truncate.cascade,
								  change->data.truncate.restart_seqs);
		OutputPluginWrite(ctx, true);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

static void
pgoutput_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 XLogRecPtr message_lsn, bool transactional, const char *prefix, Size sz,
				 const char *message)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	TransactionId xid = InvalidTransactionId;

	update_replication_progress(ctx, false);

	if (!data->messages)
		return;

	/*
	 * Remember the xid for the message in streaming mode. See
	 * pgoutput_change.
	 */
	if (in_streaming)
		xid = txn->xid;

	/*
	 * Output BEGIN if we haven't yet. Avoid for non-transactional messages.
	 */
	if (transactional)
	{
		PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;

		/* Send BEGIN if we haven't yet */
		if (txndata && !txndata->sent_begin_txn)
			pgoutput_send_begin(ctx, txn);
	}

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_message(ctx->out,
							 xid,
							 message_lsn,
							 transactional,
							 prefix,
							 sz,
							 message);
	OutputPluginWrite(ctx, true);
}

/*
 * Currently we always forward.
 */
static bool
pgoutput_origin_filter(LogicalDecodingContext *ctx,
					   RepOriginId origin_id)
{
	return false;
}

/*
 * Shutdown the output plugin.
 *
 * Note, we don't need to clean the data->context and data->cachectx as
 * they are child context of the ctx->context so it will be cleaned up by
 * logical decoding machinery.
 */
static void
pgoutput_shutdown(LogicalDecodingContext *ctx)
{
	if (RelationSyncCache)
	{
		hash_destroy(RelationSyncCache);
		RelationSyncCache = NULL;
	}
}

/*
 * Load publications from the list of publication names.
 */
static List *
LoadPublications(List *pubnames)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, pubnames)
	{
		char	   *pubname = (char *) lfirst(lc);
		Publication *pub = GetPublicationByName(pubname, false);

		result = lappend(result, pub);
	}

	return result;
}

/*
 * Publication syscache invalidation callback.
 *
 * Called for invalidations on pg_publication.
 */
static void
publication_invalidation_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	publications_valid = false;

	/*
	 * Also invalidate per-relation cache so that next time the filtering info
	 * is checked it will be updated with the new publication settings.
	 */
	rel_sync_cache_publication_cb(arg, cacheid, hashvalue);
}

/*
 * START STREAM callback
 */
static void
pgoutput_stream_start(struct LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	/* we can't nest streaming of transactions */
	Assert(!in_streaming);

	/*
	 * If we already sent the first stream for this transaction then don't
	 * send the origin id in the subsequent streams.
	 */
	if (rbtxn_is_streamed(txn))
		send_replication_origin = false;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_stream_start(ctx->out, txn->xid, !rbtxn_is_streamed(txn));

	send_repl_origin(ctx, txn->origin_id, InvalidXLogRecPtr,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);

	/* we're streaming a chunk of transaction now */
	in_streaming = true;
}

/*
 * STOP STREAM callback
 */
static void
pgoutput_stream_stop(struct LogicalDecodingContext *ctx,
					 ReorderBufferTXN *txn)
{
	/* we should be streaming a trasanction */
	Assert(in_streaming);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_stop(ctx->out);
	OutputPluginWrite(ctx, true);

	/* we've stopped streaming a transaction */
	in_streaming = false;
}

/*
 * Notify downstream to discard the streamed transaction (along with all
 * it's subtransactions, if it's a toplevel transaction).
 */
static void
pgoutput_stream_abort(struct LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn,
					  XLogRecPtr abort_lsn)
{
	ReorderBufferTXN *toptxn;

	/*
	 * The abort should happen outside streaming block, even for streamed
	 * transactions. The transaction has to be marked as streamed, though.
	 */
	Assert(!in_streaming);

	/* determine the toplevel transaction */
	toptxn = (txn->toptxn) ? txn->toptxn : txn;

	Assert(rbtxn_is_streamed(toptxn));

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_abort(ctx->out, toptxn->xid, txn->xid);
	OutputPluginWrite(ctx, true);

	cleanup_rel_sync_cache(toptxn->xid, false);
}

/*
 * Notify downstream to apply the streamed transaction (along with all
 * it's subtransactions).
 */
static void
pgoutput_stream_commit(struct LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn,
					   XLogRecPtr commit_lsn)
{
	/*
	 * The commit should happen outside streaming block, even for streamed
	 * transactions. The transaction has to be marked as streamed, though.
	 */
	Assert(!in_streaming);
	Assert(rbtxn_is_streamed(txn));

	update_replication_progress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);

	cleanup_rel_sync_cache(txn->xid, true);
}

/*
 * PREPARE callback (for streaming two-phase commit).
 *
 * Notify the downstream to prepare the transaction.
 */
static void
pgoutput_stream_prepare_txn(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn,
							XLogRecPtr prepare_lsn)
{
	Assert(rbtxn_is_streamed(txn));

	update_replication_progress(ctx, false);
	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_prepare(ctx->out, txn, prepare_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * Initialize the relation schema sync cache for a decoding session.
 *
 * The hash table is destroyed at the end of a decoding session. While
 * relcache invalidations still exist and will still be invoked, they
 * will just see the null hash table global and take no action.
 */
static void
init_rel_sync_cache(MemoryContext cachectx)
{
	HASHCTL		ctl;

	if (RelationSyncCache != NULL)
		return;

	/* Make a new hash table for the cache */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelationSyncEntry);
	ctl.hcxt = cachectx;

	RelationSyncCache = hash_create("logical replication output relation cache",
									128, &ctl,
									HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);

	Assert(RelationSyncCache != NULL);

	CacheRegisterRelcacheCallback(rel_sync_cache_relation_cb, (Datum) 0);
	CacheRegisterSyscacheCallback(PUBLICATIONRELMAP,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);
	CacheRegisterSyscacheCallback(PUBLICATIONNAMESPACEMAP,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);
}

/*
 * We expect relatively small number of streamed transactions.
 */
static bool
get_schema_sent_in_streamed_txn(RelationSyncEntry *entry, TransactionId xid)
{
	ListCell   *lc;

	foreach(lc, entry->streamed_txns)
	{
		if (xid == (uint32) lfirst_int(lc))
			return true;
	}

	return false;
}

/*
 * Add the xid in the rel sync entry for which we have already sent the schema
 * of the relation.
 */
static void
set_schema_sent_in_streamed_txn(RelationSyncEntry *entry, TransactionId xid)
{
	MemoryContext oldctx;

	oldctx = MemoryContextSwitchTo(CacheMemoryContext);

	entry->streamed_txns = lappend_int(entry->streamed_txns, xid);

	MemoryContextSwitchTo(oldctx);
}

/*
 * Find or create entry in the relation schema cache.
 *
 * This looks up publications that the given relation is directly or
 * indirectly part of (the latter if it's really the relation's ancestor that
 * is part of a publication) and fills up the found entry with the information
 * about which operations to publish and whether to use an ancestor's schema
 * when publishing.
 */
static RelationSyncEntry *
get_rel_sync_entry(PGOutputData *data, Relation relation)
{
	RelationSyncEntry *entry;
	bool		found;
	MemoryContext oldctx;
	Oid			relid = RelationGetRelid(relation);

	Assert(RelationSyncCache != NULL);

	/* Find cached relation info, creating if not found */
	entry = (RelationSyncEntry *) hash_search(RelationSyncCache,
											  (void *) &relid,
											  HASH_ENTER, &found);
	Assert(entry != NULL);

	/* initialize entry, if it's new */
	if (!found)
	{
		entry->replicate_valid = false;
		entry->schema_sent = false;
		entry->streamed_txns = NIL;
		entry->pubactions.pubinsert = entry->pubactions.pubupdate =
			entry->pubactions.pubdelete = entry->pubactions.pubtruncate = false;
		entry->new_slot = NULL;
		entry->old_slot = NULL;
		memset(entry->exprstate, 0, sizeof(entry->exprstate));
		entry->entry_cxt = NULL;
		entry->publish_as_relid = InvalidOid;
		entry->columns = NULL;
		entry->attrmap = NULL;
	}

	/* Validate the entry */
	if (!entry->replicate_valid)
	{
		Oid			schemaId = get_rel_namespace(relid);
		List	   *pubids = GetRelationPublications(relid);

		/*
		 * We don't acquire a lock on the namespace system table as we build
		 * the cache entry using a historic snapshot and all the later changes
		 * are absorbed while decoding WAL.
		 */
		List	   *schemaPubids = GetSchemaPublications(schemaId);
		ListCell   *lc;
		Oid			publish_as_relid = relid;
		int			publish_ancestor_level = 0;
		bool		am_partition = get_rel_relispartition(relid);
		char		relkind = get_rel_relkind(relid);
		List	   *rel_publications = NIL;

		/* Reload publications if needed before use. */
		if (!publications_valid)
		{
			oldctx = MemoryContextSwitchTo(CacheMemoryContext);
			if (data->publications)
			{
				list_free_deep(data->publications);
				data->publications = NIL;
			}
			data->publications = LoadPublications(data->publication_names);
			MemoryContextSwitchTo(oldctx);
			publications_valid = true;
		}

		/*
		 * Reset schema_sent status as the relation definition may have
		 * changed.  Also reset pubactions to empty in case rel was dropped
		 * from a publication.  Also free any objects that depended on the
		 * earlier definition.
		 */
		entry->schema_sent = false;
		list_free(entry->streamed_txns);
		entry->streamed_txns = NIL;
		bms_free(entry->columns);
		entry->columns = NULL;
		entry->pubactions.pubinsert = false;
		entry->pubactions.pubupdate = false;
		entry->pubactions.pubdelete = false;
		entry->pubactions.pubtruncate = false;

		/*
		 * Tuple slots cleanups. (Will be rebuilt later if needed).
		 */
		if (entry->old_slot)
			ExecDropSingleTupleTableSlot(entry->old_slot);
		if (entry->new_slot)
			ExecDropSingleTupleTableSlot(entry->new_slot);

		entry->old_slot = NULL;
		entry->new_slot = NULL;

		if (entry->attrmap)
			free_attrmap(entry->attrmap);
		entry->attrmap = NULL;

		/*
		 * Row filter cache cleanups.
		 */
		if (entry->entry_cxt)
			MemoryContextDelete(entry->entry_cxt);

		entry->entry_cxt = NULL;
		entry->estate = NULL;
		memset(entry->exprstate, 0, sizeof(entry->exprstate));

		/*
		 * Build publication cache. We can't use one provided by relcache as
		 * relcache considers all publications that the given relation is in,
		 * but here we only need to consider ones that the subscriber
		 * requested.
		 */
		foreach(lc, data->publications)
		{
			Publication *pub = lfirst(lc);
			bool		publish = false;

			/*
			 * Under what relid should we publish changes in this publication?
			 * We'll use the top-most relid across all publications. Also
			 * track the ancestor level for this publication.
			 */
			Oid			pub_relid = relid;
			int			ancestor_level = 0;

			/*
			 * If this is a FOR ALL TABLES publication, pick the partition
			 * root and set the ancestor level accordingly.
			 */
			if (pub->alltables)
			{
				publish = true;
				if (pub->pubviaroot && am_partition)
				{
					List	   *ancestors = get_partition_ancestors(relid);

					pub_relid = llast_oid(ancestors);
					ancestor_level = list_length(ancestors);
				}
			}

			if (!publish)
			{
				bool		ancestor_published = false;

				/*
				 * For a partition, check if any of the ancestors are
				 * published.  If so, note down the topmost ancestor that is
				 * published via this publication, which will be used as the
				 * relation via which to publish the partition's changes.
				 */
				if (am_partition)
				{
					Oid			ancestor;
					int			level;
					List	   *ancestors = get_partition_ancestors(relid);

					ancestor = GetTopMostAncestorInPublication(pub->oid,
															   ancestors,
															   &level);

					if (ancestor != InvalidOid)
					{
						ancestor_published = true;
						if (pub->pubviaroot)
						{
							pub_relid = ancestor;
							ancestor_level = level;
						}
					}
				}

				if (list_member_oid(pubids, pub->oid) ||
					list_member_oid(schemaPubids, pub->oid) ||
					ancestor_published)
					publish = true;
			}

			/*
			 * If the relation is to be published, determine actions to
			 * publish, and list of columns, if appropriate.
			 *
			 * Don't publish changes for partitioned tables, because
			 * publishing those of its partitions suffices, unless partition
			 * changes won't be published due to pubviaroot being set.
			 */
			if (publish &&
				(relkind != RELKIND_PARTITIONED_TABLE || pub->pubviaroot))
			{
				entry->pubactions.pubinsert |= pub->pubactions.pubinsert;
				entry->pubactions.pubupdate |= pub->pubactions.pubupdate;
				entry->pubactions.pubdelete |= pub->pubactions.pubdelete;
				entry->pubactions.pubtruncate |= pub->pubactions.pubtruncate;

				/*
				 * We want to publish the changes as the top-most ancestor
				 * across all publications. So we need to check if the already
				 * calculated level is higher than the new one. If yes, we can
				 * ignore the new value (as it's a child). Otherwise the new
				 * value is an ancestor, so we keep it.
				 */
				if (publish_ancestor_level > ancestor_level)
					continue;

				/*
				 * If we found an ancestor higher up in the tree, discard the
				 * list of publications through which we replicate it, and use
				 * the new ancestor.
				 */
				if (publish_ancestor_level < ancestor_level)
				{
					publish_as_relid = pub_relid;
					publish_ancestor_level = ancestor_level;

					/* reset the publication list for this relation */
					rel_publications = NIL;
				}
				else
				{
					/* Same ancestor level, has to be the same OID. */
					Assert(publish_as_relid == pub_relid);
				}

				/* Track publications for this ancestor. */
				rel_publications = lappend(rel_publications, pub);
			}
		}

		entry->publish_as_relid = publish_as_relid;

		/*
		 * Initialize the tuple slot, map, and row filter. These are only used
		 * when publishing inserts, updates, or deletes.
		 */
		if (entry->pubactions.pubinsert || entry->pubactions.pubupdate ||
			entry->pubactions.pubdelete)
		{
			/* Initialize the tuple slot and map */
			init_tuple_slot(data, relation, entry);

			/* Initialize the row filter */
			pgoutput_row_filter_init(data, rel_publications, entry);

			/* Initialize the column list */
			pgoutput_column_list_init(data, rel_publications, entry);
		}

		list_free(pubids);
		list_free(schemaPubids);
		list_free(rel_publications);

		entry->replicate_valid = true;
	}

	return entry;
}

/*
 * Cleanup list of streamed transactions and update the schema_sent flag.
 *
 * When a streamed transaction commits or aborts, we need to remove the
 * toplevel XID from the schema cache. If the transaction aborted, the
 * subscriber will simply throw away the schema records we streamed, so
 * we don't need to do anything else.
 *
 * If the transaction is committed, the subscriber will update the relation
 * cache - so tweak the schema_sent flag accordingly.
 */
static void
cleanup_rel_sync_cache(TransactionId xid, bool is_commit)
{
	HASH_SEQ_STATUS hash_seq;
	RelationSyncEntry *entry;
	ListCell   *lc;

	Assert(RelationSyncCache != NULL);

	hash_seq_init(&hash_seq, RelationSyncCache);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		/*
		 * We can set the schema_sent flag for an entry that has committed xid
		 * in the list as that ensures that the subscriber would have the
		 * corresponding schema and we don't need to send it unless there is
		 * any invalidation for that relation.
		 */
		foreach(lc, entry->streamed_txns)
		{
			if (xid == (uint32) lfirst_int(lc))
			{
				if (is_commit)
					entry->schema_sent = true;

				entry->streamed_txns =
					foreach_delete_current(entry->streamed_txns, lc);
				break;
			}
		}
	}
}

/*
 * Relcache invalidation callback
 */
static void
rel_sync_cache_relation_cb(Datum arg, Oid relid)
{
	RelationSyncEntry *entry;

	/*
	 * We can get here if the plugin was used in SQL interface as the
	 * RelSchemaSyncCache is destroyed when the decoding finishes, but there
	 * is no way to unregister the relcache invalidation callback.
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * Nobody keeps pointers to entries in this hash table around outside
	 * logical decoding callback calls - but invalidation events can come in
	 * *during* a callback if we do any syscache access in the callback.
	 * Because of that we must mark the cache entry as invalid but not damage
	 * any of its substructure here.  The next get_rel_sync_entry() call will
	 * rebuild it all.
	 */
	if (OidIsValid(relid))
	{
		/*
		 * Getting invalidations for relations that aren't in the table is
		 * entirely normal.  So we don't care if it's found or not.
		 */
		entry = (RelationSyncEntry *) hash_search(RelationSyncCache, &relid,
												  HASH_FIND, NULL);
		if (entry != NULL)
			entry->replicate_valid = false;
	}
	else
	{
		/* Whole cache must be flushed. */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, RelationSyncCache);
		while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
		{
			entry->replicate_valid = false;
		}
	}
}

/*
 * Publication relation/schema map syscache invalidation callback
 *
 * Called for invalidations on pg_publication, pg_publication_rel, and
 * pg_publication_namespace.
 */
static void
rel_sync_cache_publication_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	RelationSyncEntry *entry;

	/*
	 * We can get here if the plugin was used in SQL interface as the
	 * RelSchemaSyncCache is destroyed when the decoding finishes, but there
	 * is no way to unregister the relcache invalidation callback.
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * There is no way to find which entry in our cache the hash belongs to so
	 * mark the whole cache as invalid.
	 */
	hash_seq_init(&status, RelationSyncCache);
	while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
	{
		entry->replicate_valid = false;
	}
}

/* Send Replication origin */
static void
send_repl_origin(LogicalDecodingContext *ctx, RepOriginId origin_id,
				 XLogRecPtr origin_lsn, bool send_origin)
{
	if (send_origin)
	{
		char	   *origin;

		/*----------
		 * XXX: which behaviour do we want here?
		 *
		 * Alternatives:
		 *  - don't send origin message if origin name not found
		 *    (that's what we do now)
		 *  - throw error - that will break replication, not good
		 *  - send some special "unknown" origin
		 *----------
		 */
		if (replorigin_by_oid(origin_id, true, &origin))
		{
			/* Message boundary */
			OutputPluginWrite(ctx, false);
			OutputPluginPrepareWrite(ctx, true);

			logicalrep_write_origin(ctx->out, origin, origin_lsn);
		}
	}
}

/*
 * Try to update progress and send a keepalive message if too many changes were
 * processed.
 *
 * For a large transaction, if we don't send any change to the downstream for a
 * long time (exceeds the wal_receiver_timeout of standby) then it can timeout.
 * This can happen when all or most of the changes are either not published or
 * got filtered out.
 */
static void
update_replication_progress(LogicalDecodingContext *ctx, bool skipped_xact)
{
	static int	changes_count = 0;

	/*
	 * We don't want to try sending a keepalive message after processing each
	 * change as that can have overhead. Tests revealed that there is no
	 * noticeable overhead in doing it after continuously processing 100 or so
	 * changes.
	 */
#define CHANGES_THRESHOLD 100

	/*
	 * If we are at the end of transaction LSN, update progress tracking.
	 * Otherwise, after continuously processing CHANGES_THRESHOLD changes, we
	 * try to send a keepalive message if required.
	 */
	if (ctx->end_xact || ++changes_count >= CHANGES_THRESHOLD)
	{
		OutputPluginUpdateProgress(ctx, skipped_xact);
		changes_count = 0;
	}
}
