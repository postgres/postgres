/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.1 1997/11/25 22:07:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_TARGET_H
#define PARSE_TARGET_H

#include <nodes/pg_list.h>
#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/primnodes.h>
#include <parser/parse_node.h>

#define EXPR_COLUMN_FIRST	1
#define EXPR_RELATION_FIRST	2

List *transformTargetList(ParseState *pstate, List *targetlist);

TargetEntry *make_targetlist_expr(ParseState *pstate,
					 char *colname,
					 Node *expr,
					 List *arrayRef);

List *expandAllTables(ParseState *pstate);

char *figureColname(Node *expr, Node *resval);

List *makeTargetNames(ParseState *pstate, List *cols);

#endif							/* PARSE_TARGET_H */

