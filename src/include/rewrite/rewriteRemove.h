/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.4 1998/09/01 04:38:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern char *RewriteGetRuleEventRel(char *rulename);
extern void RemoveRewriteRule(char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif	 /* REWRITEREMOVE_H */
