/*-------------------------------------------------------------------------
 *
 * parse_exer.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_expr.h,v 1.12 1999/07/15 15:21:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include <parser/parse_node.h>
#include "parser/parse_type.h"

extern Node *transformExpr(ParseState *pstate, Node *expr, int precedence);
extern Oid	exprType(Node *expr);
extern Node *parser_typecast2(Node *expr, Oid exprType, Type tp, int32 attypmod);

#endif	 /* PARSE_EXPR_H */
