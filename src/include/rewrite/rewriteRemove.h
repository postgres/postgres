/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.13 2002/04/18 20:01:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern void RemoveRewriteRule(Oid owningRel, const char *ruleName);
extern void RelationRemoveRules(Oid relid);

#endif   /* REWRITEREMOVE_H */
