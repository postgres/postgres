/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.2 1997/09/07 05:00:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern char    *RewriteGetRuleEventRel(char *rulename);
extern void		RemoveRewriteRule(char *ruleName);
extern void		RelationRemoveRules(Oid relid);

#endif							/* REWRITEREMOVE_H */
