/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.14 2002/06/20 20:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern void RemoveRewriteRule(Oid owningRel, const char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif   /* REWRITEREMOVE_H */
