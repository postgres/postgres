/*-------------------------------------------------------------------------
 *
 * locks.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: locks.h,v 1.2 1996/11/06 10:30:59 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	LOCKS_H
#define	LOCKS_H


extern List *matchLocks(CmdType event, RuleLock *rulelocks, int varno,
			Query *parsetree);

#endif	/* LOCKS_H */
