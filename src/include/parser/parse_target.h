/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_target.h,v 1.16 1999/11/01 05:06:20 tgl Exp $
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
								  char *colname, int attrno,
								  List *indirection);
extern Node *CoerceTargetExpr(ParseState *pstate, Node *expr,
							  Oid type_id, Oid attrtype);
extern List *checkInsertTargets(ParseState *pstate, List *cols,
								List **attrnos);

#endif	 /* PARSE_TARGET_H */
