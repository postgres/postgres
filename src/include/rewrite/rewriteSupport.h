/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.4 1997/09/08 02:38:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H


extern int	IsDefinedRewriteRule(char *ruleName);

extern void
prs2_addToRelation(Oid relid, Oid ruleId, CmdType event_type,
				   AttrNumber attno, bool isInstead, Node * qual,
				   List * actions);
extern void prs2_deleteFromRelation(Oid relid, Oid ruleId);


#endif							/* REWRITESUPPORT_H */
