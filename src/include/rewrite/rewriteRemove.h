/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.3 1997/09/08 02:38:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern char *RewriteGetRuleEventRel(char *rulename);
extern void RemoveRewriteRule(char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif							/* REWRITEREMOVE_H */
