/*-------------------------------------------------------------------------
 *
 * bgwriter.h
 *	  Exports from postmaster/bgwriter.c.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/bgwriter.h,v 1.5 2005/03/04 20:21:06 tgl Exp $
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

extern void RequestCheckpoint(bool waitforit);

extern bool ForwardFsyncRequest(RelFileNode rnode, BlockNumber segno);
extern void AbsorbFsyncRequests(void);

extern int	BgWriterShmemSize(void);
extern void BgWriterShmemInit(void);

#endif   /* _BGWRITER_H */
