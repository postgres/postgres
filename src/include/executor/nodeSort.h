/*-------------------------------------------------------------------------
 *
 * nodeSort.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSort.h,v 1.14 2001/11/05 17:46:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESORT_H
#define NODESORT_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecSort(Sort *node);
extern bool ExecInitSort(Sort *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsSort(Sort *node);
extern void ExecEndSort(Sort *node);
extern void ExecSortMarkPos(Sort *node);
extern void ExecSortRestrPos(Sort *node);
extern void ExecReScanSort(Sort *node, ExprContext *exprCtxt, Plan *parent);

#endif   /* NODESORT_H */
