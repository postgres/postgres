/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.14 2001/01/24 19:43:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "parser/parse_node.h"

extern List *parse_analyze(Node *parseTree, ParseState *parentParseState);

extern void CheckSelectForUpdate(Query *qry);

#endif	 /* ANALYZE_H */
