/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.15 1999/07/19 00:26:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_TARGET_H
#define PARSE_TARGET_H

#include "parser/parse_node.h"

extern List *transformTargetList(ParseState *pstate, List *targetlist);
extern TargetEntry *transformTargetEntry(ParseState *pstate,
										 Node *node, Node *expr,
										 char *colname, bool resjunk);
extern void updateTargetListEntry(ParseState *pstate, TargetEntry *tle,
								  char *colname, List *indirection);
extern Node *CoerceTargetExpr(ParseState *pstate, Node *expr,
							  Oid type_id, Oid attrtype);
extern List *makeTargetNames(ParseState *pstate, List *cols);

#endif	 /* PARSE_TARGET_H */
