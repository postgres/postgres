/*-------------------------------------------------------------------------
 *
 * logicalproto.h
 *		logical replication protocol
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/logicalproto.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICAL_PROTO_H
#define LOGICAL_PROTO_H

#include "replication/reorderbuffer.h"
#include "utils/rel.h"

/*
 * Protocol capabilities
 *
 * LOGICALREP_PROTO_VERSION_NUM is our native protocol and the greatest version
 * we can support. LOGICALREP_PROTO_MIN_VERSION_NUM is the oldest version we
 * have backwards compatibility for. The client requests protocol version at
 * connect time.
 */
#define LOGICALREP_PROTO_MIN_VERSION_NUM 1
#define LOGICALREP_PROTO_VERSION_NUM 1

/* Tuple coming via logical replication. */
typedef struct LogicalRepTupleData
{
	/* column values in text format, or NULL for a null value: */
	char	   *values[MaxTupleAttributeNumber];
	/* markers for changed/unchanged column values: */
	bool		changed[MaxTupleAttributeNumber];
} LogicalRepTupleData;

typedef uint32 LogicalRepRelId;

/* Relation information */
typedef struct LogicalRepRelation
{
	/* Info coming from the remote side. */
	LogicalRepRelId remoteid;	/* unique id of the relation */
	char	   *nspname;		/* schema name */
	char	   *relname;		/* relation name */
	int			natts;			/* number of columns */
	char	  **attnames;		/* column names */
	Oid		   *atttyps;		/* column types */
	char		replident;		/* replica identity */
	char		relkind;		/* remote relation kind */
	Bitmapset  *attkeys;		/* Bitmap of key columns */
} LogicalRepRelation;

/* Type mapping info */
typedef struct LogicalRepTyp
{
	Oid			remoteid;		/* unique id of the remote type */
	char	   *nspname;		/* schema name of remote type */
	char	   *typname;		/* name of the remote type */
} LogicalRepTyp;

/* Transaction info */
typedef struct LogicalRepBeginData
{
	XLogRecPtr	final_lsn;
	TimestampTz committime;
	TransactionId xid;
} LogicalRepBeginData;

typedef struct LogicalRepCommitData
{
	XLogRecPtr	commit_lsn;
	XLogRecPtr	end_lsn;
	TimestampTz committime;
} LogicalRepCommitData;

extern void logicalrep_write_begin(StringInfo out, ReorderBufferTXN *txn);
extern void logicalrep_read_begin(StringInfo in,
								  LogicalRepBeginData *begin_data);
extern void logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
									XLogRecPtr commit_lsn);
extern void logicalrep_read_commit(StringInfo in,
								   LogicalRepCommitData *commit_data);
extern void logicalrep_write_origin(StringInfo out, const char *origin,
									XLogRecPtr origin_lsn);
extern char *logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn);
extern void logicalrep_write_insert(StringInfo out, Relation rel,
									HeapTuple newtuple);
extern LogicalRepRelId logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup);
extern void logicalrep_write_update(StringInfo out, Relation rel, HeapTuple oldtuple,
									HeapTuple newtuple);
extern LogicalRepRelId logicalrep_read_update(StringInfo in,
											  bool *has_oldtuple, LogicalRepTupleData *oldtup,
											  LogicalRepTupleData *newtup);
extern void logicalrep_write_delete(StringInfo out, Relation rel,
									HeapTuple oldtuple);
extern LogicalRepRelId logicalrep_read_delete(StringInfo in,
											  LogicalRepTupleData *oldtup);
extern void logicalrep_write_truncate(StringInfo out, int nrelids, Oid relids[],
									  bool cascade, bool restart_seqs);
extern List *logicalrep_read_truncate(StringInfo in,
									  bool *cascade, bool *restart_seqs);
extern void logicalrep_write_rel(StringInfo out, Relation rel);
extern LogicalRepRelation *logicalrep_read_rel(StringInfo in);
extern void logicalrep_write_typ(StringInfo out, Oid typoid);
extern void logicalrep_read_typ(StringInfo out, LogicalRepTyp *ltyp);

#endif							/* LOGICAL_PROTO_H */
