/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.7 1999/05/25 16:14:24 momjian Exp $
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
