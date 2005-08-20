/*-------------------------------------------------------------------------
 *
 * bgwriter.h
 *	  Exports from postmaster/bgwriter.c.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/bgwriter.h,v 1.7 2005/08/20 23:26:33 tgl Exp $
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

extern void BackgroundWriterMain(void);

extern void RequestCheckpoint(bool waitforit, bool warnontime);

extern bool ForwardFsyncRequest(RelFileNode rnode, BlockNumber segno);
extern void AbsorbFsyncRequests(void);

extern Size BgWriterShmemSize(void);
extern void BgWriterShmemInit(void);

#endif   /* _BGWRITER_H */
