/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.6 1999/05/13 07:29:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include <parser/parse_node.h>

extern List *parse_analyze(List *pl, ParseState *parentParseState);
/***S*I***/
extern void create_select_list(Node *ptr, List **select_list, bool *unionall_present);
extern Node *A_Expr_to_Expr(Node *ptr, bool *intersect_present);

#endif	 /* ANALYZE_H */
