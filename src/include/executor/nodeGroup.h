/*-------------------------------------------------------------------------
 *
 * nodeGroup.h--
 *	  prototypes for nodeGroup.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeGroup.h,v 1.3 1997/09/08 02:36:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

extern TupleTableSlot *ExecGroup(Group * node);
extern bool ExecInitGroup(Group * node, EState * estate, Plan * parent);
extern int	ExecCountSlotsGroup(Group * node);
extern void ExecEndGroup(Group * node);

#endif							/* NODEGROUP_H */
