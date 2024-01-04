/*-------------------------------------------------------------------------
 *
 * parse_merge.h
 *		handle MERGE statement in parser
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_merge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_MERGE_H
#define PARSE_MERGE_H

#include "parser/parse_node.h"

extern Query *transformMergeStmt(ParseState *pstate, MergeStmt *stmt);

#endif							/* PARSE_MERGE_H */
