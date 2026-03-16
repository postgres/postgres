/*-------------------------------------------------------------------------
 *
 * parse_graphtable.h
 *		parsing of GRAPH_TABLE
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_graphtable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_GRAPHTABLE_H
#define PARSE_GRAPHTABLE_H

#include "nodes/pg_list.h"
#include "parser/parse_node.h"

extern Node *transformGraphTablePropertyRef(ParseState *pstate, ColumnRef *cref);

extern Node *transformGraphPattern(ParseState *pstate, GraphPattern *graph_pattern);

#endif							/* PARSE_GRAPHTABLE_H */
