/*-------------------------------------------------------------------------
 *
 * locks.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: locks.h,v 1.1 1996/08/28 07:24:04 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	LOCKS_H
#define	LOCKS_H

#include "rewrite/prs2lock.h"

extern List *matchLocks(CmdType event, RuleLock *rulelocks, int varno,
			Query *parsetree);

#endif	/* LOCKS_H */
