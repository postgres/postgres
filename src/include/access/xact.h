/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xact.h,v 1.32 2001/01/24 19:43:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "access/xlog.h"
#include "utils/nabstime.h"

/*
 * Xact isolation levels
 */
#define XACT_DIRTY_READ			0		/* not implemented */
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2		/* not implemented */
#define XACT_SERIALIZABLE		3

extern int	DefaultXactIsoLevel;
extern int	XactIsoLevel;

/* ----------------
 *		transaction state structure
 * ----------------
 */
typedef struct TransactionStateData
{
	TransactionId transactionIdData;
	CommandId	commandId;
	CommandId	scanCommandId;
	AbsoluteTime startTime;
	int			state;
	int			blockState;
} TransactionStateData;

typedef TransactionStateData *TransactionState;

/* ----------------
 *		transaction states
 * ----------------
 */
#define TRANS_DEFAULT			0
#define TRANS_START				1
#define TRANS_INPROGRESS		2
#define TRANS_COMMIT			3
#define TRANS_ABORT				4
#define TRANS_DISABLED			5

/* ----------------
 *		transaction block states
 * ----------------
 */
#define TBLOCK_DEFAULT			0
#define TBLOCK_BEGIN			1
#define TBLOCK_INPROGRESS		2
#define TBLOCK_END				3
#define TBLOCK_ABORT			4
#define TBLOCK_ENDABORT			5

/* ----------------
 *		transaction ID manipulation macros
 * ----------------
 */
#define TransactionIdIsValid(xid)		((bool) ((xid) != NullTransactionId))
#define TransactionIdEquals(id1, id2)	((bool) ((id1) == (id2)))
#define TransactionIdStore(xid, dest)	\
	(*((TransactionId*) (dest)) = (TransactionId) (xid))
#define StoreInvalidTransactionId(dest) \
	(*((TransactionId*) (dest)) = NullTransactionId)

/*
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_XACT_COMMIT    0x00
#define XLOG_XACT_ABORT     0x20

typedef struct xl_xact_commit
{
	time_t		xtime;
	/*
	 * Array of RelFileNode-s to drop may follow
	 * at the end of struct
	 */
} xl_xact_commit;

#define SizeOfXactCommit	((offsetof(xl_xact_commit, xtime) + sizeof(time_t)))

typedef struct xl_xact_abort
{
	time_t		xtime;
} xl_xact_abort;

#define SizeOfXactAbort	((offsetof(xl_xact_abort, xtime) + sizeof(time_t)))

/* ----------------
 *		extern definitions
 * ----------------
 */
extern int	TransactionFlushEnabled(void);
extern void SetTransactionFlushEnabled(bool state);

extern bool IsAbortedTransactionBlockState(void);
extern void OverrideTransactionSystem(bool flag);
extern TransactionId GetCurrentTransactionId(void);
extern CommandId GetCurrentCommandId(void);
extern CommandId GetScanCommandId(void);
extern void SetScanCommandId(CommandId);
extern AbsoluteTime GetCurrentTransactionStartTime(void);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern bool CommandIdIsCurrentCommandId(CommandId cid);
extern bool CommandIdGEScanCommandId(CommandId cid);
extern void CommandCounterIncrement(void);
extern void InitializeTransactionSystem(void);
extern void StartTransactionCommand(void);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern void EndTransactionBlock(void);
extern bool IsTransactionBlock(void);
extern void UserAbortTransactionBlock(void);
extern void AbortOutOfAnyTransaction(void);

extern void RecordTransactionCommit(void);

extern TransactionId DisabledTransactionId;

extern void XactPushRollback(void (*func) (void *), void* data);
extern void XactPopRollback(void);

extern void	xact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void	xact_undo(XLogRecPtr lsn, XLogRecord *record);
extern void	xact_desc(char *buf, uint8 xl_info, char* rec);

/* defined in xid.c */
extern Datum xidin(PG_FUNCTION_ARGS);
extern Datum xidout(PG_FUNCTION_ARGS);
extern Datum xideq(PG_FUNCTION_ARGS);
extern void TransactionIdAdd(TransactionId *xid, int value);

#endif	 /* XACT_H */
