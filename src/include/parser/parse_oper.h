/*-------------------------------------------------------------------------
 *
 * catalog_utils.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.10 1999/12/12 20:51:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"

typedef HeapTuple Operator;

extern Oid	any_ordering_op(Oid restype);
extern Oid	oprid(Operator op);
extern Operator oper(char *op, Oid arg1, Oid arg2, bool noWarnings);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);

#endif	 /* PARSE_OPER_H */
