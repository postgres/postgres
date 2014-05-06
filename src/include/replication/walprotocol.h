/*-------------------------------------------------------------------------
 *
 * walprotocol.h
 *	  Definitions relevant to the streaming WAL transmission protocol.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/replication/walprotocol.h,v 1.2 2010/07/06 19:19:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALPROTOCOL_H
#define _WALPROTOCOL_H

#include "access/xlogdefs.h"
#include "utils/timestamp.h"


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
 * Maximum data payload in a WAL data message.  Must be >= XLOG_BLCKSZ.
 *
 * We don't have a good idea of what a good value would be; there's some
 * overhead per message in both walsender and walreceiver, but on the other
 * hand sending large batches makes walsender less responsive to signals
 * because signals are checked only between messages.  128kB (with
 * default 8k blocks) seems like a reasonable guess for now.
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

#endif   /* _WALPROTOCOL_H */
