/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.13 2000/12/06 23:55:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "parser/parse_node.h"

extern List *parse_analyze(Node *parseTree, ParseState *parentParseState);

extern void CheckSelectForUpdate(Query *qry);

#endif	 /* ANALYZE_H */
