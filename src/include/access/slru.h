/*
 * slru.h
 *
 * Simple LRU
 *
 * Portions Copyright (c) 2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: slru.h,v 1.3 2003/08/08 21:42:32 momjian Exp $
 */
#ifndef SLRU_H
#define SLRU_H

#include "access/xlog.h"

/* exported because lwlock.c needs it */
#define NUM_CLOG_BUFFERS	8

typedef struct SlruLockData
{
	LWLockId	ControlLock;
/*
 * BufferLocks is set during CLOGShmemInit and does not change thereafter.
 * The value is automatically inherited by backends via fork, and
 * doesn't need to be in shared memory.
 */
	LWLockId	BufferLocks[NUM_CLOG_BUFFERS];	/* Per-buffer I/O locks */
} SlruLockData;
typedef SlruLockData *SlruLock;

typedef struct SlruCtlData
{
	void	   *shared;			/* pointer to SlruSharedData */
	SlruLock	locks;

/*
 * Dir is set during SimpleLruShmemInit and does not change thereafter.
 * The value is automatically inherited by backends via fork, and
 * doesn't need to be in shared memory.
 */
	char		Dir[MAXPGPATH];

/*
 * Decide which of two page numbers is "older" for truncation purposes.
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.
 */
	bool		(*PagePrecedes) (int, int);

} SlruCtlData;
typedef SlruCtlData *SlruCtl;

extern int	SimpleLruShmemSize(void);
extern void SimpleLruInit(SlruCtl ctl, const char *name, const char *subdir);
extern int	SimpleLruZeroPage(SlruCtl ctl, int pageno);
extern char *SimpleLruReadPage(SlruCtl ctl, int pageno, TransactionId xid, bool forwrite);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno);
extern void SimpleLruSetLatestPage(SlruCtl ctl, int pageno);
extern void SimpleLruFlush(SlruCtl ctl, bool checkpoint);
extern void SimpleLruTruncate(SlruCtl ctl, int cutoffPage);

#endif   /* SLRU_H */
