/*-------------------------------------------------------------------------
 *
 * nodeUnique.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeUnique.h,v 1.3 1997/09/08 02:36:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEUNIQUE_H
#define NODEUNIQUE_H

extern TupleTableSlot *ExecUnique(Unique * node);
extern bool ExecInitUnique(Unique * node, EState * estate, Plan * parent);
extern int	ExecCountSlotsUnique(Unique * node);
extern void ExecEndUnique(Unique * node);

#endif							/* NODEUNIQUE_H */
