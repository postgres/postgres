/*-------------------------------------------------------------------------
 *
 * parse_exer.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_expr.h,v 1.7 1998/02/10 16:04:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/primnodes.h>
#include <parser/parse_node.h>

extern Node *transformExpr(ParseState *pstate, Node *expr, int precedence);
extern Node *transformIdent(ParseState *pstate, Node *expr, int precedence);
extern Oid exprType(Node *expr);
extern Node *parser_typecast2(Node *expr, Oid exprType, Type tp, int16 attypmod);

#endif							/* PARSE_EXPR_H */

