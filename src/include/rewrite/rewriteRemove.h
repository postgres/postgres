/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.12 2002/03/29 19:06:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

extern void RemoveRewriteRule(List *names);
extern void RelationRemoveRules(Oid relid);

#endif   /* REWRITEREMOVE_H */
