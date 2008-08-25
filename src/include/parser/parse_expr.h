/*-------------------------------------------------------------------------
 *
 * parse_expr.h
 *	  handle expressions in parser
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_expr.h,v 1.39 2008/08/25 22:42:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include "parser/parse_node.h"

/* GUC parameters */
extern bool Transform_null_equals;

extern Node *transformExpr(ParseState *pstate, Node *expr);

#endif   /* PARSE_EXPR_H */
