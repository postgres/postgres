/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeHashjoin.h,v 1.3 1997/09/07 04:57:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

extern TupleTableSlot *ExecHashJoin(HashJoin * node);

extern bool		ExecInitHashJoin(HashJoin * node, EState * estate, Plan * parent);

extern int		ExecCountSlotsHashJoin(HashJoin * node);

extern void		ExecEndHashJoin(HashJoin * node);

extern char    *
ExecHashJoinSaveTuple(HeapTuple heapTuple, char *buffer,
					  File file, char *position);


#endif							/* NODEHASHJOIN_H */
