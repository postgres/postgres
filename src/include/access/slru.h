/*
 * slru.h
 *
 * Simple LRU
 *
 * Portions Copyright (c) 2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/slru.h,v 1.7 2004/07/01 00:51:38 tgl Exp $
 */
#ifndef SLRU_H
#define SLRU_H

#include "access/xlog.h"
#include "storage/lwlock.h"


/* Opaque structs known only in slru.c */
typedef struct SlruSharedData *SlruShared;
typedef struct SlruFlushData *SlruFlush;

/*
 * SlruCtlData is an unshared structure that points to the active information
 * in shared memory.
 */
typedef struct SlruCtlData
{
	SlruShared	shared;

	LWLockId	ControlLock;

	/*
	 * Dir is set during SimpleLruInit and does not change thereafter.
	 * Since it's always the same, it doesn't need to be in shared memory.
	 */
	char		Dir[MAXPGPATH];

	/*
	 * Decide which of two page numbers is "older" for truncation purposes.
	 * We need to use comparison of TransactionIds here in order to do the
	 * right thing with wraparound XID arithmetic.
	 */
	bool		(*PagePrecedes) (int, int);

} SlruCtlData;

typedef SlruCtlData *SlruCtl;


extern int	SimpleLruShmemSize(void);
extern void SimpleLruInit(SlruCtl ctl, const char *name, const char *subdir);
extern int	SimpleLruZeroPage(SlruCtl ctl, int pageno);
extern char *SimpleLruReadPage(SlruCtl ctl, int pageno,
							   TransactionId xid, bool forwrite);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno, SlruFlush fdata);
extern void SimpleLruSetLatestPage(SlruCtl ctl, int pageno);
extern void SimpleLruFlush(SlruCtl ctl, bool checkpoint);
extern void SimpleLruTruncate(SlruCtl ctl, int cutoffPage);

/* XLOG stuff */
#define CLOG_ZEROPAGE		0x00
#define SUBTRANS_ZEROPAGE	0x10

extern void slru_redo(XLogRecPtr lsn, XLogRecord *record);
extern void slru_undo(XLogRecPtr lsn, XLogRecord *record);
extern void slru_desc(char *buf, uint8 xl_info, char *rec);

#endif   /* SLRU_H */
