/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/analyze.h,v 1.25 2004/01/23 02:13:12 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "parser/parse_node.h"


extern List *parse_analyze(Node *parseTree, Oid *paramTypes, int numParams);
extern List *parse_analyze_varparams(Node *parseTree, Oid **paramTypes,
						int *numParams);
extern List *parse_sub_analyze(Node *parseTree, ParseState *parentParseState);
extern List *analyzeCreateSchemaStmt(CreateSchemaStmt *stmt);

extern void CheckSelectForUpdate(Query *qry);

#endif   /* ANALYZE_H */
