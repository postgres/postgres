/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.5 1999/01/18 00:10:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include <parser/parse_node.h>

extern QueryTreeList *parse_analyze(List *pl, ParseState *parentParseState);
/***S*I***/
extern void create_select_list(Node *ptr, List **select_list, bool *unionall_present);
extern Node *A_Expr_to_Expr(Node *ptr, bool *intersect_present);

#endif	 /* ANALYZE_H */
