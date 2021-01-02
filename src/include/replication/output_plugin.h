/*-------------------------------------------------------------------------
 * output_plugin.h
 *	   PostgreSQL Logical Decode Plugin Interface
 *
 * Copyright (c) 2012-2021, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef OUTPUT_PLUGIN_H
#define OUTPUT_PLUGIN_H

#include "replication/reorderbuffer.h"

struct LogicalDecodingContext;
struct OutputPluginCallbacks;

typedef enum OutputPluginOutputType
{
	OUTPUT_PLUGIN_BINARY_OUTPUT,
	OUTPUT_PLUGIN_TEXTUAL_OUTPUT
} OutputPluginOutputType;

/*
 * Options set by the output plugin, in the startup callback.
 */
typedef struct OutputPluginOptions
{
	OutputPluginOutputType output_type;
	bool		receive_rewrites;
} OutputPluginOptions;

/*
 * Type of the shared library symbol _PG_output_plugin_init that is looked up
 * when loading an output plugin shared library.
 */
typedef void (*LogicalOutputPluginInit) (struct OutputPluginCallbacks *cb);

/*
 * Callback that gets called in a user-defined plugin. ctx->private_data can
 * be set to some private data.
 *
 * "is_init" will be set to "true" if the decoding slot just got defined. When
 * the same slot is used from there one, it will be "false".
 */
typedef void (*LogicalDecodeStartupCB) (struct LogicalDecodingContext *ctx,
										OutputPluginOptions *options,
										bool is_init);

/*
 * Callback called for every (explicit or implicit) BEGIN of a successful
 * transaction.
 */
typedef void (*LogicalDecodeBeginCB) (struct LogicalDecodingContext *ctx,
									  ReorderBufferTXN *txn);

/*
 * Callback for every individual change in a successful transaction.
 */
typedef void (*LogicalDecodeChangeCB) (struct LogicalDecodingContext *ctx,
									   ReorderBufferTXN *txn,
									   Relation relation,
									   ReorderBufferChange *change);

/*
 * Callback for every TRUNCATE in a successful transaction.
 */
typedef void (*LogicalDecodeTruncateCB) (struct LogicalDecodingContext *ctx,
										 ReorderBufferTXN *txn,
										 int nrelations,
										 Relation relations[],
										 ReorderBufferChange *change);

/*
 * Called for every (explicit or implicit) COMMIT of a successful transaction.
 */
typedef void (*LogicalDecodeCommitCB) (struct LogicalDecodingContext *ctx,
									   ReorderBufferTXN *txn,
									   XLogRecPtr commit_lsn);

/*
 * Called for the generic logical decoding messages.
 */
typedef void (*LogicalDecodeMessageCB) (struct LogicalDecodingContext *ctx,
										ReorderBufferTXN *txn,
										XLogRecPtr message_lsn,
										bool transactional,
										const char *prefix,
										Size message_size,
										const char *message);

/*
 * Filter changes by origin.
 */
typedef bool (*LogicalDecodeFilterByOriginCB) (struct LogicalDecodingContext *ctx,
											   RepOriginId origin_id);

/*
 * Called to shutdown an output plugin.
 */
typedef void (*LogicalDecodeShutdownCB) (struct LogicalDecodingContext *ctx);

/*
 * Called before decoding of PREPARE record to decide whether this
 * transaction should be decoded with separate calls to prepare and
 * commit_prepared/rollback_prepared callbacks or wait till COMMIT PREPARED
 * and sent as usual transaction.
 */
typedef bool (*LogicalDecodeFilterPrepareCB) (struct LogicalDecodingContext *ctx,
											  const char *gid);

/*
 * Callback called for every BEGIN of a prepared trnsaction.
 */
typedef void (*LogicalDecodeBeginPrepareCB) (struct LogicalDecodingContext *ctx,
											 ReorderBufferTXN *txn);

/*
 * Called for PREPARE record unless it was filtered by filter_prepare()
 * callback.
 */
typedef void (*LogicalDecodePrepareCB) (struct LogicalDecodingContext *ctx,
										ReorderBufferTXN *txn,
										XLogRecPtr prepare_lsn);

/*
 * Called for COMMIT PREPARED.
 */
typedef void (*LogicalDecodeCommitPreparedCB) (struct LogicalDecodingContext *ctx,
											   ReorderBufferTXN *txn,
											   XLogRecPtr commit_lsn);

/*
 * Called for ROLLBACK PREPARED.
 */
typedef void (*LogicalDecodeRollbackPreparedCB) (struct LogicalDecodingContext *ctx,
												 ReorderBufferTXN *txn,
												 XLogRecPtr prepare_end_lsn,
												 TimestampTz prepare_time);


/*
 * Called when starting to stream a block of changes from in-progress
 * transaction (may be called repeatedly, if it's streamed in multiple
 * chunks).
 */
typedef void (*LogicalDecodeStreamStartCB) (struct LogicalDecodingContext *ctx,
											ReorderBufferTXN *txn);

/*
 * Called when stopping to stream a block of changes from in-progress
 * transaction to a remote node (may be called repeatedly, if it's streamed
 * in multiple chunks).
 */
typedef void (*LogicalDecodeStreamStopCB) (struct LogicalDecodingContext *ctx,
										   ReorderBufferTXN *txn);

/*
 * Called to discard changes streamed to remote node from in-progress
 * transaction.
 */
typedef void (*LogicalDecodeStreamAbortCB) (struct LogicalDecodingContext *ctx,
											ReorderBufferTXN *txn,
											XLogRecPtr abort_lsn);

/*
 * Called to prepare changes streamed to remote node from in-progress
 * transaction. This is called as part of a two-phase commit.
 */
typedef void (*LogicalDecodeStreamPrepareCB) (struct LogicalDecodingContext *ctx,
											  ReorderBufferTXN *txn,
											  XLogRecPtr prepare_lsn);

/*
 * Called to apply changes streamed to remote node from in-progress
 * transaction.
 */
typedef void (*LogicalDecodeStreamCommitCB) (struct LogicalDecodingContext *ctx,
											 ReorderBufferTXN *txn,
											 XLogRecPtr commit_lsn);

/*
 * Callback for streaming individual changes from in-progress transactions.
 */
typedef void (*LogicalDecodeStreamChangeCB) (struct LogicalDecodingContext *ctx,
											 ReorderBufferTXN *txn,
											 Relation relation,
											 ReorderBufferChange *change);

/*
 * Callback for streaming generic logical decoding messages from in-progress
 * transactions.
 */
typedef void (*LogicalDecodeStreamMessageCB) (struct LogicalDecodingContext *ctx,
											  ReorderBufferTXN *txn,
											  XLogRecPtr message_lsn,
											  bool transactional,
											  const char *prefix,
											  Size message_size,
											  const char *message);

/*
 * Callback for streaming truncates from in-progress transactions.
 */
typedef void (*LogicalDecodeStreamTruncateCB) (struct LogicalDecodingContext *ctx,
											   ReorderBufferTXN *txn,
											   int nrelations,
											   Relation relations[],
											   ReorderBufferChange *change);

/*
 * Output plugin callbacks
 */
typedef struct OutputPluginCallbacks
{
	LogicalDecodeStartupCB startup_cb;
	LogicalDecodeBeginCB begin_cb;
	LogicalDecodeChangeCB change_cb;
	LogicalDecodeTruncateCB truncate_cb;
	LogicalDecodeCommitCB commit_cb;
	LogicalDecodeMessageCB message_cb;
	LogicalDecodeFilterByOriginCB filter_by_origin_cb;
	LogicalDecodeShutdownCB shutdown_cb;

	/* streaming of changes at prepare time */
	LogicalDecodeFilterPrepareCB filter_prepare_cb;
	LogicalDecodeBeginPrepareCB begin_prepare_cb;
	LogicalDecodePrepareCB prepare_cb;
	LogicalDecodeCommitPreparedCB commit_prepared_cb;
	LogicalDecodeRollbackPreparedCB rollback_prepared_cb;

	/* streaming of changes */
	LogicalDecodeStreamStartCB stream_start_cb;
	LogicalDecodeStreamStopCB stream_stop_cb;
	LogicalDecodeStreamAbortCB stream_abort_cb;
	LogicalDecodeStreamPrepareCB stream_prepare_cb;
	LogicalDecodeStreamCommitCB stream_commit_cb;
	LogicalDecodeStreamChangeCB stream_change_cb;
	LogicalDecodeStreamMessageCB stream_message_cb;
	LogicalDecodeStreamTruncateCB stream_truncate_cb;
} OutputPluginCallbacks;

/* Functions in replication/logical/logical.c */
extern void OutputPluginPrepareWrite(struct LogicalDecodingContext *ctx, bool last_write);
extern void OutputPluginWrite(struct LogicalDecodingContext *ctx, bool last_write);
extern void OutputPluginUpdateProgress(struct LogicalDecodingContext *ctx);

#endif							/* OUTPUT_PLUGIN_H */
