/*-------------------------------------------------------------------------
 * logical.h
 *	   PostgreSQL logical decoding coordination
 *
 * Copyright (c) 2012-2020, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICAL_H
#define LOGICAL_H

#include "access/xlog.h"
#include "access/xlogreader.h"
#include "replication/output_plugin.h"
#include "replication/slot.h"

struct LogicalDecodingContext;

typedef void (*LogicalOutputPluginWriterWrite) (struct LogicalDecodingContext *lr,
												XLogRecPtr Ptr,
												TransactionId xid,
												bool last_write
);

typedef LogicalOutputPluginWriterWrite LogicalOutputPluginWriterPrepareWrite;

typedef void (*LogicalOutputPluginWriterUpdateProgress) (struct LogicalDecodingContext *lr,
														 XLogRecPtr Ptr,
														 TransactionId xid
);

typedef struct LogicalDecodingContext
{
	/* memory context this is all allocated in */
	MemoryContext context;

	/* The associated replication slot */
	ReplicationSlot *slot;

	/* infrastructure pieces for decoding */
	XLogReaderState *reader;
	struct ReorderBuffer *reorder;
	struct SnapBuild *snapshot_builder;

	/*
	 * Marks the logical decoding context as fast forward decoding one. Such a
	 * context does not have plugin loaded so most of the following properties
	 * are unused.
	 */
	bool		fast_forward;

	/* Are we processing the end LSN of a transaction? */
	bool		end_xact;

	OutputPluginCallbacks callbacks;
	OutputPluginOptions options;

	/*
	 * User specified options
	 */
	List	   *output_plugin_options;

	/*
	 * User-Provided callback for writing/streaming out data.
	 */
	LogicalOutputPluginWriterPrepareWrite prepare_write;
	LogicalOutputPluginWriterWrite write;
	LogicalOutputPluginWriterUpdateProgress update_progress;

	/*
	 * Output buffer.
	 */
	StringInfo	out;

	/*
	 * Private data pointer of the output plugin.
	 */
	void	   *output_plugin_private;

	/*
	 * Private data pointer for the data writer.
	 */
	void	   *output_writer_private;

	/*
	 * State for writing output.
	 */
	bool		accept_writes;
	bool		prepared_write;
	XLogRecPtr	write_location;
	TransactionId write_xid;
} LogicalDecodingContext;


extern void CheckLogicalDecodingRequirements(void);

extern LogicalDecodingContext *CreateInitDecodingContext(char *plugin,
														 List *output_plugin_options,
														 bool need_full_snapshot,
														 XLogRecPtr restart_lsn,
														 XLogReaderRoutine *xl_routine,
														 LogicalOutputPluginWriterPrepareWrite prepare_write,
														 LogicalOutputPluginWriterWrite do_write,
														 LogicalOutputPluginWriterUpdateProgress update_progress);
extern LogicalDecodingContext *CreateDecodingContext(XLogRecPtr start_lsn,
													 List *output_plugin_options,
													 bool fast_forward,
													 XLogReaderRoutine *xl_routine,
													 LogicalOutputPluginWriterPrepareWrite prepare_write,
													 LogicalOutputPluginWriterWrite do_write,
													 LogicalOutputPluginWriterUpdateProgress update_progress);
extern void DecodingContextFindStartpoint(LogicalDecodingContext *ctx);
extern bool DecodingContextReady(LogicalDecodingContext *ctx);
extern void FreeDecodingContext(LogicalDecodingContext *ctx);

extern void LogicalIncreaseXminForSlot(XLogRecPtr lsn, TransactionId xmin);
extern void LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn,
												  XLogRecPtr restart_lsn);
extern void LogicalConfirmReceivedLocation(XLogRecPtr lsn);

extern bool filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, RepOriginId origin_id);

#endif
