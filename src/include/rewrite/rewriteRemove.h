/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.9 2001/10/28 06:26:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern char *RewriteGetRuleEventRel(char *rulename);
extern void RemoveRewriteRule(char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif	 /* REWRITEREMOVE_H */
