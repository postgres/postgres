/*-------------------------------------------------------------------------
 *
 * nodeUnique.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeUnique.h,v 1.16 2002/12/05 15:50:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEUNIQUE_H
#define NODEUNIQUE_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsUnique(Unique *node);
extern UniqueState *ExecInitUnique(Unique *node, EState *estate);
extern TupleTableSlot *ExecUnique(UniqueState *node);
extern void ExecEndUnique(UniqueState *node);
extern void ExecReScanUnique(UniqueState *node, ExprContext *exprCtxt);

#endif   /* NODEUNIQUE_H */
