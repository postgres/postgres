/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.1.1.1 1996/07/09 06:21:52 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	REWRITEREMOVE_H
#define	REWRITEREMOVE_H

extern char *RewriteGetRuleEventRel(char *rulename);
extern void RemoveRewriteRule(char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif	/* REWRITEREMOVE_H */
