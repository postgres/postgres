/*-------------------------------------------------------------------------
 *
 * catalog_utils.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.7 1999/02/13 23:21:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include <parser/parse_func.h>
#include <parser/parse_node.h>

typedef HeapTuple Operator;

extern Oid	any_ordering_op(int restype);
extern Oid	oprid(Operator op);
extern Operator oper(char *op, Oid arg1, Oid arg2, bool noWarnings);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);

extern Operator oper_exact(char *op, Oid arg1, Oid arg2, Node **ltree, Node **rtree, bool noWarnings);
extern Operator oper_inexact(char *op, Oid arg1, Oid arg2, Node **ltree, Node **rtree, bool noWarnings);

#endif	 /* PARSE_OPER_H */
