/*-------------------------------------------------------------------------
 *
 * nodeResult.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeResult.h,v 1.3 1997/09/08 02:36:38 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODERESULT_H
#define NODERESULT_H

extern TupleTableSlot *ExecResult(Result * node);
extern bool ExecInitResult(Result * node, EState * estate, Plan * parent);
extern int	ExecCountSlotsResult(Result * node);
extern void ExecEndResult(Result * node);

#endif							/* NODERESULT_H */
