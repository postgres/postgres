/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xact.h,v 1.68 2004/07/31 07:39:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/xlog.h"
#include "storage/relfilenode.h"
#include "nodes/pg_list.h"
#include "utils/nabstime.h"


/*
 * Xact isolation levels
 */
#define XACT_READ_UNCOMMITTED	0
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2
#define XACT_SERIALIZABLE		3

extern int	DefaultXactIsoLevel;
extern int	XactIsoLevel;

/*
 * We only implement two distinct levels, so this is a convenience to
 * check which level we're really using internally.
 */
#define IsXactIsoLevelSerializable ((XactIsoLevel == XACT_REPEATABLE_READ || XactIsoLevel == XACT_SERIALIZABLE))

/* Xact read-only state */
extern bool DefaultXactReadOnly;
extern bool XactReadOnly;

/*
 *	end-of-transaction cleanup callbacks for dynamically loaded modules
 */
typedef void (*EOXactCallback) (bool isCommit, void *arg);


/* ----------------
 *		transaction-related XLOG entries
 * ----------------
 */

/*
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_XACT_COMMIT	0x00
#define XLOG_XACT_ABORT		0x20

typedef struct xl_xact_commit
{
	time_t		xtime;
	int			nrels;			/* number of RelFileNodes */
	int			nsubxacts;		/* number of subtransaction XIDs */
	/* Array of RelFileNode(s) to drop at commit */
	RelFileNode	xnodes[1];		/* VARIABLE LENGTH ARRAY */
	/* ARRAY OF COMMITTED SUBTRANSACTION XIDs FOLLOWS */
} xl_xact_commit;

#define MinSizeOfXactCommit	offsetof(xl_xact_commit, xnodes)

typedef struct xl_xact_abort
{
	time_t		xtime;
	int			nrels;			/* number of RelFileNodes */
	int			nsubxacts;		/* number of subtransaction XIDs */
	/* Array of RelFileNode(s) to drop at abort */
	RelFileNode	xnodes[1];		/* VARIABLE LENGTH ARRAY */
	/* ARRAY OF ABORTED SUBTRANSACTION XIDs FOLLOWS */
} xl_xact_abort;

#define MinSizeOfXactAbort offsetof(xl_xact_abort, xnodes)


/* ----------------
 *		extern definitions
 * ----------------
 */
extern bool IsTransactionState(void);
extern bool IsAbortedTransactionBlockState(void);
extern TransactionId GetTopTransactionId(void);
extern TransactionId GetCurrentTransactionId(void);
extern CommandId GetCurrentCommandId(void);
extern AbsoluteTime GetCurrentTransactionStartTime(void);
extern AbsoluteTime GetCurrentTransactionStartTimeUsec(int *usec);
extern int	GetCurrentTransactionNestLevel(void);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern void CommandCounterIncrement(void);
extern void StartTransactionCommand(void);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern bool EndTransactionBlock(void);
extern void UserAbortTransactionBlock(void);
extern void ReleaseSavepoint(List *options);
extern void DefineSavepoint(char *name);
extern void RollbackToSavepoint(List *options);
extern void BeginInternalSubTransaction(char *name);
extern void ReleaseCurrentSubTransaction(void);
extern void RollbackAndReleaseCurrentSubTransaction(void);
extern bool IsSubTransaction(void);
extern bool IsTransactionBlock(void);
extern bool IsTransactionOrTransactionBlock(void);
extern char TransactionBlockStatusCode(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventTransactionChain(void *stmtNode, const char *stmtType);
extern void RequireTransactionChain(void *stmtNode, const char *stmtType);
extern bool IsInTransactionChain(void *stmtNode);
extern void RegisterEOXactCallback(EOXactCallback callback, void *arg);
extern void UnregisterEOXactCallback(EOXactCallback callback, void *arg);

extern void RecordTransactionCommit(void);

extern int	xactGetCommittedChildren(TransactionId **ptr);

extern void XactPushRollback(void (*func) (void *), void *data);
extern void XactPopRollback(void);

extern void xact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_undo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_desc(char *buf, uint8 xl_info, char *rec);

#endif   /* XACT_H */
