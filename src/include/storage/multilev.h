/*-------------------------------------------------------------------------
 *
 * multilev.h--
 *	  multi level lock table consts/defs for single.c and multi.c and their
 *	  clients
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: multilev.h,v 1.14 1998/10/08 18:30:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MULTILEV_H
#define MULTILEV_H

#include <storage/lmgr.h>

#ifdef LowLevelLocking

/* DML locks */
#define RowShareLock			1		/* SELECT FOR UPDATE */
#define RowExclusiveLock		2		/* INSERT, UPDATE, DELETE */
#define ShareLock				3
#define ShareRowExclusiveLock	4
#define ExclusiveLock			5

/* DDL locks */
#define ObjShareLock			6
#define ObjExclusiveLock		7

/* Special locks */
#define ExtendLock				8

#else

#define READ_LOCK		2
#define WRITE_LOCK		1

/* any time a small granularity READ/WRITE lock is set.
 * Higher granularity READ_INTENT/WRITE_INTENT locks must
 * also be set.  A read intent lock is has value READ+INTENT.
 * in this implementation.
 */
#define NO_LOCK			0
#define INTENT			2
#define READ_INTENT		(READ_LOCK+INTENT)
#define WRITE_INTENT	(WRITE_LOCK+INTENT)

#define EXTEND_LOCK		5

#endif	 /* !LowLevelLocking */

#define SHORT_TERM		1
#define LONG_TERM		2
#define UNLOCK			0

#define N_LEVELS 3
#define RELN_LEVEL 0
#define PAGE_LEVEL 1
#define TUPLE_LEVEL 2
typedef int PG_LOCK_LEVEL;

/* multi.c */

extern LOCKMETHOD MultiTableId;

#ifdef NOT_USED
extern LOCKMETHOD ShortTermTableId;

#endif

/*
 * function prototypes
 */
extern LOCKMETHOD InitMultiLevelLocks(void);
extern bool MultiLockReln(LockInfo lockinfo, LOCKMODE lockmode);
extern bool MultiLockPage(LockInfo lockinfo, ItemPointer tidPtr, LOCKMODE lockmode);
extern bool MultiReleaseReln(LockInfo lockinfo, LOCKMODE lockmode);

#endif	 /* MULTILEV_H */
