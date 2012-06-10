/*-------------------------------------------------------------------------
 *
 * walprotocol.h
 *	  Definitions relevant to the streaming WAL transmission protocol.
 *
 * Portions Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * src/include/replication/walprotocol.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALPROTOCOL_H
#define _WALPROTOCOL_H

#include "access/xlogdefs.h"
#include "datatype/timestamp.h"


/*
 * All messages from WalSender must contain these fields to allow us to
 * correctly calculate the replication delay.
 */
typedef struct
{
	/* Current end of WAL on the sender */
	XLogRecPtr	walEnd;

	/* Sender's system clock at the time of transmission */
	TimestampTz sendTime;
} WalSndrMessage;


/*
 * Header for a WAL data message (message type 'w').  This is wrapped within
 * a CopyData message at the FE/BE protocol level.
 *
 * The header is followed by actual WAL data.  Note that the data length is
 * not specified in the header --- it's just whatever remains in the message.
 *
 * walEnd and sendTime are not essential data, but are provided in case
 * the receiver wants to adjust its behavior depending on how far behind
 * it is.
 */
typedef struct
{
	/* WAL start location of the data included in this message */
	XLogRecPtr	dataStart;

	/* Current end of WAL on the sender */
	XLogRecPtr	walEnd;

	/* Sender's system clock at the time of transmission */
	TimestampTz sendTime;
} WalDataMessageHeader;

/*
 * Keepalive message from primary (message type 'k'). (lowercase k)
 * This is wrapped within a CopyData message at the FE/BE protocol level.
 *
 * Note that the data length is not specified here.
 */
typedef WalSndrMessage PrimaryKeepaliveMessage;

/*
 * Reply message from standby (message type 'r').  This is wrapped within
 * a CopyData message at the FE/BE protocol level.
 *
 * Note that the data length is not specified here.
 */
typedef struct
{
	/*
	 * The xlog locations that have been written, flushed, and applied by
	 * standby-side. These may be invalid if the standby-side is unable to or
	 * chooses not to report these.
	 */
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;

	/* Sender's system clock at the time of transmission */
	TimestampTz sendTime;
} StandbyReplyMessage;

/*
 * Hot Standby feedback from standby (message type 'h').  This is wrapped within
 * a CopyData message at the FE/BE protocol level.
 *
 * Note that the data length is not specified here.
 */
typedef struct
{
	/*
	 * The current xmin and epoch from the standby, for Hot Standby feedback.
	 * This may be invalid if the standby-side does not support feedback, or
	 * Hot Standby is not yet available.
	 */
	TransactionId xmin;
	uint32		epoch;

	/* Sender's system clock at the time of transmission */
	TimestampTz sendTime;
} StandbyHSFeedbackMessage;

/*
 * Maximum data payload in a WAL data message.	Must be >= XLOG_BLCKSZ.
 *
 * We don't have a good idea of what a good value would be; there's some
 * overhead per message in both walsender and walreceiver, but on the other
 * hand sending large batches makes walsender less responsive to signals
 * because signals are checked only between messages.  128kB (with
 * default 8k blocks) seems like a reasonable guess for now.
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

#endif   /* _WALPROTOCOL_H */
