/*-------------------------------------------------------------------------
 *
 * execnodes.h--
 *    definitions for executor state nodes
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: print.h,v 1.1 1996/11/10 03:05:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRINT_H
#define PRINT_H

#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "executor/tuptable.h"

extern void print(void *obj);
extern void pprint(void *obj);
extern void print_rt(List *rtable);
extern void print_expr(Node *expr, List *rtable);
extern void print_keys(List *keys, List *rtable);
extern void print_tl(List *tlist, List *rtable);
extern void print_slot(TupleTableSlot *slot);
extern char* plannode_type (Plan* p);
extern void print_plan_recursive (Plan* p, Query *parsetree,
				int indentLevel, char* label);
extern void print_plan (Plan* p, Query* parsetree);

#endif /* PRINT_H */
