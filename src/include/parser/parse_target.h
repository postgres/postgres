/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.2 1997/11/26 01:14:10 momjian Exp $
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

extern List *transformTargetList(ParseState *pstate, List *targetlist);
extern TargetEntry *make_targetlist_expr(ParseState *pstate,
					 char *colname,
					 Node *expr,
					 List *arrayRef);
extern List *expandAllTables(ParseState *pstate);
extern char *figureColname(Node *expr, Node *resval);
extern List *makeTargetNames(ParseState *pstate, List *cols);

#endif							/* PARSE_TARGET_H */

