/*-------------------------------------------------------------------------
 *
 * rewriteSearchCycle.h
 *		Support for rewriting SEARCH and CYCLE clauses.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteSearchCycle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESEARCHCYCLE_H
#define REWRITESEARCHCYCLE_H

#include "nodes/parsenodes.h"

extern CommonTableExpr *rewriteSearchAndCycle(CommonTableExpr *cte);

#endif							/* REWRITESEARCHCYCLE_H */
