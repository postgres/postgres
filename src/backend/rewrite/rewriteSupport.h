/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.1.1.1 1996/07/09 06:21:52 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	REWRITESUPPORT_H
#define	REWRITESUPPORT_H

#include "nodes/pg_list.h"

extern int IsDefinedRewriteRule(char *ruleName);

extern void prs2_addToRelation(Oid relid, Oid ruleId, CmdType event_type,
		   AttrNumber attno, bool isInstead, Node *qual,
		   List *actions);
extern void prs2_deleteFromRelation(Oid relid, Oid ruleId);


#endif	/* REWRITESUPPORT_H */

