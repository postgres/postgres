/*-------------------------------------------------------------------------
 *
 * parse_expr.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_expr.h,v 1.14 1999/07/19 00:26:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include "parser/parse_node.h"
#include "parser/parse_type.h"

#define EXPR_COLUMN_FIRST	1
#define EXPR_RELATION_FIRST 2

extern Node *transformExpr(ParseState *pstate, Node *expr, int precedence);
extern Oid	exprType(Node *expr);
extern int32 exprTypmod(Node *expr);
extern Node *parser_typecast2(Node *expr, Oid exprType, Type tp, int32 attypmod);

#endif	 /* PARSE_EXPR_H */
