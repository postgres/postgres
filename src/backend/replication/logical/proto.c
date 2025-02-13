/*-------------------------------------------------------------------------
 *
 * proto.c
 *		logical replication protocol functions
 *
 * Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/replication/logical/proto.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "replication/logicalproto.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * Protocol message flags.
 */
#define LOGICALREP_IS_REPLICA_IDENTITY 1

#define MESSAGE_TRANSACTIONAL (1<<0)
#define TRUNCATE_CASCADE		(1<<0)
#define TRUNCATE_RESTART_SEQS	(1<<1)

static void logicalrep_write_attrs(StringInfo out, Relation rel,
								   Bitmapset *columns,
								   PublishGencolsType include_gencols_type);
static void logicalrep_write_tuple(StringInfo out, Relation rel,
								   TupleTableSlot *slot,
								   bool binary, Bitmapset *columns,
								   PublishGencolsType include_gencols_type);
static void logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel);
static void logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple);

static void logicalrep_write_namespace(StringInfo out, Oid nspid);
static const char *logicalrep_read_namespace(StringInfo in);

/*
 * Write BEGIN to the output stream.
 */
void
logicalrep_write_begin(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_BEGIN);

	/* fixed fields */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);
}

/*
 * Read transaction BEGIN from the stream.
 */
void
logicalrep_read_begin(StringInfo in, LogicalRepBeginData *begin_data)
{
	/* read fields */
	begin_data->final_lsn = pq_getmsgint64(in);
	if (begin_data->final_lsn == InvalidXLogRecPtr)
		elog(ERROR, "final_lsn not set in begin message");
	begin_data->committime = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);
}


/*
 * Write COMMIT to the output stream.
 */
void
logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_COMMIT);

	/* send the flags field (unused for now) */
	pq_sendbyte(out, flags);

	/* send fields */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
}

/*
 * Read transaction COMMIT from the stream.
 */
void
logicalrep_read_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	/* read flags (unused for now) */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* read fields */
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);
}

/*
 * Write BEGIN PREPARE to the output stream.
 */
void
logicalrep_write_begin_prepare(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_BEGIN_PREPARE);

	/* fixed fields */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.prepare_time);
	pq_sendint32(out, txn->xid);

	/* send gid */
	pq_sendstring(out, txn->gid);
}

/*
 * Read transaction BEGIN PREPARE from the stream.
 */
void
logicalrep_read_begin_prepare(StringInfo in, LogicalRepPreparedTxnData *begin_data)
{
	/* read fields */
	begin_data->prepare_lsn = pq_getmsgint64(in);
	if (begin_data->prepare_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_lsn not set in begin prepare message");
	begin_data->end_lsn = pq_getmsgint64(in);
	if (begin_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn not set in begin prepare message");
	begin_data->prepare_time = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);

	/* read gid (copy it into a pre-allocated buffer) */
	strlcpy(begin_data->gid, pq_getmsgstring(in), sizeof(begin_data->gid));
}

/*
 * The core functionality for logicalrep_write_prepare and
 * logicalrep_write_stream_prepare.
 */
static void
logicalrep_write_prepare_common(StringInfo out, LogicalRepMsgType type,
								ReorderBufferTXN *txn, XLogRecPtr prepare_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, type);

	/*
	 * This should only ever happen for two-phase commit transactions, in
	 * which case we expect to have a valid GID.
	 */
	Assert(txn->gid != NULL);
	Assert(rbtxn_is_prepared(txn));
	Assert(TransactionIdIsValid(txn->xid));

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* send fields */
	pq_sendint64(out, prepare_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.prepare_time);
	pq_sendint32(out, txn->xid);

	/* send gid */
	pq_sendstring(out, txn->gid);
}

/*
 * Write PREPARE to the output stream.
 */
void
logicalrep_write_prepare(StringInfo out, ReorderBufferTXN *txn,
						 XLogRecPtr prepare_lsn)
{
	logicalrep_write_prepare_common(out, LOGICAL_REP_MSG_PREPARE,
									txn, prepare_lsn);
}

/*
 * The core functionality for logicalrep_read_prepare and
 * logicalrep_read_stream_prepare.
 */
static void
logicalrep_read_prepare_common(StringInfo in, char *msgtype,
							   LogicalRepPreparedTxnData *prepare_data)
{
	/* read flags */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in %s message", flags, msgtype);

	/* read fields */
	prepare_data->prepare_lsn = pq_getmsgint64(in);
	if (prepare_data->prepare_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_lsn is not set in %s message", msgtype);
	prepare_data->end_lsn = pq_getmsgint64(in);
	if (prepare_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn is not set in %s message", msgtype);
	prepare_data->prepare_time = pq_getmsgint64(in);
	prepare_data->xid = pq_getmsgint(in, 4);
	if (prepare_data->xid == InvalidTransactionId)
		elog(ERROR, "invalid two-phase transaction ID in %s message", msgtype);

	/* read gid (copy it into a pre-allocated buffer) */
	strlcpy(prepare_data->gid, pq_getmsgstring(in), sizeof(prepare_data->gid));
}

/*
 * Read transaction PREPARE from the stream.
 */
void
logicalrep_read_prepare(StringInfo in, LogicalRepPreparedTxnData *prepare_data)
{
	logicalrep_read_prepare_common(in, "prepare", prepare_data);
}

/*
 * Write COMMIT PREPARED to the output stream.
 */
void
logicalrep_write_commit_prepared(StringInfo out, ReorderBufferTXN *txn,
								 XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_COMMIT_PREPARED);

	/*
	 * This should only ever happen for two-phase commit transactions, in
	 * which case we expect to have a valid GID.
	 */
	Assert(txn->gid != NULL);

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* send fields */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);

	/* send gid */
	pq_sendstring(out, txn->gid);
}

/*
 * Read transaction COMMIT PREPARED from the stream.
 */
void
logicalrep_read_commit_prepared(StringInfo in, LogicalRepCommitPreparedTxnData *prepare_data)
{
	/* read flags */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit prepared message", flags);

	/* read fields */
	prepare_data->commit_lsn = pq_getmsgint64(in);
	if (prepare_data->commit_lsn == InvalidXLogRecPtr)
		elog(ERROR, "commit_lsn is not set in commit prepared message");
	prepare_data->end_lsn = pq_getmsgint64(in);
	if (prepare_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn is not set in commit prepared message");
	prepare_data->commit_time = pq_getmsgint64(in);
	prepare_data->xid = pq_getmsgint(in, 4);

	/* read gid (copy it into a pre-allocated buffer) */
	strlcpy(prepare_data->gid, pq_getmsgstring(in), sizeof(prepare_data->gid));
}

/*
 * Write ROLLBACK PREPARED to the output stream.
 */
void
logicalrep_write_rollback_prepared(StringInfo out, ReorderBufferTXN *txn,
								   XLogRecPtr prepare_end_lsn,
								   TimestampTz prepare_time)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_ROLLBACK_PREPARED);

	/*
	 * This should only ever happen for two-phase commit transactions, in
	 * which case we expect to have a valid GID.
	 */
	Assert(txn->gid != NULL);

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* send fields */
	pq_sendint64(out, prepare_end_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, prepare_time);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);

	/* send gid */
	pq_sendstring(out, txn->gid);
}

/*
 * Read transaction ROLLBACK PREPARED from the stream.
 */
void
logicalrep_read_rollback_prepared(StringInfo in,
								  LogicalRepRollbackPreparedTxnData *rollback_data)
{
	/* read flags */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in rollback prepared message", flags);

	/* read fields */
	rollback_data->prepare_end_lsn = pq_getmsgint64(in);
	if (rollback_data->prepare_end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_end_lsn is not set in rollback prepared message");
	rollback_data->rollback_end_lsn = pq_getmsgint64(in);
	if (rollback_data->rollback_end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "rollback_end_lsn is not set in rollback prepared message");
	rollback_data->prepare_time = pq_getmsgint64(in);
	rollback_data->rollback_time = pq_getmsgint64(in);
	rollback_data->xid = pq_getmsgint(in, 4);

	/* read gid (copy it into a pre-allocated buffer) */
	strlcpy(rollback_data->gid, pq_getmsgstring(in), sizeof(rollback_data->gid));
}

/*
 * Write STREAM PREPARE to the output stream.
 */
void
logicalrep_write_stream_prepare(StringInfo out,
								ReorderBufferTXN *txn,
								XLogRecPtr prepare_lsn)
{
	logicalrep_write_prepare_common(out, LOGICAL_REP_MSG_STREAM_PREPARE,
									txn, prepare_lsn);
}

/*
 * Read STREAM PREPARE from the stream.
 */
void
logicalrep_read_stream_prepare(StringInfo in, LogicalRepPreparedTxnData *prepare_data)
{
	logicalrep_read_prepare_common(in, "stream prepare", prepare_data);
}

/*
 * Write ORIGIN to the output stream.
 */
void
logicalrep_write_origin(StringInfo out, const char *origin,
						XLogRecPtr origin_lsn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_ORIGIN);

	/* fixed fields */
	pq_sendint64(out, origin_lsn);

	/* origin string */
	pq_sendstring(out, origin);
}

/*
 * Read ORIGIN from the output stream.
 */
char *
logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn)
{
	/* fixed fields */
	*origin_lsn = pq_getmsgint64(in);

	/* return origin */
	return pstrdup(pq_getmsgstring(in));
}

/*
 * Write INSERT to the output stream.
 */
void
logicalrep_write_insert(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *newslot, bool binary,
						Bitmapset *columns,
						PublishGencolsType include_gencols_type)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_INSERT);

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	pq_sendbyte(out, 'N');		/* new tuple follows */
	logicalrep_write_tuple(out, rel, newslot, binary, columns,
						   include_gencols_type);
}

/*
 * Read INSERT from stream.
 *
 * Fills the new tuple.
 */
LogicalRepRelId
logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id */
	relid = pq_getmsgint(in, 4);

	action = pq_getmsgbyte(in);
	if (action != 'N')
		elog(ERROR, "expected new tuple but got %d",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * Write UPDATE to the output stream.
 */
void
logicalrep_write_update(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *oldslot, TupleTableSlot *newslot,
						bool binary, Bitmapset *columns,
						PublishGencolsType include_gencols_type)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_UPDATE);

	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	if (oldslot != NULL)
	{
		if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
			pq_sendbyte(out, 'O');	/* old tuple follows */
		else
			pq_sendbyte(out, 'K');	/* old key follows */
		logicalrep_write_tuple(out, rel, oldslot, binary, columns,
							   include_gencols_type);
	}

	pq_sendbyte(out, 'N');		/* new tuple follows */
	logicalrep_write_tuple(out, rel, newslot, binary, columns,
						   include_gencols_type);
}

/*
 * Read UPDATE from stream.
 */
LogicalRepRelId
logicalrep_read_update(StringInfo in, bool *has_oldtuple,
					   LogicalRepTupleData *oldtup,
					   LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id */
	relid = pq_getmsgint(in, 4);

	/* read and verify action */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O' && action != 'N')
		elog(ERROR, "expected action 'N', 'O' or 'K', got %c",
			 action);

	/* check for old tuple */
	if (action == 'K' || action == 'O')
	{
		logicalrep_read_tuple(in, oldtup);
		*has_oldtuple = true;

		action = pq_getmsgbyte(in);
	}
	else
		*has_oldtuple = false;

	/* check for new  tuple */
	if (action != 'N')
		elog(ERROR, "expected action 'N', got %c",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * Write DELETE to the output stream.
 */
void
logicalrep_write_delete(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *oldslot, bool binary,
						Bitmapset *columns,
						PublishGencolsType include_gencols_type)
{
	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	pq_sendbyte(out, LOGICAL_REP_MSG_DELETE);

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		pq_sendbyte(out, 'O');	/* old tuple follows */
	else
		pq_sendbyte(out, 'K');	/* old key follows */

	logicalrep_write_tuple(out, rel, oldslot, binary, columns,
						   include_gencols_type);
}

/*
 * Read DELETE from stream.
 *
 * Fills the old tuple.
 */
LogicalRepRelId
logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id */
	relid = pq_getmsgint(in, 4);

	/* read and verify action */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O')
		elog(ERROR, "expected action 'O' or 'K', got %c", action);

	logicalrep_read_tuple(in, oldtup);

	return relid;
}

/*
 * Write TRUNCATE to the output stream.
 */
void
logicalrep_write_truncate(StringInfo out,
						  TransactionId xid,
						  int nrelids,
						  Oid relids[],
						  bool cascade, bool restart_seqs)
{
	int			i;
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_TRUNCATE);

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	pq_sendint32(out, nrelids);

	/* encode and send truncate flags */
	if (cascade)
		flags |= TRUNCATE_CASCADE;
	if (restart_seqs)
		flags |= TRUNCATE_RESTART_SEQS;
	pq_sendint8(out, flags);

	for (i = 0; i < nrelids; i++)
		pq_sendint32(out, relids[i]);
}

/*
 * Read TRUNCATE from stream.
 */
List *
logicalrep_read_truncate(StringInfo in,
						 bool *cascade, bool *restart_seqs)
{
	int			i;
	int			nrelids;
	List	   *relids = NIL;
	uint8		flags;

	nrelids = pq_getmsgint(in, 4);

	/* read and decode truncate flags */
	flags = pq_getmsgint(in, 1);
	*cascade = (flags & TRUNCATE_CASCADE) > 0;
	*restart_seqs = (flags & TRUNCATE_RESTART_SEQS) > 0;

	for (i = 0; i < nrelids; i++)
		relids = lappend_oid(relids, pq_getmsgint(in, 4));

	return relids;
}

/*
 * Write MESSAGE to stream
 */
void
logicalrep_write_message(StringInfo out, TransactionId xid, XLogRecPtr lsn,
						 bool transactional, const char *prefix, Size sz,
						 const char *message)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_MESSAGE);

	/* encode and send message flags */
	if (transactional)
		flags |= MESSAGE_TRANSACTIONAL;

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	pq_sendint8(out, flags);
	pq_sendint64(out, lsn);
	pq_sendstring(out, prefix);
	pq_sendint32(out, sz);
	pq_sendbytes(out, message, sz);
}

/*
 * Write relation description to the output stream.
 */
void
logicalrep_write_rel(StringInfo out, TransactionId xid, Relation rel,
					 Bitmapset *columns,
					 PublishGencolsType include_gencols_type)
{
	char	   *relname;

	pq_sendbyte(out, LOGICAL_REP_MSG_RELATION);

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	/* send qualified relation name */
	logicalrep_write_namespace(out, RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	pq_sendstring(out, relname);

	/* send replica identity */
	pq_sendbyte(out, rel->rd_rel->relreplident);

	/* send the attribute info */
	logicalrep_write_attrs(out, rel, columns, include_gencols_type);
}

/*
 * Read the relation info from stream and return as LogicalRepRelation.
 */
LogicalRepRelation *
logicalrep_read_rel(StringInfo in)
{
	LogicalRepRelation *rel = palloc(sizeof(LogicalRepRelation));

	rel->remoteid = pq_getmsgint(in, 4);

	/* Read relation name from stream */
	rel->nspname = pstrdup(logicalrep_read_namespace(in));
	rel->relname = pstrdup(pq_getmsgstring(in));

	/* Read the replica identity. */
	rel->replident = pq_getmsgbyte(in);

	/* Get attribute description */
	logicalrep_read_attrs(in, rel);

	return rel;
}

/*
 * Write type info to the output stream.
 *
 * This function will always write base type info.
 */
void
logicalrep_write_typ(StringInfo out, TransactionId xid, Oid typoid)
{
	Oid			basetypoid = getBaseType(typoid);
	HeapTuple	tup;
	Form_pg_type typtup;

	pq_sendbyte(out, LOGICAL_REP_MSG_TYPE);

	/* transaction ID (if not valid, we're not streaming) */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(basetypoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", basetypoid);
	typtup = (Form_pg_type) GETSTRUCT(tup);

	/* use Oid as type identifier */
	pq_sendint32(out, typoid);

	/* send qualified type name */
	logicalrep_write_namespace(out, typtup->typnamespace);
	pq_sendstring(out, NameStr(typtup->typname));

	ReleaseSysCache(tup);
}

/*
 * Read type info from the output stream.
 */
void
logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp)
{
	ltyp->remoteid = pq_getmsgint(in, 4);

	/* Read type name from stream */
	ltyp->nspname = pstrdup(logicalrep_read_namespace(in));
	ltyp->typname = pstrdup(pq_getmsgstring(in));
}

/*
 * Write a tuple to the outputstream, in the most efficient format possible.
 */
static void
logicalrep_write_tuple(StringInfo out, Relation rel, TupleTableSlot *slot,
					   bool binary, Bitmapset *columns,
					   PublishGencolsType include_gencols_type)
{
	TupleDesc	desc;
	Datum	   *values;
	bool	   *isnull;
	int			i;
	uint16		nliveatts = 0;

	desc = RelationGetDescr(rel);

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	slot_getallattrs(slot);
	values = slot->tts_values;
	isnull = slot->tts_isnull;

	/* Write the values */
	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typtup;
		Form_pg_type typclass;
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		if (isnull[i])
		{
			pq_sendbyte(out, LOGICALREP_COLUMN_NULL);
			continue;
		}

		if (att->attlen == -1 && VARATT_IS_EXTERNAL_ONDISK(values[i]))
		{
			/*
			 * Unchanged toasted datum.  (Note that we don't promise to detect
			 * unchanged data in general; this is just a cheap check to avoid
			 * sending large values unnecessarily.)
			 */
			pq_sendbyte(out, LOGICALREP_COLUMN_UNCHANGED);
			continue;
		}

		typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typtup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);
		typclass = (Form_pg_type) GETSTRUCT(typtup);

		/*
		 * Send in binary if requested and type has suitable send function.
		 */
		if (binary && OidIsValid(typclass->typsend))
		{
			bytea	   *outputbytes;
			int			len;

			pq_sendbyte(out, LOGICALREP_COLUMN_BINARY);
			outputbytes = OidSendFunctionCall(typclass->typsend, values[i]);
			len = VARSIZE(outputbytes) - VARHDRSZ;
			pq_sendint(out, len, 4);	/* length */
			pq_sendbytes(out, VARDATA(outputbytes), len);	/* data */
			pfree(outputbytes);
		}
		else
		{
			char	   *outputstr;

			pq_sendbyte(out, LOGICALREP_COLUMN_TEXT);
			outputstr = OidOutputFunctionCall(typclass->typoutput, values[i]);
			pq_sendcountedtext(out, outputstr, strlen(outputstr));
			pfree(outputstr);
		}

		ReleaseSysCache(typtup);
	}
}

/*
 * Read tuple in logical replication format from stream.
 */
static void
logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple)
{
	int			i;
	int			natts;

	/* Get number of attributes */
	natts = pq_getmsgint(in, 2);

	/* Allocate space for per-column values; zero out unused StringInfoDatas */
	tuple->colvalues = (StringInfoData *) palloc0(natts * sizeof(StringInfoData));
	tuple->colstatus = (char *) palloc(natts * sizeof(char));
	tuple->ncols = natts;

	/* Read the data */
	for (i = 0; i < natts; i++)
	{
		char	   *buff;
		char		kind;
		int			len;
		StringInfo	value = &tuple->colvalues[i];

		kind = pq_getmsgbyte(in);
		tuple->colstatus[i] = kind;

		switch (kind)
		{
			case LOGICALREP_COLUMN_NULL:
				/* nothing more to do */
				break;
			case LOGICALREP_COLUMN_UNCHANGED:
				/* we don't receive the value of an unchanged column */
				break;
			case LOGICALREP_COLUMN_TEXT:
			case LOGICALREP_COLUMN_BINARY:
				len = pq_getmsgint(in, 4);	/* read length */

				/* and data */
				buff = palloc(len + 1);
				pq_copymsgbytes(in, buff, len);

				/*
				 * NUL termination is required for LOGICALREP_COLUMN_TEXT mode
				 * as input functions require that.  For
				 * LOGICALREP_COLUMN_BINARY it's not technically required, but
				 * it's harmless.
				 */
				buff[len] = '\0';

				initStringInfoFromString(value, buff, len);
				break;
			default:
				elog(ERROR, "unrecognized data representation type '%c'", kind);
		}
	}
}

/*
 * Write relation attribute metadata to the stream.
 */
static void
logicalrep_write_attrs(StringInfo out, Relation rel, Bitmapset *columns,
					   PublishGencolsType include_gencols_type)
{
	TupleDesc	desc;
	int			i;
	uint16		nliveatts = 0;
	Bitmapset  *idattrs = NULL;
	bool		replidentfull;

	desc = RelationGetDescr(rel);

	/* send number of live attributes */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	/* fetch bitmap of REPLICATION IDENTITY attributes */
	replidentfull = (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL);
	if (!replidentfull)
		idattrs = RelationGetIdentityKeyBitmap(rel);

	/* send the attributes */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		uint8		flags = 0;

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		/* REPLICA IDENTITY FULL means all columns are sent as part of key. */
		if (replidentfull ||
			bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			flags |= LOGICALREP_IS_REPLICA_IDENTITY;

		pq_sendbyte(out, flags);

		/* attribute name */
		pq_sendstring(out, NameStr(att->attname));

		/* attribute type id */
		pq_sendint32(out, (int) att->atttypid);

		/* attribute mode */
		pq_sendint32(out, att->atttypmod);
	}

	bms_free(idattrs);
}

/*
 * Read relation attribute metadata from the stream.
 */
static void
logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel)
{
	int			i;
	int			natts;
	char	  **attnames;
	Oid		   *atttyps;
	Bitmapset  *attkeys = NULL;

	natts = pq_getmsgint(in, 2);
	attnames = palloc(natts * sizeof(char *));
	atttyps = palloc(natts * sizeof(Oid));

	/* read the attributes */
	for (i = 0; i < natts; i++)
	{
		uint8		flags;

		/* Check for replica identity column */
		flags = pq_getmsgbyte(in);
		if (flags & LOGICALREP_IS_REPLICA_IDENTITY)
			attkeys = bms_add_member(attkeys, i);

		/* attribute name */
		attnames[i] = pstrdup(pq_getmsgstring(in));

		/* attribute type id */
		atttyps[i] = (Oid) pq_getmsgint(in, 4);

		/* we ignore attribute mode for now */
		(void) pq_getmsgint(in, 4);
	}

	rel->attnames = attnames;
	rel->atttyps = atttyps;
	rel->attkeys = attkeys;
	rel->natts = natts;
}

/*
 * Write the namespace name or empty string for pg_catalog (to save space).
 */
static void
logicalrep_write_namespace(StringInfo out, Oid nspid)
{
	if (nspid == PG_CATALOG_NAMESPACE)
		pq_sendbyte(out, '\0');
	else
	{
		char	   *nspname = get_namespace_name(nspid);

		if (nspname == NULL)
			elog(ERROR, "cache lookup failed for namespace %u",
				 nspid);

		pq_sendstring(out, nspname);
	}
}

/*
 * Read the namespace name while treating empty string as pg_catalog.
 */
static const char *
logicalrep_read_namespace(StringInfo in)
{
	const char *nspname = pq_getmsgstring(in);

	if (nspname[0] == '\0')
		nspname = "pg_catalog";

	return nspname;
}

/*
 * Write the information for the start stream message to the output stream.
 */
void
logicalrep_write_stream_start(StringInfo out,
							  TransactionId xid, bool first_segment)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_START);

	Assert(TransactionIdIsValid(xid));

	/* transaction ID (we're starting to stream, so must be valid) */
	pq_sendint32(out, xid);

	/* 1 if this is the first streaming segment for this xid */
	pq_sendbyte(out, first_segment ? 1 : 0);
}

/*
 * Read the information about the start stream message from output stream.
 */
TransactionId
logicalrep_read_stream_start(StringInfo in, bool *first_segment)
{
	TransactionId xid;

	Assert(first_segment);

	xid = pq_getmsgint(in, 4);
	*first_segment = (pq_getmsgbyte(in) == 1);

	return xid;
}

/*
 * Write the stop stream message to the output stream.
 */
void
logicalrep_write_stream_stop(StringInfo out)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_STOP);
}

/*
 * Write STREAM COMMIT to the output stream.
 */
void
logicalrep_write_stream_commit(StringInfo out, ReorderBufferTXN *txn,
							   XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_COMMIT);

	Assert(TransactionIdIsValid(txn->xid));

	/* transaction ID */
	pq_sendint32(out, txn->xid);

	/* send the flags field (unused for now) */
	pq_sendbyte(out, flags);

	/* send fields */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
}

/*
 * Read STREAM COMMIT from the output stream.
 */
TransactionId
logicalrep_read_stream_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	TransactionId xid;
	uint8		flags;

	xid = pq_getmsgint(in, 4);

	/* read flags (unused for now) */
	flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* read fields */
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);

	return xid;
}

/*
 * Write STREAM ABORT to the output stream. Note that xid and subxid will be
 * same for the top-level transaction abort.
 *
 * If write_abort_info is true, send the abort_lsn and abort_time fields,
 * otherwise don't.
 */
void
logicalrep_write_stream_abort(StringInfo out, TransactionId xid,
							  TransactionId subxid, XLogRecPtr abort_lsn,
							  TimestampTz abort_time, bool write_abort_info)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_ABORT);

	Assert(TransactionIdIsValid(xid) && TransactionIdIsValid(subxid));

	/* transaction ID */
	pq_sendint32(out, xid);
	pq_sendint32(out, subxid);

	if (write_abort_info)
	{
		pq_sendint64(out, abort_lsn);
		pq_sendint64(out, abort_time);
	}
}

/*
 * Read STREAM ABORT from the output stream.
 *
 * If read_abort_info is true, read the abort_lsn and abort_time fields,
 * otherwise don't.
 */
void
logicalrep_read_stream_abort(StringInfo in,
							 LogicalRepStreamAbortData *abort_data,
							 bool read_abort_info)
{
	Assert(abort_data);

	abort_data->xid = pq_getmsgint(in, 4);
	abort_data->subxid = pq_getmsgint(in, 4);

	if (read_abort_info)
	{
		abort_data->abort_lsn = pq_getmsgint64(in);
		abort_data->abort_time = pq_getmsgint64(in);
	}
	else
	{
		abort_data->abort_lsn = InvalidXLogRecPtr;
		abort_data->abort_time = 0;
	}
}

/*
 * Get string representing LogicalRepMsgType.
 */
const char *
logicalrep_message_type(LogicalRepMsgType action)
{
	static char err_unknown[20];

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			return "BEGIN";
		case LOGICAL_REP_MSG_COMMIT:
			return "COMMIT";
		case LOGICAL_REP_MSG_ORIGIN:
			return "ORIGIN";
		case LOGICAL_REP_MSG_INSERT:
			return "INSERT";
		case LOGICAL_REP_MSG_UPDATE:
			return "UPDATE";
		case LOGICAL_REP_MSG_DELETE:
			return "DELETE";
		case LOGICAL_REP_MSG_TRUNCATE:
			return "TRUNCATE";
		case LOGICAL_REP_MSG_RELATION:
			return "RELATION";
		case LOGICAL_REP_MSG_TYPE:
			return "TYPE";
		case LOGICAL_REP_MSG_MESSAGE:
			return "MESSAGE";
		case LOGICAL_REP_MSG_BEGIN_PREPARE:
			return "BEGIN PREPARE";
		case LOGICAL_REP_MSG_PREPARE:
			return "PREPARE";
		case LOGICAL_REP_MSG_COMMIT_PREPARED:
			return "COMMIT PREPARED";
		case LOGICAL_REP_MSG_ROLLBACK_PREPARED:
			return "ROLLBACK PREPARED";
		case LOGICAL_REP_MSG_STREAM_START:
			return "STREAM START";
		case LOGICAL_REP_MSG_STREAM_STOP:
			return "STREAM STOP";
		case LOGICAL_REP_MSG_STREAM_COMMIT:
			return "STREAM COMMIT";
		case LOGICAL_REP_MSG_STREAM_ABORT:
			return "STREAM ABORT";
		case LOGICAL_REP_MSG_STREAM_PREPARE:
			return "STREAM PREPARE";
	}

	/*
	 * This message provides context in the error raised when applying a
	 * logical message. So we can't throw an error here. Return an unknown
	 * indicator value so that the original error is still reported.
	 */
	snprintf(err_unknown, sizeof(err_unknown), "??? (%d)", action);

	return err_unknown;
}

/*
 * Check if the column 'att' of a table should be published.
 *
 * 'columns' represents the publication column list (if any) for that table.
 *
 * 'include_gencols_type' value indicates whether generated columns should be
 * published when there is no column list. Typically, this will have the same
 * value as the 'publish_generated_columns' publication parameter.
 *
 * Note that generated columns can be published only when present in a
 * publication column list, or when include_gencols_type is
 * PUBLISH_GENCOLS_STORED.
 */
bool
logicalrep_should_publish_column(Form_pg_attribute att, Bitmapset *columns,
								 PublishGencolsType include_gencols_type)
{
	if (att->attisdropped)
		return false;

	/* If a column list is provided, publish only the cols in that list. */
	if (columns)
		return bms_is_member(att->attnum, columns);

	/* All non-generated columns are always published. */
	if (!att->attgenerated)
		return true;

	/*
	 * Stored generated columns are only published when the user sets
	 * publish_generated_columns as stored.
	 */
	if (att->attgenerated == ATTRIBUTE_GENERATED_STORED)
		return include_gencols_type == PUBLISH_GENCOLS_STORED;

	return false;
}
