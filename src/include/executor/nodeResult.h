/*-------------------------------------------------------------------------
 *
 * nodeResult.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeResult.h,v 1.1 1996/08/28 07:22:24 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODERESULT_H
#define	NODERESULT_H

extern TupleTableSlot *ExecResult(Result *node);
extern bool ExecInitResult(Result *node, EState *estate, Plan *parent);
extern int ExecCountSlotsResult(Result *node);
extern void ExecEndResult(Result *node);

#endif	/* NODERESULT_H */
