/*-------------------------------------------------------------------------
 * logicalfuncs.h
 *	   PostgreSQL WAL to logical transformation support functions
 *
 * Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALFUNCS_H
#define LOGICALFUNCS_H

#include "replication/logical.h"

extern int	logical_read_local_xlog_page(XLogReaderState *state,
										 XLogRecPtr targetPagePtr,
										 int reqLen, XLogRecPtr targetRecPtr,
										 char *cur_page, TimeLineID *pageTLI);

#endif
