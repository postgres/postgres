/*-------------------------------------------------------------------------
 *
 * catalog_utils.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.12 2000/11/16 22:30:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"

typedef HeapTuple Operator;

extern Operator oper(char *op, Oid arg1, Oid arg2, bool noError);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);

extern Oid	oper_oid(char *op, Oid arg1, Oid arg2, bool noError);
extern Oid	oprid(Operator op);

extern Oid	any_ordering_op(Oid restype);

#endif	 /* PARSE_OPER_H */
