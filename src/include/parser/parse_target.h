/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.10 1998/08/25 03:17:29 momjian Exp $
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
MakeTargetEntryIdent(ParseState *pstate,
					 Node *node,
					 char **resname,
					 char *refname,
					 char *colname,
					 int16 resjunk);
extern Node *
CoerceTargetExpr(ParseState *pstate, Node *expr,
				 Oid type_id, Oid attrtype);
TargetEntry * MakeTargetEntryExpr(ParseState *pstate,
				   char *colname,
				   Node *expr,
				   List *arrayRef,
				   int16 resjunk);

#endif							/* PARSE_TARGET_H */
