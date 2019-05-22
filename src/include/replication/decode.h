/*-------------------------------------------------------------------------
 * decode.h
 *	   PostgreSQL WAL to logical transformation
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef DECODE_H
#define DECODE_H

#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "replication/reorderbuffer.h"
#include "replication/logical.h"

void		LogicalDecodingProcessRecord(LogicalDecodingContext *ctx,
										 XLogReaderState *record);

#endif
