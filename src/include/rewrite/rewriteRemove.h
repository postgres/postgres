/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.10 2001/11/05 17:46:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern char *RewriteGetRuleEventRel(char *rulename);
extern void RemoveRewriteRule(char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif   /* REWRITEREMOVE_H */
