/*-------------------------------------------------------------------------
 *
 * parse_expr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_expr.h,v 1.26 2002/03/21 16:01:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include "parser/parse_node.h"


/* GUC parameters */
extern int	max_expr_depth;
extern bool Transform_null_equals;

extern Node *transformExpr(ParseState *pstate, Node *expr);
extern Oid	exprType(Node *expr);
extern int32 exprTypmod(Node *expr);
extern bool exprIsLengthCoercion(Node *expr, int32 *coercedTypmod);
extern void parse_expr_init(void);
extern char *TypeNameToInternalName(TypeName *typename);

#endif   /* PARSE_EXPR_H */
