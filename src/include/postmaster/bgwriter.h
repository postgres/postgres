/*-------------------------------------------------------------------------
 *
 * bgwriter.h
 *	  Exports from postmaster/bgwriter.c.
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/bgwriter.h,v 1.10 2007/06/28 00:02:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BGWRITER_H
#define _BGWRITER_H

#include "storage/block.h"
#include "storage/relfilenode.h"


/* GUC options */
extern int	BgWriterDelay;
extern int	CheckPointTimeout;
extern int	CheckPointWarning;
extern double CheckPointCompletionTarget;

extern void BackgroundWriterMain(void);

extern void RequestCheckpoint(int flags);
extern void CheckpointWriteDelay(int flags, double progress);

extern bool ForwardFsyncRequest(RelFileNode rnode, BlockNumber segno);
extern void AbsorbFsyncRequests(void);

extern Size BgWriterShmemSize(void);
extern void BgWriterShmemInit(void);

#endif   /* _BGWRITER_H */
