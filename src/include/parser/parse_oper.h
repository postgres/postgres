/*-------------------------------------------------------------------------
 *
 * parse_oper.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.14 2001/02/16 03:16:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"

typedef HeapTuple Operator;

/* Routines to find operators matching a name and given input types */
/* NB: the selected operator may require coercion of the input types! */
extern Operator oper(char *op, Oid arg1, Oid arg2, bool noError);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);

/* Routines to find operators that DO NOT require coercion --- ie, their */
/* input types are either exactly as given, or binary-compatible */
extern Operator compatible_oper(char *op, Oid arg1, Oid arg2, bool noError);
/* currently no need for compatible_left_oper/compatible_right_oper */

/* Convenience routines that call compatible_oper() and return either */
/* the operator OID or the underlying function OID, or InvalidOid if fail */
extern Oid compatible_oper_opid(char *op, Oid arg1, Oid arg2, bool noError);
extern Oid compatible_oper_funcid(char *op, Oid arg1, Oid arg2, bool noError);

/* Convenience routine that packages a specific call on compatible_oper */
extern Oid	any_ordering_op(Oid argtype);

/* Extract operator OID or underlying-function OID from an Operator tuple */
extern Oid	oprid(Operator op);
extern Oid	oprfuncid(Operator op);

#endif	 /* PARSE_OPER_H */
