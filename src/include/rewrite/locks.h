/*-------------------------------------------------------------------------
 *
 * locks.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: locks.h,v 1.11 1999/02/13 23:21:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKS_H
#define LOCKS_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "rewrite/prs2lock.h"

extern List *matchLocks(CmdType event, RuleLock *rulelocks, int varno,
		   Query *parsetree);
extern void checkLockPerms(List *locks, Query *parsetree, int rt_index);

#endif	 /* LOCKS_H */
