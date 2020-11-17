/*-------------------------------------------------------------------------
 *
 * test_decoding.c
 *		  example logical decoding output plugin
 *
 * Copyright (c) 2012-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/test_decoding/test_decoding.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"

#include "replication/logical.h"
#include "replication/origin.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* These must be available to dlsym() */
extern void _PG_init(void);
extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

typedef struct
{
	MemoryContext context;
	bool		include_xids;
	bool		include_timestamp;
	bool		skip_empty_xacts;
	bool		only_local;
} TestDecodingData;

/*
 * Maintain the per-transaction level variables to track whether the
 * transaction and or streams have written any changes. In streaming mode the
 * transaction can be decoded in streams so along with maintaining whether the
 * transaction has written any changes, we also need to track whether the
 * current stream has written any changes. This is required so that if user
 * has requested to skip the empty transactions we can skip the empty streams
 * even though the transaction has written some changes.
 */
typedef struct
{
	bool		xact_wrote_changes;
	bool		stream_wrote_changes;
} TestDecodingTxnData;

static void pg_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
							  bool is_init);
static void pg_decode_shutdown(LogicalDecodingContext *ctx);
static void pg_decode_begin_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn);
static void pg_output_begin(LogicalDecodingContext *ctx,
							TestDecodingData *data,
							ReorderBufferTXN *txn,
							bool last_write);
static void pg_decode_commit_txn(LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pg_decode_change(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn, Relation rel,
							 ReorderBufferChange *change);
static void pg_decode_truncate(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn,
							   int nrelations, Relation relations[],
							   ReorderBufferChange *change);
static bool pg_decode_filter(LogicalDecodingContext *ctx,
							 RepOriginId origin_id);
static void pg_decode_message(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, XLogRecPtr message_lsn,
							  bool transactional, const char *prefix,
							  Size sz, const char *message);
static void pg_decode_stream_start(LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn);
static void pg_output_stream_start(LogicalDecodingContext *ctx,
								   TestDecodingData *data,
								   ReorderBufferTXN *txn,
								   bool last_write);
static void pg_decode_stream_stop(LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn);
static void pg_decode_stream_abort(LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn,
								   XLogRecPtr abort_lsn);
static void pg_decode_stream_commit(LogicalDecodingContext *ctx,
									ReorderBufferTXN *txn,
									XLogRecPtr commit_lsn);
static void pg_decode_stream_change(LogicalDecodingContext *ctx,
									ReorderBufferTXN *txn,
									Relation relation,
									ReorderBufferChange *change);
static void pg_decode_stream_message(LogicalDecodingContext *ctx,
									 ReorderBufferTXN *txn, XLogRecPtr message_lsn,
									 bool transactional, const char *prefix,
									 Size sz, const char *message);
static void pg_decode_stream_truncate(LogicalDecodingContext *ctx,
									  ReorderBufferTXN *txn,
									  int nrelations, Relation relations[],
									  ReorderBufferChange *change);

void
_PG_init(void)
{
	/* other plugins can perform things here */
}

/* specify output plugin callbacks */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

	cb->startup_cb = pg_decode_startup;
	cb->begin_cb = pg_decode_begin_txn;
	cb->change_cb = pg_decode_change;
	cb->truncate_cb = pg_decode_truncate;
	cb->commit_cb = pg_decode_commit_txn;
	cb->filter_by_origin_cb = pg_decode_filter;
	cb->shutdown_cb = pg_decode_shutdown;
	cb->message_cb = pg_decode_message;
	cb->stream_start_cb = pg_decode_stream_start;
	cb->stream_stop_cb = pg_decode_stream_stop;
	cb->stream_abort_cb = pg_decode_stream_abort;
	cb->stream_commit_cb = pg_decode_stream_commit;
	cb->stream_change_cb = pg_decode_stream_change;
	cb->stream_message_cb = pg_decode_stream_message;
	cb->stream_truncate_cb = pg_decode_stream_truncate;
}


/* initialize this plugin */
static void
pg_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				  bool is_init)
{
	ListCell   *option;
	TestDecodingData *data;
	bool		enable_streaming = false;

	data = palloc0(sizeof(TestDecodingData));
	data->context = AllocSetContextCreate(ctx->context,
										  "text conversion context",
										  ALLOCSET_DEFAULT_SIZES);
	data->include_xids = true;
	data->include_timestamp = false;
	data->skip_empty_xacts = false;
	data->only_local = false;

	ctx->output_plugin_private = data;

	opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
	opt->receive_rewrites = false;

	foreach(option, ctx->output_plugin_options)
	{
		DefElem    *elem = lfirst(option);

		Assert(elem->arg == NULL || IsA(elem->arg, String));

		if (strcmp(elem->defname, "include-xids") == 0)
		{
			/* if option does not provide a value, it means its value is true */
			if (elem->arg == NULL)
				data->include_xids = true;
			else if (!parse_bool(strVal(elem->arg), &data->include_xids))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-timestamp") == 0)
		{
			if (elem->arg == NULL)
				data->include_timestamp = true;
			else if (!parse_bool(strVal(elem->arg), &data->include_timestamp))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "force-binary") == 0)
		{
			bool		force_binary;

			if (elem->arg == NULL)
				continue;
			else if (!parse_bool(strVal(elem->arg), &force_binary))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));

			if (force_binary)
				opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;
		}
		else if (strcmp(elem->defname, "skip-empty-xacts") == 0)
		{

			if (elem->arg == NULL)
				data->skip_empty_xacts = true;
			else if (!parse_bool(strVal(elem->arg), &data->skip_empty_xacts))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "only-local") == 0)
		{

			if (elem->arg == NULL)
				data->only_local = true;
			else if (!parse_bool(strVal(elem->arg), &data->only_local))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-rewrites") == 0)
		{

			if (elem->arg == NULL)
				continue;
			else if (!parse_bool(strVal(elem->arg), &opt->receive_rewrites))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "stream-changes") == 0)
		{
			if (elem->arg == NULL)
				continue;
			else if (!parse_bool(strVal(elem->arg), &enable_streaming))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" = \"%s\" is unknown",
							elem->defname,
							elem->arg ? strVal(elem->arg) : "(null)")));
		}
	}

	ctx->streaming &= enable_streaming;
}

/* cleanup this plugin's resources */
static void
pg_decode_shutdown(LogicalDecodingContext *ctx)
{
	TestDecodingData *data = ctx->output_plugin_private;

	/* cleanup our own resources via memory context reset */
	MemoryContextDelete(data->context);
}

/* BEGIN callback */
static void
pg_decode_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata =
	MemoryContextAllocZero(ctx->context, sizeof(TestDecodingTxnData));

	txndata->xact_wrote_changes = false;
	txn->output_plugin_private = txndata;

	if (data->skip_empty_xacts)
		return;

	pg_output_begin(ctx, data, txn, true);
}

static void
pg_output_begin(LogicalDecodingContext *ctx, TestDecodingData *data, ReorderBufferTXN *txn, bool last_write)
{
	OutputPluginPrepareWrite(ctx, last_write);
	if (data->include_xids)
		appendStringInfo(ctx->out, "BEGIN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "BEGIN");
	OutputPluginWrite(ctx, last_write);
}

/* COMMIT callback */
static void
pg_decode_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr commit_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;
	bool		xact_wrote_changes = txndata->xact_wrote_changes;

	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (data->skip_empty_xacts && !xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "COMMIT %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "COMMIT");

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->commit_time));

	OutputPluginWrite(ctx, true);
}

static bool
pg_decode_filter(LogicalDecodingContext *ctx,
				 RepOriginId origin_id)
{
	TestDecodingData *data = ctx->output_plugin_private;

	if (data->only_local && origin_id != InvalidRepOriginId)
		return true;
	return false;
}

/*
 * Print literal `outputstr' already represented as string of type `typid'
 * into stringbuf `s'.
 *
 * Some builtin types aren't quoted, the rest is quoted. Escaping is done as
 * if standard_conforming_strings were enabled.
 */
static void
print_literal(StringInfo s, Oid typid, char *outputstr)
{
	const char *valptr;

	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			/* NB: We don't care about Inf, NaN et al. */
			appendStringInfoString(s, outputstr);
			break;

		case BITOID:
		case VARBITOID:
			appendStringInfo(s, "B'%s'", outputstr);
			break;

		case BOOLOID:
			if (strcmp(outputstr, "t") == 0)
				appendStringInfoString(s, "true");
			else
				appendStringInfoString(s, "false");
			break;

		default:
			appendStringInfoChar(s, '\'');
			for (valptr = outputstr; *valptr; valptr++)
			{
				char		ch = *valptr;

				if (SQL_STR_DOUBLE(ch, false))
					appendStringInfoChar(s, ch);
				appendStringInfoChar(s, ch);
			}
			appendStringInfoChar(s, '\'');
			break;
	}
}

/* print the tuple 'tuple' into the StringInfo s */
static void
tuple_to_stringinfo(StringInfo s, TupleDesc tupdesc, HeapTuple tuple, bool skip_nulls)
{
	int			natt;

	/* print all columns individually */
	for (natt = 0; natt < tupdesc->natts; natt++)
	{
		Form_pg_attribute attr; /* the attribute itself */
		Oid			typid;		/* type of current attribute */
		Oid			typoutput;	/* output function */
		bool		typisvarlena;
		Datum		origval;	/* possibly toasted Datum */
		bool		isnull;		/* column is null? */

		attr = TupleDescAttr(tupdesc, natt);

		/*
		 * don't print dropped columns, we can't be sure everything is
		 * available for them
		 */
		if (attr->attisdropped)
			continue;

		/*
		 * Don't print system columns, oid will already have been printed if
		 * present.
		 */
		if (attr->attnum < 0)
			continue;

		typid = attr->atttypid;

		/* get Datum from tuple */
		origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);

		if (isnull && skip_nulls)
			continue;

		/* print attribute name */
		appendStringInfoChar(s, ' ');
		appendStringInfoString(s, quote_identifier(NameStr(attr->attname)));

		/* print attribute type */
		appendStringInfoChar(s, '[');
		appendStringInfoString(s, format_type_be(typid));
		appendStringInfoChar(s, ']');

		/* query output function */
		getTypeOutputInfo(typid,
						  &typoutput, &typisvarlena);

		/* print separator */
		appendStringInfoChar(s, ':');

		/* print data */
		if (isnull)
			appendStringInfoString(s, "null");
		else if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval))
			appendStringInfoString(s, "unchanged-toast-datum");
		else if (!typisvarlena)
			print_literal(s, typid,
						  OidOutputFunctionCall(typoutput, origval));
		else
		{
			Datum		val;	/* definitely detoasted Datum */

			val = PointerGetDatum(PG_DETOAST_DATUM(origval));
			print_literal(s, typid, OidOutputFunctionCall(typoutput, val));
		}
	}
}

/*
 * callback for individual changed tuples
 */
static void
pg_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 Relation relation, ReorderBufferChange *change)
{
	TestDecodingData *data;
	TestDecodingTxnData *txndata;
	Form_pg_class class_form;
	TupleDesc	tupdesc;
	MemoryContext old;

	data = ctx->output_plugin_private;
	txndata = txn->output_plugin_private;

	/* output BEGIN if we haven't yet */
	if (data->skip_empty_xacts && !txndata->xact_wrote_changes)
	{
		pg_output_begin(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = true;

	class_form = RelationGetForm(relation);
	tupdesc = RelationGetDescr(relation);

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfoString(ctx->out, "table ");
	appendStringInfoString(ctx->out,
						   quote_qualified_identifier(get_namespace_name(get_rel_namespace(RelationGetRelid(relation))),
													  class_form->relrewrite ?
													  get_rel_name(class_form->relrewrite) :
													  NameStr(class_form->relname)));
	appendStringInfoChar(ctx->out, ':');

	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			appendStringInfoString(ctx->out, " INSERT:");
			if (change->data.tp.newtuple == NULL)
				appendStringInfoString(ctx->out, " (no-tuple-data)");
			else
				tuple_to_stringinfo(ctx->out, tupdesc,
									&change->data.tp.newtuple->tuple,
									false);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			appendStringInfoString(ctx->out, " UPDATE:");
			if (change->data.tp.oldtuple != NULL)
			{
				appendStringInfoString(ctx->out, " old-key:");
				tuple_to_stringinfo(ctx->out, tupdesc,
									&change->data.tp.oldtuple->tuple,
									true);
				appendStringInfoString(ctx->out, " new-tuple:");
			}

			if (change->data.tp.newtuple == NULL)
				appendStringInfoString(ctx->out, " (no-tuple-data)");
			else
				tuple_to_stringinfo(ctx->out, tupdesc,
									&change->data.tp.newtuple->tuple,
									false);
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			appendStringInfoString(ctx->out, " DELETE:");

			/* if there was no PK, we only know that a delete happened */
			if (change->data.tp.oldtuple == NULL)
				appendStringInfoString(ctx->out, " (no-tuple-data)");
			/* In DELETE, only the replica identity is present; display that */
			else
				tuple_to_stringinfo(ctx->out, tupdesc,
									&change->data.tp.oldtuple->tuple,
									true);
			break;
		default:
			Assert(false);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}

static void
pg_decode_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				   int nrelations, Relation relations[], ReorderBufferChange *change)
{
	TestDecodingData *data;
	TestDecodingTxnData *txndata;
	MemoryContext old;
	int			i;

	data = ctx->output_plugin_private;
	txndata = txn->output_plugin_private;

	/* output BEGIN if we haven't yet */
	if (data->skip_empty_xacts && !txndata->xact_wrote_changes)
	{
		pg_output_begin(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = true;

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfoString(ctx->out, "table ");

	for (i = 0; i < nrelations; i++)
	{
		if (i > 0)
			appendStringInfoString(ctx->out, ", ");

		appendStringInfoString(ctx->out,
							   quote_qualified_identifier(get_namespace_name(relations[i]->rd_rel->relnamespace),
														  NameStr(relations[i]->rd_rel->relname)));
	}

	appendStringInfoString(ctx->out, ": TRUNCATE:");

	if (change->data.truncate.restart_seqs
		|| change->data.truncate.cascade)
	{
		if (change->data.truncate.restart_seqs)
			appendStringInfoString(ctx->out, " restart_seqs");
		if (change->data.truncate.cascade)
			appendStringInfoString(ctx->out, " cascade");
	}
	else
		appendStringInfoString(ctx->out, " (no-flags)");

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}

static void
pg_decode_message(LogicalDecodingContext *ctx,
				  ReorderBufferTXN *txn, XLogRecPtr lsn, bool transactional,
				  const char *prefix, Size sz, const char *message)
{
	OutputPluginPrepareWrite(ctx, true);
	appendStringInfo(ctx->out, "message: transactional: %d prefix: %s, sz: %zu content:",
					 transactional, prefix, sz);
	appendBinaryStringInfo(ctx->out, message, sz);
	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_start(LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	/*
	 * Allocate the txn plugin data for the first stream in the transaction.
	 */
	if (txndata == NULL)
	{
		txndata =
			MemoryContextAllocZero(ctx->context, sizeof(TestDecodingTxnData));
		txndata->xact_wrote_changes = false;
		txn->output_plugin_private = txndata;
	}

	txndata->stream_wrote_changes = false;
	if (data->skip_empty_xacts)
		return;
	pg_output_stream_start(ctx, data, txn, true);
}

static void
pg_output_stream_start(LogicalDecodingContext *ctx, TestDecodingData *data, ReorderBufferTXN *txn, bool last_write)
{
	OutputPluginPrepareWrite(ctx, last_write);
	if (data->include_xids)
		appendStringInfo(ctx->out, "opening a streamed block for transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "opening a streamed block for transaction");
	OutputPluginWrite(ctx, last_write);
}

static void
pg_decode_stream_stop(LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "closing a streamed block for transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "closing a streamed block for transaction");
	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_abort(LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn,
					   XLogRecPtr abort_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;

	/*
	 * stream abort can be sent for an individual subtransaction but we
	 * maintain the output_plugin_private only under the toptxn so if this is
	 * not the toptxn then fetch the toptxn.
	 */
	ReorderBufferTXN *toptxn = txn->toptxn ? txn->toptxn : txn;
	TestDecodingTxnData *txndata = toptxn->output_plugin_private;
	bool		xact_wrote_changes = txndata->xact_wrote_changes;

	if (txn->toptxn == NULL)
	{
		Assert(txn->output_plugin_private != NULL);
		pfree(txndata);
		txn->output_plugin_private = NULL;
	}

	if (data->skip_empty_xacts && !xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "aborting streamed (sub)transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "aborting streamed (sub)transaction");
	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_commit(LogicalDecodingContext *ctx,
						ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;
	bool		xact_wrote_changes = txndata->xact_wrote_changes;

	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (data->skip_empty_xacts && !xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);

	if (data->include_xids)
		appendStringInfo(ctx->out, "committing streamed transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "committing streamed transaction");

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->commit_time));

	OutputPluginWrite(ctx, true);
}

/*
 * In streaming mode, we don't display the changes as the transaction can abort
 * at a later point in time.  We don't want users to see the changes until the
 * transaction is committed.
 */
static void
pg_decode_stream_change(LogicalDecodingContext *ctx,
						ReorderBufferTXN *txn,
						Relation relation,
						ReorderBufferChange *change)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	/* output stream start if we haven't yet */
	if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
	{
		pg_output_stream_start(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = txndata->stream_wrote_changes = true;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "streaming change for TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "streaming change for transaction");
	OutputPluginWrite(ctx, true);
}

/*
 * In streaming mode, we don't display the contents for transactional messages
 * as the transaction can abort at a later point in time.  We don't want users to
 * see the message contents until the transaction is committed.
 */
static void
pg_decode_stream_message(LogicalDecodingContext *ctx,
						 ReorderBufferTXN *txn, XLogRecPtr lsn, bool transactional,
						 const char *prefix, Size sz, const char *message)
{
	OutputPluginPrepareWrite(ctx, true);

	if (transactional)
	{
		appendStringInfo(ctx->out, "streaming message: transactional: %d prefix: %s, sz: %zu",
						 transactional, prefix, sz);
	}
	else
	{
		appendStringInfo(ctx->out, "streaming message: transactional: %d prefix: %s, sz: %zu content:",
						 transactional, prefix, sz);
		appendBinaryStringInfo(ctx->out, message, sz);
	}

	OutputPluginWrite(ctx, true);
}

/*
 * In streaming mode, we don't display the detailed information of Truncate.
 * See pg_decode_stream_change.
 */
static void
pg_decode_stream_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
						  int nrelations, Relation relations[],
						  ReorderBufferChange *change)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
	{
		pg_output_stream_start(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = txndata->stream_wrote_changes = true;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "streaming truncate for TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "streaming truncate for transaction");
	OutputPluginWrite(ctx, true);
}
