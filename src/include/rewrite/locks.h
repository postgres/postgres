/*-------------------------------------------------------------------------
 *
 * locks.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: locks.h,v 1.13 2000/01/26 05:58:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKS_H
#define LOCKS_H

#include "nodes/parsenodes.h"
#include "rewrite/prs2lock.h"

extern List *matchLocks(CmdType event, RuleLock *rulelocks, int varno,
		   Query *parsetree);
extern void checkLockPerms(List *locks, Query *parsetree, int rt_index);

#endif	 /* LOCKS_H */
