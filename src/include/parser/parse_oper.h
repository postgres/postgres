/*-------------------------------------------------------------------------
 *
 * catalog_utils.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.1 1997/11/25 22:06:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include <parser/parse_func.h>
#include <parser/parse_node.h>

typedef HeapTuple Operator;

Oid any_ordering_op(int restype);

Oid oprid(Operator op);

int binary_oper_get_candidates(char *opname,
						   Oid leftTypeId,
						   Oid rightTypeId,
						   CandidateList *candidates);

bool equivalentOpersAfterPromotion(CandidateList candidates);

CandidateList binary_oper_select_candidate(Oid arg1,
							 Oid arg2,
							 CandidateList candidates);

Operator oper(char *op, Oid arg1, Oid arg2, bool noWarnings);

int
unary_oper_get_candidates(char *op,
						  Oid typeId,
						  CandidateList *candidates,
						  char rightleft);

Operator right_oper(char *op, Oid arg);
						  
Operator left_oper(char *op, Oid arg);

void op_error(char *op, Oid arg1, Oid arg2);

#endif							/* PARSE_OPER_H */
