/*-------------------------------------------------------------------------
 *
 * analyze.h
 *		parse analysis for optimizable statements
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/analyze.h,v 1.45 2010/02/26 02:01:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "parser/parse_node.h"


extern Query *parse_analyze(Node *parseTree, const char *sourceText,
			  Oid *paramTypes, int numParams);
extern Query *parse_analyze_varparams(Node *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams);

extern Query *parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent);
extern Query *transformStmt(ParseState *pstate, Node *parseTree);

extern bool analyze_requires_snapshot(Node *parseTree);

extern void CheckSelectLocking(Query *qry);
extern void applyLockingClause(Query *qry, Index rtindex,
				   bool forUpdate, bool noWait, bool pushedDown);

#endif   /* ANALYZE_H */
