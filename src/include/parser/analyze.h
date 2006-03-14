/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/analyze.h,v 1.32 2006/03/14 22:48:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "parser/parse_node.h"


extern List *parse_analyze(Node *parseTree, const char *sourceText,
						   Oid *paramTypes, int numParams);
extern List *parse_analyze_varparams(Node *parseTree, const char *sourceText,
									 Oid **paramTypes, int *numParams);
extern List *parse_sub_analyze(Node *parseTree, ParseState *parentParseState);
extern List *analyzeCreateSchemaStmt(CreateSchemaStmt *stmt);
extern void CheckSelectLocking(Query *qry, bool forUpdate);

#endif   /* ANALYZE_H */
