#ifndef XLOG_RECORD_INTERNAL_H
#define XLOG_RECORD_INTERNAL_H

#include "access/xlogdefs.h"
#include "c.h"

/*
 * XLogRecPtrToBufIdx returns the index of the WAL buffer that holds, or
 * would hold if it was in cache, the page containing 'recptr'.
 */
#define XLogRecPtrToBufIdx(recptr)	\
	(((recptr) / XLOG_BLCKSZ) % (XLogCtl->XLogCacheBlck + 1))

/*
 * These are the number of bytes in a WAL page usable for WAL data.
 */
#define UsableBytesInPage (XLOG_BLCKSZ - SizeOfXLogShortPHD)

/*
 * Convert values of GUCs measured in megabytes to equiv. segment count.
 * Rounds down.
 */
#define ConvertToXSegs(x, segsize)	XLogMBVarToSegs((x), (segsize))

void ComputeUsableBytesInSegment(void);
/* Implementation remains in xlog.c for now */
void AdvanceXLInsertBuffer(XLogRecPtr upto, bool opportunistic);
char *GetXLogBuffer(XLogRecPtr ptr);
XLogRecPtr XLogBytePosToRecPtr(uint64 bytepos);
XLogRecPtr XLogBytePosToEndRecPtr(uint64 bytepos);
uint64 XLogRecPtrToBytePos(XLogRecPtr ptr);

#endif /* XLOG_RECORD_INTERNAL_H */