/*-------------------------------------------------------------------------
 *
 * catalog_utils.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_oper.h,v 1.2 1997/11/26 01:14:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include <parser/parse_func.h>
#include <parser/parse_node.h>

typedef HeapTuple Operator;

extern Oid any_ordering_op(int restype);
extern Oid oprid(Operator op);
extern int binary_oper_get_candidates(char *opname,
						   Oid leftTypeId,
						   Oid rightTypeId,
						   CandidateList *candidates);
extern bool equivalentOpersAfterPromotion(CandidateList candidates);
extern CandidateList binary_oper_select_candidate(Oid arg1,
							 Oid arg2,
							 CandidateList candidates);
extern Operator oper(char *op, Oid arg1, Oid arg2, bool noWarnings);
extern int unary_oper_get_candidates(char *op,
						  Oid typeId,
						  CandidateList *candidates,
						  char rightleft);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);
extern void op_error(char *op, Oid arg1, Oid arg2);

#endif							/* PARSE_OPER_H */
