/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xact.h,v 1.57 2003/10/16 16:50:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "access/xlog.h"
#include "utils/nabstime.h"
#include "utils/timestamp.h"

/*
 * Xact isolation levels
 */
#define XACT_DIRTY_READ			0		/* not implemented */
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2		/* not implemented */
#define XACT_SERIALIZABLE		3

extern int	DefaultXactIsoLevel;
extern int	XactIsoLevel;

/* Xact read-only state */
extern bool DefaultXactReadOnly;
extern bool XactReadOnly;

/*
 *	transaction states - transaction state from server perspective
 */
typedef enum TransState
{
	TRANS_DEFAULT,
	TRANS_START,
	TRANS_INPROGRESS,
	TRANS_COMMIT,
	TRANS_ABORT
} TransState;

/*
 *	transaction block states - transaction state of client queries
 */
typedef enum TBlockState
{
	TBLOCK_DEFAULT,
	TBLOCK_BEGIN,
	TBLOCK_INPROGRESS,
	TBLOCK_END,
	TBLOCK_ABORT,
	TBLOCK_ENDABORT
} TBlockState;

/*
 *	end-of-transaction cleanup callbacks for dynamically loaded modules
 */
typedef void (*EOXactCallback) (bool isCommit, void *arg);

/*
 *	transaction state structure
 */
typedef struct TransactionStateData
{
	TransactionId transactionIdData;
	CommandId	commandId;
	AbsoluteTime startTime;
	int			startTimeUsec;
	TransState	state;
	TBlockState blockState;
} TransactionStateData;

typedef TransactionStateData *TransactionState;


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

	/*
	 * Array of RelFileNode-s to drop may follow at the end of struct
	 */
} xl_xact_commit;

#define SizeOfXactCommit	((offsetof(xl_xact_commit, xtime) + sizeof(time_t)))

typedef struct xl_xact_abort
{
	time_t		xtime;
} xl_xact_abort;

#define SizeOfXactAbort ((offsetof(xl_xact_abort, xtime) + sizeof(time_t)))

/* ----------------
 *		extern definitions
 * ----------------
 */
extern bool IsTransactionState(void);
extern bool IsAbortedTransactionBlockState(void);
extern TransactionId GetCurrentTransactionId(void);
extern CommandId GetCurrentCommandId(void);
extern AbsoluteTime GetCurrentTransactionStartTime(void);
extern AbsoluteTime GetCurrentTransactionStartTimeUsec(int *usec);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern bool CommandIdIsCurrentCommandId(CommandId cid);
extern void CommandCounterIncrement(void);
extern void StartTransactionCommand(void);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern void EndTransactionBlock(void);
extern bool IsTransactionBlock(void);
extern bool IsTransactionOrTransactionBlock(void);
extern char TransactionBlockStatusCode(void);
extern void UserAbortTransactionBlock(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventTransactionChain(void *stmtNode, const char *stmtType);
extern void RequireTransactionChain(void *stmtNode, const char *stmtType);
extern void RegisterEOXactCallback(EOXactCallback callback, void *arg);
extern void UnregisterEOXactCallback(EOXactCallback callback, void *arg);

extern void RecordTransactionCommit(void);

extern void XactPushRollback(void (*func) (void *), void *data);
extern void XactPopRollback(void);

extern void xact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_undo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_desc(char *buf, uint8 xl_info, char *rec);

#endif   /* XACT_H */
