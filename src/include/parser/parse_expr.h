/*-------------------------------------------------------------------------
 *
 * parse_expr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_expr.h,v 1.18 2000/03/17 05:29:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include "parser/parse_node.h"
#include "parser/parse_type.h"

#define EXPR_COLUMN_FIRST	1
#define EXPR_RELATION_FIRST 2

extern int	max_expr_depth;

extern Node *transformExpr(ParseState *pstate, Node *expr, int precedence);
extern Oid	exprType(Node *expr);
extern int32 exprTypmod(Node *expr);
extern bool exprIsLengthCoercion(Node *expr, int32 *coercedTypmod);
extern void parse_expr_init(void);

#endif	 /* PARSE_EXPR_H */
