/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.5 1998/05/21 03:53:51 scrappy Exp $
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
extern void transformTargetId(ParseState *pstate, Ident *ident,
	TargetEntry *tent, char *resname, int16 resjunk);

#endif							/* PARSE_TARGET_H */
