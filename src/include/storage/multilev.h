/*-------------------------------------------------------------------------
 *
 * multilev.h--
 *    multi level lock table consts/defs for single.c and multi.c and their
 *    clients
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: multilev.h,v 1.1 1996/08/28 01:58:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MULTILEV_H
#define MULTILEV_H

#include "storage/lock.h"
#include "storage/lmgr.h"

#define READ_LOCK  	2
#define WRITE_LOCK 	1

/* any time a small granularity READ/WRITE lock is set.  
 * Higher granularity READ_INTENT/WRITE_INTENT locks must
 * also be set.  A read intent lock is has value READ+INTENT.
 * in this implementation.
 */
#define NO_LOCK		0
#define INTENT		2
#define READ_INTENT	(READ_LOCK+INTENT)
#define WRITE_INTENT	(WRITE_LOCK+INTENT)

#define EXTEND_LOCK	5

#define SHORT_TERM	1
#define LONG_TERM	2
#define UNLOCK		0

#define N_LEVELS 3
#define RELN_LEVEL 0
#define PAGE_LEVEL 1
#define TUPLE_LEVEL 2
typedef int LOCK_LEVEL;

/* multi.c */

extern LockTableId MultiTableId;
extern LockTableId ShortTermTableId;

/*
 * function prototypes
 */
extern LockTableId InitMultiLevelLockm(void);
extern bool MultiLockReln(LockInfo linfo, LOCKT lockt);
extern bool MultiLockTuple(LockInfo linfo, ItemPointer tidPtr, LOCKT lockt);
extern bool MultiLockPage(LockInfo linfo, ItemPointer tidPtr, LOCKT lockt);
extern bool MultiAcquire(LockTableId tableId, LOCKTAG *tag, LOCKT lockt,
			 LOCK_LEVEL level);
extern bool MultiReleasePage(LockInfo linfo, ItemPointer tidPtr, LOCKT lockt);
extern bool MultiReleaseReln(LockInfo linfo, LOCKT lockt);
extern bool MultiRelease(LockTableId tableId, LOCKTAG *tag, LOCKT lockt,
			 LOCK_LEVEL level);

#endif /* MULTILEV_H */
