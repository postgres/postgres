/*-------------------------------------------------------------------------
 *
 * catalog_utils.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.8 1999/07/15 15:21:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"

typedef HeapTuple Operator;

extern Oid	any_ordering_op(int restype);
extern Oid	oprid(Operator op);
extern Operator oper(char *op, Oid arg1, Oid arg2, bool noWarnings);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);

extern Operator oper_exact(char *op, Oid arg1, Oid arg2, Node **ltree, Node **rtree, bool noWarnings);
extern Operator oper_inexact(char *op, Oid arg1, Oid arg2, Node **ltree, Node **rtree, bool noWarnings);

#endif	 /* PARSE_OPER_H */
