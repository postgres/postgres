/*-------------------------------------------------------------------------
 *
 * nodeAppend.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeAppend.h,v 1.1.1.1 1996/07/09 06:21:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODEAPPEND_H
#define	NODEAPPEND_H

extern bool exec_append_initialize_next(Append *node);
extern bool ExecInitAppend(Append *node, EState *estate, Plan *parent);
extern int ExecCountSlotsAppend(Append *node);
extern TupleTableSlot *ExecProcAppend(Append *node);
extern void ExecEndAppend(Append *node);

#endif	/* NODEAPPEND_H */
