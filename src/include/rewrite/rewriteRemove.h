/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.1 1996/08/28 07:24:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	REWRITEREMOVE_H
#define	REWRITEREMOVE_H

extern char *RewriteGetRuleEventRel(char *rulename);
extern void RemoveRewriteRule(char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif	/* REWRITEREMOVE_H */
