/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteRemove.h,v 1.15 2002/07/12 18:43:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

#include "nodes/parsenodes.h"


extern void RemoveRewriteRule(Oid owningRel, const char *ruleName,
							  DropBehavior behavior);
extern void RemoveRewriteRuleById(Oid ruleOid);

#endif   /* REWRITEREMOVE_H */
