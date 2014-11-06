/*
 * xloginsert.h
 *
 * Functions for generating WAL records
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xloginsert.h
 */
#ifndef XLOGINSERT_H
#define XLOGINSERT_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/relfilenode.h"

/*
 * The rmgr data to be written by XLogInsert() is defined by a chain of
 * one or more XLogRecData structs.  (Multiple structs would be used when
 * parts of the source data aren't physically adjacent in memory, or when
 * multiple associated buffers need to be specified.)
 *
 * If buffer is valid then XLOG will check if buffer must be backed up
 * (ie, whether this is first change of that page since last checkpoint).
 * If so, the whole page contents are attached to the XLOG record, and XLOG
 * sets XLR_BKP_BLOCK(N) bit in xl_info.  Note that the buffer must be pinned
 * and exclusive-locked by the caller, so that it won't change under us.
 * NB: when the buffer is backed up, we DO NOT insert the data pointed to by
 * this XLogRecData struct into the XLOG record, since we assume it's present
 * in the buffer.  Therefore, rmgr redo routines MUST pay attention to
 * XLR_BKP_BLOCK(N) to know what is actually stored in the XLOG record.
 * The N'th XLR_BKP_BLOCK bit corresponds to the N'th distinct buffer
 * value (ignoring InvalidBuffer) appearing in the rdata chain.
 *
 * When buffer is valid, caller must set buffer_std to indicate whether the
 * page uses standard pd_lower/pd_upper header fields.  If this is true, then
 * XLOG is allowed to omit the free space between pd_lower and pd_upper from
 * the backed-up page image.  Note that even when buffer_std is false, the
 * page MUST have an LSN field as its first eight bytes!
 *
 * Note: data can be NULL to indicate no rmgr data associated with this chain
 * entry.  This can be sensible (ie, not a wasted entry) if buffer is valid.
 * The implication is that the buffer has been changed by the operation being
 * logged, and so may need to be backed up, but the change can be redone using
 * only information already present elsewhere in the XLOG entry.
 */
typedef struct XLogRecData
{
	char	   *data;			/* start of rmgr data to include */
	uint32		len;			/* length of rmgr data to include */
	Buffer		buffer;			/* buffer associated with data, if any */
	bool		buffer_std;		/* buffer has standard pd_lower/pd_upper */
	struct XLogRecData *next;	/* next struct in chain, or NULL */
} XLogRecData;

extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata);
extern XLogRecPtr log_newpage(RelFileNode *rnode, ForkNumber forkNum,
			BlockNumber blk, char *page, bool page_std);
extern XLogRecPtr log_newpage_buffer(Buffer buffer, bool page_std);
extern XLogRecPtr XLogSaveBufferForHint(Buffer buffer, bool buffer_std);
extern bool XLogCheckBufferNeedsBackup(Buffer buffer);

#endif   /* XLOGINSERT_H */
