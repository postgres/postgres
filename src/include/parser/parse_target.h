/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.8 1998/07/08 14:18:45 thomas Exp $
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
#define EXPR_RELATION_FIRST 2

extern List *transformTargetList(ParseState *pstate, List *targetlist);
extern List *makeTargetNames(ParseState *pstate, List *cols);
extern TargetEntry *
transformTargetIdent(ParseState *pstate,
					 Node *node,
					 TargetEntry *tent,
					 char **resname,
					 char *refname,
					 char *colname,
					 int16 resjunk);
extern Node *
CoerceTargetExpr(ParseState *pstate, Node *expr,
				 Oid type_id, Oid attrtype);

#endif							/* PARSE_TARGET_H */
