/*-------------------------------------------------------------------------
 *
 * catalog_utils.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.4 1998/02/26 04:42:47 momjian Exp $
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

#endif							/* PARSE_OPER_H */
