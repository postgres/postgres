/*-------------------------------------------------------------------------
 *
 * rewriteGraphTable.h
 *		Support for rewriting GRAPH_TABLE clauses.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteGraphTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEGRAPHTABLE_H
#define REWRITEGRAPHTABLE_H

#include "nodes/parsenodes.h"

extern Query *rewriteGraphTable(Query *parsetree, int rt_index);

#endif							/* REWRITEGRAPHTABLE_H */
