/*-------------------------------------------------------------------------
 *
 * nodeNestloop.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeNestloop.h,v 1.3 1997/09/08 02:36:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODENESTLOOP_H
#define NODENESTLOOP_H

extern TupleTableSlot *ExecNestLoop(NestLoop * node, Plan * parent);
extern bool ExecInitNestLoop(NestLoop * node, EState * estate, Plan * parent);
extern int	ExecCountSlotsNestLoop(NestLoop * node);
extern void ExecEndNestLoop(NestLoop * node);

#endif							/* NODENESTLOOP_H */
