/*-------------------------------------------------------------------------
 *
 * locks.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: locks.h,v 1.9 1998/09/01 04:37:57 momjian Exp $
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

#endif	 /* LOCKS_H */
