/*-------------------------------------------------------------------------
 *
 * nodeSort.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSort.h,v 1.2 1997/09/07 04:58:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESORT_H
#define NODESORT_H

extern TupleTableSlot *ExecSort(Sort * node);
extern bool		ExecInitSort(Sort * node, EState * estate, Plan * parent);
extern int		ExecCountSlotsSort(Sort * node);
extern void		ExecEndSort(Sort * node);
extern void		ExecSortMarkPos(Sort * node);
extern void		ExecSortRestrPos(Sort * node);

#endif							/* NODESORT_H */
