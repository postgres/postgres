/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.10 1999/02/13 23:22:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H

#include "access/attnum.h"
#include "nodes/pg_list.h"

extern int	IsDefinedRewriteRule(char *ruleName);

extern void prs2_addToRelation(Oid relid, Oid ruleId, CmdType event_type,
				   AttrNumber attno, bool isInstead, Node *qual,
				   List *actions);
extern void prs2_deleteFromRelation(Oid relid, Oid ruleId);


#endif	 /* REWRITESUPPORT_H */
