/* -------------------------------------------------------------------------
 *
 * decode.c
 *		This module decodes WAL records read using xlogreader.h's APIs for the
 *		purpose of logical decoding by passing information to the
 *		reorderbuffer module (containing the actual changes) and to the
 *		snapbuild module to build a fitting catalog snapshot (to be able to
 *		properly decode the changes in the reorderbuffer).
 *
 * NOTE:
 *		This basically tries to handle all low level xlog stuff for
 *		reorderbuffer.c and snapbuild.c. There's some minor leakage where a
 *		specific record's struct is used to pass data along, but those just
 *		happen to contain the right amount of data in a convenient
 *		format. There isn't and shouldn't be much intelligence about the
 *		contents of records in here except turning them into a more usable
 *		format.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/decode.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"

#include "catalog/pg_control.h"

#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"

#include "storage/standby.h"

typedef struct XLogRecordBuffer
{
	XLogRecPtr	origptr;
	XLogRecPtr	endptr;
	XLogRecord	record;
	char	   *record_data;
} XLogRecordBuffer;

/* RMGR Handlers */
static void DecodeXLogOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeHeapOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeHeap2Op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeXactOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeStandbyOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

/* individual record(group)'s handlers */
static void DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			 TransactionId xid, Oid dboid,
			 TimestampTz commit_time,
			 int nsubxacts, TransactionId *sub_xids,
			 int ninval_msgs, SharedInvalidationMessage *msg);
static void DecodeAbort(LogicalDecodingContext *ctx, XLogRecPtr lsn,
			TransactionId xid, TransactionId *sub_xids, int nsubxacts);

/* common function to decode tuples */
static void DecodeXLogTuple(char *data, Size len, ReorderBufferTupleBuf *tup);

/*
 * Take every XLogReadRecord()ed record and perform the actions required to
 * decode it using the output plugin already setup in the logical decoding
 * context.
 */
void
LogicalDecodingProcessRecord(LogicalDecodingContext *ctx, XLogRecord *record)
{
	XLogRecordBuffer buf;

	buf.origptr = ctx->reader->ReadRecPtr;
	buf.endptr = ctx->reader->EndRecPtr;
	buf.record = *record;
	buf.record_data = XLogRecGetData(record);

	/* cast so we get a warning when new rmgrs are added */
	switch ((RmgrIds) buf.record.xl_rmid)
	{
			/*
			 * Rmgrs we care about for logical decoding. Add new rmgrs in
			 * rmgrlist.h's order.
			 */
		case RM_XLOG_ID:
			DecodeXLogOp(ctx, &buf);
			break;

		case RM_XACT_ID:
			DecodeXactOp(ctx, &buf);
			break;

		case RM_STANDBY_ID:
			DecodeStandbyOp(ctx, &buf);
			break;

		case RM_HEAP2_ID:
			DecodeHeap2Op(ctx, &buf);
			break;

		case RM_HEAP_ID:
			DecodeHeapOp(ctx, &buf);
			break;

			/*
			 * Rmgrs irrelevant for logical decoding; they describe stuff not
			 * represented in logical decoding. Add new rmgrs in rmgrlist.h's
			 * order.
			 */
		case RM_SMGR_ID:
		case RM_CLOG_ID:
		case RM_DBASE_ID:
		case RM_TBLSPC_ID:
		case RM_MULTIXACT_ID:
		case RM_RELMAP_ID:
		case RM_BTREE_ID:
		case RM_HASH_ID:
		case RM_GIN_ID:
		case RM_GIST_ID:
		case RM_SEQ_ID:
		case RM_SPGIST_ID:
			break;
		case RM_NEXT_ID:
			elog(ERROR, "unexpected RM_NEXT_ID rmgr_id: %u", (RmgrIds) buf.record.xl_rmid);
	}
}

/*
 * Handle rmgr XLOG_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeXLogOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	uint8		info = buf->record.xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
			/* this is also used in END_OF_RECOVERY checkpoints */
		case XLOG_CHECKPOINT_SHUTDOWN:
		case XLOG_END_OF_RECOVERY:
			SnapBuildSerializationPoint(builder, buf->origptr);

			break;
		case XLOG_CHECKPOINT_ONLINE:

			/*
			 * a RUNNING_XACTS record will have been logged near to this, we
			 * can restart from there.
			 */
			break;
		case XLOG_NOOP:
		case XLOG_NEXTOID:
		case XLOG_SWITCH:
		case XLOG_BACKUP_END:
		case XLOG_PARAMETER_CHANGE:
		case XLOG_RESTORE_POINT:
		case XLOG_FPW_CHANGE:
		case XLOG_FPI:
			break;
		default:
			elog(ERROR, "unexpected RM_XLOG_ID record type: %u", info);
	}
}

/*
 * Handle rmgr XACT_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeXactOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	ReorderBuffer *reorder = ctx->reorder;
	XLogRecord *r = &buf->record;
	uint8		info = r->xl_info & ~XLR_INFO_MASK;

	/* no point in doing anything yet, data could not be decoded anyway */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_XACT_COMMIT:
			{
				xl_xact_commit *xlrec;
				TransactionId *subxacts = NULL;
				SharedInvalidationMessage *invals = NULL;

				xlrec = (xl_xact_commit *) buf->record_data;

				subxacts = (TransactionId *) &(xlrec->xnodes[xlrec->nrels]);
				invals = (SharedInvalidationMessage *) &(subxacts[xlrec->nsubxacts]);

				DecodeCommit(ctx, buf, r->xl_xid, xlrec->dbId,
							 xlrec->xact_time,
							 xlrec->nsubxacts, subxacts,
							 xlrec->nmsgs, invals);

				break;
			}
		case XLOG_XACT_COMMIT_PREPARED:
			{
				xl_xact_commit_prepared *prec;
				xl_xact_commit *xlrec;
				TransactionId *subxacts;
				SharedInvalidationMessage *invals = NULL;

				/* Prepared commits contain a normal commit record... */
				prec = (xl_xact_commit_prepared *) buf->record_data;
				xlrec = &prec->crec;

				subxacts = (TransactionId *) &(xlrec->xnodes[xlrec->nrels]);
				invals = (SharedInvalidationMessage *) &(subxacts[xlrec->nsubxacts]);

				DecodeCommit(ctx, buf, prec->xid, xlrec->dbId,
							 xlrec->xact_time,
							 xlrec->nsubxacts, subxacts,
							 xlrec->nmsgs, invals);

				break;
			}
		case XLOG_XACT_COMMIT_COMPACT:
			{
				xl_xact_commit_compact *xlrec;

				xlrec = (xl_xact_commit_compact *) buf->record_data;

				DecodeCommit(ctx, buf, r->xl_xid, InvalidOid,
							 xlrec->xact_time,
							 xlrec->nsubxacts, xlrec->subxacts,
							 0, NULL);
				break;
			}
		case XLOG_XACT_ABORT:
			{
				xl_xact_abort *xlrec;
				TransactionId *sub_xids;

				xlrec = (xl_xact_abort *) buf->record_data;

				sub_xids = (TransactionId *) &(xlrec->xnodes[xlrec->nrels]);

				DecodeAbort(ctx, buf->origptr, r->xl_xid,
							sub_xids, xlrec->nsubxacts);
				break;
			}
		case XLOG_XACT_ABORT_PREPARED:
			{
				xl_xact_abort_prepared *prec;
				xl_xact_abort *xlrec;
				TransactionId *sub_xids;

				/* prepared abort contain a normal commit abort... */
				prec = (xl_xact_abort_prepared *) buf->record_data;
				xlrec = &prec->arec;

				sub_xids = (TransactionId *) &(xlrec->xnodes[xlrec->nrels]);

				/* r->xl_xid is committed in a separate record */
				DecodeAbort(ctx, buf->origptr, prec->xid,
							sub_xids, xlrec->nsubxacts);
				break;
			}

		case XLOG_XACT_ASSIGNMENT:
			{
				xl_xact_assignment *xlrec;
				int			i;
				TransactionId *sub_xid;

				xlrec = (xl_xact_assignment *) buf->record_data;

				sub_xid = &xlrec->xsub[0];

				for (i = 0; i < xlrec->nsubxacts; i++)
				{
					ReorderBufferAssignChild(reorder, xlrec->xtop,
											 *(sub_xid++), buf->origptr);
				}
				break;
			}
		case XLOG_XACT_PREPARE:

			/*
			 * Currently decoding ignores PREPARE TRANSACTION and will just
			 * decode the transaction when the COMMIT PREPARED is sent or
			 * throw away the transaction's contents when a ROLLBACK PREPARED
			 * is received. In the future we could add code to expose prepared
			 * transactions in the changestream allowing for a kind of
			 * distributed 2PC.
			 */
			break;
		default:
			elog(ERROR, "unexpected RM_XACT_ID record type: %u", info);
	}
}

/*
 * Handle rmgr STANDBY_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeStandbyOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogRecord *r = &buf->record;
	uint8		info = r->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_RUNNING_XACTS:
			{
				xl_running_xacts *running = (xl_running_xacts *) buf->record_data;

				SnapBuildProcessRunningXacts(builder, buf->origptr, running);

				/*
				 * Abort all transactions that we keep track of, that are
				 * older than the record's oldestRunningXid. This is the most
				 * convenient spot for doing so since, in contrast to shutdown
				 * or end-of-recovery checkpoints, we have information about
				 * all running transactions which includes prepared ones,
				 * while shutdown checkpoints just know that no non-prepared
				 * transactions are in progress.
				 */
				ReorderBufferAbortOld(ctx->reorder, running->oldestRunningXid);
			}
			break;
		case XLOG_STANDBY_LOCK:
			break;
		default:
			elog(ERROR, "unexpected RM_STANDBY_ID record type: %u", info);
	}
}

/*
 * Handle rmgr HEAP2_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeHeap2Op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = buf->record.xl_info & XLOG_HEAP_OPMASK;
	TransactionId xid = buf->record.xl_xid;
	SnapBuild  *builder = ctx->snapshot_builder;

	/* no point in doing anything yet */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_HEAP2_MULTI_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeMultiInsert(ctx, buf);
			break;
		case XLOG_HEAP2_NEW_CID:
			{
				xl_heap_new_cid *xlrec;

				xlrec = (xl_heap_new_cid *) buf->record_data;
				SnapBuildProcessNewCid(builder, xid, buf->origptr, xlrec);

				break;
			}
		case XLOG_HEAP2_REWRITE:

			/*
			 * Although these records only exist to serve the needs of logical
			 * decoding, all the work happens as part of crash or archive
			 * recovery, so we don't need to do anything here.
			 */
			break;

			/*
			 * Everything else here is just low level physical stuff we're not
			 * interested in.
			 */
		case XLOG_HEAP2_FREEZE_PAGE:
		case XLOG_HEAP2_CLEAN:
		case XLOG_HEAP2_CLEANUP_INFO:
		case XLOG_HEAP2_VISIBLE:
		case XLOG_HEAP2_LOCK_UPDATED:
			break;
		default:
			elog(ERROR, "unexpected RM_HEAP2_ID record type: %u", info);
	}
}

/*
 * Handle rmgr HEAP_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeHeapOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = buf->record.xl_info & XLOG_HEAP_OPMASK;
	TransactionId xid = buf->record.xl_xid;
	SnapBuild  *builder = ctx->snapshot_builder;

	/* no point in doing anything yet */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_HEAP_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeInsert(ctx, buf);
			break;

			/*
			 * Treat HOT update as normal updates. There is no useful
			 * information in the fact that we could make it a HOT update
			 * locally and the WAL layout is compatible.
			 */
		case XLOG_HEAP_HOT_UPDATE:
		case XLOG_HEAP_UPDATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeUpdate(ctx, buf);
			break;

		case XLOG_HEAP_DELETE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeDelete(ctx, buf);
			break;

		case XLOG_HEAP_NEWPAGE:

			/*
			 * This is only used in places like indexams and CLUSTER which
			 * don't contain changes relevant for logical replication.
			 */
			break;

		case XLOG_HEAP_INPLACE:

			/*
			 * Inplace updates are only ever performed on catalog tuples and
			 * can, per definition, not change tuple visibility.  Since we
			 * don't decode catalog tuples, we're not interested in the
			 * record's contents.
			 *
			 * In-place updates can be used either by XID-bearing transactions
			 * (e.g.  in CREATE INDEX CONCURRENTLY) or by XID-less
			 * transactions (e.g.  VACUUM).  In the former case, the commit
			 * record will include cache invalidations, so we mark the
			 * transaction as catalog modifying here. Currently that's
			 * redundant because the commit will do that as well, but once we
			 * support decoding in-progress relations, this will be important.
			 */
			if (!TransactionIdIsValid(xid))
				break;

			SnapBuildProcessChange(builder, xid, buf->origptr);
			ReorderBufferXidSetCatalogChanges(ctx->reorder, xid, buf->origptr);
			break;

		case XLOG_HEAP_LOCK:
			/* we don't care about row level locks for now */
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

/*
 * Consolidated commit record handling between the different form of commit
 * records.
 */
static void
DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			 TransactionId xid, Oid dboid,
			 TimestampTz commit_time,
			 int nsubxacts, TransactionId *sub_xids,
			 int ninval_msgs, SharedInvalidationMessage *msgs)
{
	int			i;

	/*
	 * Process invalidation messages, even if we're not interested in the
	 * transaction's contents, since the various caches need to always be
	 * consistent.
	 */
	if (ninval_msgs > 0)
	{
		ReorderBufferAddInvalidations(ctx->reorder, xid, buf->origptr,
									  ninval_msgs, msgs);
		ReorderBufferXidSetCatalogChanges(ctx->reorder, xid, buf->origptr);
	}

	SnapBuildCommitTxn(ctx->snapshot_builder, buf->origptr, xid,
					   nsubxacts, sub_xids);

	/* ----
	 * Check whether we are interested in this specific transaction, and tell
	 * the reorderbuffer to forget the content of the (sub-)transactions
	 * if not.
	 *
	 * There basically two reasons we might not be interested in this
	 * transaction:
	 * 1) We might not be interested in decoding transactions up to this
	 *	  LSN. This can happen because we previously decoded it and now just
	 *	  are restarting or if we haven't assembled a consistent snapshot yet.
	 * 2) The transaction happened in another database.
	 *
	 * We can't just use ReorderBufferAbort() here, because we need to execute
	 * the transaction's invalidations.  This currently won't be needed if
	 * we're just skipping over the transaction because currently we only do
	 * so during startup, to get to the first transaction the client needs. As
	 * we have reset the catalog caches before starting to read WAL, and we
	 * haven't yet touched any catalogs, there can't be anything to invalidate.
	 * But if we're "forgetting" this commit because it's it happened in
	 * another database, the invalidations might be important, because they
	 * could be for shared catalogs and we might have loaded data into the
	 * relevant syscaches.
	 * ---
	 */
	if (SnapBuildXactNeedsSkip(ctx->snapshot_builder, buf->origptr) ||
		(dboid != InvalidOid && dboid != ctx->slot->data.database))
	{
		for (i = 0; i < nsubxacts; i++)
		{
			ReorderBufferForget(ctx->reorder, *sub_xids, buf->origptr);
			sub_xids++;
		}
		ReorderBufferForget(ctx->reorder, xid, buf->origptr);

		return;
	}

	/* tell the reorderbuffer about the surviving subtransactions */
	for (i = 0; i < nsubxacts; i++)
	{
		ReorderBufferCommitChild(ctx->reorder, xid, *sub_xids,
								 buf->origptr, buf->endptr);
		sub_xids++;
	}

	/* replay actions of all transaction + subtransactions in order */
	ReorderBufferCommit(ctx->reorder, xid, buf->origptr, buf->endptr,
						commit_time);
}

/*
 * Get the data from the various forms of abort records and pass it on to
 * snapbuild.c and reorderbuffer.c
 */
static void
DecodeAbort(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
			TransactionId *sub_xids, int nsubxacts)
{
	int			i;

	SnapBuildAbortTxn(ctx->snapshot_builder, lsn, xid, nsubxacts, sub_xids);

	for (i = 0; i < nsubxacts; i++)
	{
		ReorderBufferAbort(ctx->reorder, *sub_xids, lsn);
		sub_xids++;
	}

	ReorderBufferAbort(ctx->reorder, xid, lsn);
}

/*
 * Parse XLOG_HEAP_INSERT (not MULTI_INSERT!) records into tuplebufs.
 *
 * Deletes can contain the new tuple.
 */
static void
DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogRecord *r = &buf->record;
	xl_heap_insert *xlrec;
	ReorderBufferChange *change;

	xlrec = (xl_heap_insert *) buf->record_data;

	/* only interested in our database */
	if (xlrec->target.node.dbNode != ctx->slot->data.database)
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_INSERT;
	memcpy(&change->data.tp.relnode, &xlrec->target.node, sizeof(RelFileNode));

	if (xlrec->flags & XLOG_HEAP_CONTAINS_NEW_TUPLE)
	{
		Assert(r->xl_len > (SizeOfHeapInsert + SizeOfHeapHeader));

		change->data.tp.newtuple = ReorderBufferGetTupleBuf(ctx->reorder);

		DecodeXLogTuple((char *) xlrec + SizeOfHeapInsert,
						r->xl_len - SizeOfHeapInsert,
						change->data.tp.newtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, r->xl_xid, buf->origptr, change);
}

/*
 * Parse XLOG_HEAP_UPDATE and XLOG_HEAP_HOT_UPDATE, which have the same layout
 * in the record, from wal into proper tuplebufs.
 *
 * Updates can possibly contain a new tuple and the old primary key.
 */
static void
DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogRecord *r = &buf->record;
	xl_heap_update *xlrec;
	xl_heap_header_len xlhdr;
	ReorderBufferChange *change;
	char	   *data;

	xlrec = (xl_heap_update *) buf->record_data;

	/* only interested in our database */
	if (xlrec->target.node.dbNode != ctx->slot->data.database)
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_UPDATE;
	memcpy(&change->data.tp.relnode, &xlrec->target.node, sizeof(RelFileNode));

	/* caution, remaining data in record is not aligned */
	data = buf->record_data + SizeOfHeapUpdate;

	if (xlrec->flags & XLOG_HEAP_CONTAINS_NEW_TUPLE)
	{
		Assert(r->xl_len > (SizeOfHeapUpdate + SizeOfHeapHeaderLen));

		memcpy(&xlhdr, data, sizeof(xlhdr));
		data += offsetof(xl_heap_header_len, header);

		change->data.tp.newtuple = ReorderBufferGetTupleBuf(ctx->reorder);

		DecodeXLogTuple(data,
						xlhdr.t_len + SizeOfHeapHeader,
						change->data.tp.newtuple);
		/* skip over the rest of the tuple header */
		data += SizeOfHeapHeader;
		/* skip over the tuple data */
		data += xlhdr.t_len;
	}

	if (xlrec->flags & XLOG_HEAP_CONTAINS_OLD)
	{
		memcpy(&xlhdr, data, sizeof(xlhdr));
		data += offsetof(xl_heap_header_len, header);

		change->data.tp.oldtuple = ReorderBufferGetTupleBuf(ctx->reorder);

		DecodeXLogTuple(data,
						xlhdr.t_len + SizeOfHeapHeader,
						change->data.tp.oldtuple);
#ifdef NOT_USED
		data += SizeOfHeapHeader;
		data += xlhdr.t_len;
#endif
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, r->xl_xid, buf->origptr, change);
}

/*
 * Parse XLOG_HEAP_DELETE from wal into proper tuplebufs.
 *
 * Deletes can possibly contain the old primary key.
 */
static void
DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogRecord *r = &buf->record;
	xl_heap_delete *xlrec;
	ReorderBufferChange *change;

	xlrec = (xl_heap_delete *) buf->record_data;

	/* only interested in our database */
	if (xlrec->target.node.dbNode != ctx->slot->data.database)
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_DELETE;

	memcpy(&change->data.tp.relnode, &xlrec->target.node, sizeof(RelFileNode));

	/* old primary key stored */
	if (xlrec->flags & XLOG_HEAP_CONTAINS_OLD)
	{
		Assert(r->xl_len > (SizeOfHeapDelete + SizeOfHeapHeader));

		change->data.tp.oldtuple = ReorderBufferGetTupleBuf(ctx->reorder);

		DecodeXLogTuple((char *) xlrec + SizeOfHeapDelete,
						r->xl_len - SizeOfHeapDelete,
						change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, r->xl_xid, buf->origptr, change);
}

/*
 * Decode XLOG_HEAP2_MULTI_INSERT_insert record into multiple tuplebufs.
 *
 * Currently MULTI_INSERT will always contain the full tuples.
 */
static void
DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogRecord *r = &buf->record;
	xl_heap_multi_insert *xlrec;
	int			i;
	char	   *data;
	bool		isinit = (r->xl_info & XLOG_HEAP_INIT_PAGE) != 0;

	xlrec = (xl_heap_multi_insert *) buf->record_data;

	/* only interested in our database */
	if (xlrec->node.dbNode != ctx->slot->data.database)
		return;

	data = buf->record_data + SizeOfHeapMultiInsert;

	/*
	 * OffsetNumbers (which are not of interest to us) are stored when
	 * XLOG_HEAP_INIT_PAGE is not set -- skip over them.
	 */
	if (!isinit)
		data += sizeof(OffsetNumber) * xlrec->ntuples;

	for (i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change;
		xl_multi_insert_tuple *xlhdr;
		int			datalen;
		ReorderBufferTupleBuf *tuple;

		change = ReorderBufferGetChange(ctx->reorder);
		change->action = REORDER_BUFFER_CHANGE_INSERT;
		memcpy(&change->data.tp.relnode, &xlrec->node, sizeof(RelFileNode));

		/*
		 * CONTAINS_NEW_TUPLE will always be set currently as multi_insert
		 * isn't used for catalogs, but better be future proof.
		 *
		 * We decode the tuple in pretty much the same way as DecodeXLogTuple,
		 * but since the layout is slightly different, we can't use it here.
		 */
		if (xlrec->flags & XLOG_HEAP_CONTAINS_NEW_TUPLE)
		{
			change->data.tp.newtuple = ReorderBufferGetTupleBuf(ctx->reorder);

			tuple = change->data.tp.newtuple;

			/* not a disk based tuple */
			ItemPointerSetInvalid(&tuple->tuple.t_self);

			xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(data);
			data = ((char *) xlhdr) + SizeOfMultiInsertTuple;
			datalen = xlhdr->datalen;

			/*
			 * We can only figure this out after reassembling the
			 * transactions.
			 */
			tuple->tuple.t_tableOid = InvalidOid;
			tuple->tuple.t_data = &tuple->header;
			tuple->tuple.t_len = datalen
				+ offsetof(HeapTupleHeaderData, t_bits);

			memset(&tuple->header, 0, sizeof(HeapTupleHeaderData));

			memcpy((char *) &tuple->header
				   + offsetof(HeapTupleHeaderData, t_bits),
				   (char *) data,
				   datalen);
			data += datalen;

			tuple->header.t_infomask = xlhdr->t_infomask;
			tuple->header.t_infomask2 = xlhdr->t_infomask2;
			tuple->header.t_hoff = xlhdr->t_hoff;
		}

		/*
		 * Reset toast reassembly state only after the last row in the last
		 * xl_multi_insert_tuple record emitted by one heap_multi_insert()
		 * call.
		 */
		if (xlrec->flags & XLOG_HEAP_LAST_MULTI_INSERT &&
			(i + 1) == xlrec->ntuples)
			change->data.tp.clear_toast_afterwards = true;
		else
			change->data.tp.clear_toast_afterwards = false;

		ReorderBufferQueueChange(ctx->reorder, r->xl_xid,
								 buf->origptr, change);
	}
}

/*
 * Read a HeapTuple as WAL logged by heap_insert, heap_update and heap_delete
 * (but not by heap_multi_insert) into a tuplebuf.
 *
 * The size 'len' and the pointer 'data' in the record need to be
 * computed outside as they are record specific.
 */
static void
DecodeXLogTuple(char *data, Size len, ReorderBufferTupleBuf *tuple)
{
	xl_heap_header xlhdr;
	int			datalen = len - SizeOfHeapHeader;

	Assert(datalen >= 0);
	Assert(datalen <= MaxHeapTupleSize);

	tuple->tuple.t_len = datalen + offsetof(HeapTupleHeaderData, t_bits);

	/* not a disk based tuple */
	ItemPointerSetInvalid(&tuple->tuple.t_self);

	/* we can only figure this out after reassembling the transactions */
	tuple->tuple.t_tableOid = InvalidOid;
	tuple->tuple.t_data = &tuple->header;

	/* data is not stored aligned, copy to aligned storage */
	memcpy((char *) &xlhdr,
		   data,
		   SizeOfHeapHeader);

	memset(&tuple->header, 0, sizeof(HeapTupleHeaderData));

	memcpy((char *) &tuple->header + offsetof(HeapTupleHeaderData, t_bits),
		   data + SizeOfHeapHeader,
		   datalen);

	tuple->header.t_infomask = xlhdr.t_infomask;
	tuple->header.t_infomask2 = xlhdr.t_infomask2;
	tuple->header.t_hoff = xlhdr.t_hoff;
}
