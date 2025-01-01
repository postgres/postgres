/*-------------------------------------------------------------------------
 * message.h
 *	   Exports from replication/logical/message.c
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * src/include/replication/message.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_LOGICAL_MESSAGE_H
#define PG_LOGICAL_MESSAGE_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "access/xlogreader.h"

/*
 * Generic logical decoding message wal record.
 */
typedef struct xl_logical_message
{
	Oid			dbId;			/* database Oid emitted from */
	bool		transactional;	/* is message transactional? */
	Size		prefix_size;	/* length of prefix */
	Size		message_size;	/* size of the message */
	/* payload, including null-terminated prefix of length prefix_size */
	char		message[FLEXIBLE_ARRAY_MEMBER];
} xl_logical_message;

#define SizeOfLogicalMessage	(offsetof(xl_logical_message, message))

extern XLogRecPtr LogLogicalMessage(const char *prefix, const char *message,
									size_t size, bool transactional,
									bool flush);

/* RMGR API */
#define XLOG_LOGICAL_MESSAGE	0x00
extern void logicalmsg_redo(XLogReaderState *record);
extern void logicalmsg_desc(StringInfo buf, XLogReaderState *record);
extern const char *logicalmsg_identify(uint8 info);

#endif							/* PG_LOGICAL_MESSAGE_H */
