/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xact.h,v 1.49 2003/01/10 22:03:30 petere Exp $
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
extern bool	DefaultXactReadOnly;
extern bool	XactReadOnly;


/* ----------------
 *		transaction state structure
 * ----------------
 */
typedef struct TransactionStateData
{
	TransactionId transactionIdData;
	CommandId	commandId;
	AbsoluteTime startTime;
	int			startTimeUsec;
	int			state;
	int			blockState;
} TransactionStateData;

typedef TransactionStateData *TransactionState;

/*
 *	transaction states - transaction state from server perspective
 *	
 *	Syntax error could cause transaction to abort, but client code thinks
 *	it is still in a transaction, so we have to wait for COMMIT/ROLLBACK.
 */
#define TRANS_DEFAULT			0
#define TRANS_START				1
#define TRANS_INPROGRESS		2
#define TRANS_COMMIT			3
#define TRANS_ABORT				4

/*
 *	transaction block states - transaction state of client queries
 */
#define TBLOCK_DEFAULT			0
#define TBLOCK_BEGIN			1
#define TBLOCK_INPROGRESS		2
#define TBLOCK_END				3
#define TBLOCK_ABORT			4
#define TBLOCK_ENDABORT			5

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
extern void StartTransactionCommand(bool preventChain);
extern void CommitTransactionCommand(bool forceCommit);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern void EndTransactionBlock(void);
extern bool IsTransactionBlock(void);
extern void UserAbortTransactionBlock(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventTransactionChain(void *stmtNode, const char *stmtType);
extern void RequireTransactionChain(void *stmtNode, const char *stmtType);

extern void RecordTransactionCommit(void);

extern void XactPushRollback(void (*func) (void *), void *data);
extern void XactPopRollback(void);

extern void xact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_undo(XLogRecPtr lsn, XLogRecord *record);
extern void xact_desc(char *buf, uint8 xl_info, char *rec);

/* defined in xid.c */
extern Datum xidin(PG_FUNCTION_ARGS);
extern Datum xidout(PG_FUNCTION_ARGS);
extern Datum xideq(PG_FUNCTION_ARGS);
extern Datum xid_age(PG_FUNCTION_ARGS);

#endif   /* XACT_H */
