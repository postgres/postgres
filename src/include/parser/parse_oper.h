/*-------------------------------------------------------------------------
 *
 * catalog_utils.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.11 2000/01/26 05:58:27 momjian Exp $
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
