/*-------------------------------------------------------------------------
 * logical.h
 *	   PostgreSQL logical decoding coordination
 *
 * Copyright (c) 2012-2026, PostgreSQL Global Development Group
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
														 TransactionId xid,
														 bool skipped_xact
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
	 * Does the output plugin support streaming, and is it enabled?
	 */
	bool		streaming;

	/*
	 * Does the output plugin support two-phase decoding, and is it enabled?
	 */
	bool		twophase;

	/*
	 * Is two-phase option given by output plugin?
	 *
	 * This flag indicates that the plugin passed in the two-phase option as
	 * part of the START_STREAMING command. We can't rely solely on the
	 * twophase flag which only tells whether the plugin provided all the
	 * necessary two-phase callbacks.
	 */
	bool		twophase_opt_given;

	/*
	 * State for writing output.
	 */
	bool		accept_writes;
	bool		prepared_write;
	XLogRecPtr	write_location;
	TransactionId write_xid;
	/* Are we processing the end LSN of a transaction? */
	bool		end_xact;

	/* Do we need to process any change in fast_forward mode? */
	bool		processing_required;
} LogicalDecodingContext;


extern void CheckLogicalDecodingRequirements(bool repack);

extern LogicalDecodingContext *CreateInitDecodingContext(const char *plugin,
														 List *output_plugin_options,
														 bool need_full_snapshot,
														 bool for_repack,
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

extern void LogicalIncreaseXminForSlot(XLogRecPtr current_lsn,
									   TransactionId xmin);
extern void LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn,
												  XLogRecPtr restart_lsn);
extern void LogicalConfirmReceivedLocation(XLogRecPtr lsn);

extern bool filter_prepare_cb_wrapper(LogicalDecodingContext *ctx,
									  TransactionId xid, const char *gid);
extern bool filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, ReplOriginId origin_id);
extern void ResetLogicalStreamingState(void);
extern void UpdateDecodingStats(LogicalDecodingContext *ctx);

extern XLogRecPtr LogicalReplicationSlotCheckPendingWal(XLogRecPtr end_of_wal,
														XLogRecPtr scan_cutoff_lsn);
extern XLogRecPtr LogicalSlotAdvanceAndCheckSnapState(XLogRecPtr moveto,
													  bool *found_consistent_snapshot);


/*
 * This macro determines the log level for messages about starting logical
 * decoding and finding a consistent point.
 *
 * When logical decoding is triggered by a foreground SQL function (e.g.,
 * pg_logical_slot_peek_binary_changes()), these messages are logged at DEBUG1
 * to avoid excessive log noise. This is acceptable since such issues are
 * typically less critical, and the messages can still be enabled by lowering
 * client_min_messages or log_min_messages for the session.
 *
 * When the messages originate from background activity (e.g., walsender or
 * slotsync worker), they are logged at LOG, as these events are less frequent
 * and more likely to indicate issues that DBAs should notice by default.
 *
 * Note that SQL functions executed by the logical walsender are treated as
 * background activity.
 */
#define LogicalDecodingLogLevel() \
	(AmRegularBackendProcess() ? DEBUG1 : LOG)

#endif
