/*-------------------------------------------------------------------------
 *
 * nodeUnique.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeUnique.h,v 1.2 1997/09/07 04:58:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEUNIQUE_H
#define NODEUNIQUE_H

extern TupleTableSlot *ExecUnique(Unique * node);
extern bool		ExecInitUnique(Unique * node, EState * estate, Plan * parent);
extern int		ExecCountSlotsUnique(Unique * node);
extern void		ExecEndUnique(Unique * node);

#endif							/* NODEUNIQUE_H */
